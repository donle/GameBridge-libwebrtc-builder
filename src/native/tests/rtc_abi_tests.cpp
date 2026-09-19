#define NOMINMAX
#include <windows.h>
#include "gamebridge_rtc.h"
#include "rtc_media_fixture.h"
#include "rtc_connection_diagnostics.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
static_assert(sizeof(gb_rtc_handle) == 8 && sizeof(gb_rtc_result) == 4);
static_assert(sizeof(gb_rtc_config) == 24 && offsetof(gb_rtc_config, ice_json_utf8) == 8 && offsetof(gb_rtc_config, ice_json_size) == 16);
static_assert(sizeof(gb_rtc_video) == 40 && offsetof(gb_rtc_video, data) == 8 && offsetof(gb_rtc_video, keyframe) == 32);
static_assert(sizeof(gb_rtc_audio) == 32 && offsetof(gb_rtc_audio, timestamp48k) == 20);
static_assert(sizeof(gb_rtc_media_event)==16 && offsetof(gb_rtc_media_event,timestamp)==8);
static_assert(std::is_same_v<decltype(&gb_rtc_send_video), gb_rtc_result(__cdecl*)(gb_rtc_handle, const gb_rtc_video*)>);
extern "C" int gb_rtc_c_layout(void);

struct Api {
  decltype(&gb_rtc_create) create{};
  decltype(&gb_rtc_close) close{};
  decltype(&gb_rtc_create_offer) offer{};
  decltype(&gb_rtc_create_answer) answer{};
  decltype(&gb_rtc_set_remote_description) remote{};
  decltype(&gb_rtc_add_candidate) candidate{};
  decltype(&gb_rtc_send_video) video{};
  decltype(&gb_rtc_send_audio) audio{};
  decltype(&gb_rtc_send_data) data{};
  using Probe = gb_rtc_result(__cdecl*)(gb_rtc_handle, uint32_t);
  Probe probe{};
  bool load(const char* path, bool testing) {
    // A Go c-shared runtime is process-lived; never unload it with FreeLibrary.
    HMODULE dll = LoadLibraryA(path); if (!dll) return false;
    const auto* image = reinterpret_cast<const uint8_t*>(dll);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    if (exports->NumberOfNames != (testing ? 10u : 9u) || exports->NumberOfFunctions != exports->NumberOfNames) return false;
#define LOAD(field, name) field = reinterpret_cast<decltype(field)>(GetProcAddress(dll, name)); if (!field) return false
    LOAD(create, "gb_rtc_create"); LOAD(close, "gb_rtc_close");
    LOAD(offer, "gb_rtc_create_offer"); LOAD(answer, "gb_rtc_create_answer");
    LOAD(remote, "gb_rtc_set_remote_description"); LOAD(candidate, "gb_rtc_add_candidate");
    LOAD(video, "gb_rtc_send_video"); LOAD(audio, "gb_rtc_send_audio");
    LOAD(data, "gb_rtc_send_data");
#undef LOAD
    probe = reinterpret_cast<Probe>(GetProcAddress(dll, "gb_rtc_test_probe"));
    return testing ? probe != nullptr : probe == nullptr;
  }
};

struct Event {
  HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~Event() { CloseHandle(value); }
  bool wait(DWORD timeout = 5000) const { return WaitForSingleObject(value, timeout) == WAIT_OBJECT_0; }
  void set() const { SetEvent(value); }
};
struct CallbackState {
  Api* api{};
  gb_rtc_handle handle{};
  Event entered, release, finished;
  std::atomic<int> calls{}, errors{};
  bool block{}, reenter{};
  gb_rtc_handle closeOther{};
  std::vector<uint8_t> observed;
};
void __cdecl callback(void* context, uint32_t type, const uint8_t* bytes, uint32_t size) {
  if (type < 0x80000000u) return; // Ordinary lifecycle event is not a probe.
  auto& state = *static_cast<CallbackState*>(context);
  state.calls.fetch_add(1);
  state.observed.assign(bytes, bytes + size);
  if (state.reenter) {
    state.api->close(state.handle);
    if (state.api->offer(state.handle) != GB_RTC_STATE) state.errors.fetch_add(1);
  }
  state.entered.set();
  if (state.block && !state.release.wait()) state.errors.fetch_add(1);
  if (state.closeOther) state.api->close(state.closeOther);
  if (size && std::memcmp(bytes, state.observed.data(), size) != 0) state.errors.fetch_add(1);
  state.finished.set();
}

