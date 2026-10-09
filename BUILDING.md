# Building and flashing

## Download from CI

Pushes to `master` are built by GitHub Actions (`.github/workflows/build.yml`). Other branches are built on demand: "Run workflow" on the Actions tab, or `gh workflow run build --ref <branch>`. Open the run under the repo's **Actions** tab and download the artifact for your board from the bottom of the page:

* `tangcore-console60k`
* `tangcore-console138k`

Each artifact contains `tangcore_<board>.bin` and `flash_<board>.ini`.

The artifacts do **not** contain `bl616_fpga_partner_<board>.bin`, which the `.ini` file expects next to the firmware. Take it from the TangCore release zip, or from Sipeed's [download page](https://dl.sipeed.com/shareURL/TANG/Console/09_MCU_FW) (`bl616_fpga_partner_60kConsole.bin`), and rename it to `bl616_fpga_partner_<board>.bin`, like `buildall.bat` does.

## Build locally (Linux x86_64)

The Bouffalo toolchain and the bundled `cmake` / post-processing tools are x86_64 binaries, so this does not work on ARM machines. The revisions below are the ones the CI uses (see the top of the workflow file).

```bash
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git ~/toolchain
git -C ~/toolchain checkout c4afe91cbd01bf7dce525e0d23b4219c8691e8f0

git clone https://github.com/nand2mario/bouffalo_sdk.git ~/bouffalo_sdk
git -C ~/bouffalo_sdk checkout 7f44f9ea6b4ccf96db8c5236c8024b68e2a76df7

export BL_SDK_BASE=$HOME/bouffalo_sdk
export PATH=$HOME/toolchain/bin:$PATH

make TANG_BOARD=console60k       # or console138k
```

The result is `build/build_out/tangcore_bl616.bin`. To get the same file names as the CI and `buildall.bat`:

```bash
board=console60k
cp build/build_out/tangcore_bl616.bin tangcore_$board.bin
sed -e "s/bl616_fpga_partner\.bin/bl616_fpga_partner_$board.bin/" \
    -e "s/tangcore\.bin/tangcore_$board.bin/" flash.ini > flash_$board.ini
```

Run `make clean` before building for a different board.

## Flash (Windows, Bouffalo Flash Cube)

1. Put `tangcore_<board>.bin`, `bl616_fpga_partner_<board>.bin` and `flash_<board>.ini` in one folder.
2. Press and hold the "BOOT" button on the board (bottom left corner, close to one of the USB-C ports), then plug the BL616 USB-C port into the PC. This enters programming mode.
3. Open Bouffalo Flash Cube, choose chip BL616 and select the serial port.
4. Load `flash_<board>.ini` as the config, then press Download.
5. Unplug, release BOOT and power the board again.

`make flash COMX=com5` (see the README) does the same from the command line, using `flash_prog_cfg.ini`.
