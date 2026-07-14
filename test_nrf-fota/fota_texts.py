#!/usr/bin/env python3
"""fota_texts.py — central catalog of user-facing texts for the FOTA PC tools.

Python-side counterpart of nrffota/FotaTexts.h. Default language is ENGLISH;
set the environment variable FOTA_LANG=sk for Slovak. Usage:

    from fota_texts import T
    print(T("keytool_wrote", path=out))

Only human-facing text lives here (help strings, prints, error messages).
Machine-parsed stdout tokens (e.g. `[patch] ... sha256=`, `[fotapkg] ...`,
progress counters that scripts scrape) are intentionally NOT routed through
this catalog — they must stay byte-stable across languages, exactly like the
"neutral parsed formats" rule in FotaTexts.h.
"""
import os

_LANG = os.environ.get("FOTA_LANG", "en").lower()

# key -> {lang: template}. English is the default/fallback.
_TEXTS = {
    # ── fota_keytool.py ──
    "keytool_usage": {
        "en": "usage: fota_keytool.py <gen|der2hex|pub> <arg>",
        "sk": "použitie: fota_keytool.py <gen|der2hex|pub> <arg>",
    },
    "keytool_wrote": {
        "en": "[gen] wrote: {path}",
        "sk": "[gen] zapísané: {path}",
    },
    "keytool_exists": {
        "en": "[ERROR] {path} already exists — refusing to overwrite a key",
        "sk": "[CHYBA] {path} už existuje — nechcem prepísať kľúč",
    },
    "keytool_unknown_cmd": {
        "en": "[ERROR] unknown command '{cmd}'",
        "sk": "[CHYBA] neznámy príkaz '{cmd}'",
    },
    "keytool_priv_comment": {
        "en": "   # companion/expanded format",
        "sk": "   # companion/expandovaný formát",
    },
    "keytool_csnippet_hdr": {
        "en": "C snippet for s_authors (FotaReceiver_signkey.cpp):",
        "sk": "C snippet pre s_authors (FotaReceiver_signkey.cpp):",
    },
    # ── shared signing errors (fota_sender.py etc.) ──
    "sign_hex_len": {
        "en": "companion hex must be 128 hex chars (64 bytes)",
        "sk": "companion hex musí mať 128 hex znakov (64 B)",
    },
    "sign_privkey_hex_err": {
        "en": "[ERROR] --privkey-hex: {msg}",
        "sk": "[CHYBA] --privkey-hex: {msg}",
    },
    "sign_keyid0_needs_priv": {
        "en": "[ERROR] key_id=0 (prefix format) requires a privkey — for unsigned use --keyid 1",
        "sk": "[CHYBA] key_id=0 (prefix formát) vyžaduje privkey — pre unsigned použi --keyid 1",
    },
    "sign_no_privkey_legacy": {
        "en": "[init] no privkey -> legacy key_id=1, zero signature",
        "sk": "[init] bez privkey -> legacy key_id=1, nulový podpis",
    },
    # ── argparse help (fota_sender.py / export / gen / mcpy) ──
    "help_privkey": {
        "en": "Ed25519 private key (DER) to sign the HEADER",
        "sk": "Ed25519 private key (DER) na podpis HEADER",
    },
    "help_privkey_hex": {
        "en": "Ed25519 expanded key (128 hex, companion format)",
        "sk": "Ed25519 expandovaný kľúč (128 hex, companion formát)",
    },
    "help_keyid": {
        "en": "SIG key_id: 0=v0-prefix (new format, default), >=1 legacy for old FW (s_authors[keyid-1])",
        "sk": "key_id v SIG: 0=v0-prefix (nový formát, default), >=1 legacy pre staré FW (s_authors[keyid-1])",
    },
}


def T(key: str, **kw) -> str:
    entry = _TEXTS.get(key)
    if entry is None:
        return key  # unknown key -> surface it rather than crash
    tpl = entry.get(_LANG) or entry["en"]
    return tpl.format(**kw) if kw else tpl
