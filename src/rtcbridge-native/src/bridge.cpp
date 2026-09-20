#define GB_RTC_BUILD
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/candidate.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/frame_transformer_factory.h"
#include "api/make_ref_counted.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "core.h"
#include "selected_pair.h"
#include "json.h"
#ifdef GB_RTC_BENCH
#include "transport_diagnostics.h"
#include "network_diagnostics.h"
#endif
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <thread>
#include <windows.h>
#include <winsock2.h>

namespace gamebridge::rtc {
namespace w = webrtc;
using PC = w::PeerConnectionInterface;
using namespace std::chrono_literals;
class Session;
HandleTable<Session> sessions;

struct NetworkConfig {
  bool direct_only{};
  uint16_t udp_port_min{}, udp_port_max{};
  bool external_ipv4_present{};
  std::array<uint8_t, 4> external_ipv4{};
  std::string ExternalIPv4() const {
    return std::to_string(external_ipv4[0]) + "." +
           std::to_string(external_ipv4[1]) + "." +
           std::to_string(external_ipv4[2]) + "." +
           std::to_string(external_ipv4[3]);
  }
};

bool PublicIPv4(const std::array<uint8_t, 4> &address) {
  const auto a = address[0], b = address[1], c = address[2];
  return a != 0 && a != 10 && a != 127 && a < 224 &&
         !(a == 100 && b >= 64 && b <= 127) &&
         !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
         !(a == 192 && b == 0 && (c == 0 || c == 2)) &&
         !(a == 192 && b == 88 && c == 99) &&
         !(a == 192 && b == 168) &&
         !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
         !(a == 203 && b == 0 && c == 113);
}

bool HasForbiddenSDPCandidate(std::string_view sdp) {
  for (size_t start = 0; start < sdp.size();) {
    const auto end = sdp.find_first_of("\r\n", start);
    const auto line = sdp.substr(start, end - start);
    if (line.starts_with("a=candidate:")) {
      auto candidate =
          w::Candidate::ParseCandidateString(std::string(line.substr(2)));
      if (candidate.ok() && candidate.value().is_relay())
        return true;
    }
    if (end == std::string_view::npos)
      break;
    start = sdp.find_first_not_of("\r\n", end);
    if (start == std::string_view::npos)
      break;
  }
  return false;
}

// Process-lived because DLL unloading while upstream worker threads exist is
// unsupported. No system audio device is ever opened: encoded Opus is injected.
struct Runtime {
  // Windows requires this before CreateWithSocketServer, not merely before
  // the first SDP exchange. Keep it alive as long as every upstream thread.
  w::WinsockInitializer winsock;
  std::unique_ptr<w::Thread> network = w::Thread::CreateWithSocketServer();
  std::unique_ptr<w::Thread> worker = w::Thread::Create();
  std::unique_ptr<w::Thread> signaling = w::Thread::Create();
  w::scoped_refptr<w::PeerConnectionFactoryInterface> factory;
  Runtime() {
    w::LogMessage::LogToDebug(w::LS_NONE);
    w::LogMessage::SetLogToStderr(false);
    if (winsock.error() != 0 || !w::InitializeSSL() || !network->Start() ||
        !worker->Start() || !signaling->Start())
      throw std::runtime_error("rtc_init");
    auto environment = w::CreateEnvironment();
    auto audio = w::CreateAudioDeviceModule(environment,
                                            w::AudioDeviceModule::kDummyAudio);
    factory = w::CreatePeerConnectionFactory(
        network.get(), worker.get(), signaling.get(), audio,
        w::CreateBuiltinAudioEncoderFactory(),
        w::CreateBuiltinAudioDecoderFactory(),
        w::CreateBuiltinVideoEncoderFactory(),
        w::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);
    if (!factory)
      throw std::runtime_error("rtc_factory");
  }
};
Runtime &Engine() {
  static auto *runtime = new Runtime;
  return *runtime;
}

class LocalObserver : public w::SetLocalDescriptionObserverInterface {
public:
  explicit LocalObserver(std::function<void(w::RTCError)> done)
      : done_(std::move(done)) {}
  void OnSetLocalDescriptionComplete(w::RTCError error) override {
    done_(std::move(error));
  }

private:
  std::function<void(w::RTCError)> done_;
};
class RemoteObserver : public w::SetRemoteDescriptionObserverInterface {
public:
  explicit RemoteObserver(std::function<void(w::RTCError)> done)
      : done_(std::move(done)) {}
  void OnSetRemoteDescriptionComplete(w::RTCError error) override {
    done_(std::move(error));
  }

private:
  std::function<void(w::RTCError)> done_;
};
class StatsObserver : public w::RTCStatsCollectorCallback {
public:
  explicit StatsObserver(std::function<void(const w::RTCStatsReport &)> done)
      : done_(std::move(done)) {}
  void OnStatsDelivered(
      const w::scoped_refptr<const w::RTCStatsReport> &report) override {
    done_(*report);
  }

private:
  std::function<void(const w::RTCStatsReport &)> done_;
};

class Transformer : public w::FrameTransformerInterface {
public:
  explicit Transformer(
      std::function<void(std::unique_ptr<w::TransformableFrameInterface>,
                         w::scoped_refptr<w::TransformedFrameCallback>)>
          action)
      : action_(std::move(action)) {}
  void
  Transform(std::unique_ptr<w::TransformableFrameInterface> frame) override {
    w::scoped_refptr<w::TransformedFrameCallback> sink;
    {
      std::lock_guard lock(mutex_);
      sink = sink_;
    }
    action_(std::move(frame), std::move(sink));
  }
  void RegisterTransformedFrameCallback(
      w::scoped_refptr<w::TransformedFrameCallback> sink) override {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
  }
  void RegisterTransformedFrameSinkCallback(
      w::scoped_refptr<w::TransformedFrameCallback> sink, uint32_t) override {
    RegisterTransformedFrameCallback(std::move(sink));
  }
  void UnregisterTransformedFrameCallback() override {
    std::lock_guard lock(mutex_);
    sink_ = nullptr;
  }
  void UnregisterTransformedFrameSinkCallback(uint32_t) override {
    UnregisterTransformedFrameCallback();
  }

private:
  std::mutex mutex_;
  w::scoped_refptr<w::TransformedFrameCallback> sink_;
  std::function<void(std::unique_ptr<w::TransformableFrameInterface>,
                     w::scoped_refptr<w::TransformedFrameCallback>)>
      action_;
};

struct Event {
  uint32_t type;
  std::vector<uint8_t> bytes;
};
class ChannelObserver : public w::DataChannelObserver {
public:
  ChannelObserver(std::weak_ptr<Session> owner, unsigned channel)
      : owner_(std::move(owner)), channel_(channel) {}
  void OnStateChange() override;
  void OnMessage(const w::DataBuffer &buffer) override;

private:
  std::weak_ptr<Session> owner_;
  unsigned channel_;
};

class Session : public w::PeerConnectionObserver,
                public std::enable_shared_from_this<Session> {
public:
  Session(std::vector<IceServer> ice, NetworkConfig network,
          gb_rtc_event_cb cb, void *context)
      : ice_(std::move(ice)), network_(std::move(network)), callback_(cb),
        context_(context) {}
  gb_rtc_handle handle{};
  MediaQueue video{1, 0}, audio{80, 9600}, received_video{1, 0},
      received_audio{80, 9600};
  std::atomic<bool> closed{}, failed{};
  std::atomic<uint64_t> receive_contention[2]{};
  CallbackGate callbacks;
  std::function<void(uint32_t)>
      dequeue_observer; // populated only by benchmark DLL
#ifdef GB_RTC_BENCH
  std::atomic<uint64_t> video_injections{}, video_sender_transforms{},
      video_receiver_transforms{}, video_bitrate_updates{};
  std::atomic<int64_t> allocated_bitrate{}, bandwidth_allocation{};
  std::mutex diagnostic_mutex;
  TransportSnapshot diagnostic_stats;
  std::shared_ptr<DirectDiagnostics> direct_diagnostics=std::make_shared<DirectDiagnostics>();
#endif
#ifdef GB_RTC_TESTING
  gb_rtc_result Probe(uint32_t kind, std::span<const uint8_t> bytes) {
    return callbacks.Invoke([&] {
      if (callback_)
        callback_(context_, 0x80000000u + kind, bytes.data(),
                  uint32_t(bytes.size()));
    })
               ? GB_RTC_OK
               : GB_RTC_STATE;
  }
#endif

  void StartCallbacks() {
    if (!callback_)
      return;
    std::thread([self = shared_from_this()] { self->Dispatch(); }).detach();
    Queue(GB_RTC_EVENT_STATE, "{\"state\":1}");
    Queue(GB_RTC_EVENT_BITRATE, "{\"bitsPerSecond\":10000000}");
  }
  void Close() {
#ifdef GB_RTC_BENCH
    direct_diagnostics->Terminal(ProofEvidence::Other);
#endif
    if (network_.direct_only)
      direct_route_gate_.Close();
    closed = true;
    callbacks.Close();
    video.Close();
    audio.Close();
    received_video.Close();
    received_audio.Close();
    wake_.notify_all();
    if (callback_depth) {
      // A callback may close a *different* session with a null callback. Such
      // a session has no dispatcher to finish retirement, so schedule exactly
      // one external cleanup. The callback itself must never wait for it.
      if (!callback_ && !cleanup_scheduled_.exchange(true))
        std::thread([self = shared_from_this()] { self->Close(); }).detach();
      return;
    }
    StopPeer();
    if (callback_) {
      std::unique_lock lock(events_mutex_);
      wake_.wait(lock, [&] { return dispatcher_done_; });
    }
    sessions.Erase(handle);
  }
  void StopPeer() {
    std::lock_guard lock(stop_mutex_);
    if (!peer_started_)
      return;
    Engine().signaling->BlockingCall([&] {
      for (auto &channel : channels_)
        if (channel) {
          channel->UnregisterObserver();
          channel->Close();
          channel = nullptr;
        }
      video_injector_ = nullptr;
      audio_injector_ = nullptr;
      if (pc_) {
        pc_->Close();
        pc_ = nullptr;
      }
    });
    peer_started_ = false;
  }
  void Fail(std::string_view code) {
    if (closed)
      return;
#ifdef GB_RTC_BENCH
    direct_diagnostics->Terminal(code=="direct_route_unproven"?ProofEvidence::Timeout:
      code=="relay_forbidden"?ProofEvidence::Relay:ProofEvidence::Other);
#endif
    if (network_.direct_only)
      direct_route_gate_.Close();
    if (failed.exchange(true))
      return;
    {
      std::lock_guard lock(events_mutex_);
      signaling_.clear();
      candidates_.clear();
      control_.clear();
      latest_.clear();
      const auto text = "{\"code\":" + JsonString(code) + "}";
      fatal_ = Event{GB_RTC_EVENT_ERROR, {text.begin(), text.end()}};
    }
    wake_.notify_one();
    if (peer_started_)
      Engine().signaling->PostTask([self = shared_from_this()] {
        if (self->pc_)
          self->pc_->Close();
      });
  }
  void Queue(uint32_t type, std::string_view text) {
    Queue(type, std::span(reinterpret_cast<const uint8_t *>(text.data()),
                          text.size()));
  }
  void Queue(uint32_t type, std::span<const uint8_t> bytes) {
    if (!callback_ || closed || failed)
      return;
    const size_t maximum = type == GB_RTC_EVENT_DESCRIPTION
                               ? GB_RTC_MAX_DESCRIPTION_BYTES
                               : GB_RTC_MAX_CANDIDATE_BYTES;
    if (bytes.size() > maximum) {
      Fail("event_oversize");
      return;
    }
    bool overflow = false;
    {
      std::lock_guard lock(events_mutex_);
      if (closed || failed)
        return;
      Event event{type, {bytes.begin(), bytes.end()}};
      if (type == GB_RTC_EVENT_CANDIDATE) {
        if (candidates_.size() == 128)
          overflow = true;
        else
          candidates_.push_back(std::move(event));
      } else if (type == GB_RTC_EVENT_CONTROL) {
        if (control_.size() == 64)
          overflow = true;
        else
          control_.push_back(std::move(event));
      } else if (type == GB_RTC_EVENT_DESCRIPTION ||
                 type == GB_RTC_EVENT_STATE || type == GB_RTC_EVENT_GATHERING) {
        if (signaling_.size() == 32)
          overflow = true;
        else
          signaling_.push_back(std::move(event));
      } else {
        if (type == GB_RTC_EVENT_POINTER && latest_.contains(type))
          ++pointer_drops_;
        latest_.insert_or_assign(type, std::move(event));
      }
    }
    if (overflow)
      Fail(type == GB_RTC_EVENT_CONTROL ? "control_overflow"
                                        : "event_overflow");
    wake_.notify_one();
  }
  void Metrics() {
    Queue(GB_RTC_EVENT_METRICS,
          "{\"frames_dropped_bridge\":" + std::to_string(video.Dropped()) +
              ",\"audio_packets_dropped_bridge\":" +
              std::to_string(audio.Dropped()) +
              ",\"frames_dropped_receiver\":" +
              std::to_string(
                  ReceiverDrops(received_video, receive_contention[0])) +
              ",\"audio_packets_dropped_receiver\":" +
              std::to_string(
                  ReceiverDrops(received_audio, receive_contention[1])) +
              ",\"pointer_messages_dropped_receiver\":" +
              std::to_string(pointer_drops_.load()) + "}");
  }
  void WakeMedia() {
    if (closed || failed || !connected_ ||
        (network_.direct_only && !direct_route_gate_.Proven()) ||
        scheduled_.exchange(true))
      return;
    Engine().signaling->PostTask(
        [self = shared_from_this()] { self->DrainMedia(); });
  }
  gb_rtc_result Description(bool offer) {
    std::unique_lock lock(operation_, std::try_to_lock);
    if (!lock.owns_lock())
      return GB_RTC_BACKPRESSURE;
    if (closed || failed)
      return GB_RTC_STATE;
    auto promise = std::make_shared<std::promise<gb_rtc_result>>();
    auto future = promise->get_future();
    Engine().signaling->PostTask([self = shared_from_this(), offer, promise] {
      if (!self->EnsurePeer()) {
        self->Fail("connection_failed");
        promise->set_value(GB_RTC_INTERNAL);
        return;
      }
      auto expected = offer ? PC::kStable : PC::kHaveRemoteOffer;
      if (self->closed || self->failed ||
          self->pc_->signaling_state() != expected) {
        promise->set_value(GB_RTC_STATE);
        return;
      }
      auto observer = w::make_ref_counted<LocalObserver>(
          [self, promise](w::RTCError error) {
            if (!error.ok() || self->closed || !self->pc_) {
              promise->set_value(GB_RTC_STATE);
              return;
            }
            const auto *local = self->pc_->local_description();
            std::string sdp;
            if (!local || !local->ToString(&sdp)) {
              self->Fail("description_rejected");
              promise->set_value(GB_RTC_INTERNAL);
              return;
            }
            self->Queue(GB_RTC_EVENT_DESCRIPTION,
                        "{\"type\":" + JsonString(local->type()) +
                            ",\"sdp\":" + JsonString(sdp) + "}");
            promise->set_value(GB_RTC_OK);
          });
      self->pc_->SetLocalDescription(observer);
    });
    if (future.wait_for(10s) != std::future_status::ready) {
      Fail("description_rejected");
      return GB_RTC_INTERNAL;
    }
    return future.get();
  }
  gb_rtc_result Remote(const Json &json) {
    if (json.kind != Json::Object || json.object.size() != 2 ||
        !json.object.contains("type") || !json.object.contains("sdp"))
      return GB_RTC_INVALID;
    const auto &type = json.object.at("type");
    const auto &sdp = json.object.at("sdp");
    if (type.kind != Json::String || sdp.kind != Json::String ||
        (type.string != "offer" && type.string != "answer") ||
        sdp.string.find('\0') != sdp.string.npos)
      return GB_RTC_INVALID;
    auto parsed = w::CreateSessionDescription(
        type.string == "offer" ? w::SdpType::kOffer : w::SdpType::kAnswer,
        sdp.string);
    if (!parsed || !parsed->description() ||
        sdp.string.find("a=ice-ufrag:") == sdp.string.npos ||
        sdp.string.find("a=ice-pwd:") == sdp.string.npos)
      return GB_RTC_INVALID;
    if (network_.direct_only && HasForbiddenSDPCandidate(sdp.string)) {
      Fail("relay_forbidden");
      return GB_RTC_INVALID;
    }
    std::unique_lock lock(operation_, std::try_to_lock);
    if (!lock.owns_lock())
      return GB_RTC_BACKPRESSURE;
    if (closed || failed)
      return GB_RTC_STATE;
    auto promise = std::make_shared<std::promise<gb_rtc_result>>();
    auto future = promise->get_future();
    Engine().signaling->PostTask([self = shared_from_this(),
                                  parsed = std::move(parsed),
                                  promise]() mutable {
      if (!self->EnsurePeer()) {
        self->Fail("connection_failed");
        promise->set_value(GB_RTC_INTERNAL);
        return;
      }
      auto observer = w::make_ref_counted<RemoteObserver>(
          [self, promise](w::RTCError error) {
            if (!error.ok()) {
              self->Fail("description_rejected");
              promise->set_value(GB_RTC_INVALID);
              return;
            }
            if (self->closed || !self->pc_) {
              promise->set_value(GB_RTC_STATE);
              return;
            }
            for (auto &candidate : self->early_candidates_)
              if (candidate && !self->pc_->AddIceCandidate(candidate.get())) {
                self->Fail("candidate_rejected");
                promise->set_value(GB_RTC_INVALID);
                return;
              }
            self->early_candidates_.clear();
            self->remote_set_ = true;
            promise->set_value(GB_RTC_OK);
          });
      self->pc_->SetRemoteDescription(std::move(parsed), observer);
    });
    if (future.wait_for(10s) != std::future_status::ready) {
      Fail("description_rejected");
      return GB_RTC_INTERNAL;
    }
    return future.get();
  }
  gb_rtc_result Candidate(const Json &json) {
    if (json.kind != Json::Object || !json.object.contains("candidate"))
      return GB_RTC_INVALID;
    std::string text, mid;
    int index = 0;
    for (const auto &[key, value] : json.object) {
      if (key == "candidate") {
        if (value.kind != Json::String)
          return GB_RTC_INVALID;
        text = value.string;
      } else if (key == "sdpMid" || key == "usernameFragment") {
        if (value.kind != Json::Null && value.kind != Json::String)
          return GB_RTC_INVALID;
        if (key == "sdpMid")
          mid = value.string;
      } else if (key == "sdpMLineIndex") {
        if (value.kind != Json::Null &&
            (value.kind != Json::Integer || value.integer > 65535))
          return GB_RTC_INVALID;
        index = int(value.integer);
      } else
        return GB_RTC_INVALID;
    }
    if (text.find_first_of("\r\n") != text.npos ||
        text.find('\0') != text.npos || mid.find('\0') != mid.npos)
      return GB_RTC_INVALID;
    auto candidate =
        text.empty() ? nullptr : w::IceCandidate::Create(mid, index, text);
    if (!text.empty() && !candidate)
      return GB_RTC_INVALID;
    if (candidate && network_.direct_only && candidate->candidate().is_relay()) {
      Fail("relay_forbidden");
      return GB_RTC_INVALID;
    }
    std::unique_lock lock(operation_, std::try_to_lock);
    if (!lock.owns_lock())
      return GB_RTC_BACKPRESSURE;
    if (closed || failed)
      return GB_RTC_STATE;
    return Engine().signaling->BlockingCall(
        [&, candidate = std::move(candidate)]() mutable -> gb_rtc_result {
          if (closed || failed)
            return GB_RTC_STATE;
          if (!remote_set_) {
            if (early_candidates_.size() == 16)
              return GB_RTC_BACKPRESSURE;
            early_candidates_.push_back(std::move(candidate));
            return GB_RTC_OK;
          }
          if (!candidate)
            // The pinned upstream rejects null candidates (crbug.com/935898).
            // ABI end-of-generation is accepted after prior FIFO candidates;
            // it is not forwarded as an invalid upstream AddIceCandidate.
            return GB_RTC_OK;
          if (!pc_ || !pc_->AddIceCandidate(candidate.get()))
            return GB_RTC_INVALID;
          return GB_RTC_OK;
        });
  }
  gb_rtc_result Data(unsigned kind, std::span<const uint8_t> bytes) {
    if (closed || failed || !connected_ ||
        (network_.direct_only && !direct_route_gate_.Proven()))
      return GB_RTC_STATE;
    std::unique_lock lock(data_mutex_[kind - 1], std::try_to_lock);
    if (!lock.owns_lock())
      return GB_RTC_BACKPRESSURE;
    return Engine().signaling->BlockingCall([&]() -> gb_rtc_result {
      const auto send = [&]() -> gb_rtc_result {
        auto channel = channels_[kind - 1];
        if (!channel || channel->state() != w::DataChannelInterface::kOpen)
          return GB_RTC_STATE;
        const auto maximum = kind == 1 ? 65536u : 4096u;
        if (channel->buffered_amount() + bytes.size() > maximum)
          return GB_RTC_BACKPRESSURE;
        return channel->Send(w::DataBuffer(
                   w::CopyOnWriteBuffer(bytes.data(), bytes.size()), true))
                   ? GB_RTC_OK
                   : GB_RTC_BACKPRESSURE;
      };
      if (!network_.direct_only)
        return send();
      gb_rtc_result result = GB_RTC_STATE;
      direct_route_gate_.Admit([&] { result = send(); });
      return result;
    });
  }
  gb_rtc_result Video(std::span<const uint8_t> bytes, uint32_t timestamp,
                      uint32_t width, uint32_t height, bool keyframe) {
    gb_rtc_result result;
    bool request_keyframe = false;
    const auto submit = [&] {
      const auto before = video.Dropped();
      result = video.Push(bytes, timestamp, 0, width, height, keyframe);
      request_keyframe = video.Dropped() != before;
    };
    if (network_.direct_only) {
      switch (direct_route_gate_.TryAdmit(submit)) {
      case DirectAdmissionResult::Busy:
        return GB_RTC_BACKPRESSURE;
      case DirectAdmissionResult::Denied:
        return GB_RTC_STATE;
      case DirectAdmissionResult::Admitted:
        break;
      }
    } else {
      submit();
    }
    if (request_keyframe)
      Queue(GB_RTC_EVENT_KEYFRAME_REQUEST, "{}");
    if (result == GB_RTC_OK)
      WakeMedia();
    return result;
  }
  gb_rtc_result Audio(std::span<const uint8_t> bytes, uint32_t timestamp,
                      uint32_t duration) {
    gb_rtc_result result;
    const auto submit = [&] {
      result = audio.Push(bytes, timestamp, duration);
    };
    if (network_.direct_only) {
      switch (direct_route_gate_.TryAdmit(submit)) {
      case DirectAdmissionResult::Busy:
        return GB_RTC_BACKPRESSURE;
      case DirectAdmissionResult::Denied:
        return GB_RTC_STATE;
      case DirectAdmissionResult::Admitted:
        break;
      }
    } else {
      submit();
    }
    if (result == GB_RTC_OK)
      WakeMedia();
    return result;
  }
  void QueueData(uint32_t type, std::span<const uint8_t> bytes) {
    if (!callback_ || closed || failed)
      return;
    bool overflow = false, queued = false;
    const auto enqueue = [&] {
      std::lock_guard lock(events_mutex_);
      if (closed || failed)
        return;
      Event event{type, {bytes.begin(), bytes.end()}};
      if (type == GB_RTC_EVENT_CONTROL) {
        if (control_.size() == 64)
          overflow = true;
        else {
          control_.push_back(std::move(event));
          queued = true;
        }
      } else {
        if (latest_.contains(type))
          ++pointer_drops_;
        latest_.insert_or_assign(type, std::move(event));
        queued = true;
      }
    };
    bool admitted = true;
    if (!network_.direct_only) {
      enqueue();
    } else {
      admitted = direct_route_gate_.Admit(enqueue);
    }
    if (!admitted)
      return;
    if (overflow) {
      Fail("control_overflow");
      return;
    }
    if (queued)
      wake_.notify_one();
  }
  void ChannelState(unsigned channel) {
    // The data observer cannot re-enter upstream APIs; reconcile
    // asynchronously.
    Engine().signaling->PostTask([self = shared_from_this(), channel] {
      if (self->closed || self->failed || !self->pc_)
        return;
      auto dc = self->channels_[channel];
      if (!dc)
        return;
      if (dc->state() == w::DataChannelInterface::kClosed ||
          dc->state() == w::DataChannelInterface::kClosing) {
        Engine().signaling->PostDelayedTask(
            [self, channel] {
              if (self->closed || self->failed || !self->pc_)
                return;
              auto state = self->pc_->peer_connection_state();
              if (state == PC::PeerConnectionState::kConnected)
                self->Fail(channel == 0 ? "control_channel_closed"
                                        : "pointer_channel_closed");
            },
            w::TimeDelta::Millis(200));
      }
    });
  }
  void OnSignalingChange(PC::SignalingState) override {}
  void OnDataChannel(w::scoped_refptr<w::DataChannelInterface>) override {
    Fail("channel_failed");
  }
  void OnIceGatheringChange(PC::IceGatheringState state) override {
    const auto mapped = state == PC::kIceGatheringNew         ? 1
                        : state == PC::kIceGatheringGathering ? 2
                                                              : 3;
    Queue(GB_RTC_EVENT_GATHERING, "{\"state\":" + std::to_string(mapped) + "}");
    if (mapped == 3)
      Queue(GB_RTC_EVENT_CANDIDATE, "{\"candidate\":\"\"}");
  }
  void OnIceCandidate(const w::IceCandidate *candidate) override {
    if (network_.direct_only && candidate->candidate().is_relay()) {
      Fail("relay_forbidden");
      return;
    }
    std::string text = candidate->ToString();
    if (network_.direct_only && network_.external_ipv4_present &&
        candidate->candidate().is_local()) {
      auto mapped = candidate->candidate();
      mapped.set_related_address(mapped.address());
      mapped.set_address(w::SocketAddress(network_.ExternalIPv4(),
                                          mapped.address().port()));
      text = w::IceCandidate(candidate->sdp_mid(), candidate->sdp_mline_index(),
                             mapped)
                 .ToString();
    }
    Queue(GB_RTC_EVENT_CANDIDATE,
          "{\"candidate\":" + JsonString(text) +
              ",\"sdpMid\":" + JsonString(candidate->sdp_mid()) +
              ",\"sdpMLineIndex\":" +
              std::to_string(candidate->sdp_mline_index()) + "}");
  }
  void OnConnectionChange(PC::PeerConnectionState) override {
    if (!pc_ || closed)
      return;
    const auto state = pc_->peer_connection_state();
    const auto mapped = state == PC::PeerConnectionState::kNew            ? 1
                        : state == PC::PeerConnectionState::kConnecting   ? 2
                        : state == PC::PeerConnectionState::kConnected    ? 3
                        : state == PC::PeerConnectionState::kDisconnected ? 4
                        : state == PC::PeerConnectionState::kFailed       ? 5
                                                                          : 6;
    connected_ = mapped == 3;
    Queue(GB_RTC_EVENT_STATE, "{\"state\":" + std::to_string(mapped) + "}");
    if (mapped == 5)
      Fail("connection_failed");
    if (mapped == 3) {
      if (network_.direct_only && !direct_proof_) {
        direct_proof_.emplace(GetTickCount64());
        ArmDirectProofDeadline();
      }
      PollStats();
      WakeMedia();
    }
  }
  void
  OnTrack(w::scoped_refptr<w::RtpTransceiverInterface> transceiver) override {
    AttachReceiver(transceiver);
  }
  void PollStats() {
    if (closed || failed || !pc_ || !connected_)
      return;
    const auto weak = weak_from_this();
    auto observer = w::make_ref_counted<StatsObserver>(
        [weak](const w::RTCStatsReport &report) {
          auto self = weak.lock();
          if (!self || self->closed)
            return;

          DirectPairEvidence direct_evidence = DirectPairEvidence::Missing;
#ifdef GB_RTC_BENCH
          bool retained_routine_probe=false;
          ProofEvidence evidence_reason=ProofEvidence::MissingSelectedPair;
          TransportSnapshot diagnostics;
          for (const auto *stats :
               report.GetStatsOfType<w::RTCOutboundRtpStreamStats>())
            diagnostics.ObserveOutbound(*stats);
          for (const auto *stats :
               report.GetStatsOfType<w::RTCInboundRtpStreamStats>())
            diagnostics.ObserveInbound(*stats);
#endif
          for (const auto *transport :
               report.GetStatsOfType<w::RTCTransportStats>()) {
            if (!transport->selected_candidate_pair_id)
              continue;
            const auto *pair = report.GetAs<w::RTCIceCandidatePairStats>(
                *transport->selected_candidate_pair_id);
            if (!pair || !pair->local_candidate_id ||
                !pair->remote_candidate_id) {
              direct_evidence = DirectPairEvidence::Mismatched;
#ifdef GB_RTC_BENCH
              evidence_reason=!pair?ProofEvidence::MissingPairRecord:ProofEvidence::MissingCandidateRecord;
#endif
              continue;
            }
            // Legacy sessions retain their original succeeded-only reporting.
            // Direct-only sessions must inspect complete candidate identities
            // before deciding whether this is a retained routine STUN probe.
            if (!self->network_.direct_only &&
                (!pair->state || *pair->state != "succeeded"))
              continue;
#ifdef GB_RTC_BENCH
            diagnostics.AvailableOutgoingBitrate(
                pair->available_outgoing_bitrate);
#endif
            const auto *local = report.GetAs<w::RTCLocalIceCandidateStats>(
                *pair->local_candidate_id);
            const auto *remote = report.GetAs<w::RTCRemoteIceCandidateStats>(
                *pair->remote_candidate_id);
            if (!local || !remote || !local->candidate_type ||
                !remote->candidate_type) {
              direct_evidence = local && remote
                                    ? DirectPairEvidence::Unconvertible
                                    : DirectPairEvidence::Mismatched;
#ifdef GB_RTC_BENCH
              evidence_reason=local&&remote?ProofEvidence::MissingCandidateType:ProofEvidence::MissingCandidateRecord;
#endif
              continue;
            }
            if (self->network_.direct_only) {
              const auto observed = self->selected_pair_.Observe(
                  {*transport->selected_candidate_pair_id,
                   *pair->local_candidate_id, *pair->remote_candidate_id,
                   *local->candidate_type, *remote->candidate_type},
                  pair->state ? std::string_view(*pair->state) : std::string_view{});
              direct_evidence = observed.evidence;
#ifdef GB_RTC_BENCH
              retained_routine_probe=observed.retained_routine_probe;
              evidence_reason=direct_evidence==DirectPairEvidence::Mismatched?
                ProofEvidence::PairNotSucceeded:ProofEvidence::Other;
#endif
              if (direct_evidence == DirectPairEvidence::Direct ||
                  direct_evidence == DirectPairEvidence::Relay)
                break;
              continue;
            }
            const bool relay = *local->candidate_type == "relay" ||
                               *remote->candidate_type == "relay";
            self->Queue(GB_RTC_EVENT_ROUTE,
                        relay ? "{\"route\":2}" : "{\"route\":1}");
          }
          if (self->network_.direct_only && self->direct_proof_) {
            if (direct_evidence != DirectPairEvidence::Direct)
              self->selected_pair_.Invalidate();
#ifdef GB_RTC_BENCH
            if (direct_evidence != DirectPairEvidence::Direct)
              self->direct_diagnostics->SampleUnproven();
            else if (retained_routine_probe)
              self->direct_diagnostics->SampleRetainedRoutineProbe();
            else
              self->direct_diagnostics->SampleSucceeded();
#endif
            switch (self->direct_proof_->Observe(direct_evidence,
                                                 GetTickCount64())) {
            case DirectProofResult::Proven:
              if (self->MarkDirectRouteProven()) {
#ifdef GB_RTC_BENCH
                self->direct_diagnostics->Proven();
#endif
                self->Queue(GB_RTC_EVENT_ROUTE, "{\"route\":1}");
                self->WakeMedia();
              }
              break;
            case DirectProofResult::Stable:
              break;
            case DirectProofResult::Regressed:
#ifdef GB_RTC_BENCH
              self->direct_diagnostics->Regressed(evidence_reason);
#endif
              self->LoseDirectRoute(DirectProofResult::Regressed);
              self->ArmDirectProofDeadline();
              break;
            case DirectProofResult::Expired:
              self->Fail("direct_route_unproven");
              return;
            case DirectProofResult::Forbidden:
              self->Fail("relay_forbidden");
              return;
            case DirectProofResult::Pending:
              break;
            }
          }
#ifdef GB_RTC_BENCH
          LARGE_INTEGER now{};
          QueryPerformanceCounter(&now);
          diagnostics.sampled_qpc = now.QuadPart;
          {
            std::lock_guard lock(self->diagnostic_mutex);
            self->diagnostic_stats = std::move(diagnostics);
          }
#endif
          self->Metrics();
        });
    pc_->GetStats(observer.get());
    Engine().signaling->PostDelayedTask(
        [weak] {
          if (auto self = weak.lock())
            self->PollStats();
        },
        w::TimeDelta::Seconds(1));
  }

private:
  bool MarkDirectRouteProven() {
    return direct_route_gate_.Prove();
  }
  void LoseDirectRoute(DirectProofResult result) {
    DirectMediaDrops drops;
    direct_route_gate_.Invalidate([&] {
      drops = DiscardMediaForDirectRegression(result, video, audio);
    });
    if (drops.request_keyframe())
      Queue(GB_RTC_EVENT_KEYFRAME_REQUEST, "{}");
  }
  void ArmDirectProofDeadline() {
    if (!direct_proof_)
      return;
    const auto generation = ++direct_proof_generation_;
    const auto deadline = direct_proof_->DeadlineMs();
    const auto now = GetTickCount64();
    const auto delay = deadline > now ? deadline - now : 0;
    const auto weak = weak_from_this();
    Engine().signaling->PostDelayedTask(
        [weak, generation] {
          auto self = weak.lock();
          if (self && !self->closed && !self->failed &&
              generation == self->direct_proof_generation_ &&
              !self->direct_route_gate_.Proven())
            self->Fail("direct_route_unproven");
        },
        w::TimeDelta::Millis(delay));
  }

  bool EnsurePeer() {
    if (closed || failed)
      return false;
    if (pc_)
      return true;
    PC::RTCConfiguration config;
    config.sdp_semantics = w::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = PC::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = PC::kRtcpMuxPolicyRequire;
    if (network_.direct_only) {
      config.set_min_port(network_.udp_port_min);
      config.set_max_port(network_.udp_port_max);
      config.tcp_candidate_policy = PC::kTcpCandidatePolicyDisabled;
      config.continual_gathering_policy = PC::GATHER_ONCE;
    }
    for (const auto &server : ice_) {
      PC::IceServer value;
      value.urls = server.urls;
      value.username = server.username;
      value.password = server.credential;
      config.servers.push_back(std::move(value));
    }
    if (!network_.direct_only &&
        GetEnvironmentVariableW(L"GB_FORCE_RELAY", nullptr, 0))
      config.type = PC::kRelay;
    auto result = Engine().factory->CreatePeerConnectionOrError(
        config, w::PeerConnectionDependencies(this));
    if (!result.ok())
      return false;
    pc_ = result.MoveValue();
    peer_started_ = true;
    pc_->SetAudioPlayout(false);
    pc_->SetAudioRecording(false);
    w::BitrateSettings bitrate;
    bitrate.min_bitrate_bps = 1000000;
    bitrate.start_bitrate_bps = 25000000;
    bitrate.max_bitrate_bps = 40000000;
    if (!pc_->SetBitrate(bitrate).ok())
      return false;
    const auto weak = weak_from_this();
    for (unsigned kind = 0; kind < 2; ++kind) {
      auto media = kind == 0 ? w::MediaType::VIDEO : w::MediaType::AUDIO;
      w::RtpTransceiverInit init;
      if (kind == 0) {
        // The connection limit does not configure the encoding. Otherwise
        // singlecast defaults to 2.5 Mbps even for injected 1080p60 frames.
        // Keep a low floor so congestion control may back off below the
        // application's normal 12-25 Mbps quality range on a slower route.
        w::RtpEncodingParameters encoding;
        encoding.min_bitrate_bps = 1000000;
        encoding.max_bitrate_bps = 25000000;
        encoding.max_framerate = 60;
        init.send_encodings.push_back(encoding);
      }
      auto transceiver_result = pc_->AddTransceiver(media, init);
      if (!transceiver_result.ok())
        return false;
      auto transceiver = transceiver_result.MoveValue();
      std::vector<w::RtpCodecCapability> selected;
      for (auto codec :
           Engine().factory->GetRtpSenderCapabilities(media).codecs) {
        if ((kind == 0 && codec.name == "H264" &&
             codec.parameters["packetization-mode"] == "1" &&
             codec.parameters["profile-level-id"] == "42e01f") ||
            (kind == 1 && codec.name == "opus")) {
          codec.preferred_payload_type = kind == 0 ? 102 : 111;
          selected.push_back(std::move(codec));
          break;
        }
      }
      if (selected.empty() || !transceiver->SetCodecPreferences(selected).ok())
        return false;
      AttachReceiver(transceiver);
      transceiver->sender()->SetFrameTransformer(
          w::make_ref_counted<Transformer>([weak, kind](auto frame, auto sink) {
            auto self = weak.lock();
            if (!self || self->closed || self->failed)
              return;
#ifdef GB_RTC_BENCH
            if (kind == 0)
              ++self->video_sender_transforms;
#endif
            // Upstream adds a random sender timestamp offset. The public bridge
            // ABI carries the caller's exact modulo clock, so overwrite it
            // here.
            frame->SetRTPTimestamp(self->inflight_timestamp_[kind].load());
            if (sink)
              sink->OnTransformedFrame(std::move(frame));
            self->inflight_[kind] = false;
            self->WakeMedia();
          }));
      if (kind == 0)
        video_injector_ =
            transceiver->sender()->CreateEncodedVideoFrameInjector(
                [weak] {
                  if (auto self = weak.lock())
                    self->Queue(GB_RTC_EVENT_KEYFRAME_REQUEST, "{}");
                },
                [weak](int32_t allocated, int32_t bandwidth) {
                  if (auto self = weak.lock()) {
#ifdef GB_RTC_BENCH
                    self->allocated_bitrate = allocated;
                    self->bandwidth_allocation = bandwidth;
                    ++self->video_bitrate_updates;
#else
                    (void)bandwidth;
#endif
                    self->Queue(GB_RTC_EVENT_BITRATE,
                                "{\"bitsPerSecond\":" +
                                    std::to_string(std::clamp(
                                        allocated, 1000000, 40000000)) +
                                    "}");
                  }
                });
      else
        audio_injector_ =
            transceiver->sender()->CreateEncodedAudioFrameInjector(
                [](int32_t) {});
    }
    if (!video_injector_ || !audio_injector_)
      return false;
    for (unsigned i = 0; i < 2; ++i) {
      w::DataChannelInit init;
      init.negotiated = true;
      init.id = int(i);
      init.ordered = i == 0;
      if (i == 1)
        init.maxRetransmits = 0;
      auto result = pc_->CreateDataChannelOrError(
          i == 0 ? "control.reliable" : "pointer.fast", &init);
      if (!result.ok())
        return false;
      channels_[i] = result.MoveValue();
      channel_observers_[i] = std::make_unique<ChannelObserver>(weak, i);
      channels_[i]->RegisterObserver(channel_observers_[i].get());
    }
    return true;
  }
  void AttachReceiver(
      const w::scoped_refptr<w::RtpTransceiverInterface> &transceiver) {
    const bool is_video = transceiver->media_type() == w::MediaType::VIDEO;
    transceiver->receiver()->SetFrameTransformer(
        w::make_ref_counted<Transformer>([weak = weak_from_this(),
                                          is_video](auto frame, auto) {
          auto self = weak.lock();
          if (!self || self->closed || self->failed)
            return;
#ifdef GB_RTC_BENCH
          if (is_video)
            ++self->video_receiver_transforms;
#endif
          const auto bytes = frame->GetData();
          if (bytes.empty() ||
              bytes.size() > (is_video ? GB_RTC_MAX_VIDEO_BYTES
                                       : GB_RTC_MAX_AUDIO_BYTES)) {
            self->Fail("event_oversize");
            return;
          }
          const auto time = frame->GetRtpTimestampInfo();
          if (!std::holds_alternative<w::RtpTimestampWithOffset>(time)) {
            self->Fail("media_write_failed");
            return;
          }
          auto &queue = is_video ? self->received_video : self->received_audio;
          const auto samples = is_video ? 0 : OpusSamples(bytes);
          if (!is_video && !samples) {
            self->Fail("media_write_failed");
            return;
          }
          if (queue.Push(bytes, std::get<w::RtpTimestampWithOffset>(time).value,
                         samples) == GB_RTC_BACKPRESSURE)
            ++self->receive_contention[is_video ? 0 : 1];
          self->wake_.notify_one();
          // Encoded payload is the consumer output. Do not decode, play audio
          // or forward to a raw surface inside this transport-only DLL.
        }));
  }
  void DrainMedia() {
    scheduled_ = false;
    if (closed || failed || !connected_ || !pc_)
      return;
    for (unsigned kind = 0; kind < 2; ++kind) {
      const auto drain = [&] {
        if (inflight_[kind])
          return;
        auto &queue = kind == 0 ? video : audio;
        auto media = queue.Pop();
        if (!media)
          return;
        inflight_[kind] = true;
        inflight_timestamp_[kind] = media->timestamp;
        if (kind == 0) {
#ifdef GB_RTC_BENCH
          ++video_injections;
#endif
          if (dequeue_observer)
            dequeue_observer(media->timestamp);
          auto frame = w::CreateOutgoingVideoFrame(
              media->key ? w::VideoFrameType::kVideoFrameKey
                         : w::VideoFrameType::kVideoFrameDelta,
              w::PayloadType(102), media->timestamp, media->bytes,
              GetTickCount64(), {}, w::kVideoCodecH264, std::nullopt,
              uint16_t(media->width), uint16_t(media->height));
          video_injector_->InjectFrame(std::move(frame));
        } else {
          auto frame = w::CreateOutgoingAudioFrame(
              w::TransformableAudioFrameInterface::FrameType::
                  kAudioFrameSpeech,
              w::PayloadType(111), media->timestamp, media->bytes.data(),
              media->bytes.size(), std::nullopt, 0, {}, "audio/opus",
              std::nullopt, std::nullopt);
          audio_injector_->InjectFrame(std::move(frame));
        }
        queue.Recycle(std::move(*media));
      };
      if (network_.direct_only) {
        if (!direct_route_gate_.Admit(drain))
          return;
      } else {
        drain();
      }
    }
  }
  void Dispatch() {
    while (!closed) {
      std::optional<Event> event;
      {
        std::unique_lock lock(events_mutex_);
        // A short bounded wait also drains media queues without an unbounded
        // scheduler queue per received frame.
        wake_.wait_for(lock, 2ms);
        if (closed)
          break;
        if (fatal_) {
          event = std::move(fatal_);
          fatal_.reset();
        } else if (!signaling_.empty()) {
          event = std::move(signaling_.front());
          signaling_.pop_front();
        } else if (!candidates_.empty()) {
          event = std::move(candidates_.front());
          candidates_.pop_front();
        } else if (!control_.empty()) {
          event = std::move(control_.front());
          control_.pop_front();
        } else if (!latest_.empty()) {
          event = std::move(latest_.begin()->second);
          latest_.erase(latest_.begin());
        }
      }
      if (event)
        callbacks.Invoke([&] {
          callback_(context_, event->type, event->bytes.data(),
                    uint32_t(event->bytes.size()));
        });
      if (!failed &&
          (!network_.direct_only || direct_route_gate_.Proven())) {
        DeliverMedia(received_video, GB_RTC_EVENT_VIDEO);
        DeliverMedia(received_audio, GB_RTC_EVENT_AUDIO);
      }
    }
    callbacks
        .Close(); // includes a test-probe callback running on another thread
    StopPeer();
    {
      std::lock_guard lock(events_mutex_);
      dispatcher_done_ = true;
    }
    wake_.notify_all();
    sessions.Erase(handle);
  }
  void DeliverMedia(MediaQueue &queue, uint32_t type) {
    auto frame = queue.Pop();
    if (!frame)
      return;
    media_event_.resize(sizeof(gb_rtc_media_event) + frame->bytes.size());
    gb_rtc_media_event header{16, 1, frame->timestamp,
                              uint32_t(frame->bytes.size())};
    std::memcpy(media_event_.data(), &header, sizeof(header));
    std::memcpy(media_event_.data() + sizeof(header), frame->bytes.data(),
                frame->bytes.size());
    callbacks.Invoke([&] {
      callback_(context_, type, media_event_.data(),
                uint32_t(media_event_.size()));
    });
    queue.Recycle(std::move(*frame));
  }
  std::vector<IceServer> ice_;
  NetworkConfig network_;
  gb_rtc_event_cb callback_;
  void *context_;
  std::mutex operation_, stop_mutex_, events_mutex_, data_mutex_[2];
  std::condition_variable wake_;
  std::deque<Event> signaling_, candidates_, control_;
  std::map<uint32_t, Event> latest_;
  std::optional<Event> fatal_;
  std::vector<uint8_t> media_event_;
  std::atomic<uint64_t> pointer_drops_{};
  bool dispatcher_done_{}, remote_set_{};
  std::atomic<bool> peer_started_{}, connected_{}, scheduled_{}, inflight_[2]{},
      cleanup_scheduled_{};
  DirectRouteGate direct_route_gate_;
  std::atomic<uint32_t> inflight_timestamp_[2]{};
  uint64_t direct_proof_generation_{};
  std::vector<std::unique_ptr<w::IceCandidate>> early_candidates_;
  std::optional<DirectRouteProof> direct_proof_;
  SelectedPairTracker selected_pair_;
  w::scoped_refptr<PC> pc_;
  w::scoped_refptr<w::EncodedVideoFrameInjectorInterface> video_injector_;
  w::scoped_refptr<w::EncodedAudioFrameInjectorInterface> audio_injector_;
  w::scoped_refptr<w::DataChannelInterface> channels_[2];
  std::unique_ptr<ChannelObserver> channel_observers_[2];
};
void ChannelObserver::OnStateChange() {
  if (auto owner = owner_.lock())
    owner->ChannelState(channel_);
}
void ChannelObserver::OnMessage(const w::DataBuffer &buffer) {
  auto owner = owner_.lock();
  if (!owner)
    return;
  if (!buffer.binary ||
      buffer.size() > (channel_ == 0 ? GB_RTC_MAX_CONTROL_BYTES
                                     : GB_RTC_MAX_POINTER_BYTES)) {
    owner->Fail("channel_failed");
    return;
  }
  owner->QueueData(channel_ == 0 ? GB_RTC_EVENT_CONTROL
                                 : GB_RTC_EVENT_POINTER,
                   std::span(buffer.data.data(), buffer.data.size()));
}
} // namespace gamebridge::rtc

