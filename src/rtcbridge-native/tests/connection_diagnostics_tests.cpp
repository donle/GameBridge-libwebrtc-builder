#include "rtc_connection_diagnostics.h"
#include <cstdio>
#include <string>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "diagnostic check failed: %s\n", #value); return 1; } } while (false)
int main() {
  RtcConnectionDiagnostics trace;
  trace.Event(0, GB_RTC_EVENT_DESCRIPTION, "{\"type\":\"offer\",\"sdp\":\"SDP_PRIVATE_SECRET\"}");
  trace.Event(0, GB_RTC_EVENT_CANDIDATE, "{\"candidate\":\"candidate:PRIVATE_ADDRESS\",\"usernameFragment\":\"ICE_PRIVATE_SECRET\"}");
  trace.Event(0, GB_RTC_EVENT_CANDIDATE, "{\"candidate\":\"\"}");
  trace.Event(0, GB_RTC_EVENT_STATE, "{\"state\":3}");
  trace.Event(0, GB_RTC_EVENT_GATHERING, "{\"state\":3}");
  trace.Event(0, GB_RTC_EVENT_ROUTE, "{\"route\":1}");
  trace.Operation(0, RtcConnectionDiagnostics::Remote, GB_RTC_OK);
  trace.Operation(0, RtcConnectionDiagnostics::Candidate, GB_RTC_BACKPRESSURE);
  trace.Operation(0, RtcConnectionDiagnostics::Candidate, GB_RTC_OK);
  auto text = trace.Snapshot(0);
  CHECK(text == "rtc_connect phase=0 peer=0 state=3 gathering=3 route=1 events=6 descriptions=1 candidates=2 candidate_end=1 errors=0 remote_calls=1 candidate_calls=2 answer_calls=0 backpressure=1 rejected=0 last_result=0");
  trace.Event(1, GB_RTC_EVENT_ERROR, "{\"code\":\"CREDENTIAL_PRIVATE_SECRET\"}");
  trace.Event(1, GB_RTC_EVENT_STATE, "{\"state\":\"SDP_PRIVATE_SECRET\"}");
  trace.Operation(1, RtcConnectionDiagnostics::Answer, GB_RTC_INVALID);
  trace.phase = RtcConnectionDiagnostics::FirstVideo;
  text += trace.Snapshot(1);
  CHECK(text.find("phase=1 peer=1 state=0") != std::string::npos);
  CHECK(text.find("errors=1") != std::string::npos);
  CHECK(text.find("answer_calls=1 backpressure=0 rejected=1 last_result=1") != std::string::npos);
  CHECK(text.find("PRIVATE") == std::string::npos);
  CHECK(text.find("SDP") == std::string::npos);
  CHECK(text.find("candidate:") == std::string::npos);
  std::puts("connection diagnostics: deterministic state/operation counters without payload or credentials PASS");
  return 0;
}
