#include "core.h"
#include "histogram.h"
#include "json.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

// Keep assertions live in Release/CI builds too.
#undef assert
#define assert(condition)                                                      \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "line " << __LINE__ << ": " #condition "\n";                \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

using namespace gamebridge::rtc;
using namespace std::chrono_literals;

int main() {
  Histogram histogram;
  for (unsigned i = 0; i < 100; ++i)
    histogram.Record(i < 95 ? 1500 : 5000);
  assert(histogram.Count() == 100 && histogram.Percentile(95) >= 1500 &&
         histogram.Percentile(95) < 1520);
  assert(histogram.Percentile(100) >= 5000);
  assert(ParseIce("{}").has_value());
  assert(!ParseIce("{\"iceServers\":[{\"urls\":\"stun:[::::]\"}]}"));
  assert(ParseIce("{\"iceServers\":[{\"urls\":\"stun:[2001:db8::1]:3478\"}]}")
             .has_value());
  assert(ParseIce("{\"iceServers\":[{\"urls\":\"turns:relay.example:5349?"
                  "transport=tcp\",\"username\":\"u\",\"credential\":\"p\"}]}")
             .has_value());
  for (auto value :
       {"null", "[]", "{}{}", "{\"iceServers\":[],\"iceServers\":[]}",
        "{\"IceServers\":[]}",
        "{\"iceServers\":[{\"urls\":\"https:example.org\"}]}",
        "{\"iceServers\":[{\"urls\":\"turn:example.org\"}]}",
        "{\"iceServers\":[{\"urls\":\"stun:example.org:999999\"}]}",
        "{\"iceServers\":[{\"urls\":[\"stun:example.org\",\"stun:example.org\"]"
        "}]}",
        "{\"iceServers\":[{\"urls\":\"stun:example.org/#fragment\"}]}"})
    assert(!ParseIce(value));
  assert(ParseJson("{\"candidate\":\"\",\"sdpMid\":null}"));
  assert(!ParseJson("{\"x\":\"\\ud800\"}"));
  assert(!ParseJson("{\"x\":\"\xff\"}"));
  const std::string secret = "v=0\r\nprivate \"\\ data";
  auto roundtrip = ParseJson("{\"sdp\":" + JsonString(secret) + "}");
  assert(roundtrip && roundtrip->object.at("sdp").string == secret);
  HandleTable<int> handles;
  auto first = handles.Insert(std::make_shared<int>(1));
  assert(first && *handles.Get(first) == 1);
  handles.Erase(first);
  auto second = handles.Insert(std::make_shared<int>(2));
  assert(second != first && !handles.Get(first));
  handles.Erase(first);
  assert(*handles.Get(second) == 2);
  for (int i = 0; i < 10000; ++i) {
    handles.Erase(second);
    auto next = handles.Insert(std::make_shared<int>(i));
    assert(next && next != second && !handles.Get(second));
    second = next;
  }
  for (unsigned i = 1; i < GB_RTC_MAX_SESSIONS; ++i)
    assert(handles.Insert(std::make_shared<int>(i)));
  assert(!handles.Insert(std::make_shared<int>(99)));

  MediaQueue video(1, 0);
  std::vector<uint8_t> bytes{0, 0, 0, 1, 0x65, 7};
  assert(video.Push(bytes, 7, 0) == GB_RTC_OK);
  bytes.back() = 8;
  assert(video.Push(bytes, 8, 0) == GB_RTC_OK);
  bytes.back() = 9;
  auto frame = video.Pop();
  assert(frame && frame->bytes.back() == 8 && frame->timestamp == 8);
  assert(video.Dropped() == 1 && video.MaxDepth() == 1 && !video.Pop());
  std::atomic<uint64_t> receive_contention{2};
  assert(ReceiverDrops(video, receive_contention) == 3);
  MediaQueue audio(80, 9600);
  for (unsigned i = 0; i < 81; ++i)
    assert(audio.Push(bytes, i, 120) == GB_RTC_OK);
  assert(audio.Dropped() == 1 && audio.Pop()->timestamp == 1);
  MediaQueue duration(80, 9600);
  for (unsigned i = 0; i < 11; ++i)
    assert(duration.Push(bytes, i, 960) == GB_RTC_OK);
  assert(duration.Dropped() == 1 && duration.Pop()->timestamp == 1);

  assert(OpusSamples(std::vector<uint8_t>{0xf8, 0xff, 0xfe}) == 960);
  assert(!OpusSamples(std::vector<uint8_t>{3}));
  assert(!OpusSamples(std::vector<uint8_t>{3, 0}));
  assert(!OpusSamples(std::vector<uint8_t>{3, 63}));
  assert(ValidAnnexB(std::vector<uint8_t>{0, 0, 1, 0x65, 7}));
  assert(!ValidAnnexB(std::vector<uint8_t>{0, 0, 1, 0x80}));
  assert(!ValidAnnexB(std::vector<uint8_t>{0, 0, 1}));
  std::vector<uint8_t> oversized(GB_RTC_MAX_VIDEO_BYTES, 0x55);
  oversized[0] = 0;
  oversized[1] = 0;
  oversized[2] = 1;
  oversized[3] = 0x65;
  assert(
      !ValidAnnexB(oversized)); // normalization adds a fourth start-code byte
  assert(!ValidAnnexB(std::vector<uint8_t>{0, 0, 0, 0, 1, 0x65}));
  assert(!ValidAnnexB(std::vector<uint8_t>{0, 0, 1, 9})); // no deliverable NAL

  std::atomic<bool> entered{}, release{}, closed{};
  CallbackGate gate;
  std::thread emitter([&] {
    gate.Invoke([&] {
      entered = true;
      while (!release)
        std::this_thread::yield();
    });
  });
  while (!entered)
    std::this_thread::yield();
  std::thread closer([&] {
    gate.Close();
    closed = true;
  });
  std::this_thread::sleep_for(20ms);
  assert(!closed);
  release = true;
  emitter.join();
  closer.join();
  assert(closed && !gate.Invoke([] { assert(false); }));

  // Reentrant close cancels admission immediately, but an external close must
  // still wait for the callback that owns the borrowed payload to unwind.
  CallbackGate self;
  entered = release = closed = false;
  std::thread reentrant([&] {
    self.Invoke([&] {
      self.Close();
      entered = true;
      while (!release)
        std::this_thread::yield();
    });
  });
  while (!entered)
    std::this_thread::yield();
  std::thread outside([&] {
    self.Close();
    closed = true;
  });
  std::this_thread::sleep_for(20ms);
  assert(!closed);
  release = true;
  reentrant.join();
  outside.join();
  assert(closed);

  CallbackGate left, right;
  std::atomic<unsigned> ready{};
  std::thread a([&] {
    left.Invoke([&] {
      ++ready;
      while (ready != 2)
        std::this_thread::yield();
      right.Close();
    });
  });
  std::thread b([&] {
    right.Invoke([&] {
      ++ready;
      while (ready != 2)
        std::this_thread::yield();
      left.Close();
    });
  });
  a.join();
  b.join();
  left.Close();
  right.Close();

  std::cout << "native core: handles, ownership, queues, Opus, Annex-B, "
               "callback barriers PASS\n";
}
