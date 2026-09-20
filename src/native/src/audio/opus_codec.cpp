#include "gamebridge/audio/opus_codec.h"
#include <opus/opus.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gamebridge::audio {
OpusPacketizer::OpusPacketizer(int64_t epoch_qpc,int64_t qpc_frequency):clock_(epoch_qpc,qpc_frequency){
  int error=OPUS_OK;
  encoder_=opus_encoder_create(kAudioSampleRate,kAudioChannels,OPUS_APPLICATION_AUDIO,&error);
  if(error!=OPUS_OK||!encoder_)throw std::runtime_error("opus encoder create");
  if(opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),OPUS_SET_BITRATE(kOpusBitrate))!=OPUS_OK||
     opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),OPUS_SET_DTX(0))!=OPUS_OK){
    opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));encoder_=nullptr;throw std::runtime_error("opus encoder configure");
  }
}
OpusPacketizer::~OpusPacketizer(){if(encoder_)opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));}
void OpusPacketizer::Reset() noexcept {pending_.clear();have_timestamp_=false;if(encoder_)opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),OPUS_RESET_STATE);}
std::vector<EncodedAudio> OpusPacketizer::Push(std::span<const float> input,uint32_t frames,int64_t qpc,bool discontinuity,bool timestamp_error){
  if(frames>48000||input.size()!=size_t(frames)*kAudioChannels)throw std::invalid_argument("invalid PCM block");
  if(timestamp_error){Reset();return{};}
  const auto stamp=clock_.Map(qpc,discontinuity,timestamp_error);
  if(!stamp.accepted)return{};
  if(stamp.discontinuity)Reset();
  if(!have_timestamp_){pending_timestamp_=stamp.timestamp48k;have_timestamp_=true;}
  pending_.insert(pending_.end(),input.begin(),input.end());
  std::vector<EncodedAudio> output;
  constexpr size_t packet_samples=size_t(kOpusPacketFrames)*kAudioChannels;
  while(pending_.size()>=packet_samples){
    std::vector<uint8_t> bytes(4000);
    const auto count=opus_encode_float(static_cast<OpusEncoder*>(encoder_),pending_.data(),kOpusPacketFrames,bytes.data(),static_cast<opus_int32>(bytes.size()));
    if(count<0)throw std::runtime_error("opus encode");
    bytes.resize(static_cast<size_t>(count));
    output.push_back({std::move(bytes),pending_timestamp_,kOpusPacketFrames});
    pending_.erase(pending_.begin(),pending_.begin()+static_cast<std::ptrdiff_t>(packet_samples));
    pending_timestamp_+=kOpusPacketFrames;
  }
  return output;
}
OpusDecoder::OpusDecoder(){int error=OPUS_OK;decoder_=opus_decoder_create(kAudioSampleRate,kAudioChannels,&error);if(error!=OPUS_OK||!decoder_)throw std::runtime_error("opus decoder create");}
OpusDecoder::~OpusDecoder(){if(decoder_)opus_decoder_destroy(static_cast<::OpusDecoder*>(decoder_));}
void OpusDecoder::Reset()noexcept{if(decoder_)opus_decoder_ctl(static_cast<::OpusDecoder*>(decoder_),OPUS_RESET_STATE);}
std::optional<DecodedAudio> OpusDecoder::Decode(std::span<const uint8_t> packet,uint32_t timestamp,bool conceal){
  if(packet.size()>65536||(!conceal&&packet.empty()))return{};
  DecodedAudio output;output.interleaved.resize(size_t(kOpusPacketFrames)*kAudioChannels);
  const auto frames=opus_decode_float(static_cast<::OpusDecoder*>(decoder_),conceal?nullptr:packet.data(),conceal?0:static_cast<opus_int32>(packet.size()),output.interleaved.data(),kOpusPacketFrames,0);
  if(frames<=0)return{};
  output.frames=static_cast<uint32_t>(frames);output.timestamp48k=timestamp;output.concealed=conceal;
  output.interleaved.resize(size_t(output.frames)*kAudioChannels);return output;
}
}
