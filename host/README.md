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
- `hold select+start+l 400ms` — hold for a sim-time duration (combos).
- `expect-screen "text"` / `expect-no-screen "text"` — OSD contains (or for
  the whole timeout, never contains) the substring.
- `expect-cursor-row 9` — the `>` cursor is on that OSD row.
- `expect-core smstang` — the programmed core (`monitor nestang snestang
  gbatang mdtang smstang pctang`).
- `expect-config-bit 17 1` — a bit of the last `core_config` word (on the
  RTL backend this reads the core's real register).
- `expect-overlay on|off`.
- `expect-file saves/sms/game.sav size 32768` / `expect-no-file <rel>`.
- `poke-save 0x10 0x42` — write one save-RAM byte and raise the dirty notice
  (`0x0B`): direct RAM write on fake, game-path WRAM write on RTL.
- `wram-write 0x10 0x42` — a game-path write into save RAM (dirties the save
  like a running game would; on fake identical to `poke-save`).
- `wram-burst 0x000 512 0xA5` — `len` game-path writes of a seeded pattern
  (`seed+i`) from `off`.
- `expect-save-ram 0x10 0x42` — check a save-RAM byte (verifies restore
  frames after a reload; on RTL reads the SDRAM model).
- `churn on|off` — the "game" continuously scribbles WRAM (RTL only; the
  fake has no game model and ignores it). For combo/reset-during-dump tests.
- `wait-dump [timeout]` — returns once a save-block request (`0x12`) is
  seen, i.e. a dump is in progress, so a held combo races it.
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
- `h-scanlines` — game menu scanlines screen drives `core_config` bits
  16/19:18/20/21 live, with preview; choice survives a power cycle.

## RTL backend (core co-simulation)

The same firmware binary can talk to a Verilator model of a core's real
interface logic instead of the fake core:

```bash
bash host/run-tests.sh --rtl-nes[=/path/to/nestang/sim/cosim]     # r-*.script (also plain --rtl)
bash host/run-tests.sh --rtl-snes[=/path/to/snestang/sim/cosim]   # n-*.script
bash host/run-tests.sh --rtl-md[=/path/to/mdtang/sim/cosim]       # m-*.script
bash host/run-tests.sh --rtl-sms[=/path/to/smstang/sim/cosim]     # s-*.script
```

Each builds the core's model once (`make model` in its `sim/cosim`, needs
docker), builds `host/build-rtl-<core>tang/tangcore-sim` against it, and
runs that core's suite with `--core <core>tang-rtl`. The path defaults to
`../../<core>tang/sim/cosim` relative to `host/`.

A binary links exactly one core model: every `backend_rtl/model_<core>.cpp`
defines `new_rtl_model()`, and CMake (`tangcore_rtl_model()`) refuses more
than one `*_COSIM_DIR` per build dir. `main.cpp` checks `--core` against the
linked model's `name()`. Adding a core means a `model_<core>.cpp`, one
`tangcore_rtl_model()` line, and a case in `run-tests.sh`.

- `r-save-roundtrip` — battery round trip through the real save engine and
  SDRAM (burst → `.sav` → power cycle → restore → same bytes in SDRAM).
- `r-combo-save` — menu combo while a dump is in progress with the game
  writing WRAM continuously (pause bit set, Resume clears it).
- `r-reset-save` — reset combo likewise (back to the running game).
- `r-config` — Scanlines screen drives the real `core_config` register;
  Resume clears the pause bit.
