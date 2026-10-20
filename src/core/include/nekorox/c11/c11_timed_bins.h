#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
namespace goden::c11 {
inline std::uint64_t durationTicks(std::uint64_t frames,std::uint32_t rate,
                                   std::uint64_t frequency) noexcept {
    if(!rate||!frequency)return 0;
    const auto seconds=frames/rate, remainder=frames%rate;
    const auto limit=(std::numeric_limits<std::uint64_t>::max)();
    if(seconds>limit/frequency||remainder>limit/frequency)return limit;
    const auto whole=seconds*frequency,part=(remainder*frequency)/rate;
    return whole>limit-part?limit:whole+part;
}
inline std::uint64_t chunkEndTicks(std::uint64_t firstQpc,std::uint64_t frames,
                                  std::uint32_t rate,std::uint64_t frequency) noexcept {
    const auto ticks=durationTicks(frames,rate,frequency);
    if(!firstQpc||ticks>(std::numeric_limits<std::uint64_t>::max)()-firstQpc)return 0;
    return firstQpc+ticks;
}
struct TimedBin {
    std::uint64_t beginQpc=0,endQpc=0,firstFrame=0,endFrame=0,segment=0;
    std::uint64_t scalarCount=0;
    double sumSquares=0;
    float peak=0;
    std::uint32_t sampleRate=0,channels=0;
    bool known=false;
};
struct TimedReading {
    double seconds=0,rms=0,peak=0,crestDb=0;
    std::size_t bins=0;
    bool available=false,complete=false;
};
inline TimedReading readTail(std::span<const TimedBin> bins,double minimumSeconds=.4) noexcept {
    TimedReading out;double sum=0;std::uint64_t scalars=0;
    if(bins.empty()||minimumSeconds<=0)return out;
    const auto segment=bins.back().segment;
    std::uint64_t nextFrame=bins.back().endFrame;
    for(std::size_t i=bins.size();i>0;--i){
        const auto& b=bins[i-1];
        if(!b.known||b.segment!=segment||!b.sampleRate||!b.channels||
           b.endFrame<=b.firstFrame||b.endFrame!=nextFrame||
           b.endQpc<=b.beginQpc||!std::isfinite(b.sumSquares)||b.sumSquares<0)break;
        const auto frames=b.endFrame-b.firstFrame;
        if(frames>(std::numeric_limits<std::uint64_t>::max)()/b.channels ||
           b.scalarCount!=frames*b.channels)break;
        out.seconds+=double(frames)/b.sampleRate;
        sum+=b.sumSquares;scalars+=b.scalarCount;
        out.peak=(std::max)(out.peak,double(b.peak));++out.bins;
        nextFrame=b.firstFrame;
        if(out.seconds+1e-9>=minimumSeconds){out.complete=true;break;}
    }
    out.available=scalars>0;
    if(out.available){
        out.rms=std::sqrt(sum/double(scalars));
        out.crestDb=out.rms>0?20*std::log10((std::max)(out.peak,out.rms)/out.rms):0;
    }
    // Includes the oldest whole measured bin; reports its actual duration.
    // Never assumes constant energy inside only a fraction of a bin.
    return out;
}
inline const TimedBin* hitTime(std::span<const TimedBin> bins,std::uint64_t qpc) noexcept {
    std::size_t lo=0,hi=bins.size();
    while(lo<hi){auto mid=lo+(hi-lo)/2;if(bins[mid].endQpc<=qpc)lo=mid+1;else hi=mid;}
    if(lo==bins.size())return nullptr;
    const auto& b=bins[lo];
    return b.known&&b.beginQpc<=qpc&&qpc<b.endQpc?&b:nullptr;
}
} // namespace goden::c11
