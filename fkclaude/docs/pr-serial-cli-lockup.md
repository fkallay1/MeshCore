# PR draft: serial CLI lockup fix (upstream meshcore-dev/MeshCore, base `dev`)

Branch: `fkallay1:fix/serial-cli-buffer-lockup` (be236e13, based on upstream/dev 795989b9)

## Title

```
fix(examples): serial CLI locks up permanently on buffer overflow without CR
```

## Body

```markdown
## Problem

The serial CLI loop in the examples locks up permanently when ≥159 characters
arrive without a CR. The buffer-full branch tries to force-complete the line:

```c
if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
}
```

but the line-complete check below tests `command[len - 1]`, which is
`command[sizeof-2]` when the buffer is full — one position *before* the byte
just written. The result:

1. The line never completes, so the buffer is never reset.
2. The read loop's condition (`len < sizeof(command)-1`) stays false forever —
   the CLI stops reading serial input entirely until reboot. Everything else
   (mesh, radio) keeps running, so the node looks alive but ignores all commands.
3. Writing at `[sizeof-1]` also overwrites the string's NUL terminator, so the
   next `strlen(command)` reads past the end of the buffer (UB).

### How it triggers in practice

Reproduced on hardware (T1000-E repeater, remote site): the serial console was
shared through tmux+picocom, and ~53 arrow-key presses in the attached terminal
(3-byte `ESC[A` escape sequences, 159 bytes with no CR) filled the buffer. The
CLI never accepted a command again until the device was rebooted.

Any input source that produces long CR-less byte runs triggers it: pasted text,
cursor-key escape sequences, line noise on a hardware UART, or a script sending
LF-only line endings (LF is skipped, other bytes accumulate).

## Fix

Write the forced `'\r'` at `command[sizeof-2]` — exactly the position the
completion check tests. The overflowed input is then processed as an (unknown)
command, the buffer resets, and the CLI keeps working.

The same copy-pasted pattern exists in four examples; all are fixed:

- `examples/simple_repeater/main.cpp`
- `examples/simple_sensor/main.cpp`
- `examples/simple_room_server/main.cpp`
- `examples/simple_secure_chat/main.cpp`

## Testing

- Bug reproduced on hardware (build without the fix): 159 CR-less bytes → CLI
  dead until reboot.
- Fix verified on the same hardware (T1000-E repeater): sending 200 CR-less
  characters force-completes the overflowed line as an unknown command and the
  very next `ver` command answers normally.
- Built `t1000e_repeater` on this branch (no other changes).

*Found and fixed with the help of Claude (Anthropic) while debugging a remote
repeater; reviewed and tested on hardware by the author.*
```

## Submit command

```powershell
cd "D:\FkDev\cli_home\temp\claude\D--FkDev-FkProj-VSC-MeshCore\156e0822-b4a1-47fc-8202-d19cb3bf1b6e\scratchpad\mc-pr"
gh pr create -R meshcore-dev/MeshCore --base dev `
  --head fkallay1:fix/serial-cli-buffer-lockup `
  --title "fix(examples): serial CLI locks up permanently on buffer overflow without CR" `
  --body-file <telo z tohto súboru>
```
