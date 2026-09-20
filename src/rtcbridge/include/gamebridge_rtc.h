#ifndef GAMEBRIDGE_RTC_H
#define GAMEBRIDGE_RTC_H
#include <stdint.h>

#if defined(_WIN32)
#define GB_RTC_CALL __cdecl
#if defined(GB_RTC_BUILD)
#define GB_RTC_API __declspec(dllexport)
#else
#define GB_RTC_API __declspec(dllimport)
#endif
#else
#define GB_RTC_CALL
#define GB_RTC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GB_RTC_ABI_VERSION 1u
#define GB_RTC_MAX_SESSIONS 256u
#define GB_RTC_MAX_ICE_BYTES 65536u
#define GB_RTC_MAX_VIDEO_BYTES 4194304u
#define GB_RTC_MAX_AUDIO_BYTES 65536u
#define GB_RTC_MAX_DESCRIPTION_BYTES 262144u
#define GB_RTC_MAX_CANDIDATE_BYTES 8192u
#define GB_RTC_AUDIO_CAPACITY 80u
#define GB_RTC_AUDIO_DURATION_MS 200u
#define GB_RTC_MAX_CONTROL_BYTES 4096u
#define GB_RTC_MAX_POINTER_BYTES 256u
#define GB_RTC_CANDIDATE_CAPACITY 16u

typedef uint64_t gb_rtc_handle;
// Fixed-width storage, independent of compiler enum-size flags.
typedef int32_t gb_rtc_result;
enum { GB_RTC_OK=0, GB_RTC_INVALID=1, GB_RTC_STATE=2, GB_RTC_BACKPRESSURE=3, GB_RTC_INTERNAL=4 };
enum {
  GB_RTC_EVENT_STATE=1, GB_RTC_EVENT_DESCRIPTION=2, GB_RTC_EVENT_CANDIDATE=3,
  GB_RTC_EVENT_ERROR=4, GB_RTC_EVENT_ROUTE=5, GB_RTC_EVENT_GATHERING=6,
  GB_RTC_EVENT_BITRATE=7, GB_RTC_EVENT_VIDEO=8, GB_RTC_EVENT_AUDIO=9,
  GB_RTC_EVENT_CONTROL=10, GB_RTC_EVENT_POINTER=11, GB_RTC_EVENT_METRICS=12,
  GB_RTC_EVENT_KEYFRAME_REQUEST=13
};
enum { GB_RTC_CHANNEL_RELIABLE=1, GB_RTC_CHANNEL_POINTER=2 };

// VIDEO/AUDIO payload: this 16-byte LITTLE-ENDIAN header immediately followed
// by data_size encoded bytes. Read fields with memcpy (alignment is not
// guaranteed). VIDEO is one complete AU, normalized to four-byte Annex-B start
// codes; NAL contents are preserved, AUD/filler NALs may be omitted by Pion.
// Both submitted and normalized AU sizes must fit MAX_VIDEO_BYTES. A submitted
// AU has at most 4096 NAL units, bounding work for adversarial tiny-NAL input.
// AUDIO is one unchanged Opus packet. Timestamp is the sender's modulo-2^32
// 90 kHz video / 48 kHz audio clock. No width/keyframe metadata crosses RTP;
// decoders obtain those from H.264 SPS/NAL types. Struct inputs remain ABI v1.
typedef struct gb_rtc_media_event {
  uint32_t size, abi_version, timestamp, data_size;
} gb_rtc_media_event;
enum {
  GB_RTC_STATE_NEW=1, GB_RTC_STATE_CONNECTING=2, GB_RTC_STATE_CONNECTED=3,
  GB_RTC_STATE_DISCONNECTED=4, GB_RTC_STATE_FAILED=5, GB_RTC_STATE_CLOSED=6
};
enum { GB_RTC_ROUTE_DIRECT=1, GB_RTC_ROUTE_RELAY=2 };
enum { GB_RTC_GATHERING_NEW=1, GB_RTC_GATHERING_ACTIVE=2, GB_RTC_GATHERING_COMPLETE=3 };

