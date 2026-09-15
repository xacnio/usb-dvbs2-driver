cmake_minimum_required(VERSION 3.16)
project(usb_dvbs2_driver VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The tuner: hardware drivers, the engine that runs the receiver, the daemon
# that puts it behind a command channel, the client library programs start
# and drive the daemon with, a command-line tool and tests. It builds on
# Windows, Linux and macOS, on its own or inside a parent project, which may
# pass its own copies of the folders below.

# The card server client and DVB-CSA descrambling. OFF leaves both out
# (and libdvbcsa with them): free services stream as before, scrambled ones
# stay scrambled, and the CAMD command reports that support is missing.
option(USB_DVBS2_CAMD "Build the card server client and DVB-CSA descrambling" ON)

if(NOT DEFINED USB_DVBS2_LIBUSB_DIR)
    set(USB_DVBS2_LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
endif()
if(NOT DEFINED USB_DVBS2_LIBDVBCSA_DIR)
    set(USB_DVBS2_LIBDVBCSA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdvbcsa)
endif()
# The receiver's firmware: tested, and packaged beside the executables.
if(NOT DEFINED USB_DVBS2_FIRMWARE_DIR)
    set(USB_DVBS2_FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/firmware)
endif()

find_package(Threads REQUIRED)

# --- libusb ---
#
# Windows: the prebuilt MinGW64 binaries from libusb's release, copied beside
# every executable. Linux and macOS: the system's, found with pkg-config
# (libusb-1.0-0-dev, or brew install libusb pkg-config).
set(LIBUSB_DIR ${USB_DVBS2_LIBUSB_DIR})
if(WIN32)
    # GLOBAL: a parent project links usbdvbs2core, whose interface names this target.
    add_library(libusb STATIC IMPORTED GLOBAL)
    set_target_properties(libusb PROPERTIES
        IMPORTED_LOCATION ${LIBUSB_DIR}/lib/libusb-1.0.dll.a
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUSB_DIR})
    set_property(GLOBAL PROPERTY USB_DVBS2_LIBUSB_DLL ${LIBUSB_DIR}/lib/libusb-1.0.dll)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET GLOBAL libusb-1.0)
    add_library(libusb INTERFACE IMPORTED GLOBAL)
    set_target_properties(libusb PROPERTIES
        INTERFACE_LINK_LIBRARIES PkgConfig::LIBUSB)
endif()

# Copies the libusb runtime DLL beside an executable on Windows; elsewhere
# the system library is found by the loader. Usable from the parent.
function(usb_dvbs2_copy_libusb target)
    if(WIN32)
        get_property(dll GLOBAL PROPERTY USB_DVBS2_LIBUSB_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${dll} $<TARGET_FILE_DIR:${target}>/libusb-1.0.dll)
    endif()
endfunction()

if(WIN32)
    set(USB_DVBS2_SYSTEM_LIBS ws2_32)
else()
    set(USB_DVBS2_SYSTEM_LIBS Threads::Threads)
endif()

