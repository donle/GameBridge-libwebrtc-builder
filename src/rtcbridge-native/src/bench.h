// Included only by the benchmark DLL. Production has no measurement exports.
#include "histogram.h"
#include <sstream>

namespace gamebridge::rtc {
struct Benchmark {
  std::shared_ptr<Session> sender, receiver;
  int64_t *stamps{};
  uint32_t count{}, first{}, step{};
  int64_t frequency{};
  Histogram histogram;
  uint64_t errors{}, sender_drops{}, receiver_drops{};
  void Observe(uint32_t timestamp) {
    if (timestamp < first)
      return;
    const auto difference = timestamp - first;
    if (difference % step) {
      ++errors;
      return;
    }
    const auto index = difference / step;
    if (index >= count)
      return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const auto start = stamps[index];
    if (start <= 0 || now.QuadPart < start) {
      ++errors;
      return;
    }
    histogram.Record(uint64_t((now.QuadPart - start) * 1000000 / frequency));
  }
};
inline std::mutex benchmark_mutex;
inline std::shared_ptr<Benchmark> benchmark;
} // namespace gamebridge::rtc
extern "C" {
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_bench_begin(
    gb_rtc_handle sender, gb_rtc_handle receiver, int64_t *stamps,
    uint32_t count, uint32_t first, uint32_t step, int64_t frequency) {
  return Boundary([&]() -> gb_rtc_result {
    std::lock_guard lock(benchmark_mutex);
    if (benchmark || !stamps || !count || count > 500000 || !step ||
        frequency <= 0)
      return GB_RTC_INVALID;
    auto s = sessions.Get(sender), r = sessions.Get(receiver);
    if (!s || !r || s->closed || r->closed)
      return GB_RTC_STATE;
    auto b = std::make_shared<Benchmark>();
    b->sender = s;
    b->receiver = r;
    b->stamps = stamps;
    b->count = count;
    b->first = first;
    b->step = step;
    b->frequency = frequency;
    b->sender_drops = s->video.Dropped();
    b->receiver_drops =
        ReceiverDrops(r->received_video, r->receive_contention[0]);
    Engine().signaling->BlockingCall([&] {
      s->dequeue_observer = [b](uint32_t time) { b->Observe(time); };
    });
    benchmark = std::move(b);
    return GB_RTC_OK;
  });
}
GB_RTC_API uint32_t GB_RTC_CALL gb_rtc_bench_end(uint8_t *output,
                                                 uint32_t capacity) {
  try {
    if (!output || capacity < 65536)
      return 0;
    std::shared_ptr<Benchmark> b;
    {
      std::lock_guard lock(benchmark_mutex);
      b = std::move(benchmark);
    }
    if (!b)
      return 0;
    Engine().signaling->BlockingCall([&] { b->sender->dequeue_observer = {}; });
    std::ostringstream json;
    json << "{\"rtc_backend\":\"libwebrtc\",\"libwebrtc_revision\":"
            "\"c250ac7568212f05892447d8e8673f8e55d716d9\","
            "\"gc_applicable\":false,\"gc_pause_ms_max\":0,\"gc_cycles\":0,"
            "\"bridge_p95_ms\":"
         << double(b->histogram.Percentile(95)) / 1000
         << ",\"bridge_latency_samples\":" << b->histogram.Count()
         << ",\"video_queue_depth_max\":" << b->sender->video.MaxDepth()
         << ",\"metrics_errors\":" << b->errors << ",\"frames_dropped_bridge\":"
         << b->sender->video.Dropped() - b->sender_drops
         << ",\"frames_dropped_receiver\":"
         << ReceiverDrops(b->receiver->received_video,
                          b->receiver->receive_contention[0]) -
                b->receiver_drops
         << ",\"bridge_latency_histogram\":[";
    bool comma = false;
    for (size_t i = 0; i < b->histogram.buckets.size(); ++i)
      if (b->histogram.buckets[i]) {
        if (comma)
          json << ',';
        comma = true;
        json << "{\"upper_us\":" << b->histogram.Upper(i)
             << ",\"count\":" << b->histogram.buckets[i] << '}';
      }
    json << "]}";
    const auto bytes = json.str();
    if (bytes.size() > capacity)
      return 0;
    std::memcpy(output, bytes.data(), bytes.size());
    return uint32_t(bytes.size());
  } catch (...) {
    return 0;
  }
}
}
