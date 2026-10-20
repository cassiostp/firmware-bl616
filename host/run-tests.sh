#!/bin/bash
# Build tangcore-sim and run the scripted host-sim test suite.
# Each test gets a fresh virtual SD card; the sim exits non-zero on the
# first failed expectation (printing an OSD dump), which fails the test.
#
#   bash host/run-tests.sh                        fake-FPGA suite (fast, default)
#   bash host/run-tests.sh --rtl-<core>[=COSIM]   one core's RTL suite against its
#                                                 Verilated model (docker builds it
#                                                 once). <core>: nes (r-*.script,
#                                                 also plain --rtl), snes (n-*),
#                                                 md (m-*), sms (s-*),
#                                                 gba (g-rtl-*).
# COSIM is the core's sim/cosim directory; it defaults to
# ../../<core>tang/sim/cosim relative to host/ (sibling worktrees).
set -u

HOST_DIR="$(cd "$(dirname "$0")" && pwd)"
RTL_CORE=""
RTL_COSIM=""
for arg in "$@"; do
    case "$arg" in
        --rtl|--rtl=*) RTL_CORE=nes; RTL_COSIM="${arg#--rtl}" ;;
        --rtl-*) a="${arg#--rtl-}"; RTL_CORE="${a%%=*}"
                 case "$a" in *=*) RTL_COSIM="${a#*=}" ;; *) RTL_COSIM="" ;; esac ;;
        *) echo "usage: $0 [--rtl-<nes|snes|md|sms|gba>[=<core>/sim/cosim]]"; exit 2 ;;
    esac
done
RTL_COSIM="${RTL_COSIM#=}"

seed_bad_ss() {
    # $1 = dir, $2 = rom stem: a state file whose header names another
    # mapper (firmware must refuse it before touching the core).
    local dir="$1" stem="$2"
    mkdir -p "$dir/saves/nes"
    python3 - "$dir/saves/nes/$stem.ss4" <<'EOF'
import sys
h = bytearray(512)
h[0:4] = b'TCSS'
h[4] = 1; h[5] = 1; h[6] = 1
h[8] = 4                      # mapper 4, not the running game's mapper 1
h[16:20] = (22016).to_bytes(4, 'little')
open(sys.argv[1], 'wb').write(bytes(h) + bytes(22016 - 512))
EOF
}

seed_bad_ss_sms() {
    # $1 = dir, $2 = rom stem: a state file for another game/build, which
    # the firmware must refuse before touching the core (GG flag set while
    # the running game is SMS, plus zero ROM identity).
    local dir="$1" stem="$2"
    mkdir -p "$dir/saves/sms"
    python3 - "$dir/saves/sms/$stem.ss4" <<'EOF'
import sys
h = bytearray(512)
h[0:4] = b'TCSS'
h[4] = 1; h[5] = 1; h[6] = 5
h[8] = 1                       # GG mode, not the running SMS game
h[16:20] = (58880).to_bytes(4, 'little')
h[20:24] = (16384).to_bytes(4, 'little')
h[24:28] = (8192).to_bytes(4, 'little')
open(sys.argv[1], 'wb').write(bytes(h) + bytes(58880 - 512))
EOF
}

add_nes_battery() {
    # $1 = dir, $2 = name: minimal iNES ROM (16 KB PRG + 8 KB CHR) WITH the
    # battery bit, so the firmware treats WRAM as battery-backed (needed by
    # the RTL save tests; the fake-suite ROMs intentionally lack it).
    local dir="$1" name="$2"
    mkdir -p "$dir/nes"
    python3 - "$dir/nes/$name" <<'EOF'
import sys
d = bytearray(b'NES\x1a\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00')
d[6] |= 0x02  # battery-backed WRAM
d += bytes(24560)
open(sys.argv[1], 'wb').write(bytes(d))
EOF
}

add_nes_plain() {
    # $1 = dir, $2 = name: the same ROM WITHOUT the battery bit (a game whose
    # $6000 RAM is scratch, e.g. an MMC3 cart's work RAM).
    local dir="$1" name="$2"
    mkdir -p "$dir/nes"
    printf 'NES\x1a\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$dir/nes/$name"
    head -c 24560 /dev/zero >> "$dir/nes/$name"
}

