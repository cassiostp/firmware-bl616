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
