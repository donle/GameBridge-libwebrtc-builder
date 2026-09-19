// Exercise the real consumer callback, including its noexcept/error paths.
#define main rtc_bench_embedded_main
#include "../../native/bench/rtc_bridge_bench.cpp"
#undef main

namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void Video(Context &context, uint32_t index, uint8_t payload = 7,
           uint32_t timestamp_offset = 0) {
  std::vector<uint8_t> bytes(sizeof(gb_rtc_media_event) + 1);
  gb_rtc_media_event header{16, 1, index * context.step + timestamp_offset, 1};
  std::memcpy(bytes.data(), &header, sizeof(header));
  bytes.back() = payload;
  callback(&context, GB_RTC_EVENT_VIDEO, bytes.data(), uint32_t(bytes.size()));
}
void Error(Context &context, const char *code) {
  const auto json = std::string("{\"code\":\"") + code + "\"}";
  callback(&context, GB_RTC_EVENT_ERROR,
           reinterpret_cast<const uint8_t *>(json.data()),
           uint32_t(json.size()));
}
template <class C> void ReasonsAndPhases() {
  if constexpr (requires(C &c) {
                  c.fatal_reason[0].load();
                  c.fatal_phase[0].load();
                  c.phase.load();
                  c.bridge_error_reason[0].load();
                  c.bridge_error_code_mask.load();
                  c.payload_size_errors.load();
                  c.payload_content_errors.load();
                  c.payload_matches_other_fixture.load();
                }) {
    std::vector<Frame> frames{{{7}, 0, true}, {{8}, 0, false}};
    for (auto &frame : frames)
      frame.digest = hash(frame.data.data(), frame.data.size());
    C c;
    c.frames = &frames;
    c.first = 2;
    c.count = 4;
    c.seen.resize(4);
    const uint8_t invalid[] = {0};
    callback(&c, GB_RTC_EVENT_VIDEO, invalid, sizeof(invalid));
    Check(c.fatal == 1 && c.fatal_reason[0] == 1 && c.fatal_phase[0] == 1,
          "header error must be classified during setup");
    c.phase = 1; // prewarm, still validated but outside measured counters
    Video(c, 0);
    Video(c, 1, 8);
    Check(c.delivered == 0 && c.fatal == 1,
          "valid prewarm must not be counted or rejected");
    Video(c, 0, 7, 1);
    Check(c.fatal_reason[1] == 1 && c.fatal_phase[1] == 1,
          "timestamp error classification");
    c.phase = 2;
    // Keep the existing measurement window behavior: a valid AU outside it
    // is not a delivered measured frame. Range errors refer to ABI byte bounds.
    Video(c, 6);
    Check(c.fatal == 2 && c.delivered == 0,
          "measurement window semantics changed");
    std::array<uint8_t, 17> invalidRange{};
    gb_rtc_media_event invalidHeader{16, 1, 2 * c.step, 2};
    std::memcpy(invalidRange.data(), &invalidHeader, sizeof(invalidHeader));
    callback(&c, GB_RTC_EVENT_VIDEO, invalidRange.data(),
             uint32_t(invalidRange.size()));
    Check(c.fatal_reason[2] == 1,
          "declared payload outside the event byte range must be classified");
    Video(c, 2);
    Video(c, 2);
    Check(c.delivered == 1 && c.fatal_reason[3] == 1,
          "duplicate must remain fatal");
    Video(c, 3); // fixture index 1 is {8}; complete bytes match wrong fixture
                 // index 0
    Check(c.fatal_reason[4] == 1 && c.payload_content_errors == 1 &&
              c.payload_size_errors == 0 &&
              c.payload_matches_other_fixture == 1,
          "payload mismatch must identify content/association without logging "
          "bytes");
    c.pending.resize(256);
    callback(&c, GB_RTC_EVENT_DESCRIPTION, invalid, sizeof(invalid));
    Check(c.fatal_reason[6] == 1,
          "bounded signaling overflow must remain other fatal");
    c.phase = 3;
    Error(c, "connection_failed");
    Check(c.fatal_reason[5] == 1 && c.bridge_error_reason[2] == 1 &&
              c.fatal_phase[3] == 1,
          "drain connection errors must remain fatal");
    c.phase = 4;
    Error(c, "control_channel_closed");
    Error(c, "pointer_channel_closed");
    Error(c, "PRIVATE_SENTINEL");
    Check(c.fatal_reason[5] == 4 && c.bridge_error_reason[0] == 1 &&
              c.bridge_error_reason[1] == 1 && c.bridge_error_reason[3] == 1 &&
              c.fatal_phase[4] == 3 && c.fatal == 10,
          "teardown must classify but not suppress channel/unknown errors");
    uint64_t reasons = 0, phases = 0;
    for (const auto &value : c.fatal_reason)
      reasons += value.load();
    for (const auto &value : c.fatal_phase)
      phases += value.load();
    Check(reasons == c.fatal && phases == c.fatal,
          "fatal partitions must reconcile exactly");
    // The context is marked closed only after the real close callback barrier.
    c.closed = true;
    Error(c, "control_channel_closed");
    Video(c, 4);
    Check(c.stale == 2 && c.fatal == 10,
          "post-close callbacks must remain stale failures");
    C codes;
    const char *known[] = {"control_channel_closed", "pointer_channel_closed",
                           "connection_failed",      "event_oversize",
                           "control_overflow",       "event_overflow",
                           "description_rejected",   "candidate_rejected",
                           "channel_failed",         "media_write_failed"};
    for (const auto *code : known)
      Error(codes, code);
    Check(codes.bridge_error_code_mask == 1023 && codes.fatal == 10,
          "every fixed native error code needs a distinct safe numeric bit");
    Error(codes, "PRIVATE_SENTINEL");
    Check(codes.bridge_error_code_mask == 2047 && codes.fatal == 11,
          "unknown bridge errors must retain only the fixed unknown bit");
  } else {
    Check(
        false,
        "consumer lacks reason-specific and phase-specific fatal diagnostics");
  }
}
} // namespace
int main() {
  try {
    ReasonsAndPhases<Context>();
    std::puts("RTC consumer diagnostics: actual callback reasons, phases, "
              "teardown and stale barrier PASS");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
