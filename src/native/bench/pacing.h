#pragma once
#include <algorithm>
#include <cstdint>
namespace gamebridge::bench {
// A 60 Hz fixture remains scheduled against its original clock. If Windows
// wakes the source late, recover with at least 2 ms between submissions rather
// than manufacturing a burst of stale frames in consecutive ABI calls. No
// transport queue is inspected, enlarged or waited upon; duration/count stay real.
inline int64_t SubmissionDeadline(int64_t scheduled,int64_t previous,int64_t frequency) {
  return previous ? std::max(scheduled,previous+frequency/500) : scheduled;
}
}
