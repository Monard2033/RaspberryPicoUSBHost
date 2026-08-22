# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")
option(WIRELESS_KEYBOARD_UART_DIAGNOSTIC
       "Enable bounded DAPLink UART diagnostics in WirelessKeyboard" OFF)
set(POWER_SAVING_DIAGNOSTIC_IDLE_MS "" CACHE STRING
    "Override radio inactivity timeout for a diagnostic build only")

cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH "C:/Users/Monard/.pico-sdk/sdk/2.3.0")
include(pico_sdk_import.cmake)

project(WirelessKeyboard C CXX ASM)

pico_sdk_init()

# ---- Pico-PIO-USB -----------------------------------------------------
set(PICO_PIO_USB_DIR "${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB")
add_subdirectory(${PICO_PIO_USB_DIR} pico_pio_usb)

add_executable(WirelessKeyboard
    WirelessKeyboard.c
)

pico_set_program_name(WirelessKeyboard "WirelessKeyboard")
pico_set_program_version(WirelessKeyboard "0.2")

pico_enable_stdio_usb(WirelessKeyboard 0)
pico_enable_stdio_uart(WirelessKeyboard ${WIRELESS_KEYBOARD_UART_DIAGNOSTIC})

# === TINYUSB HOST MODE (PIO-USB on RHPORT1) ===
target_compile_definitions(WirelessKeyboard PRIVATE
    CFG_TUH_ENABLED=1
    CFG_TUH_RPI_PIO_USB=1
    CFG_TUH_HID=4
    CFG_TUH_HID_KEYBOARD=1
    CFG_TUD_ENABLED=0
    PIO_USB_USE_TINYUSB
    WIRELESS_KEYBOARD_OTA_SUPPORT=1
)

if(POWER_SAVING_DIAGNOSTIC_IDLE_MS)
    target_compile_definitions(WirelessKeyboard PRIVATE
        RADIO_INACTIVITY_MS=${POWER_SAVING_DIAGNOSTIC_IDLE_MS}
    )
endif()

if(WIRELESS_KEYBOARD_UART_DIAGNOSTIC)
    target_compile_definitions(WirelessKeyboard PRIVATE
        RUNTIME_LOGGING=1
        HID_DIAGNOSTIC_LOG=1
        HOT_PATH_DEBUG=0
        PERIODIC_DEBUG=0
        CONSUMER_DEBUG=0
    )
endif()

target_link_libraries(WirelessKeyboard
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_board
    pico_pio_usb
    hardware_pio
    hardware_dma
    hardware_spi
    hardware_adc
    hardware_pwm
    hardware_flash
    pico_flash
    hardware_sync
)

target_include_directories(WirelessKeyboard PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_SDK_PATH}/lib/tinyusb/src
)

# FIX: compile hcd_pio_usb.c explicitly (SDK does not auto-include it)
target_sources(WirelessKeyboard PRIVATE
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

pico_add_extra_outputs(WirelessKeyboard)

# Artifact roles (do not duplicate the same image under multiple names):
#   firmware/WirelessKeyboard.uf2      - current build, BOOTSEL cable image
#   firmware/WirelessKeyboard_OTA.bin  - raw OTA payload (make_ota_package input)
#   firmware/WirelessKeyboard_OTA.wkota - strict OTA package for flash_ota.exe
# Validated snapshots are managed by tools/save_good_build.ps1 (firmware/good).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(TARGET WirelessKeyboard POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_LIST_DIR}/firmware"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/WirelessKeyboard.uf2"
                "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboard.uf2"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/WirelessKeyboard.bin"
                "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboard_OTA.bin"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_LIST_DIR}/tools/make_ota_package.py"
                --input "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboard_OTA.bin"
                --output "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboard_OTA.wkota"
        COMMENT "Updating firmware/WirelessKeyboard.uf2 and the OTA package set (_OTA.bin/.wkota)"
    )
else()
    add_custom_command(TARGET WirelessKeyboard POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_LIST_DIR}/firmware"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/WirelessKeyboard.uf2"
                "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboard.uf2"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/WirelessKeyboard.bin"
                "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboard_OTA.bin"
        COMMENT "Updating firmware/WirelessKeyboard.uf2 and _OTA.bin (Python not found: regenerate .wkota manually)"
    )
