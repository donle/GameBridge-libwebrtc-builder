#pragma once
#include <cstdint>

namespace gamebridge::audio {
struct AudioClockStamp {
  bool accepted{};
  bool discontinuity{};
  uint32_t timestamp48k{};
};

class AudioClock {
 public:
  AudioClock(int64_t epoch_qpc,int64_t qpc_frequency) noexcept;
  AudioClockStamp Map(int64_t capture_qpc,bool discontinuity,bool timestamp_error) const noexcept;
 private:
  int64_t epoch_qpc_{};
  int64_t frequency_{};
};
}
