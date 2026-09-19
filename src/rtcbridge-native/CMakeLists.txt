cmake_minimum_required(VERSION 3.25)
project(GameBridgeRtcCore LANGUAGES CXX)
enable_testing()
add_executable(rtc_native_core_tests tests/core_tests.cpp)
target_compile_features(rtc_native_core_tests PRIVATE cxx_std_20)
target_include_directories(rtc_native_core_tests PRIVATE src include)
target_link_libraries(rtc_native_core_tests PRIVATE ws2_32)
add_test(NAME rtc_native_core COMMAND rtc_native_core_tests)
set(RTC_WEBRTC_SOURCE "" CACHE PATH "Exact pinned libwebrtc checkout for cold-start regression")
if(RTC_WEBRTC_SOURCE)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pinned_injector_methods.h"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare_injector_test.ps1"
      -SourceRoot "${RTC_WEBRTC_SOURCE}" -Output "${CMAKE_CURRENT_BINARY_DIR}/pinned_injector_methods.h"
    DEPENDS "${RTC_WEBRTC_SOURCE}/pc/encoded_video_frame_injector.cc" tests/prepare_injector_test.ps1)
  add_executable(rtc_injector_cold_start_tests tests/injector_cold_start_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/pinned_injector_methods.h")
  target_compile_features(rtc_injector_cold_start_tests PRIVATE cxx_std_20)
  target_include_directories(rtc_injector_cold_start_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME rtc_injector_cold_start COMMAND rtc_injector_cold_start_tests)
endif()
