# PR 2978 — zatvoriť, nahradené upstream #3395

**NEODOSLANÉ — čaká na Fedorovo schválenie** (2026-10-02).

Upstream #3395 (jbrazio, zmergnuté 2026-09-11) opravil plný buffer sériového CLI
rovnako ako náš PR (`command[sizeof(command)-2] = '\r'`) a navyše drží NUL na konci.
Pri syncu 2026-10-02 sme zobrali upstream verziu. PR 2978 je voči dev v konflikte a
nemá už čo pridať. Po odoslaní komentára zatvoriť (`gh pr close 2978`).

## Comment

Closing this, since #3395 fixed the same line the same way and also keeps the buffer NUL-terminated. Thanks for merging that one.
