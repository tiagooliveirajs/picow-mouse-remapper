# Generated Cmake Pico project file

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
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(picow_ble_usb_hid_bridge C CXX ASM)

pico_sdk_init()

add_compile_options(-Wall
        -Wno-format
        -Wno-unused-function
        -Wno-maybe-uninitialized
        )

add_executable(picow_ble_usb_hid_bridge
    main.c
    usb_descriptors.c
    hog_host_demo_poc.c
    picow_bt_example_common.c
    Common.c
    canonical_hid.c
    device_profile.c
    remote_hid_queue.c
    remap_profile.c
    logitech_hidpp.c
    remap_engine.c
    pico_hat_ui.c
    pico07_status_pairing_test_ui.c
    )

# BTstack and TinyUSB publish overlapping HID identifiers. Keep the PICO-05
# identity implementation isolated instead of exposing btstack.h in its public
# header. SDK 2.2.0 defines critical_section_t in pico/critical_section.h.
set_source_files_properties(device_profile.c PROPERTIES
    COMPILE_OPTIONS "-include;btstack.h;-include;pico/critical_section.h")

target_link_libraries(picow_ble_usb_hid_bridge
    pico_stdlib
    pico_multicore
    hardware_sync
    hardware_spi
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
    PICO_CORE1_STACK_SIZE=0x1000
    )
pico_btstack_make_gatt_header(picow_ble_usb_hid_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/hog_host_demo.gatt
    )

pico_enable_stdio_usb(picow_ble_usb_hid_bridge 0)
pico_enable_stdio_uart(picow_ble_usb_hid_bridge 1)

pico_add_extra_outputs(picow_ble_usb_hid_bridge)
