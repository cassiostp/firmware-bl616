# tangcore-sim: Linux host simulator for the TangCore BL616 firmware.
# Builds the REAL firmware sources (main.cpp, ui/, utils/, core/) against
# thin host shims (host/shim + host/sim); no BL616 toolchain needed.
cmake_minimum_required(VERSION 3.16)
project(tangcore-sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Real firmware sources. Excluded on purpose (stubbed by host/sim/stubs.cpp):
#   utils/init.cpp      real GPIO/UART setup (no hardware here)
#   fpga/programmer.cpp JTAG bit-bang programmer (records + pretends success)
#   usb/*               CherryUSB host stack (no USB devices in the sim)
set(FW_SOURCES
    ${FIRMWARE_DIR}/main.cpp
    ${FIRMWARE_DIR}/ui/console.cpp
    ${FIRMWARE_DIR}/ui/file_chooser.cpp
    ${FIRMWARE_DIR}/ui/game_controls.cpp
    ${FIRMWARE_DIR}/ui/menu_manager.cpp
    ${FIRMWARE_DIR}/ui/overlay.cpp
    ${FIRMWARE_DIR}/utils/settings.cpp
    ${FIRMWARE_DIR}/utils/utils.cpp
    ${FIRMWARE_DIR}/core/cores.cpp
    ${FIRMWARE_DIR}/core/gba.cpp
    ${FIRMWARE_DIR}/core/md.cpp
    ${FIRMWARE_DIR}/core/nes.cpp
    ${FIRMWARE_DIR}/core/pcxt.cpp
    ${FIRMWARE_DIR}/core/saves.cpp
    ${FIRMWARE_DIR}/core/sms.cpp
    ${FIRMWARE_DIR}/core/snes.cpp
)

set(SIM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/sim_time.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/rtos.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/ffsim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_fake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sim/stubs.cpp
)

add_executable(tangcore-sim ${SIM_SOURCES} ${FW_SOURCES})

# RTL backend (optional): Verilated cosim_top of a core, prebuilt by that
# core's sim/cosim/Makefile (needs docker at model-build time, not here).
#   cmake -S host -B host/build -DNESTANG_COSIM_DIR=<nestang>/sim/cosim
# The directory must contain build/obj/Vcosim_top__ALL.a (the model) and
# build/runtime/verilated.h (Verilator runtime headers); without it the sim
# builds with the fake core only and --core nestang-rtl reports unbuilt.
set(NESTANG_COSIM_DIR "" CACHE PATH "Nestang sim/cosim dir with a built model")
if (NESTANG_COSIM_DIR)
    set(NES_MODEL_LIB "${NESTANG_COSIM_DIR}/build/obj/Vcosim_top__ALL.a")
    set(NES_RT_HDR "${NESTANG_COSIM_DIR}/build/runtime/verilated.h")
    if (EXISTS "${NES_MODEL_LIB}" AND EXISTS "${NES_RT_HDR}")
        target_sources(tangcore-sim PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/backend_rtl.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/model_nes.cpp)
        target_include_directories(tangcore-sim PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl
            ${NESTANG_COSIM_DIR}/build/obj
            ${NESTANG_COSIM_DIR}/build/runtime
            ${NESTANG_COSIM_DIR}/build/runtime/vltstd)
        target_link_libraries(tangcore-sim PRIVATE
            "${NES_MODEL_LIB}"
            "${NESTANG_COSIM_DIR}/build/obj/libverilated.a")
        target_compile_definitions(tangcore-sim PRIVATE TANGCORE_HAVE_RTL=1)
        message(STATUS "RTL backend: nestang model from ${NESTANG_COSIM_DIR}")
    else ()
        message(FATAL_ERROR "NESTANG_COSIM_DIR lacks a built model "
            "(missing ${NES_MODEL_LIB}); run make model in <nestang>/sim/cosim first")
    endif ()
endif ()

# MDTANG_COSIM_DIR: same shape for the MegaDrive core (mdtang sim/cosim, run
# make model there). backend_rtl.cpp stays byte-identical across cores: it
# constructs its model through new_nes_model(), so this build renames that
# call to new_md_model() at compile time. One RTL model per binary (the
# rename is per-TU, so both cores' models cannot coexist in one link; build
# them in separate build dirs, e.g. host/build-rtl and host/build-rtl-md).
set(MDTANG_COSIM_DIR "" CACHE PATH "Mdtang sim/cosim dir with a built model")
if (MDTANG_COSIM_DIR)
    if (NESTANG_COSIM_DIR)
        message(FATAL_ERROR "one RTL model per binary: configure either "
            "NESTANG_COSIM_DIR or MDTANG_COSIM_DIR, in separate build dirs")
    endif ()
    set(MD_MODEL_LIB "${MDTANG_COSIM_DIR}/build/obj/Vcosim_top__ALL.a")
    set(MD_RT_HDR "${MDTANG_COSIM_DIR}/build/runtime/verilated.h")
    if (EXISTS "${MD_MODEL_LIB}" AND EXISTS "${MD_RT_HDR}")
        target_sources(tangcore-sim PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/backend_rtl.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/model_md.cpp)
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/backend_rtl.cpp
            PROPERTIES COMPILE_DEFINITIONS "new_nes_model=new_md_model")
        target_include_directories(tangcore-sim PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl
            ${MDTANG_COSIM_DIR}/build/obj
            ${MDTANG_COSIM_DIR}/build/runtime
            ${MDTANG_COSIM_DIR}/build/runtime/vltstd)
        target_link_libraries(tangcore-sim PRIVATE
            "${MD_MODEL_LIB}"
            "${MDTANG_COSIM_DIR}/build/obj/libverilated.a")
        target_compile_definitions(tangcore-sim PRIVATE TANGCORE_HAVE_RTL=1)
        message(STATUS "RTL backend: mdtang model from ${MDTANG_COSIM_DIR}")
    else ()
        message(FATAL_ERROR "MDTANG_COSIM_DIR lacks a built model "
            "(missing ${MD_MODEL_LIB}); run make model in <mdtang>/sim/cosim first")
    endif ()
endif ()

target_include_directories(tangcore-sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sim
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${FIRMWARE_DIR}
    ${FIRMWARE_DIR}/ui
    ${FIRMWARE_DIR}/utils
    ${FIRMWARE_DIR}/core
    ${FIRMWARE_DIR}/fpga
    ${FIRMWARE_DIR}/usb
)

# Console 138K layout (BOARD_NAME=console138k); USB_DBG_INFO satisfies the
# vendored CherryUSB config header included via core/cores.h; TANGCORE_HOST
# guards the one host-only hook in the firmware (see utils/utils.h).
target_compile_definitions(tangcore-sim PRIVATE
    TANG_CONSOLE138K
    TANGCORE_HOST=1
    USB_DBG_INFO=2
)

# Firmware main() becomes a thread entry. (The firmware's weak putchar() loses
# against libc's strong one at link time, so stdout stays ours.)
set_source_files_properties(${FIRMWARE_DIR}/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=firmware_main")

target_compile_options(tangcore-sim PRIVATE -Wall -Wextra -Wno-unused-parameter -g)
target_link_libraries(tangcore-sim PRIVATE pthread)
