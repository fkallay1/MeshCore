/* flasher.ld — linker script pre standalone flasher binary (MeshCore)
 *
 * Cieľová adresa: 0xEB000 (mimo FOTA FS, app flash aj InternalFS)
 * Kód a rodata: FLASH 4kB
 * BSS (NOLOAD):  RAM  — iba pre linker, nepotrebuje inicializáciu,
 *                       objcopy vytiahne len .text (flasher_code.h)
 */

/* STREAMING flasher: patch sa NEdekomprimuje celý do RAM. Spodných 128kB RAM
 * (0x20000000-0x20020000) je vstupný KOMPRIMOVANÝ patch (~60kB, PATCH_RAM_ADDR),
 * z ktorého puff_stream streamuje. Flasher .bss (puff_stream_t + FlashCtx +
 * temp_cache ~26kB) + stack idú NAD patch (0x20020000-0x20040000 = 128kB).
 * Po sd_softdevice_disable je celá RAM voľná, takže priestor je bohatý. */
MEMORY {
    FLASH (rx)  : ORIGIN = 0xEB000,    LENGTH = 4096
    RAM   (rwx) : ORIGIN = 0x20020000, LENGTH = 0x20000   /* 128kB: .bss + stack, nad patchom */
}

SECTIONS {
    . = 0xEB000;

    .text : {
        KEEP(*(.text.flasher_entry))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* BSS — linker potrebuje vedieť kam, ale nie je súčasťou .bin */
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM

    /DISCARD/ : { *(.data*) *(.comment) *(.ARM*) }
}