// Stable ABI v1 event payloads (length-delimited UTF-8 JSON):
// STATE:       {"state":<GB_RTC_STATE_*>}
// DESCRIPTION: {"type":"offer"|"answer","sdp":"..."}
// CANDIDATE:   {"candidate":"...","sdpMid":string|null,
//               "sdpMLineIndex":uint16|null,"usernameFragment":string|null}
//              Optional candidate fields may be omitted. candidate="" ends
//              that gathering generation. Forward it after earlier candidates.
// ERROR:       {"code":"connection_failed"|"event_overflow"|
//                      "event_oversize"|"candidate_rejected"|"description_rejected"|
//                      "relay_forbidden"|"direct_route_unproven"}
// ROUTE:       {"route":<GB_RTC_ROUTE_*>}, from selected candidate-pair stats.
// GATHERING:   {"state":<GB_RTC_GATHERING_*>}
// BITRATE:     {"bitsPerSecond":1000000..40000000}, initial target 10000000.
// CONTROL/POINTER: raw binary message bytes (no JSON/envelope).
// KEYFRAME_REQUEST: {} (RTCP PLI or a dropped outgoing reference frame;
// ask the encoder for an IDR).
// METRICS: cumulative uint64 counters frames_dropped_bridge,
// audio_packets_dropped_bridge, frames_dropped_receiver,
// audio_packets_dropped_receiver, pointer_messages_dropped_receiver.
// Additional sanitized ERROR codes: channel_failed, control_overflow,
// media_write_failed, control_channel_closed, control_channel_error,
// pointer_channel_closed, pointer_channel_error. Unexpected channel-only
// termination is reconciled for 200 ms with whole-peer shutdown, then fails
// the session so a live media stream cannot silently retain unusable input.
// Normal local/remote whole-peer closure suppresses these channel errors.
// Unknown additive event IDs may be ignored by consumers.
// SDP/candidates contain private connection data: forward only to the intended
// peer. The bridge never logs their contents or ICE server credentials.
// Connection states are serialized snapshots of Pion's current state; delayed
// callback arguments and duplicate notifications cannot replay obsolete states.
// Pion callbacks enqueue owned events without waiting for native callbacks.
// Description/state/gathering have 32 reserved slots; candidates have 128.
// Route/bitrate/metrics/keyframe updates coalesce. A full signaling queue
// closes the transport and emits one reserved ERROR that supersedes pending
// events. External close cancels pending events and is the final callback barrier.

// Windows x64 ABI. Set size=sizeof(struct), abi_version=1 and reserved=0.
// JSON is length-delimited UTF-8 (no trailing NUL included in its size).
typedef struct gb_rtc_config {
  uint32_t size;
  uint32_t abi_version;
  const char* ice_json_utf8;
  uint32_t ice_json_size;
  uint32_t reserved;
} gb_rtc_config;

enum { GB_RTC_NETWORK_DIRECT_ONLY = 1u };

typedef struct gb_rtc_network_config {
  uint32_t size;
  uint32_t abi_version;
  uint32_t policy;
  uint32_t udp_port_min;
  uint32_t udp_port_max;
  uint32_t external_ipv4_present;
  uint8_t external_ipv4[4];
  uint8_t reserved0[4];
  uint64_t reserved[4];
} gb_rtc_network_config;

typedef struct gb_rtc_video {
  uint32_t size;
  uint32_t abi_version;
  const uint8_t* data;
  uint32_t data_size;
  uint32_t timestamp90k;
  uint32_t width;
  uint32_t height;
  uint8_t keyframe;
  uint8_t reserved[7];
} gb_rtc_video;

typedef struct gb_rtc_audio {
  uint32_t size;
  uint32_t abi_version;
  const uint8_t* data;
  uint32_t data_size;
  uint32_t timestamp48k;
  uint32_t reserved[2];
} gb_rtc_audio;

