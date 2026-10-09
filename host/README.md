# tangcore-sim — Linux host simulator for the TangCore firmware

Exercises the menu, file browser, ROM loader and battery-save logic on a PC,
without flashing a Tang Console. It compiles the real firmware sources
(`main.cpp`, `ui/`, `utils/`, `core/`) against thin host shims and talks to
a fake FPGA core over the same framed UART protocol as the hardware.

## Build

System gcc/g++ and cmake, no BL616 toolchain:

```bash
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build -j
```

This builds a Console 138K layout (`BOARD_NAME=console138k`). The build
compiles the firmware with `-DTANGCORE_HOST=1`, which guards the only two
host-only spots in firmware sources: the `strcasestr` declaration in
`utils/utils.h`, and the USB register writes in `ui/game_controls.cpp`
(skipped on the host, which has no SoC registers).

## Run interactively

```bash
./host/build/tangcore-sim --sd /path/to/sd-dir
```

The SD directory is the virtual SD card. It needs the usual layout:

```
cores/console138k/monitor.bin
cores/console138k/nestang.bin   # ... one per core
nes/ snes/ gba/ genesis/ sms/ pc/
saves/                           # created by the firmware
```

The OSD (32×28 text console) is redrawn in the terminal. Keys:

| Key | Pad button |
|---|---|
| arrows | D-pad |
| z / x | B / A |
| a / s | Y / X |
| Enter | Start |
| Backspace or Tab | Select |
| q / w | L / R |
| g | menu combo (hold 0.5 s) |
| t | reset combo (hold 0.5 s) |
| T | reset combo held 3.5 s (close game) |
| m | MODE button (FPGA reloads from flash, firmware reboots) |
| p | reboot the sim |
| Q or Esc | quit |

Note: in the main menu, **B** selects an entry (that is what the firmware
does: only B breaks out of its choice loop).

## Scripted mode

```bash
./host/build/tangcore-sim --sd /path/to/sd-dir --script host/tests/a-menu.script
```

Script commands (one per line, `#` comments, `"quoted strings"`):

- `wait 200ms` / `wait 2s` — sleep.
- `press down`, `press a`, `press select+start+l` — tap buttons. The press is
  released as soon as the OSD reacts, so navigation steps exactly once.
  Buttons: `up down left right a b x y l r start select` (`sel` = select).
- `hold select+start+l 400ms` — hold for a wall-clock duration (combos).
- `expect-screen "text"` / `expect-no-screen "text"` — OSD contains (or for
  the whole timeout, never contains) the substring.
- `expect-cursor-row 9` — the `>` cursor is on that OSD row.
- `expect-core smstang` — the programmed core (`monitor nestang snestang
  gbatang mdtang smstang pctang`).
- `expect-config-bit 17 1` — a bit of the last `core_config` word.
- `expect-overlay on|off`.
- `expect-file saves/sms/game.sav size 32768` / `expect-no-file <rel>`.
- `poke-save 0x10 0x42` — write one fake-FPGA save-RAM byte and raise the
  dirty notice (`0x0B`).
- `expect-save-ram 0x10 0x42` — check a fake-FPGA save-RAM byte (verifies
  restore frames after a reload).
- `expect-alive 5s` — the watchdog heartbeat keeps moving.
- `mode [silence-ms]` — press the MODE button: the FPGA goes silent, then
  answers as core 0, and the firmware reboots. The script resumes after this
  line in the fresh process.
- `mode-now [silence-ms]` — the same, pressed at once instead of after the
  firmware's first poll of a freshly programmed core.
- `power-cycle` — reboot the sim with the same SD; the script resumes after
  this line.
- `host-rm <relpath>` — delete a file from the virtual SD mid-run.
- `echo <text>` — log line.

Every `expect-*` polls (10 s default; `expect-no-*` 2 s; `expect-file` 15 s;
an explicit timeout may be appended). The first failed expectation prints an
OSD dump and exits non-zero.

`press`/`hold` drive the FPGA pad state that the firmware polls; `mode` and
`power-cycle` re-exec the sim, so anything in RAM (FPGA save RAM, programmed
core) is lost while the SD directory persists — just like real hardware.

## Tests

```bash
bash host/run-tests.sh
```

Builds the sim, creates a fresh virtual SD per test (tiny dummy ROMs and
bitstreams), and runs `host/tests/*.script`:

- `a-menu` — cursor after boot and after closing a game; heartbeat alive.
- `b-hidden-message-mode` — `gba_bios.bin` hidden by the `.gba` filter,
  missing-BIOS message box dismissed with A, MODE reboots.
- `c-pause` — game menu sets `core_config` bit 17 (pause), Resume clears it,
  no spurious save file.
- `d-sms-save` — SMS battery round trip (poke → dirty → 32768-byte `.sav` →
  power cycle → reload → same bytes restored).
- `e-options-persist` — scanlines toggle saved to `tangcore.cfg`, bit 16 set
  after a power cycle.
- `f-md-save` — MegaDrive battery round trip (16384 bytes from an `RA` header).
- `g-mode-early` — MODE pressed right after a game loads, before the
  firmware has polled the new core, still restarts.

## Current state and limits

- USB is stubbed: no gamepads or USB drives; keyboard/script drive pad 1,
  and `usb:` maps to the same directory as `sd:`.
- FPGA programming is stubbed: the bitstream filename selects the reported
  core ID (instant success, no JTAG, no bitstream parsing).
- The OSD wrap rule (column 32 wraps to the next row) is the sim's guess at
  the hardware text console.
- Directory listings are sorted alphabetically for deterministic tests;
  real FAT returns directory order.
- Simulated wall-clock time (no time scaling): debounce and watchdog
  constants behave as on hardware.
- The Flash-mode menu path is not usable in the sim: there is no ROM
  loader to reboot into, so scripts must not select it.
- `GLB_SW_System_Reset` (MODE, flash mode) and `power-cycle` re-exec the sim
  process; SD contents persist, RAM state does not.