# --- libdvbcsa (vendored, GPL-2.0) ---
# DVB-CSA decoder using bitslicing: SSE2 on x86, a plain C 128-bit word on
# other processors (third_party/libdvbcsa/config.h).
if(USB_DVBS2_CAMD)
    set(LIBDVBCSA_DIR ${USB_DVBS2_LIBDVBCSA_DIR})
    add_library(dvbcsa STATIC
        ${LIBDVBCSA_DIR}/dvbcsa_algo.c
        ${LIBDVBCSA_DIR}/dvbcsa_block.c
        ${LIBDVBCSA_DIR}/dvbcsa_stream.c
        ${LIBDVBCSA_DIR}/dvbcsa_key.c
        ${LIBDVBCSA_DIR}/dvbcsa_bs_algo.c
        ${LIBDVBCSA_DIR}/dvbcsa_bs_block.c
        ${LIBDVBCSA_DIR}/dvbcsa_bs_stream.c
        ${LIBDVBCSA_DIR}/dvbcsa_bs_key.c
        ${LIBDVBCSA_DIR}/dvbcsa_bs_transpose.c
        ${LIBDVBCSA_DIR}/dvbcsa_bs_transpose128.c)
    target_include_directories(dvbcsa PUBLIC ${LIBDVBCSA_DIR})
    if(NOT MSVC)
        # Third-party source; suppress local warnings.
        target_compile_options(dvbcsa PRIVATE -w)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
            target_compile_options(dvbcsa PRIVATE -msse2)
        endif()
    endif()
    # Tests the plain C word on an x86 machine: -DUSB_DVBS2_CSA_PORTABLE=ON.
    option(USB_DVBS2_CSA_PORTABLE "Use the plain C bitslice word on x86 too" OFF)
    if(USB_DVBS2_CSA_PORTABLE)
        target_compile_definitions(dvbcsa PUBLIC DVBCSA_FORCE_PORTABLE128)
    endif()
endif()

# --- Core: platform, drivers, stream handling, DVB tables, card server ---
set(USB_DVBS2_CORE_SOURCES
    core/dtv_platform.c
    core/dtv_net.c
    core/dtv_ipc.c
    core/dtv_sha256.c
    core/usb_backend.c
    core/dtv_thread.c
    core/firmware_verify.c
    core/hardware_profile.c
    core/it9300.c
    core/avl62x1.c
    core/rda5815m.c
    core/ts_services.c
    core/ts_epg.c
    core/dvb_text.c)
if(USB_DVBS2_CAMD)
    list(APPEND USB_DVBS2_CORE_SOURCES
        core/dtv_camd_crypt.c
        core/dtv_camd.c
        core/dtv_camd_newcamd.c
        core/dtv_camd_cs378x.c
        core/dtv_csa.c)
else()
    list(APPEND USB_DVBS2_CORE_SOURCES core/dtv_camd_disabled.c)
endif()
add_library(usbdvbs2core STATIC ${USB_DVBS2_CORE_SOURCES})
target_include_directories(usbdvbs2core PUBLIC core)
target_link_libraries(usbdvbs2core PUBLIC libusb ${USB_DVBS2_SYSTEM_LIBS})
if(USB_DVBS2_CAMD)
    target_link_libraries(usbdvbs2core PUBLIC dvbcsa)
endif()

# --- SDK: starts the daemon and talks to it ---
add_library(usbdvbs2client STATIC sdk/usb_dvbs2.c sdk/dtv_process.c)
target_include_directories(usbdvbs2client PUBLIC sdk)
target_link_libraries(usbdvbs2client PUBLIC usbdvbs2core)

# --- Engine: the receiver in-process ---
add_library(usbdvbs2engine STATIC engine/usb_dvbs2_engine.c
    engine/ts_udp.c engine/tool_util.c)
target_include_directories(usbdvbs2engine PUBLIC engine)
target_link_libraries(usbdvbs2engine PUBLIC usbdvbs2core)

# --- Daemon: the engine behind a command channel; see README.md ---
add_executable(usb-dvbs2-daemon daemon/usb_dvbs2_daemon.c)
target_link_libraries(usb-dvbs2-daemon PRIVATE usbdvbs2engine)
usb_dvbs2_copy_libusb(usb-dvbs2-daemon)

add_executable(usb-dvbs2-probe daemon/avl_tuner_probe.c
    engine/ts_udp.c engine/tool_util.c)
target_include_directories(usb-dvbs2-probe PRIVATE engine)
target_link_libraries(usb-dvbs2-probe PRIVATE usbdvbs2core)
usb_dvbs2_copy_libusb(usb-dvbs2-probe)

# --- Command-line tool on the SDK ---
add_executable(usb-dvbs2 cli/usb_dvbs2_cli.c)
target_link_libraries(usb-dvbs2 PRIVATE usbdvbs2client)

