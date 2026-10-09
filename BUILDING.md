# Building and flashing

## Download from CI

Pushes to `master` are built by GitHub Actions (`.github/workflows/build.yml`). Other branches are built on demand: "Run workflow" on the Actions tab, or `gh workflow run build --ref <branch>`. Open the run under the repo's **Actions** tab and download the artifact for your board from the bottom of the page:

* `tangcore-console60k`
* `tangcore-console138k`

Each artifact is a complete flash set: `tangcore_<board>.bin`, `flash_<board>.ini`, and Sipeed's `bl616_fpga_partner_<board>.bin`. The partner image is closed source; CI copies it from this repo's [`sipeed-partner-2025030317`](https://github.com/cassiostp/firmware-bl616/releases/tag/sipeed-partner-2025030317) release and verifies its SHA-256.

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

1. Unzip the CI artifact (or put `tangcore_<board>.bin`, `bl616_fpga_partner_<board>.bin` and `flash_<board>.ini` in one folder).
2. Put the BL616 into programming mode, either way:
   - **From the menu** (this firmware or later): **Options → Flash mode... → Restart in flash mode**, then plug the BL616 USB-C port into the PC. No need to open the case.
   - **With the BOOT button:** press and hold "BOOT" on the board (bottom left corner, close to one of the USB-C ports), then plug the BL616 USB-C port into the PC.
3. Open Bouffalo Flash Cube, choose chip BL616 and select the serial port.
4. Load `flash_<board>.ini` as the config, then press Download.
5. Unplug (release BOOT if held) and power the board again.

The BOOT button always works, so it's the way back if a flash goes wrong.

`make flash COMX=com5` (see the README) does the same from the command line, using `flash_prog_cfg.ini`.
