// Real pinned PacketBuffer plus the receiver's exact consumed-frame method.
// No pacing/network timing model: incomplete frame, ring rollover, and RTP wrap
// are exercised with >40,000 real packet insertions.
#include "modules/video_coding/packet_buffer.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
#undef RTC_DCHECK_RUN_ON
#define RTC_DCHECK_RUN_ON(x) ((void)0)
namespace webrtc {
struct RtpVideoStreamReceiver2 {
  video_coding::PacketBuffer packet_buffer_{512, 2048};
  std::optional<int64_t> newest_media_seq_num_;
  uint64_t consumed_frame_epoch_ = 0;
  void FrameConsumed(int64_t, uint64_t);
};
#include "pinned_receiver_consumed.h"
}
namespace w = webrtc;
using Buffer = w::video_coding::PacketBuffer;

Buffer::InsertResult Insert(w::RtpVideoStreamReceiver2& receiver, int64_t seq,
                            unsigned frame, unsigned index, unsigned last=45) {
  auto packet=std::make_unique<Buffer::Packet>();
  packet->sequence_number=seq;
  packet->timestamp=frame*1500;
  packet->video_header.codec=w::kVideoCodecH264;
  packet->video_header.generic.emplace();
  packet->video_header.is_first_packet_in_frame=index==0;
  packet->video_header.is_last_packet_in_frame=index==last;
  packet->marker_bit=index==last;
  if (!receiver.newest_media_seq_num_ || seq>*receiver.newest_media_seq_num_)
    receiver.newest_media_seq_num_=seq;
  return receiver.packet_buffer_.InsertPacket(std::move(packet));
}
void Rollover() {
  w::RtpVideoStreamReceiver2 receiver;
  std::array<bool,900> delivered{};
  unsigned clears=0;
  int64_t sequence=30000;
  for(unsigned frame=0;frame<delivered.size();++frame) {
    // Unsent end of interrupted first frame has no assigned RTP sequence.
    for(unsigned index=0;index<(frame==0?30u:46u);++index) {
      auto result=Insert(receiver,sequence++,frame,index);
      clears+=result.buffer_cleared;
      for(const auto& packet:result.packets) if(packet->is_last_packet_in_frame()) {
        delivered[packet->timestamp/1500]=true;
        receiver.FrameConsumed(packet->seq_num(),0);
      }
    }
  }
  unsigned measured=0;
  for(unsigned frame=1;frame<delivered.size();++frame) measured+=delivered[frame];
  std::printf("consumed receiver: completed=%u/899 ring_clears=%u\n",measured,clears);
  CHECK(!delivered[0]); CHECK(measured==899); CHECK(clears==0);
}
void NewerIncompleteAndDelayedAcknowledgement() {
  w::RtpVideoStreamReceiver2 receiver;
  // Frame 1 is missing its middle packet while frame 0 is fully consumed.
  CHECK(Insert(receiver,65534,0,0,0).packets.size()==1);
  CHECK(Insert(receiver,65535,1,0,2).packets.empty());
  CHECK(Insert(receiver,65537,1,2,2).packets.empty());
  receiver.FrameConsumed(65534,0);
  auto recovered=Insert(receiver,65536,1,1,2);
  CHECK(recovered.packets.size()==3); CHECK(!recovered.buffer_cleared);
  receiver.FrameConsumed(65537,0);
  CHECK(Insert(receiver,65538,2,0,1).packets.empty());
  receiver.FrameConsumed(65534,0); // old acknowledgement cannot advance cleanup
  CHECK(Insert(receiver,65539,2,1,1).packets.size()==2);
}
void InvalidEpochAndFutureAcknowledgements() {
  w::RtpVideoStreamReceiver2 receiver;
  CHECK(Insert(receiver,100,0,0,1).packets.empty());
  receiver.consumed_frame_epoch_=1;
  receiver.FrameConsumed(101,0); // stale source epoch
  receiver.FrameConsumed(101,1); // beyond any received packet
  CHECK(Insert(receiver,101,0,1,1).packets.size()==2);
}
int main() {
  Rollover(); NewerIncompleteAndDelayedAcknowledgement();
  InvalidEpochAndFutureAcknowledgements();
  std::puts("pinned receiver: consumed-only cleanup, partial recovery, delayed/wrapped/epoch guards PASS");
}
