#pragma once
#include "gamebridge/audio/audio_clock.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gamebridge::audio {
inline constexpr uint32_t kAudioSampleRate=48000;
inline constexpr uint32_t kAudioChannels=2;
inline constexpr uint32_t kOpusPacketFrames=960;
inline constexpr uint32_t kOpusBitrate=128000;

struct EncodedAudio {
  std::vector<uint8_t> bytes;
  uint32_t timestamp48k{};
  uint32_t source_frames{};
};
struct DecodedAudio {
  std::vector<float> interleaved;
  uint32_t frames{};
  uint32_t timestamp48k{};
  bool concealed{};
};

class OpusPacketizer {
 public:
  OpusPacketizer(int64_t epoch_qpc,int64_t qpc_frequency);
  ~OpusPacketizer();
  OpusPacketizer(const OpusPacketizer&)=delete;
  OpusPacketizer& operator=(const OpusPacketizer&)=delete;
  std::vector<EncodedAudio> Push(std::span<const float> interleaved,uint32_t frames,
      int64_t capture_qpc,bool discontinuity,bool timestamp_error);
  void Reset() noexcept;
 private:
  void* encoder_{};
  AudioClock clock_;
  std::vector<float> pending_;
  uint32_t pending_timestamp_{};
  bool have_timestamp_{};
};

class OpusDecoder {
 public:
  OpusDecoder();
  ~OpusDecoder();
  OpusDecoder(const OpusDecoder&)=delete;
  OpusDecoder& operator=(const OpusDecoder&)=delete;
  void Reset() noexcept;
  std::optional<DecodedAudio> Decode(std::span<const uint8_t> packet,uint32_t timestamp48k,bool conceal=false);
 private:
  void* decoder_{};
};
}
