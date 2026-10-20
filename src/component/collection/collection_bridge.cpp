#include "collection_bridge.h"
#include "collection_audio_history.h"
#include "nekorox/contracts.h"
#include <windows.h>
namespace nekorox::component::collection {
Bridge::Bridge(){LARGE_INTEGER f{};if(QueryPerformanceFrequency(&f)&&f.QuadPart>0)qpcFrequency_=std::uint64_t(f.QuadPart);for(auto&s:pool_)s=std::make_shared<AudioFrame>();}
Bridge& Bridge::instance(){static Bridge value;return value;}
void Bridge::ingest(const ::nekorox::RealtimeFrameV1& f,bool discontinuity,
    bool notesEnabled)noexcept{
 if(f.sampleCount>f.samples.size()||f.frameCount==0||f.channels==0||
    std::uint64_t(f.frameCount)*f.channels!=f.sampleCount)return;
 analyzer_.setNotesEnabled(notesEnabled);
 analyzer_.ingest(std::span<const float>(f.samples.data(),f.sampleCount),f.sampleRate,f.channels,f.playbackSerial,f.recordingIdentityHash,
  f.firstFrameIndex,f.qpcTicks,qpcFrequency_,discontinuity,[&](const AudioFrame& frame)noexcept{
    retainAudioRows(frame); // C6: independent of UI sampling/pool availability

   std::shared_ptr<AudioFrame> slot;
   for(std::size_t i=0;i<pool_.size();++i){auto index=(cursor_+i)%pool_.size();if(pool_[index].use_count()==1){slot=pool_[index];cursor_=(index+1)%pool_.size();break;}}
   if(!slot){drops_.fetch_add(1,std::memory_order_relaxed);return;}
   *slot=frame;const auto seq=sequence_.load(std::memory_order_relaxed)+1;slot->sequence=seq;
   latest_.store(slot,std::memory_order_release);sequence_.store(seq,std::memory_order_release);
  });
}
} // namespace
