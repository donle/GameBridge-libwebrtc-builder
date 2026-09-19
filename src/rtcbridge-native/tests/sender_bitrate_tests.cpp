#include "sender_bitrate_fixture.h"
#include <cstdio>
int main() {
  const auto legacy = PinnedSinglecast(Config{}, 1920, 1080);
  if (legacy.max != 2500000) {
    std::fprintf(stderr, "Pinned singlecast default changed\n");
    return 1;
  }
  const auto current = PinnedSinglecast(ActualVideoConfig(), 1920, 1080);
  std::printf("Pinned singlecast: legacy=%d configured=%d min=%d fps=%d\n",
              legacy.max, current.max, current.min, current.fps);
  if (current.max != 25000000 || current.min != 1000000 || current.fps != 60 ||
      ActualMediaInit(0).send_encodings.size() != 1 ||
      !ActualMediaInit(1).send_encodings.empty()) {
    std::fprintf(stderr, "Video encoding must explicitly admit 25 Mbps/60 fps "
                         "while retaining low-rate backoff\n");
    return 1;
  }
  const auto transport = ActualTransportSettings();
  if (transport.min_bitrate_bps != 1000000 ||
      transport.start_bitrate_bps != 25000000 ||
      transport.max_bitrate_bps != 40000000) {
    std::fprintf(stderr, "Transport startup must match 25 Mbps source with "
                         "congestion backoff and overhead headroom\n");
    return 1;
  }
  auto low = ActualVideoConfig();
  low.max_bitrate_bps = 2000000;
  const auto constrained = PinnedSinglecast(low, 1920, 1080);
  if (constrained.max != 2000000 || constrained.min > constrained.max) {
    std::fprintf(stderr, "Lower negotiated bandwidth must remain effective\n");
    return 1;
  }
  std::puts("Actual bridge encoding/transport settings and pinned singlecast "
            "limits PASS");
}
