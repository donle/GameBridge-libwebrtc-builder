#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#if __has_include("../src/transport_diagnostics.h")
#include "../src/transport_diagnostics.h"
#define HAVE_TRANSPORT_DIAGNOSTICS 1
#endif

int main() {
#ifndef HAVE_TRANSPORT_DIAGNOSTICS
  std::fprintf(stderr,
               "missing fixed-schema transport statistics projection\n");
  return 1;
#else
  using gamebridge::rtc::TransportSnapshot;
  // Value fixtures exercise the numeric projection, not a fake RTC transport.
  struct Outbound {
    std::optional<std::string> kind = "video";
    std::optional<uint64_t> packets_sent = 1200, bytes_sent = 90000,
                            retransmitted_packets_sent = 4;
    std::optional<uint32_t> frames_encoded = 900, frames_sent = 899,
                            nack_count = 3;
    std::optional<double> total_packet_send_delay = 0.25,
                          target_bitrate = 25000000;
  } outbound;
  struct Inbound {
    std::optional<std::string> kind = "video";
    std::optional<uint32_t> packets_received = 1198, frames_received = 0,
                            nack_count = 3;
    std::optional<int32_t> packets_lost = -1;
    std::optional<uint64_t> bytes_received = 80000, packets_discarded = 2;
  } inbound;
  TransportSnapshot snapshot;
  snapshot.ObserveOutbound(outbound);
  snapshot.ObserveInbound(inbound);
  snapshot.AvailableOutgoingBitrate(30000000);
  snapshot.sampled_qpc = 1000;
  outbound.kind = "audio";
  outbound.packets_sent = 9999;
  snapshot.ObserveOutbound(outbound);
  inbound.kind = "PRIVATE_SENTINEL";
  inbound.packets_received = 9999;
  snapshot.ObserveInbound(inbound);
  std::ostringstream json;
  snapshot.Write(json, "native_sender_", 1250, 1000);
  const auto text = json.str();
  for (const auto *required :
       {"\"native_sender_outbound_packets_sent\":1200",
        "\"native_sender_inbound_packets_lost\":-1",
        "\"native_sender_available_outgoing_bitrate_bps\":3e+07",
        "\"native_sender_stats_age_ms\":250"}) {
    if (text.find(required) == std::string::npos) {
      std::fprintf(stderr, "missing safe numeric projection: %s\n", required);
      return 1;
    }
  }
  if (text.find("PRIVATE_SENTINEL") != std::string::npos ||
      text.find("9999") != std::string::npos) {
    return 1;
  }
  TransportSnapshot absent;
  std::ostringstream empty;
  absent.Write(empty, "native_receiver_", 1250, 1000);
  if (!empty.str().empty()) {
    std::fprintf(stderr, "absent stats invented zero samples\n");
    return 1;
  }
  outbound.kind = "video";
  outbound.target_bitrate = std::numeric_limits<double>::infinity();
  absent.ObserveOutbound(outbound);
  absent.sampled_qpc = 1000;
  std::ostringstream finite;
  absent.Write(finite, "native_receiver_", 1250, 1000);
  if (finite.str().find("target_bitrate") != std::string::npos) {
    return 1;
  }
  std::puts("Transport diagnostics: fixed numeric projection, absent stats, "
            "audio/opaque filtering, finite numbers PASS");
  return 0;
#endif
}