using namespace gamebridge::rtc;
template <class F> gb_rtc_result Boundary(F &&action) noexcept {
  try {
    return action();
  } catch (...) {
    return GB_RTC_INTERNAL;
  }
}
gb_rtc_result CreateSession(const gb_rtc_config *config, NetworkConfig network,
                            gb_rtc_event_cb callback, void *context,
                            gb_rtc_handle *handle) {
  if (!handle || !config || config->size != sizeof(*config) ||
      config->abi_version != 1 || config->reserved ||
      !config->ice_json_utf8 || !config->ice_json_size ||
      config->ice_json_size > GB_RTC_MAX_ICE_BYTES)
    return GB_RTC_INVALID;
  auto ice =
      ParseIce(std::string_view(config->ice_json_utf8, config->ice_json_size));
  if (!ice || (network.direct_only && !ice->empty()))
    return GB_RTC_INVALID;
  auto session = std::make_shared<Session>(std::move(*ice), std::move(network),
                                           callback, context);
  auto value = sessions.Insert(session);
  if (!value)
    return GB_RTC_BACKPRESSURE;
  session->handle = value;
  *handle = value;
  try {
    session->StartCallbacks();
  } catch (...) {
    sessions.Erase(value);
    *handle = 0;
    return GB_RTC_INTERNAL;
  }
  return GB_RTC_OK;
}
extern "C" {
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create(const gb_rtc_config *config,
                                                   gb_rtc_event_cb callback,
                                                   void *context,
                                                   gb_rtc_handle *handle) {
  if (handle)
    *handle = 0;
  return Boundary(
      [&] { return CreateSession(config, NetworkConfig{}, callback, context, handle); });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create_v2(
    const gb_rtc_config *config, const gb_rtc_network_config *network,
    gb_rtc_event_cb callback, void *context, gb_rtc_handle *handle) {
  if (handle)
    *handle = 0;
  return Boundary([&]() -> gb_rtc_result {
    if (!handle || !network || network->size != sizeof(*network) ||
        network->abi_version != GB_RTC_ABI_VERSION ||
        (network->policy & ~GB_RTC_NETWORK_DIRECT_ONLY) ||
        network->external_ipv4_present > 1)
      return GB_RTC_INVALID;
    for (auto value : network->reserved0)
      if (value)
        return GB_RTC_INVALID;
    for (auto value : network->reserved)
      if (value)
        return GB_RTC_INVALID;
    NetworkConfig parsed;
    parsed.direct_only =
        (network->policy & GB_RTC_NETWORK_DIRECT_ONLY) != 0;
    if (parsed.direct_only &&
        (network->udp_port_min != 47981 || network->udp_port_max != 47990))
      return GB_RTC_INVALID;
    parsed.udp_port_min = static_cast<uint16_t>(network->udp_port_min);
    parsed.udp_port_max = static_cast<uint16_t>(network->udp_port_max);
    parsed.external_ipv4_present = network->external_ipv4_present != 0;
    parsed.external_ipv4 = {network->external_ipv4[0],
                            network->external_ipv4[1],
                            network->external_ipv4[2],
                            network->external_ipv4[3]};
    if (parsed.external_ipv4_present && !PublicIPv4(parsed.external_ipv4))
      return GB_RTC_INVALID;
    return CreateSession(config, std::move(parsed), callback, context, handle);
  });
}
GB_RTC_API void GB_RTC_CALL gb_rtc_close(gb_rtc_handle handle) {
  try {
    if (auto session = sessions.Get(handle))
      session->Close();
  } catch (...) {
  }
}
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create_offer(gb_rtc_handle handle) {
  return Boundary([&] {
    auto s = sessions.Get(handle);
    return s ? s->Description(true) : GB_RTC_STATE;
  });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL
gb_rtc_create_answer(gb_rtc_handle handle) {
  return Boundary([&] {
    auto s = sessions.Get(handle);
    return s ? s->Description(false) : GB_RTC_STATE;
  });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_set_remote_description(
    gb_rtc_handle handle, const uint8_t *bytes, uint32_t size) {
  return Boundary([&]() -> gb_rtc_result {
    auto s = sessions.Get(handle);
    if (!s || s->closed || s->failed)
      return GB_RTC_STATE;
    if (!bytes || !size || size > GB_RTC_MAX_DESCRIPTION_BYTES)
      return GB_RTC_INVALID;
    auto j = ParseJson(
        std::string_view(reinterpret_cast<const char *>(bytes), size));
    return j ? s->Remote(*j) : GB_RTC_INVALID;
  });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_add_candidate(gb_rtc_handle handle,
                                                          const uint8_t *bytes,
                                                          uint32_t size) {
  return Boundary([&]() -> gb_rtc_result {
    auto s = sessions.Get(handle);
    if (!s || s->closed || s->failed)
      return GB_RTC_STATE;
    if (!bytes || !size || size > GB_RTC_MAX_CANDIDATE_BYTES)
      return GB_RTC_INVALID;
    auto j = ParseJson(
        std::string_view(reinterpret_cast<const char *>(bytes), size));
    return j ? s->Candidate(*j) : GB_RTC_INVALID;
  });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL
gb_rtc_send_video(gb_rtc_handle handle, const gb_rtc_video *input) {
  return Boundary([&]() -> gb_rtc_result {
    auto s = sessions.Get(handle);
    if (!s || s->closed || s->failed)
      return GB_RTC_STATE;
    if (!input || input->size != sizeof(*input) || input->abi_version != 1 ||
        !input->data || !input->data_size ||
        input->data_size > GB_RTC_MAX_VIDEO_BYTES || !input->width ||
        !input->height || input->width > 16384 || input->height > 16384 ||
        input->keyframe > 1)
      return GB_RTC_INVALID;
    for (auto byte : input->reserved)
      if (byte)
        return GB_RTC_INVALID;
    const std::span bytes(input->data, input->data_size);
    if (!ValidAnnexB(bytes))
      return GB_RTC_INVALID;
    return s->Video(bytes, input->timestamp90k, input->width, input->height,
                    input->keyframe != 0);
  });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL
gb_rtc_send_audio(gb_rtc_handle handle, const gb_rtc_audio *input) {
  return Boundary([&]() -> gb_rtc_result {
    auto s = sessions.Get(handle);
    if (!s || s->closed || s->failed)
      return GB_RTC_STATE;
    if (!input || input->size != sizeof(*input) || input->abi_version != 1 ||
        !input->data || !input->data_size ||
        input->data_size > GB_RTC_MAX_AUDIO_BYTES || input->reserved[0] ||
        input->reserved[1])
      return GB_RTC_INVALID;
    const std::span bytes(input->data, input->data_size);
    auto duration = OpusSamples(bytes);
    if (!duration)
      return GB_RTC_INVALID;
    return s->Audio(bytes, input->timestamp48k, duration);
  });
}
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_send_data(gb_rtc_handle handle,
                                                      uint32_t channel,
                                                      const uint8_t *bytes,
                                                      uint32_t size) {
  return Boundary([&]() -> gb_rtc_result {
    auto s = sessions.Get(handle);
    if (!s || s->closed || s->failed)
      return GB_RTC_STATE;
    if (!bytes || !size || (channel != 1 && channel != 2) ||
        size > (channel == 1 ? GB_RTC_MAX_CONTROL_BYTES
                             : GB_RTC_MAX_POINTER_BYTES))
      return GB_RTC_INVALID;
    return s->Data(channel, std::span(bytes, size));
  });
}
#ifdef GB_RTC_TESTING
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_test_probe(gb_rtc_handle handle,
                                                       uint32_t kind) {
  return Boundary([&]() -> gb_rtc_result {
    auto s = sessions.Get(handle);
    if (!s || s->closed)
      return GB_RTC_STATE;
    if (kind == 99)
      throw std::runtime_error("probe");
    auto frame = kind == 1   ? s->video.Pop()
                 : kind == 2 ? s->audio.Pop()
                             : std::optional<Media>(Media{{1, 2, 3}, 0, 0});
    if (!frame)
      return GB_RTC_STATE;
    return s->Probe(kind, frame->bytes);
  });
}
#endif
} // extern C
#ifdef GB_RTC_BENCH
#include "bench.h"
#endif
