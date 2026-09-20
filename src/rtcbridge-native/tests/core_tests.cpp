#include "core.h"
#include "histogram.h"
#include "json.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <functional>
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

  assert(DirectTrafficAllowed(false, false));
  assert(!DirectTrafficAllowed(true, false));
  assert(DirectTrafficAllowed(true, true));

  DirectRouteProof missing(1000);
  assert(missing.Observe(DirectPairEvidence::Missing, 5999) ==
         DirectProofResult::Pending);
  DirectRouteProof unconvertible(1000);
  assert(unconvertible.Observe(DirectPairEvidence::Unconvertible, 5999) ==
         DirectProofResult::Pending);
  DirectRouteProof mismatched(1000);
  assert(mismatched.Observe(DirectPairEvidence::Mismatched, 5999) ==
         DirectProofResult::Pending);
  assert(missing.Observe(DirectPairEvidence::Missing, 6000) ==
         DirectProofResult::Expired);
  DirectRouteProof proven(1000);
  assert(proven.Observe(DirectPairEvidence::Direct, 5999) ==
         DirectProofResult::Proven);

  // A later selected-pair record with different IDs but direct candidate
  // types is still complete direct evidence.
  assert(proven.Observe(DirectPairEvidence::Direct, 6000) ==
         DirectProofResult::Stable);

  DirectRouteProof relayed(1000);
  assert(relayed.Observe(DirectPairEvidence::Direct, 2000) ==
         DirectProofResult::Proven);
  assert(relayed.Observe(DirectPairEvidence::Relay, 2001) ==
         DirectProofResult::Forbidden);

  for (auto evidence : {DirectPairEvidence::Missing,
                        DirectPairEvidence::Unconvertible,
                        DirectPairEvidence::Mismatched}) {
    DirectRouteProof regressed(1000);
    assert(regressed.Observe(DirectPairEvidence::Direct, 2000) ==
           DirectProofResult::Proven);
    assert(regressed.Observe(evidence, 3000) ==
           DirectProofResult::Regressed);
    assert(regressed.Observe(evidence, 7999) ==
           DirectProofResult::Pending);
    assert(regressed.Observe(evidence, 8000) ==
           DirectProofResult::Expired);
  }

  MediaQueue regression_video(1, 0), regression_audio(80, 9600);
  assert(regression_video.Push(bytes, 10, 0) == GB_RTC_OK);
  assert(regression_audio.Push(bytes, 10, 120) == GB_RTC_OK);
  assert(regression_audio.Push(bytes, 11, 120) == GB_RTC_OK);
  auto regression_drops = DiscardMediaForDirectRegression(
      DirectProofResult::Regressed, regression_video, regression_audio);
  assert(regression_drops.video == 1 && regression_drops.audio == 2);
  assert(regression_drops.request_keyframe());
  assert(regression_video.Dropped() == 1 && regression_audio.Dropped() == 2);
  assert(!regression_video.Pop() && !regression_audio.Pop());

  // Repeated unproven observations carry no stale media and must not create a
  // keyframe-request storm.
  auto repeated_drops = DiscardMediaForDirectRegression(
      DirectProofResult::Pending, regression_video, regression_audio);
  assert(repeated_drops.video == 0 && repeated_drops.audio == 0);
  assert(!repeated_drops.request_keyframe());

  // A different complete direct pair remains stable and must retain valid
  // media queued under the continuously proven route.
  assert(regression_video.Push(bytes, 12, 0) == GB_RTC_OK);
  auto direct_change_drops = DiscardMediaForDirectRegression(
      DirectProofResult::Stable, regression_video, regression_audio);
  assert(direct_change_drops.video == 0 &&
         !direct_change_drops.request_keyframe());
  assert(regression_video.Pop()->timestamp == 12);

  // Audio-only regression updates exact metrics without requesting a video
  // recovery frame.
  assert(regression_audio.Push(bytes, 13, 120) == GB_RTC_OK);
  auto audio_only_drops = DiscardMediaForDirectRegression(
      DirectProofResult::Regressed, regression_video, regression_audio);
  assert(audio_only_drops.video == 0 && audio_only_drops.audio == 1);
  assert(!audio_only_drops.request_keyframe());

  // Outbound data may wait for the signaling thread after an optimistic
  // caller-side check. The effect-point admission must observe an intervening
  // invalidation, then recover after a new direct proof.
  DirectRouteGate outbound_gate;
  assert(outbound_gate.Prove());
  std::atomic<bool> outbound_checked{}, outbound_continue{}, outbound_sent{},
      outbound_admitted{};
  std::thread outbound_waiter([&] {
    assert(outbound_gate.Proven());
    outbound_checked = true;
    while (!outbound_continue)
      std::this_thread::yield();
    outbound_admitted = outbound_gate.Admit([&] { outbound_sent = true; });
  });
  while (!outbound_checked)
    std::this_thread::yield();
  outbound_gate.Invalidate([] {});
  outbound_continue = true;
  outbound_waiter.join();
  assert(!outbound_admitted && !outbound_sent);
  assert(outbound_gate.Prove());
  assert(outbound_gate.Admit([&] { outbound_sent = true; }));
  assert(outbound_sent);

  // An inbound message already inside the admission boundary may finish
  // queueing before invalidation. Invalidation waits for that boundary, and no
  // later event is queued until recovery.
  DirectRouteGate inbound_gate;
  assert(inbound_gate.Prove());
  std::atomic<bool> inbound_entered{}, inbound_release{}, invalidation_started{},
      invalidation_finished{};
  std::atomic<unsigned> inbound_events{};
  std::thread inbound_admitted([&] {
    assert(inbound_gate.Admit([&] {
      inbound_entered = true;
      while (!inbound_release)
        std::this_thread::yield();
      ++inbound_events;
    }));
  });
  while (!inbound_entered)
    std::this_thread::yield();
  assert(inbound_gate.TryAdmit([&] { ++inbound_events; }) ==
         DirectAdmissionResult::Busy);
  assert(inbound_events == 0);
  std::thread inbound_invalidator([&] {
    invalidation_started = true;
    inbound_gate.Invalidate([] {});
    invalidation_finished = true;
  });
  while (!invalidation_started)
    std::this_thread::yield();
  for (unsigned i = 0; i < 1000 && !invalidation_finished; ++i)
    std::this_thread::yield();
  assert(!invalidation_finished);
  inbound_release = true;
  inbound_admitted.join();
  inbound_invalidator.join();
  assert(invalidation_finished && inbound_events == 1);
  assert(!inbound_gate.Admit([&] { ++inbound_events; }));
  assert(inbound_events == 1);
  assert(inbound_gate.Prove());
  assert(inbound_gate.Admit([&] { ++inbound_events; }));
  assert(inbound_events == 2);

  // Terminal failure closes admission permanently. Outbound and inbound
  // operations paused after an optimistic proof check must both be denied at
  // their effect points, and a later proof cannot reopen the gate.
  DirectRouteGate terminal_gate;
  assert(terminal_gate.Prove());
  std::atomic<unsigned> terminal_checked{}, terminal_effects{};
  std::atomic<bool> terminal_continue{}, terminal_outbound_admitted{},
      terminal_inbound_admitted{};
  const auto terminal_waiter = [&](std::atomic<bool> &admitted) {
    assert(terminal_gate.Proven());
    ++terminal_checked;
    while (!terminal_continue)
      std::this_thread::yield();
    admitted = terminal_gate.Admit([&] { ++terminal_effects; });
  };
  std::thread terminal_outbound(terminal_waiter,
                                std::ref(terminal_outbound_admitted));
  std::thread terminal_inbound(terminal_waiter,
                               std::ref(terminal_inbound_admitted));
  while (terminal_checked != 2)
    std::this_thread::yield();
  terminal_gate.Close();
  terminal_continue = true;
  terminal_outbound.join();
  terminal_inbound.join();
  assert(!terminal_gate.Proven());
  assert(!terminal_outbound_admitted && !terminal_inbound_admitted);
  assert(terminal_effects == 0);
  assert(!terminal_gate.Prove() && !terminal_gate.Proven());
  assert(!terminal_gate.Admit([&] { ++terminal_effects; }));
  assert(terminal_gate.TryAdmit([&] { ++terminal_effects; }) ==
         DirectAdmissionResult::Denied);
  assert(terminal_effects == 0);

  // Terminal closure is safe even when a lower-level effect discovers the
  // failure while already admitted; publication can then happen after Admit
  // returns without recursively acquiring the gate.
  DirectRouteGate effect_failure_gate;
  assert(effect_failure_gate.Prove());
  bool effect_returned = false;
  assert(effect_failure_gate.Admit([&] {
    effect_failure_gate.Close();
    effect_returned = true;
  }));
  assert(effect_returned && !effect_failure_gate.Proven());
  assert(!effect_failure_gate.Prove());

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
