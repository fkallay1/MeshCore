# Komentar k RadioLib issue 1857 (doplnenie po merani na LR11x0)

**NEODOSLANE.** Ciel: `jgromes/RadioLib`, issue 1857.

Prikaz (telo = len cast za poslednym nadpisom Body; `rindex`, nie `index`):

```bash
python -c "import io;p='fkclaude/docs/radiolib-issue-1857-comment.md';s=io.open(p,encoding='utf-8').read();i=s.rindex(chr(35)*2+' Body')+7;io.open('body.md','w',encoding='utf-8',newline='').write(s[i:].lstrip(chr(10)))"

D:/FkDev/GHcli/bin/gh.exe issue comment 1857 --repo jgromes/RadioLib --body-file body.md
```

Pravidla: bez zmienky o AI, bez URL a bez mriezkovych cisel. Dva odkazy su v texte
opisane slovami (jeho vlastna diskusia o getPacketLength()==0 a hlasenie v
archivovanom Semtechovom lr1110_driver) — ak by sa mali uviest cislami, doplnit.

## Body

Follow-up with measurements from the other family, and one boundary worth drawing.

**LR11x0 confirmed on hardware.** Same method as above - a debug build skipping the BUSY
wait on every fourth length read, on a Seeed T1000-E in a live mesh at SF7. 4 of 4
sabotaged reads came back as a length of 0 and reported `CMD_OK`; re-reading returned the
true length each time, recovering 84, 20 and 196 byte frames. So the discriminator holds
on both families, not just on LR2021.

**Not every zero is this race, though.** On the same board, a length of 0 while RX_DONE is
set also happens for genuine reasons, and often: 25 times in three minutes, roughly one
per two and a half received frames, with the IRQ word reading 0x38, 0x78 (header error) or
0xB8 (CRC error). Those all report `CMD_DAT`, and a probe doing three extra reads on each
recovered nothing - 0 out of 25. Worth knowing that the archived Semtech LR1110 driver
repository carries an unanswered report of the same shape: RSSI readable through the LoRa
packet-status command while the buffer-status command returns a payload length of 0. So
part of this is chip behaviour rather than the reply race, and only the status byte tells
the two apart - which is the whole point of the request above.

**One more reason to make the failure visible.** A zero length is not just a lost frame in
some callers - it can stop reception. In a discussion here about `getPacketLength()`
returning 0 in RX IRQ mode, the answer was that `getPacketLength` does not clear the IRQ,
so bailing out when it returns 0 leaves the flags set and the IRQ line asserted. That is
exactly what an application does if it treats 0 as "nothing to read" and skips
`readData()`. Where the driver re-arms Rx afterwards the flags get cleared as a side
effect and it goes unnoticed; where it does not, reception can stall. A read that returned
the status stream instead of a reply is therefore worth surfacing as an error, not just as
a plausible-looking zero.
