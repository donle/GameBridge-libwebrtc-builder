#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gamebridge_rtc.h"
#include "pacing.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <psapi.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
int64_t qpc() {
  LARGE_INTEGER n{};
  QueryPerformanceCounter(&n);
  return n.QuadPart;
}
uint64_t memory(bool peak = false) {
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof(m);
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),
                            sizeof(m)))
    throw std::runtime_error("memory_sample");
  return peak ? m.PeakPagefileUsage : m.PrivateUsage;
}
void require(bool condition, const char *stage) {
  if (!condition)
    throw std::runtime_error(stage);
}
uint64_t hash(const uint8_t *data, size_t length) {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < length; ++i) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}
struct Frame {
  std::vector<uint8_t> data;
  uint64_t digest{};
  bool key{};
};
std::vector<Frame> fixture(const char *path) {
  std::ifstream in(path, std::ios::binary);
  require(bool(in), "fixture_open");
  std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
  std::vector<Frame> frames;
  Frame current;
  auto finish = [&] {
    if (!current.data.empty()) {
      current.digest = hash(current.data.data(), current.data.size());
      frames.push_back(std::move(current));
      current = Frame{};
    }
  };
  std::vector<std::pair<size_t, size_t>> starts;
  for (size_t i = 0; i + 3 < data.size();) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      starts.push_back({i, i + 3});
      i += 3;
    } else if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1) {
      starts.push_back({i, i + 4});
      i += 4;
    } else
      ++i;
  }
  for (size_t i = 0; i < starts.size(); ++i) {
    const auto begin = starts[i].second,
               end = i + 1 < starts.size() ? starts[i + 1].first : data.size();
    require(begin < end, "fixture_nal");
    const auto type = data[begin] & 31;
    if (type == 9) {
      finish();
      continue;
    }
    if (type == 12)
      continue;
    require(type > 0 && type < 24, "fixture_nal_type");
    current.data.insert(current.data.end(), {0, 0, 0, 1});
    current.data.insert(current.data.end(), data.begin() + begin,
                        data.begin() + end);
    current.key = current.key || type == 5;
  }
  finish();
  require(frames.size() == 120 && frames.front().key, "fixture_frames");
  return frames;
}
struct Api {
  decltype(&gb_rtc_create) create{};
  decltype(&gb_rtc_close) close{};
  decltype(&gb_rtc_create_offer) offer{};
  decltype(&gb_rtc_create_answer) answer{};
  decltype(&gb_rtc_set_remote_description) remote{};
  decltype(&gb_rtc_add_candidate) candidate{};
  decltype(&gb_rtc_send_video) video{};
  using Begin = gb_rtc_result(__cdecl *)(gb_rtc_handle, gb_rtc_handle,
                                         int64_t *, uint32_t, uint32_t,
                                         uint32_t, int64_t);
  Begin begin{};
  using End = uint32_t(__cdecl *)(uint8_t *, uint32_t);
  End end{};
  explicit Api(const char *path) {
    auto dll = LoadLibraryA(path);
    require(dll != nullptr, "dll_load");
#define LOAD(field, name)                                                      \
  field = reinterpret_cast<decltype(field)>(GetProcAddress(dll, name));        \
  require(field != nullptr, "dll_export")
    LOAD(create, "gb_rtc_create");
    LOAD(close, "gb_rtc_close");
    LOAD(offer, "gb_rtc_create_offer");
    LOAD(answer, "gb_rtc_create_answer");
    LOAD(remote, "gb_rtc_set_remote_description");
    LOAD(candidate, "gb_rtc_add_candidate");
    LOAD(video, "gb_rtc_send_video");
    LOAD(begin, "gb_rtc_bench_begin");
    LOAD(end, "gb_rtc_bench_end");
#undef LOAD
  }
};
struct Context {
  struct Signal {
    uint32_t type;
    std::vector<uint8_t> bytes;
  };
  std::mutex mutex;
  std::vector<Signal> pending;
  const std::vector<Frame> *frames{};
  std::vector<uint8_t> seen;
  std::atomic<uint64_t> delivered{}, bytes{}, fatal{}, stale{};
  std::atomic<bool> connected{}, route{}, closed{};
  uint32_t first = 600, count{}, step = 1500;
};
void __cdecl callback(void *pointer, uint32_t type, const uint8_t *data,
                      uint32_t size) noexcept {
  auto &c = *static_cast<Context *>(pointer);
  try {
    if (c.closed.load()) {
      ++c.stale;
      return;
    }
    if (type == GB_RTC_EVENT_ERROR) {
      ++c.fatal;
      return;
    }
    if (type == GB_RTC_EVENT_STATE) {
      if (std::string_view(reinterpret_cast<const char *>(data), size) ==
          "{\"state\":3}")
        c.connected = true;
      return;
    }
    if (type == GB_RTC_EVENT_ROUTE) {
      if (std::string_view(reinterpret_cast<const char *>(data), size) ==
          "{\"route\":1}")
        c.route = true;
      return;
    }
    if (type == GB_RTC_EVENT_DESCRIPTION || type == GB_RTC_EVENT_CANDIDATE) {
      std::lock_guard lock(c.mutex);
      if (c.pending.size() >= 256) {
        ++c.fatal;
        return;
      }
      c.pending.push_back({type, {data, data + size}});
      return;
    }
    if (type != GB_RTC_EVENT_VIDEO)
      return;
    if (size < 16) {
      ++c.fatal;
      return;
    }
    gb_rtc_media_event h{};
    std::memcpy(&h, data, 16);
    if (h.size != 16 || h.abi_version != 1 || h.data_size != size - 16 ||
        h.timestamp % c.step) {
      ++c.fatal;
      return;
    }
    const auto index = h.timestamp / c.step;
    const auto &expected = (*c.frames)[index % c.frames->size()];
    if (h.data_size != expected.data.size() ||
        hash(data + 16, h.data_size) != expected.digest) {
      ++c.fatal;
      return;
    }
    if (index < c.first || index >= c.first + c.count)
      return;
    if (c.seen[index - c.first]++) {
      ++c.fatal;
      return;
    }
    c.bytes.fetch_add(h.data_size);
    ++c.delivered;
  } catch (...) {
    ++c.fatal;
  }
}
struct Peers {
  Api &api;
  gb_rtc_handle handles[2]{};
  Context context[2];
  explicit Peers(Api &a) : api(a) {};
  ~Peers() {
    for (unsigned i = 0; i < 2; ++i)
      if (handles[i]) {
        api.close(handles[i]);
        context[i].closed = true;
      }
  }
};
void connect(Peers &peers) {
  const char ice[] = "{\"iceServers\":[]}";
  gb_rtc_config config{sizeof(config), 1, ice, sizeof(ice) - 1, 0};
  for (unsigned i = 0; i < 2; ++i)
    require(peers.api.create(&config, callback, &peers.context[i],
                             &peers.handles[i]) == GB_RTC_OK,
            "create");
  require(peers.api.offer(peers.handles[0]) == GB_RTC_OK, "offer");
  const auto limit = Clock::now() + std::chrono::seconds(10);
  while (!(peers.context[0].connected && peers.context[1].connected &&
           peers.context[0].route && peers.context[1].route)) {
    require(Clock::now() < limit, "connect_timeout");
    for (unsigned i = 0; i < 2; ++i) {
      std::vector<Context::Signal> events;
      {
        std::lock_guard lock(peers.context[i].mutex);
        events.swap(peers.context[i].pending);
      }
      for (const auto &event : events) {
        auto fn = event.type == GB_RTC_EVENT_DESCRIPTION ? peers.api.remote
                                                         : peers.api.candidate;
        gb_rtc_result r;
        do {
          r = fn(peers.handles[1 - i], event.bytes.data(),
                 static_cast<uint32_t>(event.bytes.size()));
          if (r == GB_RTC_BACKPRESSURE)
            Sleep(1);
        } while (r == GB_RTC_BACKPRESSURE && Clock::now() < limit);
        require(r == GB_RTC_OK, "signal_apply");
        if (event.type == GB_RTC_EVENT_DESCRIPTION && i == 0)
          require(peers.api.answer(peers.handles[1]) == GB_RTC_OK, "answer");
      }
    }
    require(!peers.context[0].fatal && !peers.context[1].fatal,
            "connect_error");
    Sleep(1);
  }
}
class Timer {
  HANDLE timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  int64_t frequency_;

public:
  explicit Timer(int64_t frequency) : frequency_(frequency) {
    require(timer_ != nullptr, "timer_create");
  }
  ~Timer() { CloseHandle(timer_); }
  void until(int64_t deadline) {
    for (;;) {
      const auto remaining = deadline - qpc();
      if (remaining <= 0)
        return;
      if (remaining > frequency_ / 2000) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(1, (remaining - frequency_ / 4000) *
                                                 10000000 / frequency_);
        require(SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE),
                "timer_set");
        WaitForSingleObject(timer_, INFINITE);
      } else
        YieldProcessor();
    }
  }
};
int run(int argc, char **argv) {
  require(argc == 5, "arguments");
  const auto seconds = std::stoul(argv[1]);
  require(seconds >= 5 && seconds <= 7200, "duration");
  const auto frames = fixture(argv[2]);
  Api api(argv[3]);
  Peers peers(api);
  const auto count = static_cast<uint32_t>(seconds * 60);
  std::vector<int64_t> stamps(count);
  for (auto &c : peers.context) {
    c.frames = &frames;
    c.count = count;
    c.seen.resize(count);
  }
  connect(peers);
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  Timer timer(frequency.QuadPart);
  uint64_t submitted = 0, submissionFailures = 0, sendBytes = 0;
  const auto warm = qpc();
  auto send = [&](uint32_t index, bool recorded) {
    const auto &f = frames[index % frames.size()];
    gb_rtc_video v{sizeof(v),
                   1,
                   f.data.data(),
                   static_cast<uint32_t>(f.data.size()),
                   index * 1500,
                   1920,
                   1080,
                   static_cast<uint8_t>(f.key),
                   {}};
    if (recorded)
      stamps[index - 600] = qpc();
    const auto result = api.video(peers.handles[0], &v);
    if (recorded) {
      if (result == GB_RTC_OK) {
        ++submitted;
        sendBytes += f.data.size();
      } else {
        ++submissionFailures;
        if (result != GB_RTC_BACKPRESSURE)
          ++peers.context[0].fatal;
      }
    } else
      require(result == GB_RTC_OK, "prewarm_submit");
  };
  for (uint32_t i = 0; i < 600; ++i) {
    timer.until(warm + static_cast<int64_t>(i) * frequency.QuadPart / 60);
    send(i, false);
  }
  timer.until(warm + 10 * frequency.QuadPart);
  require(api.begin(peers.handles[0], peers.handles[1], stamps.data(), count,
                    600 * 1500, 1500, frequency.QuadPart) == GB_RTC_OK,
          "measurement_begin");
  const auto baseline = memory();
  auto peak = std::max(baseline, memory(true));
  const auto start = qpc();
  auto nextSample = start + frequency.QuadPart;
  uint32_t progress = 0;
  std::fprintf(
      stderr,
      "RTC recording started: duration=%lu seconds, prewarm=10 seconds\n",
      seconds);
  for (uint32_t i = 0; i < count; ++i) {
    timer.until(gamebridge::bench::SubmissionDeadline(
        start + static_cast<int64_t>(i) * frequency.QuadPart / 60,
        i ? stamps[i-1] : 0, frequency.QuadPart));
    send(600 + i, true);
    if (qpc() >= nextSample) {
      peak = std::max(peak, memory(true));
      nextSample += frequency.QuadPart;
    }
    if (i / 3600 > progress) {
      progress = i / 3600;
      std::fprintf(
          stderr, "RTC progress seconds=%u delivered=%llu fatal=%llu\n",
          progress * 60,
          static_cast<unsigned long long>(peers.context[1].delivered.load()),
          static_cast<unsigned long long>(peers.context[0].fatal +
                                          peers.context[1].fatal));
    }
  }
  timer.until(start + static_cast<int64_t>(seconds) * frequency.QuadPart);
  const double elapsed = static_cast<double>(qpc() - start) /
                         static_cast<double>(frequency.QuadPart);
  const auto drain = Clock::now() + std::chrono::seconds(2);
  while (peers.context[1].delivered < submitted && Clock::now() < drain)
    Sleep(1);
  const auto ending = memory();
  peak = std::max(peak, memory(true));
  std::vector<uint8_t> metrics(65536);
  const auto length =
      api.end(metrics.data(), static_cast<uint32_t>(metrics.size()));
  require(length > 2, "measurement_end");
  for (unsigned i = 0; i < 2; ++i) {
    api.close(peers.handles[i]);
    peers.context[i].closed = true;
    gb_rtc_video stale{sizeof(stale),
                       1,
                       frames[0].data.data(),
                       static_cast<uint32_t>(frames[0].data.size()),
                       0,
                       1920,
                       1080,
                       1,
                       {}};
    require(api.video(peers.handles[i], &stale) == GB_RTC_STATE,
            "stale_handle");
    peers.handles[i] = 0;
  }
  Sleep(100);
  std::ofstream out(argv[4], std::ios::binary);
  require(bool(out), "report_open");
  out << std::setprecision(12);
  out.write(reinterpret_cast<const char *>(metrics.data()), length - 1);
  const auto delivered = peers.context[1].delivered.load();
  uint64_t lateFrames = 0, burstIntervals = 0;
  int64_t maximumLateness = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto late = stamps[i] - (start + static_cast<int64_t>(i) * frequency.QuadPart / 60);
    maximumLateness = std::max(maximumLateness, late);
    if (late > frequency.QuadPart / 60) ++lateFrames;
    if (i && stamps[i] - stamps[i-1] < frequency.QuadPart / 1000) ++burstIntervals;
  }
  const auto fatal = peers.context[0].fatal + peers.context[1].fatal;
  const auto stale = peers.context[0].stale + peers.context[1].stale;
  out << ",\"duration_seconds\":" << elapsed
      << ",\"requested_duration_seconds\":" << seconds
      << ",\"prewarm_seconds\":10,\"frames_submitted\":" << submitted
      << ",\"submission_failures\":" << submissionFailures
      << ",\"frames_delivered\":" << delivered << ",\"throughput_bps\":"
      << static_cast<double>(peers.context[1].bytes.load()) * 8 / elapsed
      << ",\"submitted_bps\":" << static_cast<double>(sendBytes) * 8 / elapsed
      << ",\"native_memory_baseline_bytes\":" << baseline
      << ",\"native_memory_peak_bytes\":" << peak
      << ",\"native_memory_ending_bytes\":" << ending
      << ",\"memory_growth_mib\":"
      << static_cast<double>(peak - baseline) / 1048576
      << ",\"memory_ending_growth_mib\":"
      << static_cast<double>(static_cast<int64_t>(ending) -
                             static_cast<int64_t>(baseline)) /
             1048576
      << ",\"fatal_errors\":" << fatal
      << ",\"stale_handle_callbacks\":" << stale
      << ",\"producer_late_frames\":" << lateFrames
      << ",\"producer_bursts_under_1ms\":" << burstIntervals
      << ",\"producer_max_lateness_ms\":" << static_cast<double>(maximumLateness)*1000/frequency.QuadPart
      << ",\"producer_catchup_minimum_interval_ms\":2"
      << ",\"width\":1920,\"height\":1080,\"fps\":60,\"route\":\"direct\","
         "\"transport\":\"C ABI/RTC/DTLS-SRTP/UDP/C "
         "callback\",\"payload_validation\":\"all complete AUs matched fixture "
         "FNV-1a64\"}";
  require(bool(out), "report_write");
  return fatal || stale ? 1 : 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "RTC benchmark failed at %s\n", e.what());
    return 1;
  }
}
