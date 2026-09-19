#pragma once
#include <cstdint>
#include <vector>

// Synthetic black 16x16 H.264 (FFmpeg color, x264 baseline/zerolatency, one
// frame, SEI removed). Full SPS/PPS/slice headers are required by libwebrtc's
// H.264 depacketizer; the previous four-byte SPS was truncated. A valid 4 KiB
// unregistered-user-data SEI makes this AU exercise RTP fragmentation too.
inline std::vector<uint8_t> RtcMediaFixture() {
  std::vector<uint8_t> bytes{
      0, 0, 0,    1,    0x67, 0x42, 0xc0, 0x1f, 0xdd, 0xec, 0x04, 0x40, 0,
      0, 3, 0,    0x40, 0,    0,    0x1e, 0x23, 0xc6, 0x0c, 0xe0, 0,    0,
      0, 1, 0x68, 0xce, 0x0f, 0xc8, 0,    0,    0,    1,    0x06, 0x05};
  bytes.insert(bytes.end(), 16, 0xff);
  bytes.push_back(0x10); // payload size 4096
  bytes.insert(bytes.end(), 4096, 0x55);
  bytes.push_back(0x80);
  const uint8_t idr[]{0,    0,    0,    1, 0x65, 0x88, 0x84,
                      0x3a, 0x26, 0x28, 0, 9,    2,    0xe0};
  bytes.insert(bytes.end(), std::begin(idr), std::end(idr));
  return bytes;
}