add_snes() {
    # $1 = dir, $2 = name: 32 KB SNES ROM with a valid LoROM header at 0x7FC0
    # (core/snes.cpp's rules): ASCII title, map_ctrl 0, ROM size 5 (32 KB),
    # SRAM size 3 -- 8 KB battery RAM, so saves/snes/<rom>.sav is 16 blocks
    # (8192 bytes) -- checksum pair summing to 0xFFFF, reset vector 0x8000.
    local dir="$1" name="$2"
    mkdir -p "$dir/snes"
    python3 - "$dir/snes/$name" <<'EOF'
import sys
d = bytearray(32768)
d[0x7FC0:0x7FC0 + 21] = b'COSIM TEST GAME'.ljust(21, b' ')
d[0x7FD5] = 0x00        # map_ctrl: LoROM
d[0x7FD6] = 0x31        # rom type: LoROM, ROM + SRAM (battery)
d[0x7FD7] = 0x05        # ROM size: 32 KB
d[0x7FD8] = 0x03        # SRAM size byte: 8 KB (16 save blocks)
d[0x7FDC:0x7FDE] = b'\x00\x00'    # checksum
d[0x7FDE:0x7FE0] = b'\xFF\xFF'    # complement (sum = 0xFFFF)
d[0x7FFC:0x7FFE] = b'\x00\x80'    # reset vector 0x8000
open(sys.argv[1], 'wb').write(bytes(d))
EOF
}

add_gba_sram() {
    # $1 = dir, $2 = name: 16 KB dummy GBA ROM carrying the SRAM_V113 backup
    # ID (the firmware scans the stream for it and picks backup type 3, 64
    # blocks = 32 KB) plus the gba_bios.bin the GBA loader wants (present so
    # no missing-BIOS message box needs dismissing; the RTL model never runs
    # it, it just gets streamed).
    local dir="$1" name="$2"
    mkdir -p "$dir/gba"
    python3 - "$dir/gba/$name" <<'EOF'
import sys
d = bytearray(16384)
d[0xC0:0xC9] = b'SRAM_V113'
open(sys.argv[1], 'wb').write(bytes(d))
EOF
    head -c 16384 /dev/zero > "$dir/gba/gba_bios.bin"
}

add_md_quirk() {
    # $1 = dir, $2 = name, $3 = 8-char header serial (at $183): 2 KB ROM
    # whose serial core/md.cpp maps to a cart quirk word (no "RA" header:
    # serial-EEPROM carts size their save from the chip table instead).
    local dir="$1" name="$2" serial="$3"
    mkdir -p "$dir/genesis"
    python3 - "$dir/genesis/$name" "$serial" <<'EOF'
import sys
data = bytearray(2048)
data[0x183:0x18B] = sys.argv[2].encode().ljust(8, b' ')[:8]
open(sys.argv[1], 'wb').write(data)
EOF
}

add_md_big() {
    # $1 = dir, $2 = name: 5 MB ROM (SSF2 size) with the add_md "RA" header.
    local dir="$1" name="$2"
    mkdir -p "$dir/genesis"
    python3 - "$dir/genesis/$name" <<'EOF'
import sys
data = bytearray(2048)
data[0x1B0:0x1B2] = b'RA'
data[0x1B4:0x1B8] = (0x00200000).to_bytes(4, 'big')
data[0x1B8:0x1BC] = (0x00203FFF).to_bytes(4, 'big')
with open(sys.argv[1], 'wb') as f:
    f.write(data)
    f.truncate(5 * 1024 * 1024)
EOF
}

add_md() {
    # $1 = dir, $2 = name: 2 KB ROM with an "RA" backup-RAM header for
    # $200000-$203FFF (16 KB image = 32 save blocks).
    local dir="$1" name="$2"
    mkdir -p "$dir/genesis"
    python3 - "$dir/genesis/$name" <<'EOF'
import sys
data = bytearray(2048)
data[0x1B0:0x1B2] = b'RA'
data[0x1B4:0x1B8] = (0x00200000).to_bytes(4, 'big')
data[0x1B8:0x1BC] = (0x00203FFF).to_bytes(4, 'big')
open(sys.argv[1], 'wb').write(data)
EOF
}

add_120hz() {
    # $1 = dir: a 120 Hz build next to every game core's bitstream.
    local dir="$1" c
    for c in nestang snestang gbatang mdtang smstang; do
        cp "$dir/cores/console138k/$c.bin" "$dir/cores/console138k/${c}_120hz.bin"
    done
}

add_many() {
    # $1 = dir: 30 numbered SMS ROMs, so the ROM browser spans two pages.
    local dir="$1" i
    mkdir -p "$dir/sms"
    for i in $(seq -w 1 30); do
        head -c 8192 /dev/zero > "$dir/sms/game$i.sms"
    done
}

