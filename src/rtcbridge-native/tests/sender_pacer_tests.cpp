#include "api/field_trials_view.h"
#include "modules/pacing/pacing_controller.h"
#include "sender_bitrate_fixture.h"
#include <array>
#include <cstdio>
#include <memory>

namespace rtc = webrtc;
namespace {
constexpr size_t kFrames = 900;
// Deterministic encoded-size model, not a network or H264 decoder simulation:
// 60 fps, a large keyframe every 120 frames, 25.08 Mbps over each full GOP.
size_t FrameBytes(size_t index) { return index % 120 ? 51000 : 200000; }
class DefaultTrials final : public rtc::FieldTrialsView {
public:
  std::string Lookup(absl::string_view) const override { return {}; }
};
class ModelClock final : public rtc::Clock {
public:
  int64_t us = 1000000;
  rtc::Timestamp CurrentTime() override { return rtc::Timestamp::Micros(us); }
  rtc::NtpTime ConvertTimestampToNtpTime(rtc::Timestamp) override { return {}; }
};
class PacketSink final : public rtc::PacingController::PacketSender {
public:
  std::array<size_t, kFrames> received{};
  void SendPacket(std::unique_ptr<rtc::RtpPacketToSend> packet,
                  const rtc::PacedPacketInfo &) override {
    if (packet->Timestamp() < received.size())
      received[packet->Timestamp()] += packet->payload_size();
  }
  std::vector<std::unique_ptr<rtc::RtpPacketToSend>> FetchFec() override {
    return {};
  }
  std::vector<std::unique_ptr<rtc::RtpPacketToSend>>
  GeneratePadding(rtc::DataSize) override {
    return {};
  }
};
int IncompleteFrames(int stream_bps, double pacing_factor) {
  DefaultTrials trials;
  ModelClock clock;
  PacketSink sink;
  // The actual pinned pacer uses its unmodified queue, keyframe flushing,
  // debt, burst, probing and drain settings. Only clock, source and sink are
  // deterministic. This is NOT a substitute for running GCC or the native gate.
  rtc::PacingController pacer(&clock, &sink, trials);
  pacer.SetPacerConfig(rtc::PacerConfig::Create(
      clock.CurrentTime(),
      rtc::DataRate::BitsPerSec(stream_bps * pacing_factor),
      rtc::DataRate::Zero(), rtc::TimeDelta::Millis(5)));
  const int64_t start = clock.us;
  auto advance = [&](int64_t until) {
    while (clock.us < until) {
      pacer.ProcessPackets();
      clock.us = std::min(until, clock.us + 100);
    }
    pacer.ProcessPackets();
  };
  for (size_t i = 0; i < kFrames; ++i) {
    advance(start + int64_t(i) * 1000000 / 60);
    const size_t bytes = FrameBytes(i);
    for (size_t offset = 0; offset < bytes; offset += 1100) {
      auto packet = std::make_unique<rtc::RtpPacketToSend>(nullptr);
      packet->SetSsrc(7);
      packet->SetTimestamp(uint32_t(i));
      packet->set_packet_type(rtc::RtpPacketMediaType::kVideo);
      packet->set_is_key_frame(i % 120 == 0);
      packet->set_first_packet_of_frame(offset == 0);
      packet->set_last_packet_of_frame(offset + 1100 >= bytes);
      packet->SetPayloadSize(std::min(size_t(1100), bytes - offset));
      pacer.EnqueuePacket(std::move(packet));
    }
  }
  advance(start + 17000000); // Same two-second drain, virtual time only.
  int incomplete = 0;
  for (size_t i = 0; i < kFrames; ++i)
    if (sink.received[i] != FrameBytes(i))
      ++incomplete;
  std::printf("Pinned pacer source=25.08Mbps max=%d factor=%.1f incomplete=%d "
              "queued=%zu\n",
              stream_bps, pacing_factor, incomplete, pacer.QueueSizePackets());
  return incomplete;
}
} // namespace
int main() {
  const auto legacy = PinnedSinglecast(Config{}, 1920, 1080);
  const auto current = PinnedSinglecast(ActualVideoConfig(), 1920, 1080);
  // Model both pinned GoogCC multipliers: post-feedback 1.1 and startup 2.5.
  // Allocated stream rate is an explicit scenario input, not a claimed replay
  // of the hosted packet schedule. A deficient configuration must be detected.
  for (double factor : {1.1, 2.5}) {
    if (IncompleteFrames(legacy.max, factor) == 0 ||
        IncompleteFrames(current.max, factor) != 0) {
      std::fprintf(stderr, "Encoding rate does not sustain the source in the "
                           "actual pinned pacer scenario\n");
      return 1;
    }
  }
  std::puts(
      "Pinned pacer overload witness and configured 900-frame delivery PASS");
}
