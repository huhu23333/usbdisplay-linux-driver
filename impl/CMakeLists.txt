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
pkg_check_modules(PULSE IMPORTED_TARGET libpulse)
option(USBDISPLAY_BUILD_DAEMON "Build the evdi virtual display daemon" ON)
pkg_check_modules(X264 IMPORTED_TARGET x264)
pkg_check_modules(SWSCALE IMPORTED_TARGET libswscale)
find_library(EVDI_LIBRARY NAMES evdi libevdi.so.1 libevdi.so.0
  PATHS /lib /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib64
        /usr/lib/aarch64-linux-gnu)
if(NOT EVDI_LIBRARY)
  # 部分发行版只安装带 SONAME 的运行时库，没有 libevdi.so 开发符号链接。
  find_file(EVDI_LIBRARY NAMES libevdi.so.1 libevdi.so.0
    PATHS /lib /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib64
          /usr/lib/aarch64-linux-gnu)
endif()

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
  src/audio/audio_pump.cpp
  src/audio/audio_backend.cpp
  src/audio/audio_open_retry.cpp
  src/audio/default_sink_guard.cpp
  src/audio/pulse_card.cpp
  src/audio/pulse_discovery.cpp
  src/audio/pulse_player.cpp
  src/media_plan.cpp
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
if(PULSE_FOUND)
  target_link_libraries(usbdisplay PRIVATE PkgConfig::PULSE)
  target_compile_definitions(usbdisplay PRIVATE USBDISPLAY_HAVE_PULSE=1)
  message(STATUS "PulseAudio found: USB sink playback enabled")
else()
  message(STATUS "PulseAudio not found: PulseAudio playback disabled")
endif()
set_target_properties(usbdisplay PROPERTIES OUTPUT_NAME usbdisplay)

# evdi 公共模块：EDID、marker、USB 状态机与 x264 输出聚合，均不依赖真实设备。
add_library(usbdisplay_evdi_common STATIC
  src/evdi/edid.cpp
  src/evdi/marker.cpp
  src/evdi/state.cpp
  src/evdi/h264_au.cpp
  src/evdi/shared_frame.cpp
  src/evdi/frame_lease_protocol.cpp
  src/evdi/frame_lease_server.cpp
  src/evdi/frame_lease_client.cpp
  src/evdi/control_server.cpp
)
target_include_directories(usbdisplay_evdi_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 工具程序
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

# 测试程序
enable_testing()

foreach(t test_json test_util test_annexb test_wire test_packetizer test_wav test_hid test_usb_video test_h264_sps test_auto_reset test_audio_pump test_audio_backend test_default_sink_guard test_pulse_card
    test_audio_retry test_media_plan test_pulse_player test_evdi_edid
    test_evdi_marker test_daemon_state test_h264_au test_shared_frame
    test_frame_lease test_control_server test_send_cadence test_evdi_capture)
  add_executable(${t} tests/${t}.cpp)
  if(t STREQUAL "test_evdi_edid" OR t STREQUAL "test_evdi_marker" OR
     t STREQUAL "test_daemon_state" OR t STREQUAL "test_h264_au" OR
     t STREQUAL "test_shared_frame" OR t STREQUAL "test_frame_lease" OR
     t STREQUAL "test_control_server")
    target_link_libraries(${t} PRIVATE usbdisplay_evdi_common)
  elseif(t STREQUAL "test_evdi_capture")
    target_sources(${t} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/evdi/evdi_capture.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/evdi/evdi_driver.cpp)
    target_include_directories(${t} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/evdi)
    target_link_libraries(${t} PRIVATE
      usbdisplay usbdisplay_evdi_common PkgConfig::LIBUSB ${EVDI_LIBRARY})
  else()
    target_link_libraries(${t} PRIVATE usbdisplay)
  endif()
  target_include_directories(${t} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# evdi daemon 依赖系统 libevdi、libx264 与 libswscale；任一缺失可通过
# -DUSBDISPLAY_BUILD_DAEMON=OFF 关闭。
if(USBDISPLAY_BUILD_DAEMON)
  if(EVDI_LIBRARY)
    add_executable(usbdisplay_evdi_grabber
      tools/usbdisplay_evdi_grabber.cpp
      src/evdi/evdi_capture.cpp
      src/evdi/evdi_driver.cpp)
    target_include_directories(usbdisplay_evdi_grabber PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/evdi)
    target_link_libraries(usbdisplay_evdi_grabber PRIVATE
      usbdisplay
      usbdisplay_evdi_common
      ${EVDI_LIBRARY})
    message(STATUS "evdi grabber enabled: libevdi=${EVDI_LIBRARY}")
  else()
    message(STATUS "evdi grabber disabled: missing libevdi")
  endif()

  add_executable(usbdisplay_evdi_ctl tools/usbdisplay_evdi_ctl.cpp)
  target_include_directories(usbdisplay_evdi_ctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  if(X264_FOUND AND SWSCALE_FOUND)
    add_executable(usbdisplay_daemon
      tools/usbdisplay_daemon.cpp
      src/evdi/x264_encoder.cpp)
    target_include_directories(usbdisplay_daemon PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(usbdisplay_daemon PRIVATE
      usbdisplay
      usbdisplay_evdi_common
      PkgConfig::LIBUSB
      PkgConfig::X264
      PkgConfig::SWSCALE)
    message(STATUS "consume daemon enabled: shared frame + libx264")
  else()
    message(STATUS "consume daemon disabled: missing libx264 or libswscale")
  endif()
else()
  message(STATUS "evdi grabber/daemon disabled by USBDISPLAY_BUILD_DAEMON=OFF")
endif()

# 跨进程测试：模拟接收器校验发送端生成的报文。
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

# 安装规则
include(GNUInstallDirs)
install(TARGETS usbdisplay usbdisplay_diag usbdisplay_send usbdisplay_probe usbdisplay_json usbdisplay_audio fake_device
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# udev 规则文件位于仓库 tools 目录，本工程不会自动写入 /etc。
if(TARGET usbdisplay_evdi_grabber)
  install(TARGETS usbdisplay_evdi_grabber RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET usbdisplay_daemon)
  install(TARGETS usbdisplay_daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../tools/99-usbdisplay-usb.rules
              ${CMAKE_CURRENT_SOURCE_DIR}/../tools/99-usbdisplay-audio-ignore.rules
        DESTINATION ${CMAKE_INSTALL_DATADIR}/usbdisplay OPTIONAL)
