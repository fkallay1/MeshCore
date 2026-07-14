# FOTA: overenie podpisu voči ACL adminom + podpis companion kľúčom — dizajn

**Dátum:** 2026-07-15 (schválené v session „41")
**Vetvy:** MeshCore `features/nrf-fota`, meshcore-open `feature/nrf-ota-sender`

## Cieľ

Podpísaný `.fotapkg` sa dnes overuje len proti zakompilovaným kľúčom
(`s_authors` vo `FotaReceiver_signkey.cpp`, adresované `key_id`). Doplniť:

1. Overenie podpisu aj voči **autentifikovaným správcom v ACL** repeatera
   (`ClientACL`, len záznamy s `PERM_ACL_ADMIN`).
2. Podpisovanie **privátnym kľúčom companion identity** — 64 B hex string,
   ktorý sa dá zobraziť v telefónnej appke.
3. 4 nové zakompilované kľúče `fota_signkey1..4.der` + nástroj na generovanie
   a konverziu kľúčov.

## Kľúčové zistenie: formát companion kľúča

MeshCore používa **orlp/ed25519** (`lib/ed25519/`): 64 B privátny kľúč =
`SHA512(seed)` s clampingom — **expandovaný** kľúč, nie seed. Dôsledky:

- **hex → .der NEJDE** (SHA512 je jednosmerná; PKCS#8 ukladá 32 B seed).
- **.der → hex ÁNO** (seed → SHA512+clamp → 64 B companion hex).
- **Podpis expandovaným kľúčom priamo ÁNO** — vlastný signer (orlp matika),
  výsledok je štandardný RFC8032 podpis, existujúci `fota_ed25519_verify`
  na receiveri ho overí bez zmeny.

## Wire formát SIG paketu

- **Nový jednotný formát (`key_id=0`):**
  `[HDR_SIG][prot_inf][old_sha256 32][key_id=0][sig 64][signer_prefix 4]` = **103 B**
  (limit plaintextu 165 B — rezerva OK). `signer_prefix` = prvé 4 B Ed25519
  pubkey podpisovateľa. Identifikácia kľúča VÝHRADNE prefixom — platí pre
  zakompilované kľúče aj ACL adminov.
- **Legacy formát (`key_id≥1`):** nezmenených 99 B; mapovanie
  `key_id N → s_authors[N-1]` (test kľúč = index 0 = doterajšie `key_id=1`).
  Ostáva kvôli update reťazcu zariadení so starým FW; po pretečení flotily
  sa dá vetva zmazať.
- Starý FW pri novom formáte: `key_id=0` nenájde → INVALID → odmietne
  (žiadané; dlhší paket mu neprekáža — kontroluje len `plen <`).

## Firmvér (receiver)

- `FotaAuthorEntry` stráca pole `id` — ostáva `pub_key[32]`.
  `s_authors` = test kľúč + `fota_signkey1..4` (5 záznamov).
- `try_verify_header()`:
  - `key_id==0`: kandidáti = `s_authors` so zhodným 4 B prefixom, potom ACL
    záznamy s `isAdmin()` a zhodným prefixom; verify postupne (typicky
    presne 1 kandidát → ~150 ms ako dnes, beží deferovane v `loop()`),
    prvý úspech = OK. Prefix bez kandidáta → drop bez verify.
  - `key_id>=1`: legacy `s_authors[key_id-1]` (bounds check).
- **ACL hook:** `FotaReceiver` je app-agnostický (aj ZephCore build) — ACL
  kandidátov dodá `extern` funkcia s weak default implementáciou (0
  kandidátov); silnú implementáciu dodá `FotaMyMesh.cpp` iterovaním
  `MyMesh::acl` (len `PERM_ACL_ADMIN`; Read/Write a nižšie sa nekvalifikujú).
- `FotaState` + meta súbor: nové pole `hdr_signer_prefix[4]` persistované
  spolu s `hdr_sig`/`hdr_key_id` (reboot-resilience; starý meta na zariadení
  sa rozpozná podľa dĺžky).
- Bezpečnosť: podpis kryje celú 102 B META; revokácia = vyhodenie admina
  z ACL; base-FW gating (`fota_meta_base_ok`) ostáva prvou obranou.

## PC tooling (`test_nrf-fota/`)

- **Pure-Python orlp signer** vo `fota_sender.py` (~60 riadkov, `hashlib.sha512`
  + ed25519 point math). Z expandovaného kľúča **odvodí aj pubkey**
  (scalarmult) → prefix garantovane sedí, pub hex netreba zadávať.
- `fota_sender.py` / `fota_export_pkg.py` / `gen_fotapkg.py`:
  nový `--privkey-hex <128 hex>`; `--privkey <der>` ostáva.
  **Default výstup = nový formát** (`key_id=0` + prefix) pre všetky kľúče;
  `--keyid N` = legacy prepínač.
- `.fotapkg.json` blok `signed`: pribudne `signer_prefix` (+ `key_id:0`).
- **`fota_keytool.py`** (nový):
  - `gen <meno.der>` — keypair → .der; vypíše pub hex, prefix, expandovaný
    priv hex (companion formát), C snippet pre `s_authors`;
  - `der2hex <key.der>` — seed → expandovaný hex + pub hex;
  - `pub <der|hex>` — pubkey/prefix/C snippet;
  - `hex2der` zámerne neexistuje (jednosmernosť — zdokumentovať).
- Vygenerovať `fota_signkey1..4.der` (gitignored ako `test_key.der`),
  pubkey do `s_authors`.

## Flutter appka (`meshcore-open`, `lib/fota/`)

- `FotaKeyStore`: import aj 128-znakového hexu (expandovaný identity kľúč);
  autodetekcia podľa dĺžky (64 = seed, 128 = expandovaný).
- **Dart signer pre expandovaný kľúč** — port `sign.c` + odvodenie pubkey;
  korektnosť cez golden vektory z Pythonu (zabehnutý vzor).
- `buildSig`: default nový formát pre oba typy kľúčov; UI checkbox
  „legacy podpis (staré FW)" → `key_id=1` bez prefixu.
- Presigned balíky: appka prenáša SIG z `.fotapkg.json` ako dnes.

## Chybové stavy a diagnostika

- Nové logy (cez `FotaTexts.h`, EN/SK): `SIG: signer prefix XXXXXXXX not
  found (s_authors+ACL)`; `HEADER OK signer=builtin|ACL admin`.
- Tooling: validácia hexu (dĺžka/znaky) s jasnou chybou; UNSIGNED varovanie
  ostáva.

## Testovanie

1. **Python golden:** rovnaká META podpísaná pycryptodome (.der) aj
   expanded-hex signerom (po `der2hex`) → identický podpis; odvodený pub ==
   pub z .der.
2. **Dart golden:** vektory z Pythonu pre expandovaný signer.
3. **On-device e2e:** (a) `fota_signkey1` nový formát → prijme;
   (b) admin login → ACL → podpis identity hexom → prijme;
   (c) neznámy kľúč/neadmin → INVALID; (d) legacy `--keyid 1` → prijme.
4. Build verify `ProMicro_repeater_fota` (+ ostatné FOTA envy).

## Mimo rozsah

- Room-server ACL, Read/Write rola, revokačné zoznamy pre `s_authors`,
  on-device tvorba patchu v appke (Fáza B), zmeny MeshCore core vrstiev.
