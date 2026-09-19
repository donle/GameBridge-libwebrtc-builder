cmake_minimum_required(VERSION 3.25)
project(GameBridgeRtcCore LANGUAGES CXX)
enable_testing()
add_executable(rtc_native_core_tests tests/core_tests.cpp)
target_compile_features(rtc_native_core_tests PRIVATE cxx_std_20)
target_include_directories(rtc_native_core_tests PRIVATE src include)
target_link_libraries(rtc_native_core_tests PRIVATE ws2_32)
add_test(NAME rtc_native_core COMMAND rtc_native_core_tests)
add_executable(rtc_connection_diagnostics_tests tests/connection_diagnostics_tests.cpp)
target_compile_features(rtc_connection_diagnostics_tests PRIVATE cxx_std_20)
target_include_directories(rtc_connection_diagnostics_tests PRIVATE ../native/tests include)
add_test(NAME rtc_connection_diagnostics COMMAND rtc_connection_diagnostics_tests)
add_executable(rtc_bench_diagnostics_tests tests/bench_diagnostics_tests.cpp)
target_compile_features(rtc_bench_diagnostics_tests PRIVATE cxx_std_20)
target_include_directories(rtc_bench_diagnostics_tests PRIVATE ../rtcbridge/include)
target_link_libraries(rtc_bench_diagnostics_tests PRIVATE psapi)
add_test(NAME rtc_bench_diagnostics COMMAND rtc_bench_diagnostics_tests)
add_executable(rtc_transport_diagnostics_tests tests/transport_diagnostics_tests.cpp)
target_compile_features(rtc_transport_diagnostics_tests PRIVATE cxx_std_20)
add_test(NAME rtc_transport_diagnostics COMMAND rtc_transport_diagnostics_tests)
set(RTC_WEBRTC_SOURCE "" CACHE PATH "Exact pinned libwebrtc checkout for cold-start regression")
if(RTC_WEBRTC_SOURCE)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pinned_sender_settings.h"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare_sender_test.ps1"
      -SourceRoot "${RTC_WEBRTC_SOURCE}" -Output "${CMAKE_CURRENT_BINARY_DIR}/pinned_sender_settings.h"
    DEPENDS src/bridge.cpp tests/prepare_sender_test.ps1
      "${RTC_WEBRTC_SOURCE}/video/config/encoder_stream_factory.cc"
      "${RTC_WEBRTC_SOURCE}/modules/pacing/pacing_controller.cc"
      "${RTC_WEBRTC_SOURCE}/video/encoder_bitrate_adjuster.cc"
      "${RTC_WEBRTC_SOURCE}/modules/congestion_controller/goog_cc/goog_cc_network_control.cc")
  add_executable(rtc_sender_bitrate_tests tests/sender_bitrate_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/pinned_sender_settings.h")
  target_compile_features(rtc_sender_bitrate_tests PRIVATE cxx_std_20)
  target_include_directories(rtc_sender_bitrate_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME rtc_sender_bitrate COMMAND rtc_sender_bitrate_tests)
  # Compile the real pinned pacer and dependency sources, not a pacing model.
  set(pacer_dependencies
    modules/pacing/pacing_controller.cc modules/pacing/prioritized_packet_queue.cc modules/pacing/bitrate_prober.cc
    modules/rtp_rtcp/source/rtp_packet_to_send.cc modules/rtp_rtcp/source/rtp_video_header.cc
    rtc_base/copy_on_write_buffer.cc rtc_base/checks.cc api/rtp_packet_info.cc
    api/video/video_frame_metadata.cc api/video/color_space.cc rtc_base/ref_counter.cc
    rtc_base/strings/string_builder.cc api/rtp_headers.cc modules/rtp_rtcp/source/rtp_packet.cc
    modules/rtp_rtcp/source/rtp_header_extensions.cc modules/rtp_rtcp/source/rtp_header_extension_map.cc
    api/video/video_timing.cc api/video/video_content_type.cc api/video/hdr_metadata.cc
    api/transport/network_types.cc rtc_base/experiments/field_trial_parser.cc
    rtc_base/experiments/field_trial_units.cc api/units/timestamp.cc
    third_party/abseil-cpp/absl/base/throw_delegate.cc third_party/abseil-cpp/absl/strings/numbers.cc
    third_party/abseil-cpp/absl/strings/ascii.cc third_party/abseil-cpp/absl/strings/charconv.cc
    third_party/abseil-cpp/absl/strings/internal/charconv_bigint.cc
    third_party/abseil-cpp/absl/strings/internal/charconv_parse.cc
    third_party/abseil-cpp/absl/numeric/int128.cc third_party/abseil-cpp/absl/base/internal/raw_logging.cc
    third_party/abseil-cpp/absl/strings/match.cc third_party/abseil-cpp/absl/strings/internal/memutil.cc
    third_party/abseil-cpp/absl/strings/internal/stringify_sink.cc)
  list(TRANSFORM pacer_dependencies PREPEND "${RTC_WEBRTC_SOURCE}/")
  add_executable(rtc_sender_pacer_tests tests/sender_pacer_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/pinned_sender_settings.h" ${pacer_dependencies})
  target_compile_features(rtc_sender_pacer_tests PRIVATE cxx_std_20)
  target_compile_definitions(rtc_sender_pacer_tests PRIVATE NDEBUG WEBRTC_WIN RTC_DISABLE_LOGGING NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(rtc_sender_pacer_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${RTC_WEBRTC_SOURCE}" "${RTC_WEBRTC_SOURCE}/third_party/abseil-cpp")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(rtc_sender_pacer_tests PRIVATE -Wno-nullability-completeness -Wno-format)
  endif()
  add_test(NAME rtc_sender_pacer COMMAND rtc_sender_pacer_tests)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime_network_prefix.h"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare_runtime_test.ps1"
      -SourceRoot "${RTC_WEBRTC_SOURCE}" -Output "${CMAKE_CURRENT_BINARY_DIR}/runtime_network_prefix.h"
    DEPENDS src/bridge.cpp tests/prepare_runtime_test.ps1)
  add_executable(rtc_runtime_socket_tests tests/runtime_socket_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/runtime_network_prefix.h")
  target_compile_features(rtc_runtime_socket_tests PRIVATE cxx_std_20)
  target_compile_definitions(rtc_runtime_socket_tests PRIVATE WEBRTC_WIN)
  target_include_directories(rtc_runtime_socket_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${RTC_WEBRTC_SOURCE}")
  target_link_libraries(rtc_runtime_socket_tests PRIVATE ws2_32)
  add_test(NAME rtc_runtime_socket COMMAND rtc_runtime_socket_tests)
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
