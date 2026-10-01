# Komentár do PR 3512 (carlhodder, LR2021 continuous RX race)

**NEODOSLANÉ — čaká na Fedorovo schválenie** (2026-10-02). Odoslať až PO úprave PR 3261
(rebase + LR2021-only), lebo komentár naň odkazuje.

Podklady: rozbor diffu a komentárov 2026-10-02 (draft PR, báza `main`, CI action_required,
maintainer zatiaľ nereagoval). S naším #3261 sa textovo nebije (overené `git apply --3way`
na dev + #3261 + NiceRF vetva, build OK).

## Comment

This fits well with #3261 rather than overlapping it. Yours drops the frame when the FIFO level and the length disagree, mine makes sure the length itself is not a stale reply, so with both in place a stale length gets re-read and kept instead of dropped, and your check catches the cases mine cannot see, like the empty FIFO and the second packet already arriving. I applied both on current dev together and they build and merge cleanly.

One thing worth considering: getRxFifoLevel() is a get command as well, so it can be hit by the same early read. If that happens, the level comes back as part of the IRQ word and a good frame is dropped. Checking its return state and the CMD_DAT status the same way would keep the check from firing on those. It also returns an error code that is currently ignored.