# --- Diagnostic tools for development ---
add_executable(usb-dvbs2-usb-probe tools/probe.c)
target_link_libraries(usb-dvbs2-usb-probe PRIVATE libusb)
usb_dvbs2_copy_libusb(usb-dvbs2-usb-probe)

function(usb_dvbs2_tool target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE usbdvbs2core)
    usb_dvbs2_copy_libusb(${target})
endfunction()

usb_dvbs2_tool(usb-dvbs2-bridge-test tools/bridge_test.c)
usb_dvbs2_tool(usb-dvbs2-i2c-scan tools/i2c_scan.c)
usb_dvbs2_tool(usb-dvbs2-demod-probe tools/demod_probe.c)
usb_dvbs2_tool(usb-dvbs2-gpio-scan tools/gpio_scan.c)
usb_dvbs2_tool(usb-dvbs2-avl-id tools/avl_id.c)
usb_dvbs2_tool(usb-dvbs2-avl-boot tools/avl_boot.c)
usb_dvbs2_tool(usb-dvbs2-tuner-test tools/tuner_test.c)
usb_dvbs2_tool(usb-dvbs2-demod-id tools/demod_id.c)
usb_dvbs2_tool(usb-dvbs2-reg-dump tools/reg_dump.c)
usb_dvbs2_tool(usb-dvbs2-strap-test tools/strap_test.c)
usb_dvbs2_tool(usb-dvbs2-ts-scan tools/ts_scan.c)
set(USB_DVBS2_TOOLS
    usb-dvbs2-bridge-test usb-dvbs2-i2c-scan usb-dvbs2-demod-probe usb-dvbs2-gpio-scan
    usb-dvbs2-avl-id usb-dvbs2-avl-boot usb-dvbs2-tuner-test usb-dvbs2-demod-id
    usb-dvbs2-reg-dump usb-dvbs2-strap-test usb-dvbs2-ts-scan)
if(USB_DVBS2_CAMD)
    # Checks live CAM server settings without a tuner.
    usb_dvbs2_tool(usb-dvbs2-camd-test tools/camd_test.c)
    list(APPEND USB_DVBS2_TOOLS usb-dvbs2-camd-test)
endif()

# --- Tests ---
enable_testing()

add_executable(usb-dvbs2-dvb-text-test tests/dvb_text_test.c core/dvb_text.c)
target_include_directories(usb-dvbs2-dvb-text-test PRIVATE core)
add_test(NAME dvb-text COMMAND usb-dvbs2-dvb-text-test)

add_executable(usb-dvbs2-epg-test tests/epg_test.c core/ts_epg.c core/dvb_text.c)
target_include_directories(usb-dvbs2-epg-test PRIVATE core)
add_test(NAME epg COMMAND usb-dvbs2-epg-test)

add_executable(usb-dvbs2-nit-test tests/nit_test.c core/ts_services.c core/dvb_text.c)
target_include_directories(usb-dvbs2-nit-test PRIVATE core)
add_test(NAME nit COMMAND usb-dvbs2-nit-test)

# CA, teletext and PMT parsing.
add_executable(usb-dvbs2-ca-test tests/ca_test.c core/ts_services.c core/dvb_text.c)
target_include_directories(usb-dvbs2-ca-test PRIVATE core)
add_test(NAME ca COMMAND usb-dvbs2-ca-test)

# Clocks, threads, events, SHA-256 and the command channel on this system.
add_executable(usb-dvbs2-platform-test tests/platform_test.c)
target_link_libraries(usb-dvbs2-platform-test PRIVATE usbdvbs2core)
add_test(NAME platform COMMAND usb-dvbs2-platform-test)