endif()

# === Safe variant: identical input pipeline, strict OTA DFU receiver fully
# compiled out (WIRELESS_KEYBOARD_OTA_SUPPORT=0). Immune to flash_ota.exe;
# build on demand with: cmake --build build --target WirelessKeyboardSafe
add_executable(WirelessKeyboardSafe EXCLUDE_FROM_ALL
    WirelessKeyboard.c
)

pico_set_program_name(WirelessKeyboardSafe "WirelessKeyboardSafe")
pico_set_program_version(WirelessKeyboardSafe "0.2")

pico_enable_stdio_usb(WirelessKeyboardSafe 0)
pico_enable_stdio_uart(WirelessKeyboardSafe ${WIRELESS_KEYBOARD_UART_DIAGNOSTIC})

target_compile_definitions(WirelessKeyboardSafe PRIVATE
    CFG_TUH_ENABLED=1
    CFG_TUH_RPI_PIO_USB=1
    CFG_TUH_HID=4
    CFG_TUH_HID_KEYBOARD=1
    CFG_TUD_ENABLED=0
    PIO_USB_USE_TINYUSB
    WIRELESS_KEYBOARD_OTA_SUPPORT=0
)

target_link_libraries(WirelessKeyboardSafe
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_board
    pico_pio_usb
    hardware_pio
    hardware_dma
    hardware_spi
    hardware_adc
    hardware_pwm
    hardware_flash
    pico_flash
    hardware_sync
)

target_include_directories(WirelessKeyboardSafe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_SDK_PATH}/lib/tinyusb/src
)

target_sources(WirelessKeyboardSafe PRIVATE
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

pico_add_extra_outputs(WirelessKeyboardSafe)

add_custom_command(TARGET WirelessKeyboardSafe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/WirelessKeyboardSafe.uf2"
            "${CMAKE_CURRENT_LIST_DIR}/firmware/WirelessKeyboardSafe.uf2"
    COMMENT "Updating firmware/WirelessKeyboardSafe.uf2 (no-OTA safe image)"
)

# RAM-only utility used to diagnose a QSPI flash that refuses UF2/OpenOCD
# erase operations. It intentionally erases/programs TEST_FLASH_OFFSET.
add_executable(FlashStatusDiag
    tools/flash_status_diag.c
)
target_link_libraries(FlashStatusDiag
    pico_stdlib
    hardware_flash
)
pico_enable_stdio_usb(FlashStatusDiag 0)
pico_enable_stdio_uart(FlashStatusDiag 1)
pico_set_binary_type(FlashStatusDiag no_flash)

# Volatile recovery/test image for boards whose QSPI flash cannot be written.
# It runs the exact application from SRAM and remains active until power/reset.
add_executable(WirelessKeyboardRam EXCLUDE_FROM_ALL
    WirelessKeyboard.c
)
pico_set_program_name(WirelessKeyboardRam "WirelessKeyboardRam")
pico_set_program_version(WirelessKeyboardRam "0.2")
pico_enable_stdio_usb(WirelessKeyboardRam 0)
pico_enable_stdio_uart(WirelessKeyboardRam 0)
target_compile_definitions(WirelessKeyboardRam PRIVATE
    CFG_TUH_ENABLED=1
    CFG_TUH_RPI_PIO_USB=1
    CFG_TUH_HID=4
    CFG_TUH_HID_KEYBOARD=1
    CFG_TUD_ENABLED=0
    PIO_USB_USE_TINYUSB
)
target_link_libraries(WirelessKeyboardRam
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_board
    pico_pio_usb
    hardware_pio
    hardware_dma
    hardware_spi
    hardware_adc
    hardware_pwm
    hardware_flash
    pico_flash
    hardware_sync
)
target_include_directories(WirelessKeyboardRam PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_SDK_PATH}/lib/tinyusb/src
)
target_sources(WirelessKeyboardRam PRIVATE
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
pico_set_binary_type(WirelessKeyboardRam no_flash)
