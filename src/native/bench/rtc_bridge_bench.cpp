#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gamebridge_rtc.h"
#include "pacing.h"
#include <algorithm>
#include <array>
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
  decltype(&gb_rtc_create_v2) create{};
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
    LOAD(create, "gb_rtc_create_v2");
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
enum class FatalReason : unsigned {
  Header,
  Timestamp,
  Range,
  Duplicate,
  Payload,
  Bridge,
  Other
};
enum class Phase : unsigned { Setup, Prewarm, Measurement, Drain, Teardown };
struct MissingFrames {
  uint32_t total{}, head{}, tail{}, interior{}, runs{}, longest{}, samples{},
      omitted{};
  std::array<uint32_t, 64> indices{};
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
  std::atomic<uint64_t> prewarm_delivered{}, after_window_delivered{},
      keyframe_requests{};
  std::atomic<int64_t> last_arrival_qpc{};
  std::array<std::atomic<uint64_t>, 7> fatal_reason{};
  std::array<std::atomic<uint64_t>, 5> fatal_phase{};
  // Fixed codes only: control closed, pointer closed, connection failed, other.
  std::array<std::atomic<uint64_t>, 4> bridge_error_reason{};
  std::atomic<uint64_t> bridge_error_code_mask{};
  std::atomic<uint64_t> payload_size_errors{}, payload_content_errors{},
      payload_matches_other_fixture{};
  std::atomic<unsigned> phase{static_cast<unsigned>(Phase::Setup)};
  std::atomic<bool> connected{}, route{}, closed{};
  uint32_t first = 600, count{}, step = 1500;
  void RecordFatal(FatalReason reason) noexcept {
    ++fatal_reason[static_cast<unsigned>(reason)];
    ++fatal_phase[phase.load()];
    ++fatal;
  }
  // Called only after both close barriers, when seen is no longer being
  // written.
  MissingFrames SummarizeMissing(const std::vector<uint8_t> &accepted) const {
    require(accepted.size() == seen.size(), "missing_frame_accounting");
    MissingFrames result;
    auto missing = [&](size_t index) {
      return !accepted[index] || !seen[index];
    };
    uint32_t run = 0;
    for (size_t i = 0; i < seen.size(); ++i) {
      if (!missing(i)) {
        run = 0;
        continue;
      }
      ++result.total;
      if (!run++)
        ++result.runs;
      result.longest = std::max(result.longest, run);
      if (result.samples < result.indices.size())
        result.indices[result.samples++] = uint32_t(i);
    }
    while (result.head < seen.size() && missing(result.head))
      ++result.head;
    while (result.tail < seen.size() && missing(seen.size() - 1 - result.tail))
      ++result.tail;
    result.interior =
        result.total - std::min(result.total, result.head + result.tail);
    result.omitted = result.total - result.samples;
    return result;
  }
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
      const std::string_view text(
          data ? reinterpret_cast<const char *>(data) : "", data ? size : 0);
      const unsigned reason =
          text == "{\"code\":\"control_channel_closed\"}"   ? 0
          : text == "{\"code\":\"pointer_channel_closed\"}" ? 1
          : text == "{\"code\":\"connection_failed\"}"      ? 2
                                                            : 3;
      ++c.bridge_error_reason[reason];
      // Known strings never leave this boundary. Bit 10 means an unknown code;
      // bits 0..9 identify the fixed native bridge error vocabulary below.
      constexpr std::string_view codes[] = {
          "{\"code\":\"control_channel_closed\"}",
          "{\"code\":\"pointer_channel_closed\"}",
          "{\"code\":\"connection_failed\"}",
          "{\"code\":\"event_oversize\"}",
          "{\"code\":\"control_overflow\"}",
          "{\"code\":\"event_overflow\"}",
          "{\"code\":\"description_rejected\"}",
          "{\"code\":\"candidate_rejected\"}",
          "{\"code\":\"channel_failed\"}",
          "{\"code\":\"media_write_failed\"}"};
      unsigned code = 0;
      while (code < std::size(codes) && text != codes[code])
        ++code;
      c.bridge_error_code_mask.fetch_or(uint64_t{1} << code);
      c.RecordFatal(FatalReason::Bridge);
      return;
    }
    if (type == GB_RTC_EVENT_STATE) {
      if (std::string_view(reinterpret_cast<const char *>(data), size) ==
          "{\"state\":3}")
        c.connected = true;
      return;
    }
    if (type == GB_RTC_EVENT_KEYFRAME_REQUEST) {
      ++c.keyframe_requests;
      return;
    }
    if (type == GB_RTC_EVENT_ROUTE) {
      const bool direct = data && std::string_view(reinterpret_cast<const char *>(data), size) ==
          "{\"route\":1}";
      const bool previously_direct = c.route.exchange(direct);
      if (!direct && (previously_direct ||
          (data && std::string_view(reinterpret_cast<const char *>(data), size) == "{\"route\":2}")))
        c.RecordFatal(FatalReason::Other);
      return;
    }
    if (type == GB_RTC_EVENT_DESCRIPTION || type == GB_RTC_EVENT_CANDIDATE) {
      std::lock_guard lock(c.mutex);
      if (c.pending.size() >= 256) {
        c.RecordFatal(FatalReason::Other);
        return;
      }
      c.pending.push_back({type, {data, data + size}});
      return;
    }
    if (type != GB_RTC_EVENT_VIDEO)
      return;
    if (!data || size < 16) {
      c.RecordFatal(FatalReason::Header);
      return;
    }
    gb_rtc_media_event h{};
    std::memcpy(&h, data, 16);
    if (h.size != 16 || h.abi_version != 1) {
      c.RecordFatal(FatalReason::Header);
      return;
    }
    if (h.data_size != size - 16) {
      c.RecordFatal(FatalReason::Range);
      return;
    }
    if (!c.step || h.timestamp % c.step) {
      c.RecordFatal(FatalReason::Timestamp);
      return;
    }
    const auto index = h.timestamp / c.step;
    const auto &expected = (*c.frames)[index % c.frames->size()];
    const auto digest = hash(data + 16, h.data_size);
    if (h.data_size != expected.data.size() || digest != expected.digest) {
      if (h.data_size != expected.data.size())
        ++c.payload_size_errors;
      else
        ++c.payload_content_errors;
      for (const auto &frame : *c.frames) {
        if (h.data_size == frame.data.size() && digest == frame.digest) {
          ++c.payload_matches_other_fixture;
          break;
        }
      }
      c.RecordFatal(FatalReason::Payload);
      return;
    }
    if (index < c.first) {
      ++c.prewarm_delivered;
      return;
    }
    if (index >= c.first + c.count) {
      ++c.after_window_delivered;
      return;
    }
    if (c.seen[index - c.first]++) {
      c.RecordFatal(FatalReason::Duplicate);
      return;
    }
    c.bytes.fetch_add(h.data_size);
    c.last_arrival_qpc = qpc();
    ++c.delivered;
  } catch (...) {
    c.RecordFatal(FatalReason::Other);
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
  gb_rtc_network_config network{sizeof(network), GB_RTC_ABI_VERSION,
      GB_RTC_NETWORK_DIRECT_ONLY, 47981, 47990, 0, {}, {}, {}};
  for (unsigned i = 0; i < 2; ++i)
    require(peers.api.create(&config, &network, callback, &peers.context[i],
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
  std::vector<uint8_t> accepted(count);
  for (auto &c : peers.context) {
    c.frames = &frames;
    c.count = count;
    c.seen.resize(count);
  }
  connect(peers);
  for (auto &c : peers.context)
    c.phase = static_cast<unsigned>(Phase::Prewarm);
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
        accepted[index - 600] = 1;
        ++submitted;
        sendBytes += f.data.size();
      } else {
        ++submissionFailures;
        if (result != GB_RTC_BACKPRESSURE)
          peers.context[0].RecordFatal(FatalReason::Other);
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
  for (auto &c : peers.context)
    c.phase = static_cast<unsigned>(Phase::Measurement);
  std::fprintf(
      stderr,
      "RTC recording started: duration=%lu seconds, prewarm=10 seconds\n",
      seconds);
  for (uint32_t i = 0; i < count; ++i) {
    timer.until(gamebridge::bench::SubmissionDeadline(
        start + static_cast<int64_t>(i) * frequency.QuadPart / 60,
        i ? stamps[i - 1] : 0, frequency.QuadPart));
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
  const auto drainStart = qpc();
  const auto deliveredBeforeDrain = peers.context[1].delivered.load();
  const auto drain = Clock::now() + std::chrono::seconds(2);
  for (auto &c : peers.context)
    c.phase = static_cast<unsigned>(Phase::Drain);
  while (peers.context[1].delivered < submitted && Clock::now() < drain)
    Sleep(1);
  const auto drainEnd = qpc();
  const auto deliveredAfterDrain = peers.context[1].delivered.load();
  const auto lastArrivalAtDrainEnd = peers.context[1].last_arrival_qpc.load();
  const auto ending = memory();
  peak = std::max(peak, memory(true));
  std::vector<uint8_t> metrics(65536);
  const auto length =
      api.end(metrics.data(), static_cast<uint32_t>(metrics.size()));
  require(length > 2, "measurement_end");
  // This is an observation boundary, not permission to ignore errors. All
  // teardown errors still count toward fatal=0 until an upstream cause is
  // proven.
  for (auto &c : peers.context)
    c.phase = static_cast<unsigned>(Phase::Teardown);
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
  const auto missing = peers.context[1].SummarizeMissing(accepted);
  out << ",\"missing_frame_count\":" << missing.total
      << ",\"missing_head_frames\":" << missing.head
      << ",\"missing_tail_frames\":" << missing.tail
      << ",\"missing_interior_frames\":" << missing.interior
      << ",\"missing_frame_runs\":" << missing.runs
      << ",\"missing_longest_run\":" << missing.longest
      << ",\"missing_indices_omitted\":" << missing.omitted
      << ",\"measurement_first_rtp_timestamp\":900000,\"measurement_rtp_"
         "timestamp_step\":1500"
      << ",\"missing_frame_indices\":[";
  for (uint32_t i = 0; i < missing.samples; ++i) {
    if (i)
      out << ',';
    out << missing.indices[i];
  }
  out << ']' << ",\"frames_delivered_before_drain\":" << deliveredBeforeDrain
      << ",\"frames_delivered_during_drain\":"
      << deliveredAfterDrain - deliveredBeforeDrain
      << ",\"frames_delivered_after_drain\":"
      << peers.context[1].delivered.load() - deliveredAfterDrain
      << ",\"drain_elapsed_ms\":"
      << double(drainEnd - drainStart) * 1000 / frequency.QuadPart
      << ",\"prewarm_frames_delivered\":"
      << peers.context[1].prewarm_delivered.load()
      << ",\"after_window_frames_delivered\":"
      << peers.context[1].after_window_delivered.load()
      << ",\"keyframe_requests_total\":"
      << peers.context[0].keyframe_requests +
             peers.context[1].keyframe_requests;
  if (lastArrivalAtDrainEnd > 0 && lastArrivalAtDrainEnd <= drainEnd)
    out << ",\"last_frame_age_at_drain_end_ms\":"
        << double(drainEnd - lastArrivalAtDrainEnd) * 1000 / frequency.QuadPart;
  constexpr const char *reasons[] = {"header",    "timestamp", "range",
                                     "duplicate", "payload",   "bridge",
                                     "other"};
  constexpr const char *phases[] = {"setup", "prewarm", "measurement", "drain",
                                    "teardown"};
  constexpr const char *bridgeReasons[] = {"control_closed", "pointer_closed",
                                           "connection", "other"};
  for (unsigned i = 0; i < std::size(reasons); ++i)
    out << ",\"fatal_" << reasons[i] << "_errors\":"
        << peers.context[0].fatal_reason[i] + peers.context[1].fatal_reason[i];
  for (unsigned i = 0; i < std::size(phases); ++i)
    out << ",\"fatal_" << phases[i] << "_errors\":"
        << peers.context[0].fatal_phase[i] + peers.context[1].fatal_phase[i];
  for (unsigned i = 0; i < std::size(bridgeReasons); ++i)
    out << ",\"fatal_bridge_" << bridgeReasons[i] << "_errors\":"
        << peers.context[0].bridge_error_reason[i] +
               peers.context[1].bridge_error_reason[i];
  out << ",\"bridge_error_code_mask\":"
      << (peers.context[0].bridge_error_code_mask.load() |
          peers.context[1].bridge_error_code_mask.load())
      << ",\"payload_size_errors\":"
      << peers.context[0].payload_size_errors +
             peers.context[1].payload_size_errors
      << ",\"payload_content_errors\":"
      << peers.context[0].payload_content_errors +
             peers.context[1].payload_content_errors
      << ",\"payload_matches_other_fixture\":"
      << peers.context[0].payload_matches_other_fixture +
             peers.context[1].payload_matches_other_fixture;
  const auto delivered = peers.context[1].delivered.load();
  uint64_t lateFrames = 0, burstIntervals = 0;
  int64_t maximumLateness = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto late =
        stamps[i] - (start + static_cast<int64_t>(i) * frequency.QuadPart / 60);
    maximumLateness = std::max(maximumLateness, late);
    if (late > frequency.QuadPart / 60)
      ++lateFrames;
    if (i && stamps[i] - stamps[i - 1] < frequency.QuadPart / 1000)
      ++burstIntervals;
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
      << ",\"producer_max_lateness_ms\":"
      << static_cast<double>(maximumLateness) * 1000 / frequency.QuadPart
      << ",\"producer_catchup_minimum_interval_ms\":2"
      << ",\"width\":1920,\"height\":1080,\"fps\":60,\"route\":\""
      << (peers.context[0].route && peers.context[1].route ? "direct" : "unknown")
      << "\",\"transport\":\"C ABI/RTC/DTLS-SRTP/UDP/C "
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
