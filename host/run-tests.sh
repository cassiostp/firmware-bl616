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

if [ -n "$RTL_CORE" ]; then
    case "$RTL_CORE" in
        nes)  SUITE=r; add_rom() { add_nes_battery "$1" game.nes; } ;;
        snes) SUITE=n; add_rom() { add_snes "$1" game.smc; } ;;
        md)   SUITE=m; add_rom() { add_md "$1" game.md; } ;;
        gba)  SUITE=g-rtl; add_rom() { add_gba_sram "$1" game.gba; } ;;
        sms)  SUITE=s; add_rom() {    # $2 = test: s-gg-config gets a Game Gear file
                  local f=game.sms; [ "$2" = s-gg-config ] && f=game.gg
                  mkdir -p "$1/sms"; head -c 8192 /dev/zero > "$1/sms/$f"; } ;;
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
            sms:*) add_sms "$sd" "${spec#sms:}" ;;
            gba) add_gba "$sd" ;;
            md:*) add_md "$sd" "${spec#md:}" ;;
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

run_test a-menu nes:test.nes
run_test b-hidden-message-mode gba
run_test c-pause sms:cgame.sms
run_test d-sms-save sms:game.sms
run_test e-options-persist
run_test f-md-save md:game.md
run_test g-mode-early sms:cgame.sms
run_test h-scanlines sms:cgame.sms
run_test i-video-color sms:cgame.sms
run_test j-video-persist sms:cgame.sms
run_test k-video-grid sms:cgame.sms sms:game.gg
run_test l-video-grid-gba gba

echo "=== $PASS passed, $FAIL failed ==="
if [ "$FAIL" -ne 0 ]; then
    echo "failed: ${FAILED_NAMES[*]}"
    exit 1
fi