// Payload is borrowed only until callback returns; copy it to retain it. Never
// throw an exception across this boundary. Callback/context must be native
// pointers; their owner keeps them alive until close has completed. A null
// callback is valid. At most one callback executes per session at a time.
typedef void (GB_RTC_CALL *gb_rtc_event_cb)(void* context, uint32_t type, const uint8_t* data, uint32_t size);

// All input buffers are borrowed for this call only. On OK, their contents
// have been copied to bounded Go-owned storage; no caller pointer is retained.
// Queue submissions never wait for capacity/consumption or callback execution.
// Video has exactly ONE pending access unit; new input replaces that unit and
// increments frames_dropped_bridge. Audio holds at most 200 ms (maximum 80
// packets at 2.5 ms); overflow drops oldest whole packets and increments its
// metric. BACKPRESSURE means brief producer-lock contention. For legacy
// gb_rtc_create, media submitted before connection remains bounded until the
// connection is ready. Direct-only gb_rtc_create_v2 SendVideo/SendAudio/
// SendData return STATE unless a current direct selected pair is proven.
// No asynchronous RTP pacer queue: capture cadence paces frames, GCC emits
// bitrate targets for the native encoder. Timestamp progression uses caller
// RTP clocks, never wall-clock sleeps or a lookahead frame.
// Signaling is applied immediately with bounded owned copies. Concurrent
// signaling operations may return BACKPRESSURE and can be retried. Offer
// requires stable signaling state; answer requires a remote offer. Up to 16
// candidates may precede the remote description, with BACKPRESSURE at capacity.
// The remote description schema is exactly {"type":"offer"|"answer","sdp":"..."}.
// ICE config is {"iceServers":[{"urls":string|string[],"username":string,
// "credential":string}]}; iceServers defaults empty; TURN requires credentials.
// Unknown fields, duplicate keys/URLs, malformed URLs and oversized JSON fail
// with INVALID. Initial events may arrive during gb_rtc_create; initialize the
// callback context before calling it. No SDP/ICE is logged.
// The Go runtime DLL must remain loaded for the lifetime of the process.
#ifndef GB_RTC_NO_DECLARATIONS
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create(const gb_rtc_config*, gb_rtc_event_cb, void*, gb_rtc_handle*);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create_v2(const gb_rtc_config*, const gb_rtc_network_config*, gb_rtc_event_cb, void*, gb_rtc_handle*);
// Idempotent for zero/stale handles. The synchronization exception to queue
// nonblocking behavior: external close waits for an admitted callback to end.
// When called from ANY RTC callback, it cancels new target callbacks and returns
// immediately; an already running target callback finishes asynchronously. Keep
// its context alive until that callback unwinds, or call close again from outside
// callbacks to wait for completion. This also permits callbacks closing each
// other's sessions without a cyclic wait. No later callback can be admitted.
GB_RTC_API void GB_RTC_CALL gb_rtc_close(gb_rtc_handle);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create_offer(gb_rtc_handle);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_create_answer(gb_rtc_handle);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_set_remote_description(gb_rtc_handle, const uint8_t*, uint32_t);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_add_candidate(gb_rtc_handle, const uint8_t*, uint32_t);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_send_video(gb_rtc_handle, const gb_rtc_video*);
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_send_audio(gb_rtc_handle, const gb_rtc_audio*);
// Negotiated SCTP IDs 0/1: control.reliable is ordered/reliable, pointer.fast
// unordered with MaxRetransmits=0. Send buffers are separately bounded to
// 64 KiB/4 KiB; a closed/not-yet-open channel returns STATE, a full buffer
// BACKPRESSURE. OK means SCTP copied the bytes, not a delivery acknowledgement.
// Incoming reliable queue has 64 slots; overflow fails the session explicitly.
// Incoming pointer queue keeps only the latest pending message.
GB_RTC_API gb_rtc_result GB_RTC_CALL gb_rtc_send_data(gb_rtc_handle, uint32_t channel, const uint8_t*, uint32_t);
#endif

#ifdef __cplusplus
}
#endif
#endif
