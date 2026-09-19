#pragma once
#include <algorithm>
#include <optional>
#include <vector>

// Isolated value types only. No substitute stream-limit or pacing algorithm.
namespace w {
struct BitrateSettings {
  std::optional<int> min_bitrate_bps, start_bitrate_bps, max_bitrate_bps;
};
struct RtpEncodingParameters {
  std::optional<int> min_bitrate_bps, max_bitrate_bps;
  std::optional<double> max_framerate;
};
struct RtpTransceiverInit {
  std::vector<RtpEncodingParameters> send_encodings;
};
} // namespace w
struct Layer {
  int min_bitrate_bps = -1, max_bitrate_bps = -1, max_framerate = -1;
};
struct Config {
  int max_bitrate_bps = -1;
  std::vector<Layer> simulcast_layers{1};
};
struct DataRate {
  int bps() const { return 0; }
};
template <class T> T saturated_cast(int value) { return T(value); }
constexpr int kDefaultMinVideoBitrateBps = 30000;
constexpr int kDefaultVideoMaxFramerate = 60;
struct Limits {
  int min, max, fps;
};
#include "pinned_sender_settings.h"
inline Config ActualVideoConfig() {
  Config config;
  const auto init = ActualMediaInit(0);
  if (!init.send_encodings.empty()) {
    const auto &encoding = init.send_encodings.front();
    config.simulcast_layers[0] = {encoding.min_bitrate_bps.value_or(-1),
                                  encoding.max_bitrate_bps.value_or(-1),
                                  int(encoding.max_framerate.value_or(-1))};
  }
  return config;
}
