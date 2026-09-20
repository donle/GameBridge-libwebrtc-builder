#pragma once
#include "gamebridge_rtc.h"
#include <span>

namespace gamebridge::rtc {
// libwebrtc removes RTP padding before invoking the audio transformer. A
// padding-only audio packet is therefore an empty payload, not encoded media.
// Match ChannelReceive::OnReceivedPayloadData without relaxing size checks on
// any nonempty payload or accepting an empty video access unit.
template <class Accept, class Reject>
void ReceiveMediaPayload(bool video, std::span<const uint8_t> bytes,
                         Accept accept, Reject reject) {
  if (!video && bytes.empty())
    return;
  if (bytes.empty() || bytes.size() >
                           (video ? GB_RTC_MAX_VIDEO_BYTES
                                  : GB_RTC_MAX_AUDIO_BYTES)) {
    reject();
    return;
  }
  accept(bytes);
}
} // namespace gamebridge::rtc
