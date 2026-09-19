#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

namespace gamebridge::rtc {
// Benchmark-only projection from the existing periodic RTCStats report. No
// addresses, IDs, SDP, codec/implementation strings, or opaque values survive.
struct TransportSnapshot {
  int64_t sampled_qpc{};
  template <class T> void ObserveOutbound(const T &s) {
    if (!s.kind || *s.kind != "video")
      return;
    Set(0, s.packets_sent);
    Set(1, s.bytes_sent);
    Set(2, s.frames_encoded);
    Set(3, s.frames_sent);
    Set(4, s.retransmitted_packets_sent);
    Set(5, s.nack_count);
    Set(6, s.total_packet_send_delay);
    Set(7, s.target_bitrate);
  }
  template <class T> void ObserveInbound(const T &s) {
    if (!s.kind || *s.kind != "video")
      return;
    Set(8, s.packets_received);
    Set(9, s.bytes_received);
    Set(10, s.packets_lost);
    Set(11, s.packets_discarded);
    Set(12, s.frames_received);
    Set(13, s.nack_count);
  }
  void AvailableOutgoingBitrate(std::optional<double> value) { Set(14, value); }
  // Emits JSON members only; prefixes are fixed caller literals, never user
  // data.
  void Write(std::ostream &json, std::string_view prefix, int64_t now,
             int64_t frequency) const {
    if (sampled_qpc <= 0 || now < sampled_qpc || frequency <= 0)
      return;
    constexpr std::string_view names[] = {
        "outbound_packets_sent",
        "outbound_bytes_sent",
        "outbound_frames_encoded",
        "outbound_frames_sent",
        "outbound_retransmitted_packets_sent",
        "outbound_nack_count",
        "outbound_total_packet_send_delay_seconds",
        "outbound_target_bitrate_bps",
        "inbound_packets_received",
        "inbound_bytes_received",
        "inbound_packets_lost",
        "inbound_packets_discarded",
        "inbound_frames_received",
        "inbound_nack_count",
        "available_outgoing_bitrate_bps"};
    for (size_t i = 0; i < values_.size(); ++i)
      if (values_[i])
        json << ",\"" << prefix << names[i] << "\":" << *values_[i];
    json << ",\"" << prefix
         << "stats_age_ms\":" << double(now - sampled_qpc) * 1000 / frequency;
  }

private:
  template <class T> void Set(size_t index, std::optional<T> value) {
    values_[index].reset();
    if (value && std::isfinite(double(*value)))
      values_[index] = double(*value);
  }
  std::array<std::optional<double>, 15> values_{};
};
} // namespace gamebridge::rtc
