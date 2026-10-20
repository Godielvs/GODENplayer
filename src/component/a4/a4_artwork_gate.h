#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace nekorox::component::a4 {
enum class ArtworkState : unsigned char { loading,ready,missing,error };
// Identity is assigned by the owner after exact string comparison, not a lossy
// album-title hash. This owns only the bounded handover, never GPU resources.
class ArtworkGate final {
    std::uint64_t expected_=0,displayed_=0;
    ArtworkState state_=ArtworkState::missing;
    double started_=0,hold_=0,duration_=.18;
    float from_=0;
    bool fading_=false;
public:
    void observe(std::uint64_t expected,ArtworkState state,bool hasDisplayed,
        double now,bool animate=true) noexcept
    {
        if(!std::isfinite(now))return;
        if(state==ArtworkState::ready){expected_=displayed_=expected;state_=state;fading_=false;from_=1;return;}
        const bool sameDisplay=hasDisplayed&&displayed_==expected;
        if(state==ArtworkState::loading&&sameDisplay){expected_=expected;state_=state;fading_=false;from_=1;return;}
        if(expected_==expected&&state_==state)return;
        const float current=hasDisplayed?opacity(now):0.f;
        const bool identityChanged=expected_!=expected;
        expected_=expected;state_=state;
        if(!hasDisplayed){from_=0;fading_=false;return;}
        from_=identityChanged&&!fading_?1.f:current;
        started_=now;hold_=state==ArtworkState::loading?.12:0;
        duration_=animate?.18:0;fading_=true;
    }
    float opacity(double now)const noexcept {
        if(!fading_)return from_;
        if(duration_<=0)return 0;
        float t=std::clamp(float((now-started_-hold_)/duration_),0.f,1.f);
        t=t*t*(3-2*t);return from_*(1-t);
    }
    bool needsFrame(double now)const noexcept{return fading_&&opacity(now)>0;}
    bool shouldClear(double now)const noexcept{return fading_&&opacity(now)<=0;}
    void cleared()noexcept{displayed_=0;from_=0;fading_=false;}
    std::uint64_t expected()const noexcept{return expected_;}
};
}
