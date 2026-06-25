/* flasher/flasher.c — standalone nRF52840 HPatchLite in-place flasher
 *
 * Bežíme z 0xF2000 (MIMO aplikačnej flash 0x26000-0xD4000).
 * Aplikujeme HPatchLite inplaceB patch z 0xD4000 do 0x26000 in-place.
 *
 * VSTUP (ARM AAPCS):
 *   r0 = patch_addr  — 0xD4000 (staged patch — raw alebo ZLIB komprimovaný)
 *   r1 = patch_size  — veľkosť staged dát v bajtoch
 *   r2 = new_fw_size — (ignoruje sa, čítame z patch hlavičky)
 *
 * Patch formát — nekomprimovaný:
 *   HPatchLite inplaceB (hdiffi -inplaceB old.bin new.bin patch.bin)
 *
 * Patch formát — komprimovaný (preferovaný):
 *   [magic 4B: 'Z','L','I','B']
 *   [uncomp_size 4B LE]     → veľkosť dekomprimovaného HPatchLite patchu
 *   [new_fw_size 4B LE]     → veľkosť nového firmvéru (pre info)
 *   [raw DEFLATE data...]   → Python: zlib.compress(patch, level=9, wbits=-9)
 *
 * Dekomprimácia:
 *   puff() z 0xD4000+12 → RAM buffer @ DECOMP_BUF_ADDR (0x20000000)
 *   Max uncomp_size: DECOMP_BUF_MAX (220kB)
 *
 * Bezpečnosť in-place zápisu:
 *   hpatchi_inplaceB oneskorí zápis o extraSafeSize bajtov cez ring buffer.
 *   Počas zápisu na adresu X sú staré dáta na X stále v XIP flash.
 *
 * STANDALONE: žiadne Arduino/BSP/FreeRTOS.
 * KOMPILUJ:   python tools/build_flasher.py  →  src/flasher_code.h
 */

// Kompiluje sa LEN do standalone flashera cez nrffota/tools/build_flasher.py
// (definuje -DFOTA_FLASHER_BUILD). V MeshCore FW builde (kde tento súbor zoberie
// rekurzívny build_src_filter) ostáva PRÁZDNY — flasher beží mimo app flash.
#ifdef FOTA_FLASHER_BUILD

#include <stdint.h>
#include <string.h>
#include "hpatch_lite.h"
#include "puff_stream.h"   /* STREAMING DEFLATE — patch sa NEdekomprimuje celý do RAM */

/* ── Flash layout — per-board (single source, freestanding-safe) ────── */
/* build_flasher.py kopíruje shared/flash_layout.h do tmp a dáva -DBOARD_* */
#include "flash_layout.h"  /* APP_FLASH_START, APP_FLASH_MAX, FLASH_TRACE_ADDR */
#define PAGE_SIZE         FLASH_PAGE_SIZE

/* Maximálny extraSafeSize z patch hlavičky. Pre malú zmenu vo veľkom FW je
 * extra_safe typicky 0 (overené hdiffi); 4096 je bohatá rezerva. */
#define MAX_EXTRA_SAFE    4096u

/* Read cache pre hpatchi diff stream (okrem extra_safe). Väčší = rýchlejší
 * patch. temp_cache = MAX_EXTRA_SAFE + READ_CACHE, alokovaný v .bss. */
#define READ_CACHE        16384u

/* KOMPRIMOVANÝ patch (~60kB) v RAM — vstup pre puff_stream. Bežný FW ho sem
 * skopíruje pred skokom (memmove po sd_disable). Flasher z neho STREAMUJE.
 * Žiadny veľký DECOMP_BUF — patch sa dekomprimuje on-demand. */
#define PATCH_RAM_ADDR   0x20000000u

/* Magic pre komprimovaný formát: "ZLIB" v LE uint32 */
#define ZPATCH_MAGIC     0x42494C5Au      /* bytes: 5A 4C 49 42 = 'Z','L','I','B' */

