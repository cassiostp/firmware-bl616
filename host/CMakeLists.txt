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

# RTL backend (co-simulation): link ONE core's Verilated model, built with
# `make model` in that core's sim/cosim directory, e.g.
#   cmake -S host -B host/build-rtl-nestang -DNESTANG_COSIM_DIR=<nestang>/sim/cosim
# Every model_<core>.cpp defines new_rtl_model(), and the models share
# Verilated symbol names, so a binary holds exactly one; run-tests.sh keeps a
# build dir per core. main.cpp checks --core against the linked model's name.
set(RTL_MODEL_COUNT 0)
function(tangcore_rtl_model DIRVAR LIBNAME MODELSRC)
    set(DIR "${${DIRVAR}}")
    if (NOT DIR)
        return()
    endif ()
    math(EXPR N "${RTL_MODEL_COUNT} + 1")
    set(RTL_MODEL_COUNT ${N} PARENT_SCOPE)
    if (N GREATER 1)
        message(FATAL_ERROR "one RTL model per binary: set only one *_COSIM_DIR per build dir")
    endif ()
    set(LIB "${DIR}/build/obj/${LIBNAME}")
    if (NOT EXISTS "${LIB}" OR NOT EXISTS "${DIR}/build/runtime/verilated.h")
        message(FATAL_ERROR "${DIRVAR} lacks a built model (missing ${LIB}); run make model in ${DIR} first")
    endif ()
    target_sources(tangcore-sim PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/backend_rtl.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl/${MODELSRC})
    target_include_directories(tangcore-sim PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sim/backend_rtl
        ${DIR}/build/obj ${DIR}/build/runtime ${DIR}/build/runtime/vltstd)
    target_link_libraries(tangcore-sim PRIVATE "${LIB}" "${DIR}/build/obj/libverilated.a")
    target_compile_definitions(tangcore-sim PRIVATE TANGCORE_HAVE_RTL=1)
    message(STATUS "RTL backend: ${MODELSRC} with ${DIR}")
endfunction()

set(NESTANG_COSIM_DIR "" CACHE PATH "nestang sim/cosim dir with a built model")
set(SNESTANG_COSIM_DIR "" CACHE PATH "snestang sim/cosim dir with a built model")
set(MDTANG_COSIM_DIR "" CACHE PATH "mdtang sim/cosim dir with a built model")
set(SMSTANG_COSIM_DIR "" CACHE PATH "smstang sim/cosim dir with a built model")
tangcore_rtl_model(NESTANG_COSIM_DIR Vcosim_top__ALL.a model_nes.cpp)
tangcore_rtl_model(SNESTANG_COSIM_DIR Vsnestang_cosim__ALL.a model_snes.cpp)
tangcore_rtl_model(MDTANG_COSIM_DIR Vcosim_top__ALL.a model_md.cpp)
tangcore_rtl_model(SMSTANG_COSIM_DIR Vcosim_top__ALL.a model_sms.cpp)


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
