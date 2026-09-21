// Exercise the actual pinned transformer/delegate ownership boundary. The task
// queue is deterministic; frames, ref-counting, delegate and callbacks are real.
#include "api/make_ref_counted.h"
#include "modules/rtp_rtcp/source/rtp_video_stream_receiver_frame_transformer_delegate.h"
#include <cstdio>
#include <cstdlib>
#include <deque>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(false)
namespace w=webrtc;
struct Queue final : w::TaskQueueBase {
  std::deque<absl::AnyInvocable<void() &&>> tasks;
  void Delete() override { std::abort(); }
  void Drain() {
    CurrentTaskQueueSetter current(this);
    while (!tasks.empty()) { auto next=std::move(tasks.front()); tasks.pop_front(); std::move(next)(); }
  }
  void PostTaskImpl(absl::AnyInvocable<void() &&> task,const PostTaskTraits&,const w::Location&) override { tasks.push_back(std::move(task)); }
  void PostDelayedTaskImpl(absl::AnyInvocable<void() &&>,w::TimeDelta,const PostDelayedTaskTraits&,const w::Location&) override { std::abort(); }
};
struct Receiver final : w::RtpVideoFrameReceiver {
  unsigned decoded=0;
  void ManageFrame(std::unique_ptr<w::RtpFrameObject>) override { ++decoded; }
};
struct Transformer : w::FrameTransformerInterface {
  std::unique_ptr<w::TransformableFrameInterface> frame;
  w::scoped_refptr<w::TransformedFrameCallback> sink;
  void Transform(std::unique_ptr<w::TransformableFrameInterface> value) override { frame=std::move(value); }
  void RegisterTransformedFrameSinkCallback(w::scoped_refptr<w::TransformedFrameCallback> value,uint32_t) override { sink=std::move(value); }
  void UnregisterTransformedFrameSinkCallback(uint32_t) override { sink=nullptr; }
};
std::unique_ptr<w::RtpFrameObject> Frame() {
  w::RTPVideoHeader header;
  header.codec=w::kVideoCodecH264;
  header.video_type_header=w::RTPVideoHeaderH264{};
  header.frame_type=w::VideoFrameType::kVideoFrameKey;
  return std::make_unique<w::RtpFrameObject>(1,2,true,0,
      w::Timestamp::Millis(1),w::Timestamp::Millis(2),1500,0,
      w::VideoSendTiming(),96,w::kVideoCodecH264,w::kVideoRotation_0,
      w::VideoContentType::UNSPECIFIED,header,std::nullopt,std::nullopt,
      w::RtpPacketInfos(),w::EncodedImageBuffer::Create(8));
}
int main() {
  Queue queue;
  Receiver receiver,other_receiver;
  auto transformer=w::make_ref_counted<Transformer>();
  auto delegate=w::make_ref_counted<w::RtpVideoStreamReceiverFrameTransformerDelegate>(
      &receiver,nullptr,transformer,&queue,1);
  delegate->Init();
  unsigned consumed=0;
  delegate->TransformFrame(Frame(),[&]{++consumed;});
  transformer->sink->OnFrameConsumed(std::move(transformer->frame));
  CHECK(consumed==0); CHECK(receiver.decoded==0); // owning queue only
  queue.Drain();
  CHECK(consumed==1); CHECK(receiver.decoded==0);

  // Destroying a rejected frame is not an acknowledgement.
  delegate->TransformFrame(Frame(),[&]{++consumed;});
  transformer->frame.reset(); queue.Drain(); CHECK(consumed==1);

  auto other_transformer=w::make_ref_counted<Transformer>();
  auto other=w::make_ref_counted<w::RtpVideoStreamReceiverFrameTransformerDelegate>(
      &other_receiver,nullptr,other_transformer,&queue,2);
  other->Init();
  delegate->TransformFrame(Frame(),[&]{++consumed;});
  other_transformer->sink->OnFrameConsumed(std::move(transformer->frame));
  queue.Drain(); CHECK(consumed==1); CHECK(other_receiver.decoded==0);

  // An acknowledgement queued before Reset cannot touch a retired receiver.
  delegate->TransformFrame(Frame(),[&]{++consumed;});
  transformer->sink->OnFrameConsumed(std::move(transformer->frame));
  delegate->Reset(); queue.Drain(); CHECK(consumed==1);
  other->Reset();
  std::puts("pinned receiver transformer: accepted-only, queue ownership, foreign receiver, reset, no decode PASS");
}
