#pragma once
// Network schema 2 / producer schema 1. Fixed numeric counters only; never store transport IDs,
// addresses, SDP, payloads, credentials, or upstream strings.
#include "gamebridge_rtc.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <ostream>

namespace gamebridge::rtc {
enum class ProducerSource : unsigned { Video, Audio, Data };
enum class ProofEvidence : unsigned {
  MissingSelectedPair, MissingPairRecord, MissingCandidateRecord,
  MissingCandidateType, PairNotSucceeded, Relay, Timeout, Other
};
inline constexpr const char* ProofEvidenceNames[] = {
  "missing_selected_pair", "missing_pair_record", "missing_candidate_record",
  "missing_candidate_type", "pair_not_succeeded", "relay", "timeout", "other"
};

// Benchmark producer thread only; counts the measurement window, not warmup.
// CLOSED is a diagnostic subdivision of ABI STATE when closure is known.
// It is not a new public ABI result code. INTERNAL/unknown codes map to other.
struct ProducerDiagnostics {
  std::array<std::array<uint64_t, 6>, 3> results{};
  std::array<uint64_t, 3> attempts{};
  void Record(ProducerSource source, gb_rtc_result result, bool closed=false) {
    const auto index=static_cast<unsigned>(source);
    if(index>=results.size())return;
    const unsigned status=result==GB_RTC_OK?0:result==GB_RTC_BACKPRESSURE?1:
      result==GB_RTC_STATE?(closed?4:2):result==GB_RTC_INVALID?3:5;
    ++attempts[index];++results[index][status];
  }
  void Write(std::ostream& out) const {
    constexpr const char* sources[]={"video","audio","data"};
    constexpr const char* statuses[]={"ok","backpressure","state","invalid","closed","other"};
    out<<",\"producer_evidence_schema\":1";
    for(unsigned source=0;source<3;++source){
      out<<",\"producer_"<<sources[source]<<"_attempts\":"<<attempts[source];
      for(unsigned status=0;status<6;++status)
        out<<",\"producer_"<<sources[source]<<'_'<<statuses[status]<<"\":"<<results[source][status];
    }
  }
};

// Only the benchmark DLL owns these counters. Snapshot after close barriers.
struct DirectDiagnostics {
  std::atomic<uint64_t> initial{},regressions{},recoveries{},terminal{},transitions{};
  std::atomic<uint64_t> samples{},succeeded_samples{},retained_routine_probes{},unproven_samples{};
  std::array<std::atomic<uint64_t>,8> regression_reason{},terminal_reason{};
  std::atomic<bool> ever_proven{},terminal_seen{};
  static unsigned Index(ProofEvidence reason){const auto n=static_cast<unsigned>(reason);return n<8?n:7;}
  void Proven(){if(ever_proven.exchange(true))++recoveries;else ++initial;++transitions;}
  void Regressed(ProofEvidence reason){++regressions;++regression_reason[Index(reason)];++transitions;}
  void Terminal(ProofEvidence reason){if(terminal_seen.exchange(true))return;++terminal;++terminal_reason[Index(reason)];++transitions;}
  void SampleSucceeded(){++samples;++succeeded_samples;}
  void SampleRetainedRoutineProbe(){++samples;++retained_routine_probes;}
  void SampleUnproven(){++samples;++unproven_samples;}
  void Write(std::ostream& out,const char* role)const{
    const auto field=[&](const char* name,uint64_t value){out<<",\"native_"<<role<<'_'<<name<<"\":"<<value;};
    field("proof_initial",initial.load());field("proof_regressions",regressions.load());
    field("proof_recoveries",recoveries.load());field("proof_terminal",terminal.load());field("proof_transitions",transitions.load());
    field("proof_samples",samples.load());field("proof_succeeded_samples",succeeded_samples.load());
    field("proof_retained_routine_probes",retained_routine_probes.load());field("proof_unproven_samples",unproven_samples.load());
    for(unsigned i=0;i<8;++i){
      out<<",\"native_"<<role<<"_regression_"<<ProofEvidenceNames[i]<<"\":"<<regression_reason[i].load();
      out<<",\"native_"<<role<<"_terminal_"<<ProofEvidenceNames[i]<<"\":"<<terminal_reason[i].load();
    }
  }
};
}