/* Magic pre komprimovaný formát: "ZLIB" v LE uint32 */
#define ZPATCH_MAGIC     0x42494C5Au      /* bytes: 5A 4C 49 42 = 'Z','L','I','B' */
#define ZPATCH_HDR_SIZE  12u              /* magic(4) + uncomp_size(4) + new_fw_size(4) */

/* ── Debug status marker — NRF_POWER->GPREGRET2 ───────────────────── *
 * Používame hardwarový retenčný register (0x40000514), NIE RAM.
 * Dôvod: RAM 0x2003FFF0 sa prepisuje main firmware startup stackom
 * (SP=0x20040000, prologue uloží LR+reg na 0x2003FFxx) skôr, ako
 * fota_check_flasher_debug() ho prečíta.
 * GPREGRET2 prežíva SYSRESETREQ a startup code ho nikdy netouchne.
 * SoftDevice je disabled keď flasher beží → direct zápis je bezpečný.
 * Main firmware číta cez sd_power_gpregret_get(1, &val).
 * Compile with -DFLASHER_DEBUG=0 to strip markers from production build.
 */
#ifndef FLASHER_DEBUG
#   define FLASHER_DEBUG 1
#endif
/* fmark() je teraz no-op — nahradené flash trace logom (ftrace), ktorý dáva
 * kompletnú sekvenciu eventov, nie len posledný krok. Ponechané kvôli úspore
 * miesta (flasher kód musí byť < 4kB) — všetky fmark() volania sa vyparia. */
#define fmark(step) ((void)0)
/* Step codes: */
#define FM_STARTED          0xFFu  /* flasher_main bol zavolaný */
#define FM_ZLIB_DETECTED    0x01u
#define FM_PUFF_OK          0x02u
#define FM_OPEN_OK          0x03u
#define FM_PATCH_OK         0x04u
#define FM_FLUSH_OK         0x05u
#define FM_RESET            0x06u
/* Error codes: */
#define FM_ERR_UNCOMP_SZ    0xE0u  /* uncomp_size == 0 or > max */
#define FM_ERR_PUFF         0xE1u  /* puff() != 0 */
#define FM_ERR_DESTLEN      0xE2u  /* destlen mismatch */
#define FM_ERR_OPEN         0xE3u  /* hpatchi_inplace_open failed */
#define FM_ERR_COMPRESS     0xE4u  /* compress_type != no */
#define FM_ERR_SAFE         0xE5u  /* extra_safe > MAX_EXTRA_SAFE */
#define FM_ERR_PATCH        0xE6u  /* hpatchi_inplaceB failed */

/* ── WDT feeding — defensive, bezpečné aj ak WDT nie je aktívny ────── *
 * Flash: 47 strán × 170ms ≈ 8s. Adafruit BSP môže mať WDT < 8s.
 * nRF52840 WDT RR[0..7] @ 0x40010600; zápis 0x6E524635 kŕmi časovač.
 */
/* non-static: volá ju aj puff.c počas dlhej dekompresie (extern decl v puff.c) */
void wdt_feed(void) {
    volatile uint32_t* rr = (volatile uint32_t*)0x40010600u;
    for (int i = 0; i < 8; i++) rr[i] = 0x6E524635UL;
}

/* ── NVMC (nRF52840 — priamy prístup bez BSP) ─────────────────────── */
#define R_NVMC_READY      (*(volatile uint32_t*)0x4001E400u)
#define R_NVMC_CONFIG     (*(volatile uint32_t*)0x4001E504u)
#define R_NVMC_ERASEPAGE  (*(volatile uint32_t*)0x4001E508u)