if [ -n "$RTL_CORE" ]; then
    case "$RTL_CORE" in
        nes)  SUITE=r; add_rom() { add_nes_battery "$1" game.nes; add_nes_plain "$1" plain.nes; seed_bad_ss "$1" game; } ;;
        snes) SUITE=n; add_rom() { add_snes "$1" game.smc; } ;;
        md)   SUITE=m; add_rom() { add_md "$1" game.md; } ;;
        gba)  SUITE=g-rtl; add_rom() { add_gba_sram "$1" game.gba; } ;;
        sms)  SUITE=s; add_rom() {    # $2 = test: s-gg-config gets a Game Gear file
                  local f=game.sms; [ "$2" = s-gg-config ] && f=game.gg
                  mkdir -p "$1/sms"; head -c 8192 /dev/zero > "$1/sms/$f"
                  seed_bad_ss_sms "$1" game; } ;;
        *) echo "unknown RTL core: $RTL_CORE"; exit 2 ;;
    esac
    BACKEND="${RTL_CORE}tang-rtl"
    COSIM="${RTL_COSIM:-$HOST_DIR/../../${RTL_CORE}tang/sim/cosim}"
    COSIM_VAR="$(echo "${RTL_CORE}tang" | tr a-z A-Z)_COSIM_DIR"
    BUILD_DIR="$HOST_DIR/build-rtl-${RTL_CORE}tang"
    echo "=== building ${BACKEND%-rtl} RTL model ($COSIM) ==="
    make -C "$COSIM" model || exit 1
    SIM="$BUILD_DIR/tangcore-sim"
    echo "=== building tangcore-sim (RTL) ==="
    cmake -S "$HOST_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
        -D$COSIM_VAR="$COSIM" || exit 1
    cmake --build "$BUILD_DIR" -j"$(nproc)" || exit 1

    PASS=0
    FAIL=0
    FAILED_NAMES=()
    run_rtl_test() {
        # $1 = script base name ($SUITE-*.script, runs with --core $BACKEND).
        local name="$1"
        local sd
        sd="$(mktemp -d)"
        rm -rf "$sd"
        mkdir -p "$sd/cores/console138k"
        local c
        for c in monitor nestang snestang gbatang mdtang smstang pctang; do
            head -c 4096 /dev/urandom > "$sd/cores/console138k/$c.bin"
        done
        add_rom "$sd" "$name"
        echo "=== test $name (sd: $sd) ==="
        if timeout 400 "$SIM" --sd "$sd" --core "$BACKEND" \
                --script "$HOST_DIR/tests/$name.script"; then
            echo "--- PASS $name"
            PASS=$((PASS + 1))
            rm -rf "$sd"
        else
            echo "--- FAIL $name (sd kept at $sd)"
            FAIL=$((FAIL + 1))
            FAILED_NAMES+=("$name")
        fi
    }

    for script in "$HOST_DIR/tests/$SUITE-"*.script; do
        run_rtl_test "$(basename "$script" .script)"
    done

    echo "=== $PASS passed, $FAIL failed ==="
    if [ "$FAIL" -ne 0 ]; then
        echo "failed: ${FAILED_NAMES[*]}"
        exit 1
    fi
    exit 0
fi

BUILD_DIR="$HOST_DIR/build"
SIM="$BUILD_DIR/tangcore-sim"

echo "=== building tangcore-sim ==="
cmake -S "$HOST_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release || exit 1
cmake --build "$BUILD_DIR" -j"$(nproc)" || exit 1

CORES=(monitor nestang snestang gbatang mdtang smstang pctang)

make_sd() {
    # $1 = dir; common skeleton: one dummy bitstream per core.
    local dir="$1"
    rm -rf "$dir"
    mkdir -p "$dir/cores/console138k"
    local c
    for c in "${CORES[@]}"; do
        head -c 4096 /dev/urandom > "$dir/cores/console138k/$c.bin"
    done
}

add_nes() {
    # $1 = dir, $2 = name: minimal iNES ROM (16 KB PRG + 8 KB CHR, no battery).
    local dir="$1" name="$2"
    mkdir -p "$dir/nes"
    printf 'NES\x1a\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$dir/nes/$name"
    head -c 24560 /dev/zero >> "$dir/nes/$name"
}

