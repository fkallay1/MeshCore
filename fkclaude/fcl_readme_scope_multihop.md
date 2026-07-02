# FOTA scope (smerovanie) + multi-hop cez LoRa — popis a ladenie

Doplnenie **scope/route volieb** do FOTA odosielateľa (`test_nrf-fota/fota_sender.py`), aby
repeatre vedeli, či majú alebo nemajú paket preposielať (a tým sa zbytočne nezahlcovala sieť),
plus rozchodenie **multi-hop direct FOTA** na živej SK sieti. Nadväzuje na
[fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md) (samotný FOTA systém) a
[fcl_e2e_runbook_nrf-fota.md](fcl_e2e_runbook_nrf-fota.md) (ako spustiť test).

---

## 1. Scope režimy (`--scope`)

MeshCore má 2-bitový route type (header bity 0-1, `PH_ROUTE_MASK`). Z neho odvodzujeme
4 použiteľné režimy pre `PAYLOAD_TYPE_GRP_DATA`. `fota_sender.py --scope`:

| `--scope` | route type | repeater správanie | header bajt | wire (za GRP_DATA payloadom) |
|---|---|---|---|---|
| `zerohop` (**default**) | DIRECT, `path_len=0` | nikto nerepeatuje; spracujú priami susedia | `0x1A` | `[hdr][0x00][ch_hash][MAC+ct]` |
| `flood` | FLOOD | každý repeater re-flooduje (buduje path) | `0x19` | `[hdr][0x00][ch_hash][MAC+ct]` |
| `region` | TRANSPORT_FLOOD + 4B codes | re-flood len v zhodnom regióne (`RegionMap`) | `0x18` | `[hdr][code1_LE][code2_LE][0x00][ch_hash][MAC+ct]` |
| `direct` | DIRECT + path hashe | každý menovaný hop preposlie raz; posledný hop → count-0 broadcast → cieľ prijme | `0x1A` | `[hdr][path_len][path][ch_hash][MAC+ct]` |

- **`region`**: `--scope-name <hashtag>` (key = `SHA256(name)[:16]`) alebo `--scope-key <16B hex>`.
  Transport code = `HMAC-SHA256(key16, type(1B)+payload)` prvé 2B little-endian (0x0000/0xFFFF
  rezervované). Replika `TransportKey::calcTransportCode` (`src/helpers/TransportKeyStore.cpp`).
  Pozn.: poľné repeatre musia mať región nakonfigurovaný, inak paket zahodia.
- **`direct`**: `--path 6363,6868` (čiarkou hex hashe hopov v poradí) + `--path-hashsize {1,2,3}`.
  Node hash = **prefix public key** (`Identity.h`, `memcmp(hash, pub_key, len)`). `path_len`
  encoding: bity 0-5 = počet hopov, bity 6-7 = `hash_size-1`.

**Wire réžia (NIE je v payload poli → payload nepretečie):** `flood`/`zerohop` +0 B,
`region` +4 B (transport_codes), `direct` +N B (path). `MAX_TRANS_UNIT=255` má dosť rezervy.

### `--header-every N` (redundancia headera)
- **Default `0`**: header sa pošle **1× za beh**, na pozícii podľa `--packetorder`
  (`normal`/`hbegin`=začiatok, `hmiddle`=stred, `hend`=koniec).
- **`N>0`**: header sa **navyše** pošle po každých N chunkoch (napr. `--header-every 2` pri
  4 chunkoch = 3 headery/beh). Header je jediný kritický paket (`total=0` blokuje celú session)
  a nemá akumulačnú výhodu ako N nezávislých chunkov → na slabom/relay spoji mu treba viac pokusov.

---

## 2. Multi-hop direct — ako rozchodiť

### Pacing je nutný (duty-cycle relaya)
Štandardný repeater má **airtime/duty-cycle budget** — pri burste paketov prestane forwardovať.
Namerané (path cez 1 relay, SK 869.618/SF8):

| rozostup paketov (`--delay`) | relay doručené |
|---|---|
| 1.5 / 2.5 s | 2/10 |
| 3.5 s | 6/8 |
| **5.0 s** | **10/10** |

Relay **1-hop otočka** (čas medzi originálom a forwardom) ≈ **1.5 s** (1.1–2.5 s).
→ Pre multi-hop direct používaj **`--delay 5`** (alebo viac pri viacerých hopoch).

### Path pravidlá
- **Distinct hopy** (žiadne opakovanie uzla). `6363,6868` funguje; `6363,216D,6363` **NIE** —
  `hasSeen` (dedup po payload-hash, nezávislý od hop-countu) potlačí druhý forward tým istým
  uzlom = potlačenie slučky (správne správanie).
