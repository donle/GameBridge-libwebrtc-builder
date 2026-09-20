#include "gamebridge/audio/audio_clock.h"
#include <limits>

namespace gamebridge::audio {
AudioClock::AudioClock(int64_t epoch_qpc,int64_t qpc_frequency) noexcept
    :epoch_qpc_(epoch_qpc),frequency_(qpc_frequency){}
AudioClockStamp AudioClock::Map(int64_t capture_qpc,bool discontinuity,bool timestamp_error) const noexcept {
  if(timestamp_error||frequency_<=0||capture_qpc<epoch_qpc_)return{};
  const auto elapsed=static_cast<uint64_t>(capture_qpc-epoch_qpc_);
  const auto whole=elapsed/static_cast<uint64_t>(frequency_);
  const auto remainder=elapsed%static_cast<uint64_t>(frequency_);
  constexpr uint64_t audio_rate=48000;
  const auto timestamp=whole*audio_rate+(remainder*audio_rate)/static_cast<uint64_t>(frequency_);
  return {true,discontinuity,static_cast<uint32_t>(timestamp)};
}
}
