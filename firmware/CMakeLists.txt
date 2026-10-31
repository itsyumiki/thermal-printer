cmake_minimum_required(VERSION 3.13)

# Pico-PIO-USB is vendored as a submodule, pinned to the release TinyUSB in
# the SDK expects. The build uses a patched copy in the build directory
# (lib/pico-pio-usb-bulk-multi-xact.patch: several bulk packets per frame
# instead of one), so the submodule itself stays pristine. The path must be
# set before the SDK pulls in TinyUSB.
set(FM1T_PIO_USB_SRC ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB)
set(FM1T_PIO_USB_PATCH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-pio-usb-bulk-multi-xact.patch)
set(PICO_PIO_USB_PATH ${CMAKE_BINARY_DIR}/Pico-PIO-USB-patched)
file(REMOVE_RECURSE ${PICO_PIO_USB_PATH})
file(COPY ${FM1T_PIO_USB_SRC}/src DESTINATION ${PICO_PIO_USB_PATH})
execute_process(
    COMMAND patch -p1 --forward --quiet -i ${FM1T_PIO_USB_PATCH}
    WORKING_DIRECTORY ${PICO_PIO_USB_PATH}
    RESULT_VARIABLE FM1T_PATCH_RESULT)
if (NOT FM1T_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Applying ${FM1T_PIO_USB_PATCH} failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FM1T_PIO_USB_PATCH})

# Which board drives the FM-1: xiao (Seeed XIAO RP2040, GP0 = D+, GP1 = D-) or
# debugprobe (Raspberry Pi Debug Probe, UART connector: GP4 = D+, GP6 = D-).
# On the Debug Probe D- is two pins above D+, which Pico-PIO-USB does not
# support, so lib/pico-pio-usb-debugprobe.patch teaches it that, and the
# headers it ships are removed so the ones generated from the patched .pio
# files are used.
set(FM1T_BOARD xiao CACHE STRING "Board: xiao or debugprobe")
set_property(CACHE FM1T_BOARD PROPERTY STRINGS xiao debugprobe)
if (FM1T_BOARD STREQUAL "debugprobe")
    set(FM1T_PROBE_PATCH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-pio-usb-debugprobe.patch)
    execute_process(
        COMMAND patch -p1 --forward --quiet -i ${FM1T_PROBE_PATCH}
        WORKING_DIRECTORY ${PICO_PIO_USB_PATH}
        RESULT_VARIABLE FM1T_PATCH_RESULT)
    if (NOT FM1T_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Applying ${FM1T_PROBE_PATCH} failed")
    endif()
    file(REMOVE ${PICO_PIO_USB_PATH}/src/usb_tx.pio.h ${PICO_PIO_USB_PATH}/src/usb_rx.pio.h)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FM1T_PROBE_PATCH})
    set(FM1T_BOARD_DEBUGPROBE 1)
elseif (FM1T_BOARD STREQUAL "xiao")
    set(FM1T_BOARD_DEBUGPROBE 0)
else()
    message(FATAL_ERROR "FM1T_BOARD must be xiao or debugprobe, not ${FM1T_BOARD}")
endif()

# TinyUSB debug level (CFG_TUSB_DEBUG). 2 logs host enumeration during M0.
set(LOG 2)

include(pico_sdk_import.cmake)

project(fm1_transporter C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

function(fm1t_add_firmware TARGET)
    add_executable(${TARGET}
        src/main.c
        src/recovery.c
        src/pio_host.c
        src/usb_device.c
        src/log.c
        src/jieli_uboot.c
        src/transporter_proto.c
    )

    pico_generate_pio_header(${TARGET}
        ${CMAKE_CURRENT_LIST_DIR}/pio/usb_key.pio
    )

    # tusb_config.h
    target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_compile_definitions(${TARGET} PRIVATE FM1T_BOARD_DEBUGPROBE=${FM1T_BOARD_DEBUGPROBE})

    target_link_libraries(${TARGET}
        pico_stdlib
        pico_multicore
        pico_unique_id
        pico_usb_reset_interface_headers
        hardware_pio
        hardware_gpio
        hardware_clocks
        hardware_dma
        tinyusb_device
        tinyusb_host
        tinyusb_pico_pio_usb
    )

    # The console is our own TinyUSB CDC device (see src/usb_device.c,
    # src/log.c); pico_stdio_usb cannot coexist with the TinyUSB host stack.
    pico_enable_stdio_usb(${TARGET} 0)
    pico_enable_stdio_uart(${TARGET} 0)

    pico_add_extra_outputs(${TARGET})
endfunction()

# M0 firmware: USB_KEY recovery -> PIO USB host.
fm1t_add_firmware(fm1_transporter)

# Host bring-up firmware: PIO USB host only, no USB_KEY.
fm1t_add_firmware(fm1_transporter_hostonly)
target_compile_definitions(fm1_transporter_hostonly PRIVATE FM1T_HOST_ONLY=1)