struct PeerEvents {
  struct Payload { uint32_t type; std::vector<uint8_t> bytes; };
  std::mutex mutex;
  std::vector<Payload> pending;
  std::atomic<unsigned> callbacks{};
};
void __cdecl peer_callback(void* context, uint32_t type, const uint8_t* bytes, uint32_t size) {
  auto& events = *static_cast<PeerEvents*>(context);
  std::lock_guard lock(events.mutex);
  events.pending.push_back({type, {bytes, bytes + size}});
  ++events.callbacks;
}
int connect_native_peers(Api& api, const gb_rtc_config& config) {
  PeerEvents events[2];
  gb_rtc_handle peers[2]{};
  RtcConnectionDiagnostics trace;
  struct Lifetime {
    Api& api;
    gb_rtc_handle (&peers)[2];
    PeerEvents (&events)[2];
    RtcConnectionDiagnostics& trace;
    void Close() {
      for (auto& peer : peers) if (peer) { api.close(peer); peer = 0; }
    }
    ~Lifetime() {
      if (trace.phase != RtcConnectionDiagnostics::Complete) {
        for (unsigned i = 0; i < 2; ++i) {
          trace.Print(i);
          std::lock_guard lock(events[i].mutex);
          std::fprintf(stderr, "rtc_connect peer=%u callbacks=%u pending=%zu\n", i,
                       events[i].callbacks.load(), events[i].pending.size());
        }
      }
      // Every failure path retires callbacks before their context is destroyed.
      Close();
    }
  } lifetime{api, peers, events, trace};
  CHECK(api.create(&config, peer_callback, &events[0], &peers[0]) == GB_RTC_OK);
  CHECK(api.create(&config, peer_callback, &events[1], &peers[1]) == GB_RTC_OK);
  CHECK(api.answer(peers[1]) == GB_RTC_STATE);
  CHECK(api.offer(peers[0]) == GB_RTC_OK);
  bool connected[2]{}, routed[2]{}, gathered[2]{}, end_forwarded[2]{};
  const auto deadline = GetTickCount64() + 5000;
  while (!(connected[0] && connected[1] && routed[0] && routed[1] && gathered[0] && gathered[1] && end_forwarded[0] && end_forwarded[1])) {
    CHECK(GetTickCount64() < deadline);
    for (unsigned source = 0; source < 2; ++source) {
      std::vector<PeerEvents::Payload> pending;
      { std::lock_guard lock(events[source].mutex); pending.swap(events[source].pending); }
      for (const auto& event : pending) {
        const std::string value(event.bytes.begin(), event.bytes.end());
        trace.Event(source, event.type, value);
        if (event.type <= GB_RTC_EVENT_GATHERING) trace.Print(source);
        CHECK(event.type != GB_RTC_EVENT_ERROR);
        if (event.type == GB_RTC_EVENT_STATE && value == "{\"state\":3}") connected[source] = true;
        if (event.type == GB_RTC_EVENT_ROUTE) { CHECK(value == "{\"route\":1}"); routed[source] = true; }
        if (event.type == GB_RTC_EVENT_GATHERING && value == "{\"state\":3}") gathered[source] = true;
        if (event.type != GB_RTC_EVENT_DESCRIPTION && event.type != GB_RTC_EVENT_CANDIDATE) continue;
        auto* input = static_cast<uint8_t*>(VirtualAlloc(nullptr, event.bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(input); std::memcpy(input, event.bytes.data(), event.bytes.size());
        const auto call = event.type == GB_RTC_EVENT_DESCRIPTION ? api.remote : api.candidate;
        gb_rtc_result result;
        do {
          result = call(peers[1-source], input, static_cast<uint32_t>(event.bytes.size()));
          trace.Operation(1-source, event.type == GB_RTC_EVENT_DESCRIPTION ? RtcConnectionDiagnostics::Remote : RtcConnectionDiagnostics::Candidate, result);
          if (result == GB_RTC_BACKPRESSURE) Sleep(1);
        } while (result == GB_RTC_BACKPRESSURE && GetTickCount64() < deadline);
        std::memset(input, 0xff, event.bytes.size()); CHECK(VirtualFree(input, 0, MEM_RELEASE));
        CHECK(result == GB_RTC_OK);
        if (event.type == GB_RTC_EVENT_CANDIDATE && trace.peers[source].candidate_end)
          end_forwarded[source] = true;
        if (event.type == GB_RTC_EVENT_DESCRIPTION && source == 0) {
          const auto answer = api.answer(peers[1]);
          trace.Operation(1, RtcConnectionDiagnostics::Answer, answer);
          CHECK(answer == GB_RTC_OK);
        }
      }
    }
    Sleep(1);
  }
  const auto videoBytes = RtcMediaFixture();
  trace.phase = RtcConnectionDiagnostics::FirstVideo;
  trace.Print(0); trace.Print(1);
  const uint8_t audioBytes[]{0xf8,0xff,0xfe}, controlBytes[]{1,2,3,4}, pointerBytes[]{4,3,2,1};
  // Free the caller's allocation as soon as each ABI submission returns.
  auto* borrowed=static_cast<uint8_t*>(VirtualAlloc(nullptr,videoBytes.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
  CHECK(borrowed);std::memcpy(borrowed,videoBytes.data(),videoBytes.size());
  gb_rtc_video video{sizeof(video),GB_RTC_ABI_VERSION,borrowed,static_cast<uint32_t>(videoBytes.size()),900,1920,1080,1,{}};
  CHECK(api.video(peers[0],&video)==GB_RTC_OK);
  std::memset(borrowed,0xff,videoBytes.size());CHECK(VirtualFree(borrowed,0,MEM_RELEASE));
  // Cold-start regression: the first and only submitted video AU must arrive
  // before any second media/data call can accidentally kick the native engine.
  // This is the full-transport counterpart of the pinned injector unit test.
  const auto mediaDeadline=GetTickCount64()+5000;
  unsigned videoCount{};
  while(videoCount!=1) {
    CHECK(GetTickCount64()<mediaDeadline);
    std::vector<PeerEvents::Payload> pending;
    {std::lock_guard lock(events[1].mutex);pending.swap(events[1].pending);}
    for(const auto& event:pending) {
      trace.Event(1, event.type, std::string_view(event.bytes.empty() ? "" : reinterpret_cast<const char*>(event.bytes.data()), event.bytes.size()));
      CHECK(event.type!=GB_RTC_EVENT_ERROR);
      if(event.type!=GB_RTC_EVENT_VIDEO)continue;
      ++videoCount;
      CHECK(event.bytes.size()==sizeof(gb_rtc_media_event)+videoBytes.size());
      gb_rtc_media_event header{};std::memcpy(&header,event.bytes.data(),sizeof(header));
      CHECK(header.size==16&&header.abi_version==1&&header.timestamp==900&&header.data_size==videoBytes.size());
      CHECK(std::memcmp(event.bytes.data()+16,videoBytes.data(),videoBytes.size())==0);
    }
    Sleep(1);
  }
  gb_rtc_audio audio{sizeof(audio),GB_RTC_ABI_VERSION,audioBytes,sizeof(audioBytes),480,{}};
  trace.phase = RtcConnectionDiagnostics::MediaData;
  CHECK(api.audio(peers[0],&audio)==GB_RTC_OK);
  for (uint32_t kind : {GB_RTC_CHANNEL_RELIABLE,GB_RTC_CHANNEL_POINTER}) {
    gb_rtc_result result; const auto* bytes=kind==GB_RTC_CHANNEL_RELIABLE?controlBytes:pointerBytes;
    do {result=api.data(peers[0],kind,bytes,4);if(result==GB_RTC_STATE||result==GB_RTC_BACKPRESSURE)Sleep(1);} while(result!=GB_RTC_OK&&GetTickCount64()<mediaDeadline);
    CHECK(result==GB_RTC_OK);
  }
  unsigned audioCount{},controlCount{},pointerCount{};
  while(videoCount!=1||audioCount!=1||controlCount!=1||pointerCount!=1) {
    CHECK(GetTickCount64()<mediaDeadline);
    std::vector<PeerEvents::Payload> pending;
    {std::lock_guard lock(events[1].mutex);pending.swap(events[1].pending);}
    for(const auto& event:pending) {
      trace.Event(1, event.type, std::string_view(event.bytes.empty() ? "" : reinterpret_cast<const char*>(event.bytes.data()), event.bytes.size()));
      CHECK(event.type!=GB_RTC_EVENT_ERROR);
      if(event.type==GB_RTC_EVENT_VIDEO||event.type==GB_RTC_EVENT_AUDIO) {
        CHECK(event.bytes.size()>=sizeof(gb_rtc_media_event));gb_rtc_media_event header{};std::memcpy(&header,event.bytes.data(),sizeof(header));
        CHECK(header.size==16&&header.abi_version==1&&header.data_size==event.bytes.size()-16);
        if(event.type==GB_RTC_EVENT_VIDEO){++videoCount;CHECK(header.timestamp==900&&header.data_size==videoBytes.size());CHECK(std::memcmp(event.bytes.data()+16,videoBytes.data(),videoBytes.size())==0);}
        else{++audioCount;CHECK(header.timestamp==480&&header.data_size==sizeof(audioBytes));CHECK(std::memcmp(event.bytes.data()+16,audioBytes,sizeof(audioBytes))==0);}
      } else if(event.type==GB_RTC_EVENT_CONTROL){++controlCount;CHECK(event.bytes==std::vector<uint8_t>(controlBytes,controlBytes+4));}
      else if(event.type==GB_RTC_EVENT_POINTER){++pointerCount;CHECK(event.bytes==std::vector<uint8_t>(pointerBytes,pointerBytes+4));}
    }
    Sleep(1);
  }
  trace.phase = RtcConnectionDiagnostics::Closing;
  lifetime.Close();
  const auto first = events[0].callbacks.load(), second = events[1].callbacks.load();
  Sleep(10);
  CHECK(events[0].callbacks == first && events[1].callbacks == second);
  trace.phase = RtcConnectionDiagnostics::Complete;
  return 0;
}

int main(int argc, char** argv) {
  CHECK(argc == 3); // Production and instrumented copies run in separate processes.
  const bool testing = std::strcmp(argv[2], "probe") == 0;
  Api api; CHECK(api.load(argv[1], testing));
  CHECK(gb_rtc_c_layout());
  const char ice[] = "{}";
  gb_rtc_config config{sizeof(config), GB_RTC_ABI_VERSION, ice, 2, 0};
  CHECK(connect_native_peers(api, config) == 0);
  gb_rtc_handle handle = 99;
  CHECK(api.create(nullptr, nullptr, nullptr, &handle) == GB_RTC_INVALID && handle == 0);
  CHECK(api.create(&config, nullptr, nullptr, nullptr) == GB_RTC_INVALID);
  auto invalid = config; invalid.size--;
  CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID);
  invalid = config; invalid.abi_version++;
  CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID);
  invalid = config; invalid.ice_json_utf8 = nullptr;
  CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID);
  invalid = config; invalid.ice_json_size = GB_RTC_MAX_ICE_BYTES + 1;
  CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID);
  invalid = config; invalid.reserved = 1;
  CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID);
  invalid = config; invalid.ice_json_size = 0;
  CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID);
  for (const char* malformed : {"{", "null", "[]", "{}{}", "\xff", "{\"iceServers\":[{\"urls\":\"https://invalid\"}]}"}) {
    invalid = config; invalid.ice_json_utf8 = malformed; invalid.ice_json_size = static_cast<uint32_t>(std::strlen(malformed));
    CHECK(api.create(&invalid, nullptr, nullptr, &handle) == GB_RTC_INVALID && handle == 0);
  }
  CHECK(api.create(&config, nullptr, nullptr, &handle) == GB_RTC_OK && handle != 0);
  CHECK(api.answer(handle) == GB_RTC_STATE);
  CHECK(api.offer(handle) == GB_RTC_OK && api.offer(handle) == GB_RTC_STATE);
  CHECK(api.video(handle, nullptr) == GB_RTC_INVALID && api.audio(handle, nullptr) == GB_RTC_INVALID);
  uint8_t videoBytes[]{0,0,0,1,0x65,3};
  gb_rtc_video video{sizeof(video), GB_RTC_ABI_VERSION, videoBytes, sizeof(videoBytes), 900, 2, 2, 1, {}};
  auto badVideo = video; badVideo.data_size = GB_RTC_MAX_VIDEO_BYTES + 1;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.abi_version++;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.size--;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.keyframe = 2;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.data = nullptr;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.data_size = 0;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.width = 0;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  badVideo = video; badVideo.reserved[6] = 1;
  CHECK(api.video(handle, &badVideo) == GB_RTC_INVALID);
  CHECK(api.video(handle, &video) == GB_RTC_OK);
  CHECK(api.video(handle, &video) == GB_RTC_OK); // latest pending AU replaces old
  uint8_t audioBytes[]{0xf8,0xff,0xfe};
  gb_rtc_audio audio{sizeof(audio), GB_RTC_ABI_VERSION, audioBytes, sizeof(audioBytes), 48, {}};
  auto badAudio = audio; badAudio.size--;
  CHECK(api.audio(handle, &badAudio) == GB_RTC_INVALID);
  badAudio = audio; badAudio.abi_version++;
  CHECK(api.audio(handle, &badAudio) == GB_RTC_INVALID);
  badAudio = audio; badAudio.data = nullptr;
  CHECK(api.audio(handle, &badAudio) == GB_RTC_INVALID);
  badAudio = audio; badAudio.data_size = GB_RTC_MAX_AUDIO_BYTES + 1;
  CHECK(api.audio(handle, &badAudio) == GB_RTC_INVALID);
  badAudio = audio; badAudio.reserved[1] = 1;
  CHECK(api.audio(handle, &badAudio) == GB_RTC_INVALID);
  for (unsigned i = 0; i < GB_RTC_AUDIO_CAPACITY; ++i) CHECK(api.audio(handle, &audio) == GB_RTC_OK);
  CHECK(api.audio(handle, &audio) == GB_RTC_OK); // oldest complete packets dropped
  CHECK(api.data(handle,GB_RTC_CHANNEL_RELIABLE,audioBytes,sizeof(audioBytes))==GB_RTC_STATE);
  CHECK(api.data(handle,0,audioBytes,sizeof(audioBytes))==GB_RTC_INVALID);
  CHECK(api.data(handle,GB_RTC_CHANNEL_POINTER,audioBytes,GB_RTC_MAX_POINTER_BYTES+1)==GB_RTC_INVALID);
  CHECK(api.data(handle,GB_RTC_CHANNEL_RELIABLE,nullptr,1)==GB_RTC_INVALID);
  const char description[] = "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\n\"}";
  const char candidate[] = "{\"candidate\":\"candidate:1 1 udp 1 127.0.0.1 1234 typ host\"}";
  CHECK(api.remote(handle, nullptr, 1) == GB_RTC_INVALID);
  CHECK(api.remote(handle, reinterpret_cast<const uint8_t*>("{"), 1) == GB_RTC_INVALID);
  CHECK(api.remote(handle, reinterpret_cast<const uint8_t*>(description), GB_RTC_MAX_DESCRIPTION_BYTES + 1) == GB_RTC_INVALID);
  CHECK(api.remote(handle, reinterpret_cast<const uint8_t*>(description), sizeof(description)-1) == GB_RTC_INVALID);
  CHECK(api.candidate(handle, nullptr, 1) == GB_RTC_INVALID);
  CHECK(api.candidate(handle, reinterpret_cast<const uint8_t*>(candidate), GB_RTC_MAX_CANDIDATE_BYTES + 1) == GB_RTC_INVALID);
  CHECK(api.candidate(handle, reinterpret_cast<const uint8_t*>("{}"), 2) == GB_RTC_INVALID);
  for (unsigned i = 0; i < GB_RTC_CANDIDATE_CAPACITY; ++i) CHECK(api.candidate(handle, reinterpret_cast<const uint8_t*>(candidate), sizeof(candidate)-1) == GB_RTC_OK);
  CHECK(api.candidate(handle, reinterpret_cast<const uint8_t*>(candidate), sizeof(candidate)-1) == GB_RTC_BACKPRESSURE);
  api.close(handle); api.close(handle); api.close(0);
  CHECK(api.video(handle, &video) == GB_RTC_STATE);
  CHECK(api.audio(handle, &audio) == GB_RTC_STATE);
  CHECK(api.data(handle,GB_RTC_CHANNEL_RELIABLE,audioBytes,sizeof(audioBytes))==GB_RTC_STATE);
  CHECK(api.remote(handle, reinterpret_cast<const uint8_t*>(description), sizeof(description)-1) == GB_RTC_STATE);
  CHECK(api.candidate(handle, reinterpret_cast<const uint8_t*>(candidate), sizeof(candidate)-1) == GB_RTC_STATE);

  for (int i = 0; i < 10000; ++i) {
    gb_rtc_handle next{}; CHECK(api.create(&config, nullptr, nullptr, &next) == GB_RTC_OK);
    CHECK(next != handle && api.video(handle, &video) == GB_RTC_STATE && api.audio(handle, &audio) == GB_RTC_STATE);
    api.close(handle); // A stale close must not close the reused slot.
    CHECK(api.video(next, &video) == GB_RTC_OK);
    api.close(next); api.close(next); handle = next;
  }
  std::atomic<int> failures{};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < 8; ++thread) workers.emplace_back([&] {
    for (int i = 0; i < 500; ++i) {
      gb_rtc_handle value{};
      if (api.create(&config, nullptr, nullptr, &value) != GB_RTC_OK) { ++failures; continue; }
      std::thread reader([&] { const auto result = api.video(value, &video); if (result != GB_RTC_OK && result != GB_RTC_STATE && result != GB_RTC_BACKPRESSURE) ++failures; });
      api.close(value); reader.join(); api.close(value);
    }
  });
  for (auto& worker : workers) worker.join();
  CHECK(failures == 0);
  std::vector<gb_rtc_handle> bounded(GB_RTC_MAX_SESSIONS);
  for (auto& value : bounded) CHECK(api.create(&config, nullptr, nullptr, &value) == GB_RTC_OK);
  CHECK(api.create(&config, nullptr, nullptr, &handle) == GB_RTC_BACKPRESSURE);
  for (auto value : bounded) api.close(value);

  if (testing) {
    CallbackState copy; copy.api = &api;
    CHECK(api.create(&config, callback, &copy, &copy.handle) == GB_RTC_OK);
    // OS-backed caller allocation is released before probing the Go-owned queue.
    auto* borrowed = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(borrowed); std::memcpy(borrowed, videoBytes, sizeof(videoBytes));
    video.data = borrowed;
    CHECK(api.video(copy.handle, &video) == GB_RTC_OK);
    std::memset(borrowed, 0xff, sizeof(videoBytes)); CHECK(VirtualFree(borrowed, 0, MEM_RELEASE));
    CHECK(api.probe(copy.handle, 1) == GB_RTC_OK);
    CHECK(copy.observed == std::vector<uint8_t>(videoBytes, videoBytes + sizeof(videoBytes)));
    borrowed = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(borrowed); std::memcpy(borrowed, audioBytes, sizeof(audioBytes));
    audio.data = borrowed; CHECK(api.audio(copy.handle, &audio) == GB_RTC_OK);
    std::memset(borrowed, 0xff, sizeof(audioBytes)); CHECK(VirtualFree(borrowed, 0, MEM_RELEASE));
    CHECK(api.probe(copy.handle, 2) == GB_RTC_OK);
    CHECK(copy.observed == std::vector<uint8_t>(audioBytes, audioBytes + sizeof(audioBytes)));
    CHECK(api.probe(copy.handle, 99) == GB_RTC_INTERNAL); // Injected Go panic stays inside ABI.
    api.close(copy.handle); CHECK(api.probe(copy.handle, 1) == GB_RTC_STATE);
    video.data = videoBytes;

    CallbackState blocked; blocked.api = &api; blocked.block = true;
    CHECK(api.create(&config, callback, &blocked, &blocked.handle) == GB_RTC_OK);
    std::thread emitter([&] { if (api.probe(blocked.handle, 0) != GB_RTC_OK) ++failures; });
    CHECK(blocked.entered.wait());
    std::atomic<int> closeReturned{};
    std::thread closer1([&] { api.close(blocked.handle); ++closeReturned; });
    std::thread closer2([&] { api.close(blocked.handle); ++closeReturned; });
    Sleep(25); CHECK(closeReturned == 0);
    blocked.release.set(); emitter.join(); closer1.join(); closer2.join();
    CHECK(closeReturned == 2 && blocked.errors == 0 && failures == 0);
    CHECK(api.probe(blocked.handle, 0) == GB_RTC_STATE && blocked.calls == 1);

    CallbackState reentrant; reentrant.api = &api; reentrant.reenter = true; reentrant.block = true;
    CHECK(api.create(&config, callback, &reentrant, &reentrant.handle) == GB_RTC_OK);
    std::thread selfEmitter([&] { if (api.probe(reentrant.handle, 0) != GB_RTC_OK) ++failures; });
    CHECK(reentrant.entered.wait());
    // The callback closes itself, then remains blocked. A second close from
    // outside must still find the handle and wait for borrowed data to unwind.
    std::atomic<bool> selfCloseReturned{};
    std::thread selfCloser([&] { api.close(reentrant.handle); selfCloseReturned = true; });
    Sleep(25); CHECK(!selfCloseReturned);
    reentrant.release.set(); selfEmitter.join(); selfCloser.join();
    CHECK(reentrant.calls == 1 && reentrant.errors == 0);
    CHECK(api.probe(reentrant.handle, 0) == GB_RTC_STATE);
    api.close(reentrant.handle);

    CallbackState left, right; left.api = right.api = &api; left.block = right.block = true;
    CHECK(api.create(&config, callback, &left, &left.handle) == GB_RTC_OK);
    CHECK(api.create(&config, callback, &right, &right.handle) == GB_RTC_OK);
    left.closeOther = right.handle; right.closeOther = left.handle;
    std::thread leftEmitter([&] { if (api.probe(left.handle, 0) != GB_RTC_OK) ++failures; });
    std::thread rightEmitter([&] { if (api.probe(right.handle, 0) != GB_RTC_OK) ++failures; });
    CHECK(left.entered.wait() && right.entered.wait());
    left.release.set(); right.release.set();
    CHECK(left.finished.wait(2000) && right.finished.wait(2000));
    leftEmitter.join(); rightEmitter.join();
    CHECK(left.errors == 0 && right.errors == 0 && failures == 0);
    CHECK(api.probe(left.handle, 0) == GB_RTC_STATE && api.probe(right.handle, 0) == GB_RTC_STATE);
    CHECK(api.video(left.handle, &video) == GB_RTC_STATE && api.video(right.handle, &video) == GB_RTC_STATE);
    api.close(left.handle); api.close(right.handle);

    // Callback-originated close must also retire a target that has no event
    // callback/dispatcher of its own, without needing a second external close.
    CallbackState closer; closer.api = &api;
    gb_rtc_handle silent{};
    CHECK(api.create(&config,nullptr,nullptr,&silent)==GB_RTC_OK);
    CHECK(api.create(&config,callback,&closer,&closer.handle)==GB_RTC_OK);
    closer.closeOther=silent;
    CHECK(api.probe(closer.handle,0)==GB_RTC_OK);
    api.close(closer.handle);
    CHECK(api.video(silent,&video)==GB_RTC_STATE);
    const auto retirementDeadline=GetTickCount64()+2000;
    bool retired=false;
    do {
      std::vector<gb_rtc_handle> capacity;
      for(unsigned i=0;i<GB_RTC_MAX_SESSIONS;++i) {
        gb_rtc_handle value{};const auto result=api.create(&config,nullptr,nullptr,&value);
        if(result==GB_RTC_BACKPRESSURE)break;
        CHECK(result==GB_RTC_OK);capacity.push_back(value);
      }
      retired=capacity.size()==GB_RTC_MAX_SESSIONS;
      for(auto value:capacity)api.close(value);
      if(!retired)Sleep(1);
    }while(!retired&&GetTickCount64()<retirementDeadline);
    CHECK(retired);
  }
  std::puts("rtc_abi: PASS (versioned C/C++ layouts, bounded copies, malformed input, stale handles, 10000 cycles, concurrency)");
  return 0;
}