- Posledný hop spraví count-0 broadcast → cieľ (jeho sused) paket prijme a spracuje.

### Akumulácia naprieč kolami (test harness)
`broadcast_until_verified` (`fota_test_lora_repeater.py`) rebootuje DUT **len v 1. kole**
(čistý štart); ďalšie kolá kumulujú session v RAM. Reboot-per-kolo **mazal** <8-chunk session
(bitmap sa ukladá až od `FOTA_BITMAP_SAVE_EVERY=8`) — preto sa progres nikdy nenakumuloval.
(Pôvodný „stuck SX1262 receiver" dôvod reboot-u bola reálne anténa.)

---

## 3. Bridge (FK_lora-sniffer `gateway_fw`) — LBT + adaptívny resend

Build/flash (z `FK_lora-sniffer`): `pio run -e Xiao_bridge -t upload --upload-port COM3`
(bez flagu = SK preset; `-DLORA_PRESET_CZ` = CZ).

- **LBT (Listen Before Talk)** — vždy zapnuté: pred `radio.transmit()` CAD sken
  (`radio.scanChannel`), vysiela až keď je kanál voľný (backoff ~5-37 ms, max 8 pokusov, potom
  TX aj tak). Znižuje kolízie na zarušenom pásme. Počítadlo `lbtWait=` v `[LIVE]`.
- **Adaptívny resend** — **OPTION** `-DADAPTIVE_RESEND` (default vyp): po TX počúva ECHO
  (forward z 1. hopu — detekcia zhodou payloadu) a pri timeoute opakuje s rastúcim intervalom
  (2/3/5/10 s, max 3 resendy). Počítadlá `resend=`/`echo=`. **Pozor: na multi-hop airtime-drahý.**

---

## 4. Overené na živej SK sieti (869.618/62.5/SF8/CR5)

| scope / cesta | výsledok |
|---|---|
| flood / zerohop | VERIFIED (priamo z bridge) |
| region (sk-ota) | VERIFIED → flash (#115) |
| direct 1-hop (6363) @ `--delay 5` | VERIFIED → flash (#119) |
| **direct 2-hop (6363,6868) @ `--delay 5` + LBT + adaptive** | **VERIFIED za 3 kolá → flash (#120)** |
| direct s opakovaným uzlom (6363,216D,6363) | ❌ slučka (`hasSeen`) — nedoručiteľné |

### Diagnostické fakty (aby sa neopakovali zlé závery)
- **`[FOTA] ... crc BAD`** v logu = artefakt trace printera `fota_print_pkt` (`FotaReceiver.cpp`),
  ktorý počíta crc cez **AES-padovanú** dĺžku. Reálny `handle_chunk` klampuje na `exp_len`.
  **Nie je to RF korupcia ani reálny drop** — RF chybu by zachytil LoRa PHY CRC / MAC pred dešifrovaním.
- **Header NIE je väčší než plný chunk** — oba 181 B na drôte (count-0); header bol problém len
  ako single-point-of-failure (1×/kolo) na zahltenom relay spoji.
- Voliteľná diagnostika príjmu: build s `-DFOTA_GDR_DIAG` → `MyMesh::onGroupDataRecv` vypíše
  `fota_type/route/hops` (0x10=HEADER, 0x11=CHUNK) každého dešifrovaného GRP_DATA.

---

## 5. Príklady spustenia

```bash
PENV="D:/FkDev/.platformio/penv/Scripts/python.exe"
# kanál #fkotanrf, #-konvencia: psk = SHA256(mena)[0:16]
PSK=$(cd test_nrf-fota && "$PENV" -c "from fota_sender import fota_channel_secret; print(fota_channel_secret().hex())")

# zero-hop (default) — priami susedia, nikto nerepeatuje
"$PENV" test_nrf-fota/fota_sender.py --old old.bin --new new.bin --port COM3 --mode meshcore --psk $PSK --privkey test_nrf-fota/test_key.der

# region sk-ota — flood len v regióne
"$PENV" ... --scope region --scope-name sk-ota

# direct 2-hop cez relaye, správne tempo + redundancia headera
"$PENV" ... --scope direct --path 6363,6868 --path-hashsize 2 --delay 5 --header-every 2
```

Súvisí: [fcl_e2e_runbook_nrf-fota.md](fcl_e2e_runbook_nrf-fota.md),
[fcl_readme_verified_pooling.md](fcl_readme_verified_pooling.md).