- `r-mode` — MODE reloads the FPGA from flash, firmware reboots to the menu.
- `m-save-roundtrip` — MegaDrive battery round trip (cart-SRAM burst →
  16384-byte `.sav` from the header's `RA` range → power cycle → restore).
- `m-combo-save` / `m-reset-save` — the same combos racing a dump with the
  game writing cart SRAM (MD does not pause for dumps: real arbitration).
- `m-config` — the Scanlines screen and its Preview drive the real register,
  including bit 22 (the MegaDrive's pad mute; cosim_top checks the game
  never sees the pads while it is set).
- `m-mode` — MODE on the MegaDrive core.

`--core nestang-rtl` also works for manual `--script` and interactive runs.
On RTL, `press`/`hold` drive the FPGA's pad inputs (change-detect `0x03`
frames reach the firmware like hardware, 20 ms throttle included),
`poke-save`/`wram-*` go through the game WRAM path, `expect-config-bit`
reads the real register, and `expect-save-ram` reads the SDRAM model.
`SCRIPT PASS` prints sim and wall seconds (rate ≈ 0.2–1 sim-s per wall-s
on RTL; the suite takes ~2 min, model build once ~2 min).

How it works (see `host/sim/backend_rtl/` + the core's `sim/cosim/`):
bit-level UART at the real 2 Mbaud, model stepped in ≤ 256-tick batches,
fully quiet batches jumped without evaluating (only dead air is skipped).
Programming sets the model's core ID and resets it (SDRAM retained, like
hardware); MODE gates both UART directions, then resets as core 0. MCU
bytes queue and serialize back-to-back; a 4 KB bound applies hardware
FIFO backpressure (blocking `putchar` pumps sim time, so no deadlock).

### SNES

`n-save-roundtrip` / `n-combo-save` / `n-reset-save` / `n-config` /
`n-mode` mirror the `r-*` shapes with SNES fixtures (a LoROM header whose
SRAM-size byte sizes the dump: 8 KB = 16 blocks) and the core's shared
BSRAM port: `wram-*` writes go through the SNES cartridge bus and contend
with the dump's reads in the real arbiter (SNES first, save bytes slip in
between). snestang's save bridge lives in `snestang_top.v`, so its
`cosim_top.sv` carries a copy of that logic: keep the two in step.

### Master System

`s-save-roundtrip` / `s-combo-save` / `s-reset-save` / `s-config` / `s-mode`
mirror the `r-*` shapes against smstang's model; `s-gg-config` loads a
`.gg` file and checks Game Gear mode (core_config bit 0). smstang keeps its
32 KB battery RAM in on-chip dual-port RAM (iosys on one port, the game on
the other), so its model has no SDRAM: `cosim_top.sv` is iosys plus the real
`dpram.v`, and the model clocks at 21.492 MHz with `FREQ` set to match so
the wire runs at 2 Mbaud in firmware time (iosys's 20 ms pad throttle
becomes ~46 ms; the firmware doesn't mind).

## Simulated time

Everything runs on a virtual clock (`host/sim/sim_time.*`): mtimer,
`arch_delay_ms`, `vTaskDelay`, semaphore/notify timeouts and script
waits/timeouts are sim milliseconds (1 tick = 1/21.492 MHz core clock).
Threads waiting with a deadline drive the clock forward in 5 ms chunks;
pure event waits (`portMAX_DELAY`) sleep until kicked. A FIFO ticket lock
keeps chunk steps fair, clock reads are lock-free, and every chunk sleeps
~2 ms of wall time (rate cap): uncapped, the sim runs 30–3000 sim-s per
wall-s and firmware timeouts (the 1 s save-block fetch, the 2 s save
debounce) land inside normal scheduling jitter and fail spuriously. The
cap keeps them at ~100 ms of wall time each. Suite runs are deterministic
(4/4 consecutive full passes, including once under load average 20).

## Current state and limits

- USB is stubbed: no gamepads or USB drives; keyboard/script drive pad 1,
  and `usb:` maps to the same directory as `sd:`.
- FPGA programming is stubbed: the bitstream filename selects the reported
  core ID (instant success, no JTAG, no bitstream parsing).
- The OSD wrap rule (column 32 wraps to the next row) is the sim's guess at
  the hardware text console.
- Directory listings are sorted alphabetically for deterministic tests;
  real FAT returns directory order.
- The Flash-mode menu path is not usable in the sim: there is no ROM
  loader to reboot into, so scripts must not select it.
- `GLB_SW_System_Reset` (MODE, flash mode) and `power-cycle` re-exec the sim
  process; SD contents persist, RAM state does not.