static void nvmc_wait(void) {
    __asm volatile ("" ::: "memory");
    while (!(R_NVMC_READY & 1u));
    wdt_feed();
}
static void nvmc_erase_page(uint32_t addr) {
    nvmc_wait();
    R_NVMC_CONFIG = 2u;
    __asm volatile ("dsb" ::: "memory");
    R_NVMC_ERASEPAGE = addr;
    nvmc_wait();
    R_NVMC_CONFIG = 0u;
    __asm volatile ("dsb" ::: "memory");
}
static void nvmc_write_page(uint32_t addr, const uint8_t* src) {
    const uint32_t* s32 = (const uint32_t*)src;
    volatile uint32_t* d32 = (volatile uint32_t*)addr;
    R_NVMC_CONFIG = 1u;
    __asm volatile ("dsb" ::: "memory");
    for (uint32_t i = 0; i < PAGE_SIZE / 4u; i++) {
        d32[i] = s32[i];
        nvmc_wait();
    }
    R_NVMC_CONFIG = 0u;
    __asm volatile ("dsb" ::: "memory");
}

/* ── Flash trace log ──────────────────────────────────────────────────── *
 * Flasher appenduje 32-bit event kódy do voľnej stránky 0xF3000 (mimo
 * LittleFS/app/bootloader). Bežný FW prečíta CELÚ sekvenciu eventov.
 * Pod FLASHER_DEBUG=0 sa celé vynechá (žiadne miesto, žiadny NVMC zápis).
 *   ftrace(code) appenduje jeden word; ftrace_init() raz vymaže stránku.
 * Trace kódy: TR_* nižšie. Po reboote vidno presne kam flasher došiel.
 */
/* FLASH_TRACE_ADDR je z flash_layout.h (= FLASHER_META_ADDR, 0xF3000) */
#define FLASH_TRACE_MAX  512u          /* max 512 eventov (2kB z 4kB stránky) */
#if FLASHER_DEBUG
static uint32_t s_trace_idx = 0;
static void ftrace_init(void) {
    nvmc_erase_page(FLASH_TRACE_ADDR);
    s_trace_idx = 0;
}
static void ftrace(uint32_t code) {
    if (s_trace_idx >= FLASH_TRACE_MAX) return;
    volatile uint32_t* p = (volatile uint32_t*)(FLASH_TRACE_ADDR + s_trace_idx * 4u);
    R_NVMC_CONFIG = 1u;                 /* WEN */
    __asm volatile ("dsb" ::: "memory");
    *p = code;
    nvmc_wait();
    R_NVMC_CONFIG = 0u;                 /* REN */
    __asm volatile ("dsb" ::: "memory");
    s_trace_idx++;
}
#else
#  define ftrace_init() ((void)0)
#  define ftrace(code)  ((void)0)
#endif

/* non-static wrapper pre puff.c (extern) — progress počas dekompresie */
void ftrace_ext(unsigned long code) { ftrace((uint32_t)code); }

/* ── Patch stream — sekvenčné čítanie z buffra (XIP alebo RAM) ─────── */
typedef struct {
    uint32_t pos;
    uint32_t addr;
    uint32_t size;
} PatchStream;

static hpi_BOOL patch_read(hpi_TInputStreamHandle h,
                            hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    PatchStream* s = (PatchStream*)h;
    uint32_t avail = s->size - s->pos;
    uint32_t n = ((uint32_t)*size < avail) ? (uint32_t)*size : avail;
    if (n == 0) { *size = 0; return hpi_FALSE; }
    memcpy(out, (const uint8_t*)(s->addr + s->pos), n);
    s->pos += n;
    *size = (hpi_size_t)n;
    return hpi_TRUE;
}

/* ── STREAMING read_diff: dekompresia patchu on-demand cez puff_stream ──
 * Pre ZLIB patch. handle = puff_stream_t*. HPatchLite číta diff forward-only,
 * puff_stream to zaručuje. Žiadny veľký dekompresný buffer (na rozdiel od puff()). */
static puff_stream_t s_ps;   /* ~1.8kB v .bss */

