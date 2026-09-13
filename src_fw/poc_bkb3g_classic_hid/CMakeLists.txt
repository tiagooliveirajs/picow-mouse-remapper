cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico_w CACHE STRING "Board type")

# Reuse the Pico SDK import helper already tracked by the parent firmware.
include(${CMAKE_CURRENT_LIST_DIR}/../picow_ble_usb_hid_bridge/pico_sdk_import.cmake)

project(bkb3g_classic_hid C CXX ASM)

pico_sdk_init()

add_executable(bkb3g_classic_hid
    main.c
    bkb3g_classic_hid.c
)

target_link_libraries(bkb3g_classic_hid PRIVATE
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_ble
    pico_btstack_classic
)

target_include_directories(bkb3g_classic_hid PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Development POC console:
# - USB CDC is enabled so logs are available over the same USB cable as /dev/ttyACM*.
# - UART remains enabled as a fallback for low-level debugging.
# This POC does not expose USB HID yet, so USB CDC does not conflict with the final bridge stage.
pico_enable_stdio_usb(bkb3g_classic_hid 1)
pico_enable_stdio_uart(bkb3g_classic_hid 1)

pico_add_extra_outputs(bkb3g_classic_hid)