add_sms() {
    # $1 = dir, $2 = name: 8 KB dummy SMS ROM.
    local dir="$1" name="$2"
    mkdir -p "$dir/sms"
    head -c 8192 /dev/zero > "$dir/sms/$name"
}

add_gba() {
    # $1 = dir: dummy GBA ROM plus a BIOS (the test deletes it mid-run).
    local dir="$1"
    mkdir -p "$dir/gba"
    head -c 16384 /dev/zero > "$dir/gba/game.gba"
    head -c 1024 /dev/zero > "$dir/gba/gba_bios.bin"
}

add_snes_variant() {
    # $1 = dir, $2 = variant: synthetic SNES ROM (snes/game.smc) whose only
    # crafted bytes are the 64-byte header core/snes.cpp parses: ASCII title,
    # map_ctrl/chipset for the variant, a checksum pair summing to 0xFFFF
    # (as the firmware reads it) and reset vector 0x8000. No battery RAM
    # (ram byte 0) except sa1 (256 KB BW-RAM byte 8), lorobatt (2 KB battery
    # LoROM) and the gsu battery games, whose size is the expansion-RAM byte
    # $FFBD with the later extended header ([FFDA]=0x33): gsu15b (chip 0x15,
    # $FFBD 5 = 32 KB, like Yoshi's Island), gsu1ab (chip 0x1A, $FFBD 6 =
    # 64 KB, like Stunt Race FX). gsu13/14 are the non-battery GSU headers
    # (Star Fox shape: ram 0, no extended header, no save).
    # exhirom follows snes.cpp's typ-2 rule as it stands: a header at
    # 0x40FFC0 with (map_ctrl & 3) == 2. bsx carries a BS-X flash header at
    # 0x7FC0 (normal parse rejects it: no reset vector, byte 24 is a bank
    # byte, not a RAM size). notrom is all zeros.
    local dir="$1" variant="$2"
    mkdir -p "$dir/snes"
    python3 - "$dir/snes/game.smc" "$variant" <<'EOF'
import sys
path, variant = sys.argv[1], sys.argv[2]
# variant: (header_pos, map_ctrl, chipset, rom_size, ram_size, total_size,
#           company, exp_ram) -- company/exp_ram None = no extended header.
V = {
    'lorom':    (0x7FC0,   0x20, 0x00, 5,  0, 32768, None, None),
    'lorobatt': (0x7FC0,   0x20, 0x02, 5,  1, 32768, None, None),
    'hirom':    (0xFFC0,   0x21, 0x00, 6,  0, 65536, None, None),
    'exhirom':  (0x40FFC0, 0x22, 0x00, 12, 0, 0x410000, None, None),
    'gsu13':    (0x7FC0,   0x20, 0x13, 5,  0, 32768, None, None),
    'gsu14':    (0x7FC0,   0x20, 0x14, 5,  0, 32768, None, None),
    'gsu15':    (0x7FC0,   0x20, 0x15, 5,  0, 32768, None, None),
    'gsu1a':    (0x7FC0,   0x20, 0x1A, 5,  0, 32768, None, None),
    'gsu15b':   (0x7FC0,   0x20, 0x15, 5,  0, 32768, 0x33, 5),
    'gsu1ab':   (0x7FC0,   0x20, 0x1A, 5,  0, 32768, 0x33, 6),
    'sa1':      (0x7FC0,   0x23, 0x35, 5,  8, 32768, None, None),
    'sdd1':     (0xFFC0,   0x32, 0x43, 6,  0, 65536, None, None),
    'spc7110':  (0xFFC0,   0x3A, 0xF5, 6,  0, 65536, None, None),
    'cx4':      (0x7FC0,   0x20, 0xF3, 5,  0, 32768, None, None),
    'st018':    (0x7FC0,   0x30, 0xF5, 5,  0, 32768, None, None),
}
if variant == 'notrom':
    open(path, 'wb').write(bytes(32768))
    sys.exit(0)
if variant == 'bsx':
    d = bytearray(32768)
    p = 0x7FC0
    d[p:p + 21] = b'BSX FLASH TEST'.ljust(21, b' ')
    d[p + 22] = 0x10    # BS-X header byte 22 (loader's BS-X check)
    d[p + 24] = 32      # BS-X header byte 24: a normal-bank value
    d[p + 26] = 0x33    # BS-X header byte 26
    open(path, 'wb').write(bytes(d))
    sys.exit(0)
pos, mc, chip, rom, ram, total, company, expram = V[variant]
d = bytearray(total)
d[pos:pos + 21] = ('CHIP TEST ' + variant.upper()).encode().ljust(21, b' ')[:21]
d[pos + 21] = mc        # map_ctrl
d[pos + 22] = chip      # chipset (rom_type_header)
d[pos + 23] = rom       # ROM size: 1024 << rom bytes
d[pos + 24] = ram       # SRAM size byte
if company is not None:
    d[pos + 26] = company   # [FFDA]: later extended header present
if expram is not None:
    d[pos - 3] = expram    # [FFBD]: expansion RAM size (1 SHL n) KBytes
d[pos + 28:pos + 30] = b'\x00\x00'    # checksum
d[pos + 30:pos + 32] = b'\xff\xff'    # complement (pair sums to 0xFFFF)
d[pos + 60:pos + 62] = b'\x00\x80'    # reset vector 0x8000
open(path, 'wb').write(bytes(d))
EOF
}