static hpi_BOOL patch_zlib_read(hpi_TInputStreamHandle h,
                                 hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    puff_stream_t* ps = (puff_stream_t*)h;
    uint32_t n = puff_stream_read(ps, (uint8_t*)out, (uint32_t)*size);
    *size = (hpi_size_t)n;
    return (n > 0 || ps->state == PS_DONE) ? hpi_TRUE : hpi_FALSE;
}

/* ── Flash write listener ─────────────────────────────────────────────
 * hpatchi_listener_t MUSÍ byť prvý člen — hpatchi_inplaceB castuje pointer.
 */
typedef struct {
    hpatchi_listener_t base;    /* MUSÍ byť prvý */
    uint8_t  page_buf[PAGE_SIZE];
    uint32_t page_used;
    uint32_t current_page;
} FlashCtx;

static hpi_BOOL flash_read_old(hpatchi_listener_t* l,
                                hpi_pos_t pos,
                                hpi_byte* out, hpi_size_t size) {
    (void)l;
    if ((uint32_t)pos + (uint32_t)size > APP_FLASH_MAX) return hpi_FALSE;
    memcpy(out, (const uint8_t*)(APP_FLASH_START + (uint32_t)pos), size);
    return hpi_TRUE;
}

/* DRY-RUN: flasher prejde celý proces (puff + hpatchi + simulovaný zápis)
 * ale NEZAPÍŠE do app flash → zariadenie sa nezabrickuje pri debugu.
 * Nastav FLASHER_DRYRUN=0 pre ostrý zápis. */
#ifndef FLASHER_DRYRUN
#   define FLASHER_DRYRUN 0   /* 0 = ostrý zápis (chránené verify+DFU); 1 = dry-run debug */
#endif

/* FNV-1a hash výstupu (nového FW) — verifikácia správnosti patchu.
 * BEZ inicializátora (inak .data → ld script ju discard-uje). Init za behu. */
static uint32_t s_out_fnv;

static hpi_BOOL flash_write_new(hpatchi_listener_t* l,
                                 const hpi_byte* data, hpi_size_t size) {
    FlashCtx* c = (FlashCtx*)l;
    for (hpi_size_t i = 0; i < size; i++) {   /* FNV-1a cez výstupné bajty */
        s_out_fnv ^= (uint32_t)data[i];
        s_out_fnv *= 16777619u;
    }
    while (size > 0) {
        uint32_t space = PAGE_SIZE - c->page_used;
        uint32_t n = ((uint32_t)size < space) ? (uint32_t)size : space;
        memcpy(c->page_buf + c->page_used, data, n);
        c->page_used += n;
        data += n;
        size -= n;
        if (c->page_used == PAGE_SIZE) {
#if !FLASHER_DRYRUN
            uint32_t addr = APP_FLASH_START + c->current_page * PAGE_SIZE;
            nvmc_erase_page(addr);
            nvmc_write_page(addr, c->page_buf);
#else
            wdt_feed();   /* dry-run: nezapisuj app flash, len kŕm WDT */
#endif
            c->current_page++;
            c->page_used = 0;
        }
    }
    return hpi_TRUE;
}

/* ── Skutočná implementácia ───────────────────────────────────────────
 * Volaná z flasher_entry po nastavení SP.
 * Stack priestor: ~256kB minus overhead (NVIC/SP nastavuje flasher_entry).
 */