if(USB_DVBS2_CAMD)
    # CAM cryptography against published vectors.
    add_executable(usb-dvbs2-camd-crypt-test tests/camd_crypt_test.c
        core/dtv_camd_crypt.c)
    target_include_directories(usb-dvbs2-camd-crypt-test PRIVATE core)
    add_test(NAME camd-crypt COMMAND usb-dvbs2-camd-crypt-test)

    # DVB-CSA decoding.
    add_executable(usb-dvbs2-csa-test tests/csa_test.c core/dtv_csa.c)
    target_include_directories(usb-dvbs2-csa-test PRIVATE core)
    target_link_libraries(usb-dvbs2-csa-test PRIVATE dvbcsa)
    add_test(NAME csa COMMAND usb-dvbs2-csa-test)
endif()

if(EXISTS "${USB_DVBS2_FIRMWARE_DIR}/hiremco-it9303.fw" AND
   EXISTS "${USB_DVBS2_FIRMWARE_DIR}/hiremco-avl62x1.fw")
    add_executable(usb-dvbs2-firmware-verify-test tests/firmware_verify_test.c)
    target_link_libraries(usb-dvbs2-firmware-verify-test PRIVATE usbdvbs2core)
    add_test(NAME firmware-verify COMMAND usb-dvbs2-firmware-verify-test
        "${USB_DVBS2_FIRMWARE_DIR}/hiremco-it9303.fw"
        "${USB_DVBS2_FIRMWARE_DIR}/hiremco-avl62x1.fw")
endif()

# --- Firmware beside the executables ---
#
# The daemon, the engine and the tools read the firmware from firmware/
# beside the executable (or in their working directory). A copy beside the
# executables lets them run from the build folder -- a parent project's too
# -- as they do from a package.
if(EXISTS "${USB_DVBS2_FIRMWARE_DIR}")
    file(GLOB USB_DVBS2_FIRMWARE_FILES "${USB_DVBS2_FIRMWARE_DIR}/*.fw")
    add_custom_target(usb-dvbs2-firmware ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:usb-dvbs2-daemon>/firmware
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${USB_DVBS2_FIRMWARE_FILES}
                $<TARGET_FILE_DIR:usb-dvbs2-daemon>/firmware
        DEPENDS ${USB_DVBS2_FIRMWARE_FILES}
        COMMENT "Copying the receiver firmware beside the executables")
endif()

# --- Package: cmake --install <build> --prefix <folder> ---
#
# bin/ holds the executables with the firmware (and libusb on Windows) beside
# them, so the daemon runs from there as it is. include/ and lib/ are the
# SDK; the headers keep their folders, since usb_dvbs2.h names
# ../core/ts_epg.h. On Linux, share/ has the udev rule that lets a user who
# is not root open the receiver.
install(TARGETS usb-dvbs2-daemon usb-dvbs2 usb-dvbs2-probe usb-dvbs2-usb-probe
        ${USB_DVBS2_TOOLS}
        RUNTIME DESTINATION bin)
install(TARGETS usbdvbs2client usbdvbs2engine usbdvbs2core ARCHIVE DESTINATION lib)
if(USB_DVBS2_CAMD)
    install(TARGETS dvbcsa ARCHIVE DESTINATION lib)
endif()
if(WIN32)
    install(FILES ${LIBUSB_DIR}/lib/libusb-1.0.dll DESTINATION bin)
    install(FILES ${LIBUSB_DIR}/lib/libusb-1.0.dll.a DESTINATION lib)
    install(FILES ${LIBUSB_DIR}/libusb.h DESTINATION include/core)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES packaging/linux/60-usb-dvbs2.rules DESTINATION share/udev)
endif()
install(DIRECTORY sdk/ DESTINATION include/sdk FILES_MATCHING PATTERN "usb_dvbs2.h")
install(DIRECTORY engine/ DESTINATION include/engine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY core/ DESTINATION include/core FILES_MATCHING PATTERN "*.h")
if(EXISTS "${USB_DVBS2_FIRMWARE_DIR}")
    install(DIRECTORY ${USB_DVBS2_FIRMWARE_DIR}/ DESTINATION bin/firmware
            FILES_MATCHING PATTERN "*.fw")
endif()
install(FILES README.md LICENSE DESTINATION .)
