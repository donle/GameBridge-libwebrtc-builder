# Numeric network evidence schema 1

This schema is diagnostic only. It does not change route admission, retry behavior,
the public production ABI (ten functions), or performance acceptance thresholds.
The benchmark DLL adds only `gb_rtc_bench_evidence`, sampled after both close
barriers. Production and probe DLLs do not expose it.
Only the native GN consumer enables `GB_RTC_NATIVE_EVIDENCE` and requires this
export. The shared consumer's existing Pion build retains its two-export benchmark
instrumentation contract; no native gate silently falls back to absent evidence.

All values are nonnegative integer counters bounded to the exact JSON integer
range (0 through 9007199254740991). No upstream strings, addresses, identifiers,
SDP, credentials, payloads, or topology are recorded.

- `network_evidence_schema` and `producer_evidence_schema` are exactly 1.
- `producer_{video,audio,data}_{attempts,ok,backpressure,state,invalid,closed,other}`
  count consumer calls during measurement only. The current workload submits
  video only, so audio/data counters remain zero; it does not invent traffic.
  CLOSED subdivides ABI STATE when the caller knows closure; it is not a new ABI
  return value. INTERNAL and unknown results map to OTHER without recording their
  opaque values. Warmup and intentional post-close stale-handle checks are excluded.
- `fatal_producer_errors` separates non-OK/non-BACKPRESSURE producer results from
  `fatal_callback_other_errors`. Their sum must equal `fatal_other_errors`.
- `native_{sender,receiver}_proof_{initial,regressions,recoveries,terminal,transitions}`
  count each peer's lifetime transitions, including initial proof before measurement
  and idempotent terminal closure after measurement. Terminal includes explicit close
  and failure. Initial and terminal are each at most one.
- `native_{sender,receiver}_{regression,terminal}_{reason}` partitions the corresponding
  transition count. Fixed reasons: `missing_selected_pair`, `missing_pair_record`,
  `missing_candidate_record`, `missing_candidate_type`, `pair_not_succeeded`, `relay`,
  `timeout`, `other`. Explicit close and unrelated failures use OTHER. No missing
  value is treated as valid direct proof by this instrumentation.

`Test-NativeRtcEvidence` checks independent result/attempt totals, video/frame
accounting, fatal origins, transition totals and reason partitions. It does not
alter `Test-NativeRtcGate` or any latency, loss, freeze, queue or memory threshold.
Both valid failure reports and successful reports retain the same numeric evidence.
