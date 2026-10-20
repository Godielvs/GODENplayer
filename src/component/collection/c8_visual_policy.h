#pragma once
#include "collection_graph.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace nekorox::component::collection {
inline float c8Smooth(float lo,float hi,float x) noexcept {
    x=std::clamp((x-lo)/(hi-lo),0.f,1.f);return x*x*(3.f-2.f*x);
}
struct C8MotionState {
    double quietTravel=0;
    float sustain=0;
    void step(double dt,float bass,float mid,float treble) noexcept {
        if(!std::isfinite(dt)||dt<=0)return;
        dt=std::min(dt,.1);
        bass=std::clamp(bass,0.f,1.4f);mid=std::clamp(mid,0.f,1.4f);treble=std::clamp(treble,0.f,1.4f);
        const float e=std::sqrt(.60f*bass*bass+.27f*mid*mid+.13f*treble*treble);
        const float tau=e>sustain?.25f:.70f;
        sustain+=float(-std::expm1(-dt/tau))*(e-sustain);
        const float active=c8Smooth(.055f,.33f,sustain);
        const double velocity=.015+(.65+double(bass)-.015)*active;
        quietTravel+=dt*velocity;
    }
};
inline bool c8WideEligible(const char* id) noexcept {
    if(!id)return false;
    // Explicitly exclude heavy raymarch/simulation/history programs. This is
    // a bounded quality uplift, not an assertion of GPU work being free.
    for(const auto* name:{"csln1b0s","di76dy6c","rxhy4i5t","rw3dfaou","tcsfryp6","3114"})
        if(std::strcmp(id,name)==0)return true;
    return false;
}
inline Extent c8FitExtent(float width,float height,const Preset& p,bool music) noexcept {
    const auto base=fitExtent(width,height,p.maxEdge,p.alignment);
    if(!base.width||!base.height||!music||!c8WideEligible(p.id))return base;
    const double aspect=std::max(double(width),double(height))/std::min(double(width),double(height));
    const float lift=1.f+.10f*c8Smooth(1.60f,3.20f,float(aspect));
    const unsigned ceiling=std::min(2048U,unsigned(std::floor(double(p.maxEdge)*lift)));
    const auto result=fitExtent(width,height,ceiling,p.alignment);
    // Rounding must never silently exceed the 21% area allowance.
    const auto area=std::uint64_t(base.width)*base.height;
    if(std::uint64_t(result.width)*result.height*100U>area*121U)return base;
    return result;
}
} // namespace nekorox::component::collection
