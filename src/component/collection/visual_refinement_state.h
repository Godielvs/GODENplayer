#pragma once
#include "collection_audio.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nekorox::component::collection {
// Visual-owned state. Never called by the analyzer or a capture callback.
inline float visualEnergy(float x) noexcept {
    return std::isfinite(x)?std::max(x,0.f):0.f;
}
inline float visualSlew(float from,float to,double dt,double rise,double fall) noexcept {
    return from+float(-std::expm1(-std::clamp(dt,0.,.1)/(to>from?rise:fall)))*(to-from);
}
struct StormDischarge {
    std::uint32_t seed=0,band=0;
    float age=2.f,life=1.f,birthTravel=0,peak=0,release=1,rank=0,birthAspect=1;
    bool live()const noexcept {return seed&&age<life&&release>.003f;}
    // GPU descriptor: exact 24-bit seed, age, world-space birth Z, radiance.
    V4 descriptor()const noexcept {
        if(!live())return {};
        const float fade=std::clamp((life-age)/.34f,0.f,1.f);
        const float onset=std::clamp(age/.035f,0.f,1.f);
        return {float(seed),age,birthTravel,peak*release*fade*onset*rank};
    }
};
class StormState {
    std::uint32_t random_=1;
    std::uint64_t serial_=0,recording_=0,sequence_=0;
    std::array<float,64> previous_{},refractory_{},hitFloor_{},firedPeak_{},valley_{};
    unsigned primary_=3;
    std::uint32_t next()noexcept {
        random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
        return (random_&0xffffffU)+1U;
    }
    static float horizontal(std::uint32_t seed)noexcept {
        auto x=seed^(601U*0x9e3779b9U);x^=x>>16;x*=0x7feb352dU;x^=x>>15;x*=0x846ca68bU;x^=x>>16;
        return float(x>>8)*(1.f/16777216.f);
    }
public:
    std::array<StormDischarge,3> events{};
    std::uint64_t births=0;
    void seed(std::uint32_t value)noexcept{random_=value?value:0x63f127adU;}
    bool synchronizeIdentity(std::uint64_t serial,std::uint64_t recording) noexcept {
        if(!serial||!recording||(serial_==serial&&recording_==recording))return false;
        serial_=serial;recording_=recording;sequence_=0;
        previous_={};refractory_={};hitFloor_={};firedPeak_={};valley_={};primary_=3;
        for(auto&e:events)e.release=0;
        return true;
    }
    void step(const AudioFrame* audio,double delta,float travel,float intensity,bool playing,
              float aspect=16.f/9.f)noexcept {
        const float dt=std::isfinite(delta)?float(std::clamp(delta,0.,.1)):0.f;
        intensity=std::clamp(visualEnergy(intensity),0.f,2.f);
        if(!std::isfinite(travel))travel=0;
        aspect=std::isfinite(aspect)?std::clamp(aspect,.25f,4.f):1.f;
        const bool valid=audio&&audio->valid&&audio->serial&&audio->recording;
        if(valid)synchronizeIdentity(audio->serial,audio->recording);
        const bool fresh=valid&&audio->sequence!=sequence_;
        for(auto&r:refractory_)r=std::max(0.f,r-dt);
        for(unsigned i=0;i<3;++i){auto&e=events[i];
            e.age+=dt;
            if(fresh&&playing&&visualEnergy(audio->level[e.band])<.025f&&e.age>.13f)
                e.release*=float(std::exp(-dt/.04));
            else if(e.release<.99f)e.release*=float(std::exp(-dt/.04));
            e.rank=visualSlew(e.rank,i==primary_?1.f:.28f,dt,.05,.15);
        }
        if(!fresh)return;
        sequence_=audio->sequence;
        // Absolute energy rejects tiny fluctuations. Dense, sustained spectra
        // require more prominence; a fired region must fall into a real valley
        // before rearming. There is no global metronome or cooldown: unrelated
        // instruments can still strike together.
        const float absolute=std::isfinite(audio->rmsDb)?std::clamp((audio->rmsDb+54.f)/30.f,0.f,1.f):0.f;
        const float drive=absolute*absolute*(3.f-2.f*absolute);
        float busy=0;
        for(float level:audio->level)
            busy+=std::clamp((visualEnergy(level)*intensity-.12f)/.35f,0.f,1.f)/64.f;
        std::array<float,64> scores{};
        for(unsigned b=0;b<64;++b){
            float h=visualEnergy(audio->hit[b])*std::clamp(intensity,0.f,2.f);
            const float rise=h-previous_[b];previous_[b]=h;
            const float floor=hitFloor_[b];
            hitFloor_[b]=visualSlew(floor,h,dt,.45,.65);
            if(firedPeak_[b]>0&&valley_[b]<.06f)
                valley_[b]=h<std::max(.045f,.32f*firedPeak_[b])?valley_[b]+dt:0.f;
            if(!playing||audio->gap||refractory_[b]>0||
                (firedPeak_[b]>0&&valley_[b]<.06f)||
                h*drive<.085f+.18f*busy+.42f*floor||
                rise*drive<std::max(.055f,.14f*floor))continue;
            const float weight=b<15?1.f:b<43?.86f:.64f;
            scores[b]=weight*drive*(h+.18f*visualEnergy(audio->level[b]));
        }
        std::array<bool,3> used{};
        for(unsigned strike=0;strike<3;++strike){
        const auto bestBand=std::max_element(scores.begin(),scores.end());
        if(*bestBand==0)break;
        const unsigned chosen=unsigned(bestBand-scores.begin());const float score=*bestBand;
        unsigned slot=0;
        bool found=false;
        for(unsigned i=0;i<3;++i){
            // Let a channel finish its growth before replacing it, especially
            // under dense activity. Empty slots still admit simultaneous hits.
            if(used[i]||(events[i].live()&&events[i].age<.38f+.18f*busy))continue;
            if(!found||!events[i].live()||events[i].age/events[i].life>events[slot].age/events[slot].life){slot=i;found=true;}
            if(!events[i].live())break;
        }
        if(!found)break;
        auto value=next();float best=-1;
        // Avoid successive channels collapsing onto the same apparent position.
        // All attempts are bounded; shape remains random and fixed per event.
        for(unsigned trial=0;trial<8;++trial){auto candidate=next();float separation=1;
            for(unsigned i=0;i<3;++i)if(i!=slot&&events[i].live())
                separation=std::min(separation,std::abs(horizontal(candidate)-horizontal(events[i].seed)));
            if(separation>best){best=separation;value=candidate;}
            if(separation>.24f)break;
        }
        auto&e=events[slot];
        e={value,chosen,0,.6f+.8f*float((value>>8)&65535U)/65535.f,travel,
            1.4f+3.2f*score/(.25f+score),1,strike==0?1.f:.28f,aspect};
        if(strike==0)primary_=slot;
        used[slot]=true;++births;
        // Adjacent FFT regions often describe the same physical hit. Keep those
        // correlated onsets together while allowing distant regions to overlap.
        const unsigned first=chosen>5?chosen-5:0,last=std::min(chosen+5,63U);
        for(unsigned b=first;b<=last;++b){
            scores[b]=0;refractory_[b]=.20f+.32f*busy;
            firedPeak_[b]=std::max(previous_[b],previous_[chosen]*.5f);valley_[b]=0;
        }
        }
    }
};

