cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

# The Classic HID POC was physically validated on the project's RP2350 board.
# Keep the production firmware on the same target to avoid generating an RP2040
# image that can be flashed accidentally to the Pico 2 W.
set(PICO_BOARD pico2_w CACHE STRING "Board type")
if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR
        "The remapper firmware currently targets Raspberry Pi Pico 2 W (RP2350). "
        "Use -DPICO_BOARD=pico2_w (got '${PICO_BOARD}').")
endif()

include(pico_sdk_import.cmake)

project(picow_ble_usb_hid_bridge C CXX ASM)
pico_sdk_init()

add_compile_options(
    -Wall
    -Wno-format
    -Wno-unused-function
    -Wno-maybe-uninitialized
)

add_executable(picow_ble_usb_hid_bridge
    main.c
    usb_descriptors.c
    Common.c
    AppLog.c
    BluetoothHost.c
    ClassicHidHost.c

    # Existing BLE HOG implementation is intentionally retained and compiled.
    # BluetoothHost selects Classic HID for G02; a later UI gate can select the
    # transport without re-coupling USB code to either implementation.
    hog_host_demo.c
    picow_bt_example_common.c
)

target_link_libraries(picow_ble_usb_hid_bridge
    pico_stdlib
    pico_multicore
    hardware_sync
    pico_unique_id
    tinyusb_device
    tinyusb_board
    pico_btstack_ble
    pico_btstack_classic
    pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background
)

target_include_directories(picow_ble_usb_hid_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(picow_ble_usb_hid_bridge PRIVATE
    CYW43_LWIP=0
    APP_FIRMWARE_VERSION="0.2.0-g02"
    APP_GATE_NAME="G02"
    APP_BT_DEFAULT_CLASSIC=1
)

pico_btstack_make_gatt_header(picow_ble_usb_hid_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/hog_host_demo.gatt
)

# TinyUSB owns native USB. CDC diagnostics remain available as part of the
# composite TinyUSB device; UART is kept only as a fallback debug channel.
pico_enable_stdio_usb(picow_ble_usb_hid_bridge 0)
pico_enable_stdio_uart(picow_ble_usb_hid_bridge 1)

pico_add_extra_outputs(picow_ble_usb_hid_bridge)