void flasher_main(uint32_t patch_addr, uint32_t patch_size,
                   uint32_t new_fw_size) {
    (void)new_fw_size;
    /* Vypni IRQ — flasher beží MIMO OS/SoftDevice kontextu. Ak by prišlo
     * prerušenie (SysTick/RADIO/USB), CPU by skočilo cez VTOR do app handlera
     * bez platného SD/RTOS stavu → crash/reset. Flasher je čisto sekvenčný. */
    __asm volatile ("cpsid i" ::: "memory");
    ftrace_init();        /* vymaž trace stránku 0xF3000 */

    /* read_diff backend — podľa formátu patchu. Pre raw patch priame čítanie
     * z RAM (PatchStream), pre ZLIB streaming dekompresia (puff_stream). */
    PatchStream ps;                       /* pre raw (nekomprimovaný) patch */
    hpi_TInputStreamHandle diff_handle;
    hpi_TInputStream_read  diff_rd;

    /* ── Detekuj komprimovaný formát ── */
    if (patch_size >= ZPATCH_HDR_SIZE &&
        *(const uint32_t*)patch_addr == ZPATCH_MAGIC)
    {
        /* ZLIB: STREAMING — puff_stream dekomprimuje on-demand z komprimovaného
         * patchu v RAM (PATCH_RAM_ADDR). Žiadny veľký dekompresný buffer.
         * uncomp/new_fw veľkosti v hlavičke sú informatívne (hpatchi číta z patchu). */
        fmark(FM_ZLIB_DETECTED); ftrace(FM_ZLIB_DETECTED);
        puff_stream_init(&s_ps, (const uint8_t*)(patch_addr + ZPATCH_HDR_SIZE),
                         patch_size - ZPATCH_HDR_SIZE);
        diff_handle = &s_ps;
        diff_rd     = patch_zlib_read;
    } else {
        /* raw HPatchLite patch priamo v RAM */
        ps.pos = 0; ps.addr = patch_addr; ps.size = patch_size;
        diff_handle = &ps;
        diff_rd     = patch_read;
    }

    /* ── HPatchLite inplaceB (streaming read_diff) ── */
    {
        hpi_compressType compress_type = hpi_compressType_no;
        hpi_pos_t   new_size    = 0;
        hpi_pos_t   uncomp_size = 0;
        hpi_size_t  extra_safe  = 0;

        if (!hpatchi_inplace_open(diff_handle, diff_rd,
                                  &compress_type, &new_size,
                                  &uncomp_size, &extra_safe))
            { fmark(FM_ERR_OPEN); ftrace(FM_ERR_OPEN); goto FAIL; }

        /* HPatchLite patch sám musí byť nekomprimovaný — komprimujeme len wrapper */
        if (compress_type != hpi_compressType_no) { fmark(FM_ERR_COMPRESS); ftrace(FM_ERR_COMPRESS); goto FAIL; }
        if (extra_safe > MAX_EXTRA_SAFE)          { fmark(FM_ERR_SAFE);    ftrace(FM_ERR_SAFE);     goto FAIL; }
        fmark(FM_OPEN_OK);

        /* FlashCtx + temp_cache v .bss (nie na stack) — page_buf 4kB + cache ~20kB */
        static FlashCtx fc;
        static uint8_t  s_temp_cache[MAX_EXTRA_SAFE + READ_CACHE];
        memset(&fc, 0, sizeof(fc));
        fc.base.diff_data = diff_handle;
        fc.base.read_diff = diff_rd;
        fc.base.read_old  = flash_read_old;
        fc.base.write_new = flash_write_new;

        s_out_fnv = 2166136261u;   /* init FNV-1a pred patchom */
        if (!hpatchi_inplaceB(&fc.base, new_size,
                              s_temp_cache, extra_safe, (hpi_size_t)sizeof(s_temp_cache)))
            { fmark(FM_ERR_PATCH); ftrace(FM_ERR_PATCH); goto FAIL; }
        fmark(FM_PATCH_OK); ftrace(FM_PATCH_OK);

        /* Flush poslednej neúplnej stránky */
        if (fc.page_used > 0u) {
            memset(fc.page_buf + fc.page_used, 0xFF,
                   PAGE_SIZE - fc.page_used);
#if !FLASHER_DRYRUN
            uint32_t addr = APP_FLASH_START + fc.current_page * PAGE_SIZE;
            nvmc_erase_page(addr);
            nvmc_write_page(addr, fc.page_buf);
#endif
        }
        fmark(FM_FLUSH_OK);
        ftrace(0xD0u);            /* marker: nasleduje FNV-1a hpatchi výstupu */
        ftrace(s_out_fnv);        /* 32-bit hash nového FW (čo hpatchi vyprodukoval) */

#if !FLASHER_DRYRUN && !FLASHER_DEBUG
        /* ── Verifikácia po zápise: prečítaj SPÄŤ zapísanú app flash, spočítaj
         * FNV-1a a porovnaj s hpatchi výstupom. Ak NVMC zápis zlyhal/čiastočný
         * (flash != hpatchi výstup) → skok do DFU bootloadera namiesto bootu
         * pokazeného FW (zariadenie sa dá obnoviť cez USB, nie brick). */
        {
            uint32_t vfnv = 2166136261u;
            const uint8_t* app = (const uint8_t*)APP_FLASH_START;
            for (uint32_t i = 0; i < (uint32_t)new_size; i++) {
                vfnv ^= (uint32_t)app[i];
                vfnv *= 16777619u;
                if ((i & 0x3FFFu) == 0u) wdt_feed();
            }
            ftrace(0xD1u); ftrace(vfnv);   /* FNV flash-readbacku */
            if (vfnv != s_out_fnv) {
                ftrace(0xEAu);             /* VERIFY FAIL → DFU */
                /* Adafruit nRF52 bootloader: GPREGRET=0x57 (UF2/DFU magic) + reset */
                *(volatile uint32_t*)0x4000051Cu = 0x57u;
                __asm volatile ("dsb" ::: "memory");
                (*(volatile uint32_t*)0xE000ED0Cu) = 0x05FA0004u;
                __asm volatile ("dsb" ::: "memory");
                while (1);
            }
            ftrace(0xD2u);                 /* VERIFY OK — flash == hpatchi výstup */
        }
#endif
    }

    /* SystemReset — zapisujeme do SCB->AIRCR */
    fmark(FM_RESET); ftrace(FM_RESET);
    __asm volatile ("dsb" ::: "memory");
    (*(volatile uint32_t*)0xE000ED0Cu) = 0x05FA0004u;
    __asm volatile ("dsb" ::: "memory");
    while (1);

FAIL:
    /* Pri zlyhaní NEVISÍ (s cpsid i by visel navždy bez USB) — resetuje sa,
     * aby app nabootovala a prečítala trace log z 0xF3000. POZOR: ak flasher
     * už začal písať do app flash, app je corrupted → nabehne len bootloader. */
    ftrace(0xFEu);   /* FAIL reached */
    __asm volatile ("dsb" ::: "memory");
    (*(volatile uint32_t*)0xE000ED0Cu) = 0x05FA0004u;
    __asm volatile ("dsb" ::: "memory");
    while (1);
}

/* ── Entry point — nastaví vlastný SP pred volaním flasher_main ───────
 *
 * Naked funkcia: kompilátor negeneruje žiadny prológ/epilóg.
 * Parametre sú v r0, r1, r2 podľa ARM AAPCS — zachované pri bl flasher_main.
 *
 * Prečo vlastný SP: FreeRTOS task stack je 2-4kB, flasher potrebuje ~20kB
 * (FlashCtx 4kB + temp_cache 8kB + puff huffman tabuľky ~4kB + overhead).
 * Po sd_softdevice_disable je celých 256kB RAM voľných.
 */
__attribute__((naked))
void flasher_entry(uint32_t patch_addr, uint32_t patch_size, uint32_t new_fw_size)
{
    __asm volatile (
        "ldr r3, =0x20040000\n\t"   /* top of nRF52840 RAM */
        "mov sp, r3\n\t"
        "bl  flasher_main\n\t"
        "1: b 1b\n\t"               /* never reached — flasher_main resets */
        ::: "r3"
    );
    (void)patch_addr; (void)patch_size; (void)new_fw_size;  /* suppress warnings */
}

#endif  /* FOTA_FLASHER_BUILD */