class RadianceMotionState {
    std::array<float,12> speed_{};
public:
    std::array<double,12> phase{};
    void step(const AudioFrame* a,double dt,float intensity,bool playing)noexcept {
        dt=std::clamp(dt,0.,.1);
        for(unsigned i=0;i<12;++i){
            const unsigned first=i<8?i*8:(i-8)*16,count=i<8?8:16;
            float power=0,hit=0;
            if(a&&a->valid){for(unsigned j=first;j<first+count;++j){
                const float e=visualEnergy(a->level[j])*intensity;
                power+=e*e;hit=std::max(hit,visualEnergy(a->hit[j])*intensity);
            }}
            const float energy=std::sqrt(power/float(count));
            const float drive=energy/(.55f+energy);
            const float target=.12f+.025f*float(i)+5.4f*drive*drive+(playing?1.25f*hit/(.5f+hit):0.f);
            if(a&&a->valid)speed_[i]=visualSlew(speed_[i],target,dt,.07,.25);
            const double direction=(i%2? -1.:1.)*(i>=8?-1.:1.);
            phase[i]=std::remainder(phase[i]+direction*dt*double(speed_[i]),6.283185307179586);
        }
    }
};

// Presentation runs 60 ms behind observed source samples. The lattice remains
// attached to rows; only its fractional displacement changes between uploads.
class SpectrumScrollState {
    double newest_=0,cursor_=0,period_=.02;
    std::uint64_t publication_=0;
    bool initialized_=false,skipAdvance_=false;
public:
    void reset()noexcept{*this={};}
    void observe(std::uint64_t publication,double seconds,bool gap)noexcept {
        if(!publication||publication==publication_)return;
        if(!std::isfinite(seconds)||seconds<=0)seconds=double(publication)*.02;
        if(initialized_&&publication>publication_&&!gap){
            const double period=(seconds-newest_)/double(publication-publication_);
            if(period>.005&&period<.10)period_=period;
        }
        newest_=seconds;publication_=publication;
        if(!initialized_||gap||newest_-cursor_>.20||cursor_>newest_){
            cursor_=newest_-.06;initialized_=true;skipAdvance_=true;
        }
    }
    void advance(double dt)noexcept {
        if(skipAdvance_){skipAdvance_=false;return;}
        if(initialized_)cursor_=std::min(cursor_+std::clamp(dt,0.,.1),newest_-period_);
    }
    float rowsBehind()const noexcept {
        return initialized_?float(std::clamp((newest_-cursor_)/period_,1.,10.)):3.f;
    }
};
} // namespace