add_gba_rtc() {
    # $1 = dir, $2 = game code, $3 = backup ID string: 16 KB GBA ROM whose
    # game code at 0xAC marks it as an RTC (and maybe solar) cart, plus BIOS.
    local dir="$1"
    mkdir -p "$dir/gba"
    python3 - "$dir/gba/game.gba" "$2" "$3" <<'EOF'
import sys
d = bytearray(16384)
d[0xAC:0xB0] = sys.argv[2].encode()
d[0xC0:0xC0 + len(sys.argv[3])] = sys.argv[3].encode()
open(sys.argv[1], 'wb').write(bytes(d))
EOF
    head -c 1024 /dev/zero > "$dir/gba/gba_bios.bin"
}

seed_clock() {
    # $1 = dir: tangcore.cfg with the wall clock set to 2026-10-13 12:34 UTC.
    local dir="$1"
    python3 - "$dir/tangcore.cfg" <<'EOF'
import calendar, sys
u = calendar.timegm((2026, 10, 13, 12, 34, 0, 0, 0, 0))
open(sys.argv[1], 'w').write("clock_unix=%d\nclock_hbn=0\nsolar_pct=50\n" % u)
EOF
}

seed_old_scale() {
    # $1 = dir: a tangcore.cfg as the firmware before Scale moved wrote it.
    printf 'scanline_full=0\nsmoothing=1\n' > "$1/tangcore.cfg"
}

# (add_md is defined up with the RTL fixtures: both suites use it.)

PASS=0
FAIL=0
FAILED_NAMES=()

run_test() {
    # $1 = script base name, $2.. = fixture commands ("nes:test.nes" etc.)
    local name="$1"
    shift
    local sd
    sd="$(mktemp -d)"
    make_sd "$sd"
    local spec
    for spec in "$@"; do
        case "$spec" in
            nes:*) add_nes "$sd" "${spec#nes:}" ;;
            ssbad:*) seed_bad_ss "$sd" "${spec#ssbad:}" ;;
            # snes:<variant> (chip-header tests) vs snes:<file>.smc (a plain
            # LoROM fixture): variant names carry no dot.
            snes:*) case "${spec#snes:}" in
                        *.*) add_snes "$sd" "${spec#snes:}" ;;
                        *) add_snes_variant "$sd" "${spec#snes:}" ;;
                    esac ;;
            sms:*) add_sms "$sd" "${spec#sms:}" ;;
            hz120) add_120hz "$sd" ;;
            manysms) add_many "$sd" ;;
            gba) add_gba "$sd" ;;
            gba-rtc) add_gba_rtc "$sd" AXVE FLASH1M_V ;;
            gba-rtc-sram) add_gba_rtc "$sd" AXVE SRAM_V ;;
            gba-solar) add_gba_rtc "$sd" U3IE EEPROM_V ;;
            cfg-clock) seed_clock "$sd" ;;
            cfg-oldscale) seed_old_scale "$sd" ;;
            md:*) add_md "$sd" "${spec#md:}" ;;
            mdjam:*) add_md_quirk "$sd" "${spec#mdjam:}" 'T-81406 ' ;;
            mdwb:*) add_md_quirk "$sd" "${spec#mdwb:}" 'G-4060  ' ;;
            mdbig:*) add_md_big "$sd" "${spec#mdbig:}" ;;
            *) echo "bad fixture: $spec"; exit 2 ;;
        esac
    done
    echo "=== test $name (sd: $sd) ==="
    if timeout 150 "$SIM" --sd "$sd" --script "$HOST_DIR/tests/$name.script"; then
        echo "--- PASS $name"
        PASS=$((PASS + 1))
        rm -rf "$sd"
    else
        echo "--- FAIL $name (sd kept at $sd)"
        FAIL=$((FAIL + 1))
        FAILED_NAMES+=("$name")
    fi
}

