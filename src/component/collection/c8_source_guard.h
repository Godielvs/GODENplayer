#pragma once
#include "collection_audio.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nekorox::component::collection {
// Visual-only policy. It never reconstructs PCM, creates beats, changes transport,
// or passes a snapshot belonging to another recording. One instance per view.
class C8SourceGuard final {
    AudioFrame view_{};
    std::array<float,kBands> lastLevel_{},lastRelative_{},recoveryLevel_{},recoveryRelative_{};
    float lastGate_=0,recoveryGate_=0;
    std::uint64_t serial_=0,recording_=0;
    bool stale_=false,recovering_=false;
    double recoveryStart_=0;
    static float smooth(float x) noexcept {x=std::clamp(x,0.f,1.f);return x*x*(3.f-2.f*x);}
    void remember(const AudioFrame& f) noexcept {lastLevel_=f.level;lastRelative_=f.relative;lastGate_=f.gate;}
public:
    double ageMilliseconds=-1;
    float freshnessGain=0;
    std::uint64_t staleObservations=0,recoveries=0,identityRejects=0;
    void reset() noexcept {
        serial_=recording_=0;stale_=recovering_=false;lastLevel_.fill(0);lastRelative_.fill(0);
        lastGate_=recoveryGate_=0;ageMilliseconds=-1;freshnessGain=0;
    }
    const AudioFrame* sample(const AudioFrame* input,std::uint64_t expectedSerial,
        std::uint64_t expectedRecording,double age,double wallNow) noexcept {
        if(serial_!=expectedSerial||recording_!=expectedRecording){reset();serial_=expectedSerial;recording_=expectedRecording;}
        ageMilliseconds=std::isfinite(age)?age*1000.0:-1.0;
        if(!input||!input->valid||!expectedSerial||!expectedRecording||
            input->serial!=expectedSerial||input->recording!=expectedRecording||
            !std::isfinite(age)||!std::isfinite(wallNow)||age<-.025){
            if(input)++identityRejects;
            freshnessGain=0;stale_=true;recovering_=false;lastLevel_.fill(0);lastRelative_.fill(0);lastGate_=0;
            return nullptr;
        }
        age=std::max(0.0,age);
        if(age>=1.5){
            freshnessGain=0;++staleObservations;stale_=true;recovering_=false;
            lastLevel_.fill(0);lastRelative_.fill(0);lastGate_=0;return nullptr;
        }
        const bool isStale=age>.25;
        if(isStale){++staleObservations;recovering_=false;}
        if(stale_&&!isStale){
            recoveryLevel_=lastLevel_;recoveryRelative_=lastRelative_;recoveryGate_=lastGate_;
            recoveryStart_=wallNow;recovering_=true;++recoveries;
        }
        stale_=isStale;
        freshnessGain=isStale?float(std::exp(-(age-.25)/.45))*(1.f-smooth(float((age-1.10)/.40))):1.f;
        // Only aging/recovery needs a full snapshot copy. Fresh steady playback
        // retains its existing 50 Hz response, no extra normal-playback smoothing.
        const float onsetAgeGain=float(std::exp(-std::max(0.0,age-.045)/.10));
        if(!isStale&&!recovering_&&onsetAgeGain>=.99999f){remember(*input);return input;}
        view_=*input;view_.gate*=freshnessGain;
        for(unsigned i=0;i<kBands;++i){
            view_.level[i]*=freshnessGain;
            view_.relative[i]*=freshnessGain;
            view_.hit[i]*=freshnessGain*onsetAgeGain;
        }
        if(recovering_){
            const float a=smooth(float((wallNow-recoveryStart_)/.080));
            for(unsigned i=0;i<kBands;++i){
                view_.level[i]=recoveryLevel_[i]+a*(view_.level[i]-recoveryLevel_[i]);
                view_.relative[i]=recoveryRelative_[i]+a*(view_.relative[i]-recoveryRelative_[i]);
                view_.hit[i]*=a; // never resurrect a pre-gap transient
            }
            view_.gate=recoveryGate_+a*(view_.gate-recoveryGate_);
            if(a>=1.f)recovering_=false;
        }
        remember(view_);return &view_;
    }
};
} // namespace nekorox::component::collection
