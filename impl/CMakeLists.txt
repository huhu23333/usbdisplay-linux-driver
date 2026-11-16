cmake_minimum_required(VERSION 3.16)
project(usbdisplay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(ALSA IMPORTED_TARGET alsa)

set(USBDISPLAY_SOURCES
  src/common/log.cpp
  src/common/util.cpp
  src/common/json.cpp
  src/proto/nal_scan.cpp
  src/proto/wire.cpp
  src/proto/packetizer.cpp
  src/proto/annexb.cpp
  src/proto/wav.cpp
  src/proto/h264_sps.cpp
  src/transport/tcp.cpp
  src/transport/udp.cpp
  src/transport/loopback.cpp
  src/transport/hidraw.cpp
  src/transport/libusb_probe.cpp
  src/transport/usb_video.cpp
  src/transport/usb_reset.cpp
  src/transport/retry.cpp
  src/transport/netinfo.cpp
  src/hid/report_dump.cpp
  src/hid/report_sink.cpp
  src/audio/alsa_player.cpp
  src/control/jsonrpc.cpp
  src/control/hid_channel.cpp
  src/control/hid_report_channel.cpp
  src/control/null_channel.cpp
  src/controller.cpp
  src/version.cpp
)

add_library(usbdisplay STATIC ${USBDISPLAY_SOURCES})
target_include_directories(usbdisplay
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(usbdisplay
  PUBLIC Threads::Threads
  PRIVATE PkgConfig::LIBUSB
)
if(ALSA_FOUND)
  target_link_libraries(usbdisplay PRIVATE PkgConfig::ALSA)
  target_compile_definitions(usbdisplay PRIVATE USBDISPLAY_HAVE_ALSA=1)
  message(STATUS "ALSA found: USB audio playback enabled")
else()
  message(STATUS "ALSA not found: USB audio playback disabled")
endif()
set_target_properties(usbdisplay PROPERTIES OUTPUT_NAME usbdisplay)

# ---------------- tools ----------------
add_executable(usbdisplay_diag tools/usbdisplay_diag.cpp)
target_include_directories(usbdisplay_diag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(usbdisplay_diag PRIVATE usbdisplay PkgConfig::LIBUSB)

add_executable(usbdisplay_send tools/usbdisplay_send.cpp)
target_include_directories(usbdisplay_send PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(usbdisplay_send PRIVATE usbdisplay)

add_executable(usbdisplay_json tools/usbdisplay_json.cpp)
target_include_directories(usbdisplay_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(usbdisplay_json PRIVATE usbdisplay)

add_executable(usbdisplay_audio tools/usbdisplay_audio.cpp)
target_include_directories(usbdisplay_audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(usbdisplay_audio PRIVATE usbdisplay)

add_executable(usbdisplay_probe tools/usbdisplay_probe.cpp)
target_include_directories(usbdisplay_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(usbdisplay_probe PRIVATE usbdisplay)

add_executable(fake_device tools/fake_device.cpp)
target_include_directories(fake_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fake_device PRIVATE usbdisplay)

# ---------------- tests ----------------
enable_testing()

foreach(t test_json test_util test_annexb test_wire test_packetizer test_wav test_hid test_usb_video test_h264_sps test_auto_reset)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE usbdisplay)
  target_include_directories(${t} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Cross-process loop test: fake_device <-> usbdisplay_send (TCP + UDP framing).
find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
  add_test(NAME integration_loop_tcp
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_loop.sh
            ${CMAKE_BINARY_DIR} tcp)
  add_test(NAME integration_loop_udp
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_loop.sh
            ${CMAKE_BINARY_DIR} udp)
  add_test(NAME integration_loop_rtp
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_loop.sh
            ${CMAKE_BINARY_DIR} rtp)
  add_test(NAME integration_loop_hid
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_loop.sh
            ${CMAKE_BINARY_DIR} hid)
endif()

# ---------------- install ----------------
include(GNUInstallDirs)
install(TARGETS usbdisplay usbdisplay_diag usbdisplay_send usbdisplay_probe usbdisplay_json usbdisplay_audio fake_device
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# One-time udev rule (kept in the repository tools/ directory; this project
# does not install it into /etc automatically).
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../tools/99-usbdisplay.rules
        DESTINATION ${CMAKE_INSTALL_DATADIR}/usbdisplay OPTIONAL)