echo "=== test dbg-log-test ==="
if "$BUILD_DIR/dbg-log-test"; then
    echo "--- PASS dbg-log-test"
    PASS=$((PASS + 1))
else
    echo "--- FAIL dbg-log-test"
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("dbg-log-test")
fi

echo "=== test uart-rx-model ==="
if "$BUILD_DIR/uart-rx-model"; then
    echo "--- PASS uart-rx-model"
    PASS=$((PASS + 1))
else
    echo "--- FAIL uart-rx-model"
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("uart-rx-model")
fi

echo "=== test sim-time-test ==="
if timeout 60 "$BUILD_DIR/sim-time-test"; then
    echo "--- PASS sim-time-test"
    PASS=$((PASS + 1))
else
    echo "--- FAIL sim-time-test"
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("sim-time-test")
fi

run_test a-menu nes:test.nes
run_test ss-save-roundtrip nes:test.nes ssbad:test
run_test a-menu-quiet
run_test b-hidden-message-mode gba
run_test c-pause sms:cgame.sms
run_test d-sms-save sms:game.sms
run_test e-options-persist
run_test f-md-save md:game.md
run_test w-md-eeprom mdjam:jamte.md
run_test w-md-quirks mdwb:wboy.md
run_test w-md-big mdbig:big.md
run_test g-mode-early sms:cgame.sms
run_test h-scanlines sms:cgame.sms
run_test i-video-color sms:cgame.sms
run_test j-video-persist sms:cgame.sms
run_test k-video-grid sms:cgame.sms sms:game.gg
run_test l-video-grid-gba gba
run_test o-video-scale sms:cgame.sms
run_test o-video-scale-oldcfg cfg-oldscale sms:cgame.sms
run_test hz120-display sms:cgame.sms hz120
run_test hz120-fallback sms:cgame.sms
run_test hz120-video sms:cgame.sms hz120
run_test hz120-video60 sms:cgame.sms
run_test menu-scroll manysms
run_test p-mode-game-short sms:cgame.sms
run_test q-mode-menu-short
run_test u-mode-drop-once sms:cgame.sms
run_test u-hid-resend sms:cgame.sms
run_test u-link-resync sms:cgame.sms
run_test u-link-poison sms:cgame.sms
run_test v-mode-fresh-silent sms:cgame.sms
run_test p-save-fail sms:game.sms
run_test q-save-status sms:game.sms
run_test q-save-timing sms:game.sms
run_test t-wedge-main
run_test t-garbage-pads sms:cgame.sms
run_test w-wedge-scanlines sms:cgame.sms
run_test x-monitor-cores
run_test y-diag-game-menu sms:cgame.sms
run_test z-capture-stuck
run_test t-snes-lorom snes:lorom
run_test t-snes-hirom snes:hirom
run_test t-snes-exhirom snes:exhirom
run_test t-snes-gsu13 snes:gsu13
run_test t-snes-gsu14 snes:gsu14
run_test t-snes-gsu15 snes:gsu15
run_test t-snes-gsu1a snes:gsu1a
run_test t-snes-gsubatt-save snes:gsu15b
run_test t-snes-gsu1ab-save snes:gsu1ab
run_test t-snes-gsunosave snes:gsu13
run_test t-snes-lorobatt-save snes:lorobatt
run_test t-snes-sa1 snes:sa1
run_test t-snes-sdd1 snes:sdd1
run_test t-snes-spc7110 snes:spc7110
run_test t-snes-cx4 snes:cx4
run_test t-snes-st018 snes:st018
run_test t-snes-bsx snes:bsx
run_test t-snes-notrom snes:notrom
run_test w-clock-set
run_test x-clock-mode sms:cgame.sms
run_test y-clock-gba cfg-clock gba-rtc
run_test y-clock-snes cfg-clock snes:game.smc
run_test z-clock-solar cfg-clock gba-solar
run_test z-rtc-eeprom-saves cfg-clock gba-rtc-sram mdjam:jamte.md

echo "=== $PASS passed, $FAIL failed ==="
if [ "$FAIL" -ne 0 ]; then
    echo "failed: ${FAILED_NAMES[*]}"
    exit 1
fi
