Read the project-wide rules first: https://github.com/cassiostp/tangcore/blob/main/AGENTS.md

# firmware-bl616 — BL616 menu firmware

Menu, SD card, USB, saves, and FPGA loading for the Tang Console. Runs on the board BL616, talks to the FPGA core over UART1 at 2 Mbaud with a framed protocol (`AA lenHi lenLo cmd payload...`).

## Layout

- `main.cpp`: entry, `main_task`, `uart1_rx_task`, `program_fpga`/`load_game`/`menu_loadrom`, `watchdog_task`.
- `ui/`: `console.cpp`, `file_chooser.cpp`, `game_controls.cpp` (combos, MODE, flash mode), `menu_manager.cpp`, `overlay.cpp` (OSD `0x05/0x08`).
- `core/`: `cores.cpp` (core table), `saves.cpp` (battery saves), `fastload.cpp` (negotiated baud), per-core `gba.cpp`/`md.cpp`/`nes.cpp`/`snes.cpp`/`sms.cpp`/`pcxt.cpp`.
- `utils/`: `utils.cpp` (TX header, core/video config, heartbeat), `init.cpp` (GPIO+UART1), `uart_rx.cpp` (2048-B ISR ring), `clock.cpp` (`0x14`), `settings.cpp` (`tangcore.cfg`), `dbg_log.cpp`, `dbg_console.cpp`.
- `usb/`: `usb_gamepad.cpp` + `hidparser.cpp` + `usb2ps2.cpp`, `usb_console.cpp` (CDC ACM), `usb_config.h`.
- `host/`: Linux simulator (`CMakeLists.txt`, `README.md`, `run-tests.sh`, `shim/`, `sim/`, `tests/`).

## Building

CI builds both console boards (`build.yml`: `console60k`, `console138k`; `CMakeLists.txt` also knows mega/primer/nano but `buildall.bat` leaves them out):

```sh
gh workflow run build.yml -R cassiostp/firmware-bl616 --ref <branch>
```

Local: pin the toolchain and SDK at the SHAs in `build.yml`/`BUILDING.md` (`nand2mario/bouffalo_sdk`, x86_64-only), export `BL_SDK_BASE`, then `make TANG_BOARD=console60k`. Output is `build/build_out/tangcore_bl616.bin`, renamed per board. See `BUILDING.md` and `README.md` (legacy `make`/`make flash` notes).

## Host simulator

Compiles the real firmware (`main.cpp`, `ui/`, `utils/`, `core/`) against shims; see `host/README.md`:

```bash
cmake -S host -B host/build; cmake --build host/build -j
./host/build/tangcore-sim --sd <dir>            # needs cores/console138k/*.bin, nes/ snes/ gba/ genesis/ sms/ pc/, saves/
bash host/run-tests.sh                           # fake backend
bash host/run-tests.sh --rtl-<core>              # Verilated core backend (nes|snes|md|sms|gba), default dir ../../<core>tang/sim/cosim
```

CI: `gh workflow run host-sim.yml -R cassiostp/firmware-bl616 --ref <branch>`; its `core_ref` input picks which core branches the RTL suites build (defaults to `feature/smoothing`). Suites: interactive fake-backend scripts (`a-menu` … `z-*`) plus per-core `r-*`/`n-*`/`m-*`/`s-*`/`g-rtl-*` RTL suites, all `*.script` under `host/tests/` with the `wait/press/expect-*/poke-*/churn/mode/power-cycle` verbs; three unit tests (`dbg_log`, `sim_time`, `uart_rx_model`) run in `run-tests.sh`.

## UART framed protocol

Documented in `tangcore/doc/dev.md` (linked from `README.md`) with the wire details in code: header `AA lenHi lenLo cmd` (`utils/utils.cpp:fpga_tx_header`), 2 Mbaud 8N1 (`utils/init.cpp`), RX gap resync including when the reader is idle, length rejected outside 1..1024, and `0x17` link resync (`main.cpp`, `utils/utils.cpp:fpga_link_flush`). MCU->FPGA: `0x01` core-ID poll, `0x03` core_config, `0x13` video_config, `0x05` printf/OSD text, `0x06` loading, `0x07` ROM data, `0x08` overlay, `0x09` HID, `0x11/0x12` save write/fetch, `0x14` clock, `0x15/0x16` fast-UART, `0x17` link resync; FPGA->MCU: `0x01` core ID, `0x02` config string, `0x03` pad, `0x04/0x05` floppy, `0x0A` save block, `0x0B` dirty, `0x0C/0x0D` fast-UART answers.
Core IDs (`core/cores.cpp`): 1 NES, 2 SNES, 3 GBA, 4 MegaDrive, 5 SMS, 6 PCXT; monitor core is 0. Shared config bits are in `utils/utils.h` (`[16]` scanlines, `[17]` menu pause, `[19:18]` dark, `[20]` thick, `[21]` full, `[22]` mute-pads; `video_config` bright/contrast/sat/gamma/mask/grid/smoothing).

## Saves, USB console, flashing

- Saves (`core/saves.cpp`): `<drv>saves/<geomDir>/<romBase>.sav` (`drv` `sd:`/`usb:`, geom dirs `sms/snes/nes/gba/genesis`); atomic `*.sav.tmp` + `f_sync` + rename; short files blank-filled. Channel `0x11 blk+512B` restore, `0x12` fetch, `0x0A` reply, `0x0B` dirty; dumps ~1 s after dirty (4 s if dumped <10 s ago), 5 retries.
- USB debug console (`usb/usb_console.cpp`): the single BL616 USB swaps host->device to CDC ACM (`/dev/ttyACM*`, VID/PID `1209:0001`); commands `status/tasks/log/saves/link/sniff/wdtest/reboot`. Rings are drop-not-block; disabled while `drv` is `usb:`.
- Flashing: `flash.ini` puts the partner image at `0x0` and firmware at `0x40000`. Never touch the partner image at 0x0; the user flashes (Flash Cube + `flash_<board>.ini`, or the menu Options->Flash mode; `README.md`/`BUILDING.md`).

## Watchdog

`main.cpp:watchdog_task` (priority above `main_task`) feeds the ~2 s hardware watchdog (`bflb_wdg_*`, `WDG_CLKSRC_32K`) every 500 ms while the heartbeat (bumped by input/listings/transfers) is fresh; a ≥15 s stall logs and calls `tc_restart_mcu`. `wdtest` in the USB console stalls it on purpose; `expect-alive` in sim scripts guards it.
