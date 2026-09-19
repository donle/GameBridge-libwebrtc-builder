#pragma once
#include "gamebridge_rtc.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// Test-only diagnostics: never retain or format an SDP/candidate/error payload.
// Only fixed enum scalars and integer counters may reach diagnostic output.
struct RtcConnectionDiagnostics {
  enum Phase { Connecting, FirstVideo, MediaData, Closing, Complete };
  enum Call { Remote, Candidate, Answer };
  Phase phase = Connecting;
  struct Peer {
    unsigned state{}, gathering{}, route{};
    uint64_t events{}, descriptions{}, candidates{}, candidate_end{}, errors{};
    std::array<uint64_t, 3> calls{};
    uint64_t backpressure{}, rejected{};
    int32_t last_result{};
  };
  std::array<Peer, 2> peers{};
  static unsigned Scalar(std::string_view text, const char* key, unsigned maximum) {
    for (unsigned value = 1; value <= maximum; ++value)
      if (text == "{\"" + std::string(key) + "\":" + std::to_string(value) + "}")
        return value;
    return 0;
  }
  void Event(unsigned source, uint32_t type, std::string_view text) {
    auto& peer = peers.at(source);
    ++peer.events;
    if (type == GB_RTC_EVENT_STATE) peer.state = Scalar(text, "state", 6);
    if (type == GB_RTC_EVENT_GATHERING) peer.gathering = Scalar(text, "state", 3);
    if (type == GB_RTC_EVENT_ROUTE) peer.route = Scalar(text, "route", 2);
    if (type == GB_RTC_EVENT_DESCRIPTION) ++peer.descriptions;
    if (type == GB_RTC_EVENT_ERROR) ++peer.errors;
    if (type == GB_RTC_EVENT_CANDIDATE) {
      ++peer.candidates;
      if (text.find("\"candidate\":\"\"") != text.npos) ++peer.candidate_end;
    }
  }
  void Operation(unsigned destination, Call call, gb_rtc_result result) {
    auto& peer = peers.at(destination);
    ++peer.calls.at(call);
    peer.last_result = result;
    if (result == GB_RTC_BACKPRESSURE) ++peer.backpressure;
    else if (result != GB_RTC_OK) ++peer.rejected;
  }
  std::string Snapshot(unsigned source) const {
    const auto& p = peers.at(source);
    return "rtc_connect phase=" + std::to_string(phase) + " peer=" + std::to_string(source) +
      " state=" + std::to_string(p.state) + " gathering=" + std::to_string(p.gathering) +
      " route=" + std::to_string(p.route) + " events=" + std::to_string(p.events) +
      " descriptions=" + std::to_string(p.descriptions) + " candidates=" + std::to_string(p.candidates) +
      " candidate_end=" + std::to_string(p.candidate_end) + " errors=" + std::to_string(p.errors) +
      " remote_calls=" + std::to_string(p.calls[Remote]) + " candidate_calls=" + std::to_string(p.calls[Candidate]) +
      " answer_calls=" + std::to_string(p.calls[Answer]) + " backpressure=" + std::to_string(p.backpressure) +
      " rejected=" + std::to_string(p.rejected) + " last_result=" + std::to_string(p.last_result);
  }
  void Print(unsigned source) const {
    const auto line = Snapshot(source);
    std::fprintf(stderr, "%s\n", line.c_str());
  }
};
