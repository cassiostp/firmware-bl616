#!/bin/sh
# Fail the build if firmware code adds a global that needs a constructor.
#
# The SDK's linker script keeps no .init_array, so global constructors never
# run on the board: such a global starts zero-filled. A zero-filled
# std::vector happens to be a valid empty vector, but a std::string has a
# null data pointer and a std::map has no header node. (A global
# std::string in savestates.cpp froze every game start this way.) The host
# simulator runs constructors normally, so only the link map shows it: the
# linker lists each dropped .init_array under "Discarded input sections".
#
# Use plain arrays, as saves.cpp does, or construct on first use.
#
# Usage: scripts/check-static-init.sh build/build_out/tangcore_bl616.map

map=${1:?usage: $0 <link map>}
[ -f "$map" ] || { echo "check-static-init: no map at $map" >&2; exit 2; }

# Objects that already had such globals when this check was added. Each one
# so far relies on the zero-filled state being usable:
#   main.cpp          last_rom/last_core_file (std::string)
#   cores.cpp         std::vector lists
#   menu_manager.cpp  std::vector menu stack
#   console.cpp       debug console buffers (std::vector, std::string)
#   usb2ps2.cpp       std::map scancode table (PC/XT keyboard)
#   pcxt.cpp          floppy names (std::string)
known="main.cpp.o cores.cpp.o menu_manager.cpp.o console.cpp.o usb2ps2.cpp.o pcxt.cpp.o"

found=$(awk '
    /^Discarded input sections/ { in_discarded = 1; next }
    /^Memory Configuration/     { in_discarded = 0 }
    in_discarded && $1 ~ /^\.init_array/ && NF >= 4 {
        obj = $4
        if (obj ~ /riscv64-unknown-elf\/lib\//) next    # toolchain libraries
        sub(/\)$/, "", obj); sub(/^.*[(\/]/, "", obj)    # archive(member) or path
        print obj
    }' "$map" | sort -u)

status=0
for obj in $found; do
    case " $known " in
    *" $obj "*) ;;
    *)
        echo "check-static-init: $obj has a global that needs a constructor," \
             "which never runs on the board" >&2
        status=1
        ;;
    esac
done
[ $status -eq 0 ] && echo "check-static-init: ok (constructors never run; only known globals have them)"
exit $status
