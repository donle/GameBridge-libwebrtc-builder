// The two injector method definitions below are extracted verbatim from the
// pinned upstream source at build time. Only the engine/thread dependencies
// are replaced here; this regression runs the real buffer/register algorithm.
// The full DLL's one-frame-only loopback is additionally required by ABI CI.
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#define RTC_DCHECK_RUN_ON(sequence) ((void)0)
#define RTC_CHECK(condition)                                                   \
  do {                                                                         \
    if (!(condition))                                                          \
      std::abort();                                                            \
  } while (false)

namespace webrtc {
struct TransformableVideoFrameInterface {
  struct Meta {
    uint16_t GetWidth() const { return 16; }
    uint16_t GetHeight() const { return 16; }
  };
  Meta Metadata() const { return {}; }
  uint32_t timestamp;
  std::vector<uint8_t> bytes;
};
struct ProxyVideoTrack {
  unsigned black_frames{};
  void InjectBlackFrame(uint16_t, uint16_t) { ++black_frames; }
};
struct ProxyVideoEncoder {
  std::vector<std::unique_ptr<TransformableVideoFrameInterface>> delivered;
  void
  InjectEncodedFrame(std::unique_ptr<TransformableVideoFrameInterface> frame) {
    delivered.push_back(std::move(frame));
  }
};
struct MutexLock : std::lock_guard<std::mutex> {
  explicit MutexLock(std::mutex *mutex) : std::lock_guard<std::mutex>(*mutex) {}
};
struct EncodedVideoFrameInjector {
  void InjectFrame(std::unique_ptr<TransformableVideoFrameInterface> frame);
  void RegisterEncoder(ProxyVideoEncoder *encoder);
  std::unique_ptr<ProxyVideoTrack> video_track_ =
      std::make_unique<ProxyVideoTrack>();
  std::mutex encoder_lock_;
  ProxyVideoEncoder *encoder_{};
  std::deque<std::unique_ptr<TransformableVideoFrameInterface>>
      buffered_frames_;
  static constexpr size_t kMaxBufferedFrames = 20;
};
#include "pinned_injector_methods.h"
} // namespace webrtc

int main() {
  webrtc::EncodedVideoFrameInjector injector;
  webrtc::ProxyVideoEncoder encoder;
  const std::vector<uint8_t> payload{0, 0, 0, 1, 0x65, 7};
  injector.InjectFrame(
      std::make_unique<webrtc::TransformableVideoFrameInterface>(
          webrtc::TransformableVideoFrameInterface{0xfffffff0, payload}));
  RTC_CHECK(injector.video_track_->black_frames == 1);
  RTC_CHECK(injector.buffered_frames_.size() == 1);
  // Encoder readiness is an independent asynchronous event. No second
  // InjectFrame call, timer, duplicate, or caller submission can unblock this.
  injector.RegisterEncoder(&encoder);
  if (encoder.delivered.size() != 1 || !injector.buffered_frames_.empty()) {
    std::cerr << "cold-start frame stalled at encoder readiness\n";
    return 1;
  }
  RTC_CHECK(encoder.delivered[0]->timestamp == 0xfffffff0);
  RTC_CHECK(encoder.delivered[0]->bytes == payload);
  injector.RegisterEncoder(&encoder);
  RTC_CHECK(encoder.delivered.size() == 1); // no replay on re-registration
  std::cout
      << "pinned injector: one-frame cold start and timestamp ownership PASS\n";
}
