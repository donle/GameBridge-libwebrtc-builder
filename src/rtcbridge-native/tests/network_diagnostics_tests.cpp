#include <cstdio>
#include <sstream>
#include <string>
#if __has_include("../src/network_diagnostics.h")
#include "../src/network_diagnostics.h"
#define HAVE_NETWORK_DIAGNOSTICS 1
#endif

template<class T> bool RecordProofSamples(T& direct) {
  if constexpr (requires { direct.SampleSucceeded(); direct.SampleRetainedRoutineProbe(); direct.SampleUnproven(); }) {
    direct.SampleSucceeded();direct.SampleRetainedRoutineProbe();
    direct.SampleRetainedRoutineProbe();direct.SampleUnproven();
    return true;
  }
  return false;
}

int main() {
#ifndef HAVE_NETWORK_DIAGNOSTICS
  std::fprintf(stderr, "missing frozen numeric producer/direct-proof evidence\n");
  return 1;
#else
  using namespace gamebridge::rtc;
  ProducerDiagnostics producer;
  for (auto source : {ProducerSource::Video, ProducerSource::Audio, ProducerSource::Data}) {
    producer.Record(source, GB_RTC_OK);
    producer.Record(source, GB_RTC_BACKPRESSURE);
    producer.Record(source, GB_RTC_STATE);
    producer.Record(source, GB_RTC_INVALID);
    producer.Record(source, GB_RTC_STATE, true);
    producer.Record(source, GB_RTC_INTERNAL);
    producer.Record(source, -999);
  }
  std::ostringstream json;
  producer.Write(json);
  for (const auto* source : {"video", "audio", "data"}) {
    for (const auto* status : {"ok", "backpressure", "state", "invalid", "closed"})
      if (json.str().find(std::string("\"producer_")+source+"_"+status+"\":1") == std::string::npos) return 2;
    if (json.str().find(std::string("\"producer_")+source+"_other\":2") == std::string::npos ||
        json.str().find(std::string("\"producer_")+source+"_attempts\":7") == std::string::npos) return 3;
  }
  DirectDiagnostics direct;
  if (!RecordProofSamples(direct)) {
    std::fprintf(stderr,"missing retained routine-probe sample counters\n");
    return 7;
  }
  direct.Proven();
  for (auto reason : {ProofEvidence::MissingSelectedPair, ProofEvidence::MissingPairRecord,
                      ProofEvidence::MissingCandidateRecord, ProofEvidence::MissingCandidateType,
                      ProofEvidence::PairNotSucceeded, ProofEvidence::Relay,
                      ProofEvidence::Timeout, ProofEvidence::Other}) {
    direct.Regressed(reason);
    direct.Proven();
  }
  direct.Terminal(ProofEvidence::Timeout);
  direct.Terminal(ProofEvidence::Other);
  direct.Write(json, "sender");
  for (const auto* expected : {"\"native_sender_proof_initial\":1", "\"native_sender_proof_regressions\":8",
       "\"native_sender_proof_recoveries\":8", "\"native_sender_proof_terminal\":1",
       "\"native_sender_proof_transitions\":18", "\"native_sender_terminal_timeout\":1",
       "\"native_sender_terminal_other\":0"})
    if (json.str().find(expected) == std::string::npos) return 4;
  for (const auto* expected : {"\"native_sender_proof_samples\":4",
       "\"native_sender_proof_succeeded_samples\":1", "\"native_sender_proof_retained_routine_probes\":2",
       "\"native_sender_proof_unproven_samples\":1"})
    if (json.str().find(expected) == std::string::npos) return 8;
  for (const auto* reason : {"missing_selected_pair", "missing_pair_record", "missing_candidate_record",
       "missing_candidate_type", "pair_not_succeeded", "relay", "timeout", "other"})
    if (json.str().find(std::string("\"native_sender_regression_")+reason+"\":1") == std::string::npos) return 5;
  if (json.str().find("-999") != std::string::npos) return 6;
  std::puts("Network evidence: frozen numeric producer results, transitions, idempotent terminal and reason partitions PASS");
  return 0;
#endif
}
