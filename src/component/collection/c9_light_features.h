#pragma once
// C9: observation of the EXISTING 64-band FFT; not an FFT/decoder of its own.
// Values below are visual controls, not calibrated LUFS or instrument labels.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nekorox::component::collection {
struct LightBands {
    std::array<float,8> level{}, texture{};
    float master=0.f;
    bool valid=false;
};
inline float lightMasterFromDb(float rmsDb) noexcept {
    if(!std::isfinite(rmsDb)) return 0.f;
    const float x=std::clamp((rmsDb+65.f)/55.f,0.f,1.f);
    return std::pow(x,2.6f); // -65 dB: off; -45: .072; -30: .308; -10: 1.
}
// C13: one transfer law for an eight-bin V1 broad group or one projected
// note lane. "texture" here is two-sided AUDIO variation, not a visual texture.
inline float v1BandTarget(float bandDb) noexcept {
    const float x=(std::max)(0.f,(bandDb+78.f)/64.f);
    return (std::min)(1.5f,std::pow(x,1.7f));
}
inline float v1FluxTarget(float summedVariation,unsigned contributors) noexcept {
    return contributors ? (std::min)(2.f,summedVariation/(float(contributors)*3.5f)):0.f;
}
inline void v1UpdateLane(float target,float variation,float dt,float& level,float& texture) noexcept {
    const float tau=target>level?.035f:.180f;
    level+=(-std::expm1(-dt/tau))*(target-level);
    const float fluxTau=variation>texture?.028f:.140f;
    texture+=(-std::expm1(-dt/fluxTau))*(variation-texture);
}
inline void v1UpdateMaster(float target,float dt,float& master) noexcept {
    const float tau=target>master?.030f:.160f;
    master+=(-std::expm1(-dt/tau))*(target-master);
    if(master<1.e-7f)master=0.f;
}
class LightFeatureModel final {
    std::array<float,64> previous_{};
    LightBands state_{};
    bool previousValid_=false;
public:
    void reset() noexcept { previous_={}; state_={}; previousValid_=false; }
    void discontinuity(bool keepEnvelope) noexcept {
        previousValid_=false; // never turn a missing interval into an onset
        state_.texture.fill(0.f);
        if(!keepEnvelope) reset();
    }
    LightBands observe(const std::array<float,64>& db,float rmsDb,float dt) noexcept {
        if(!std::isfinite(dt)||dt<=0.f||dt>.5f) return state_;
        for(unsigned group=0;group<8;++group){
            double power=0.;float flux=0.f;
            for(unsigned j=0;j<8;++j){const unsigned b=group*8+j;
                const float x=std::isfinite(db[b])?std::clamp(db[b],-120.f,24.f):-120.f;
                power+=std::pow(10.,double(x)*.1);
                const float signal=std::clamp((x+86.f)/24.f,0.f,1.f);
                if(previousValid_) flux+=(std::min)(12.f,std::abs(x-previous_[b]))*signal;
                previous_[b]=x;
            }
            const float bandDb=float(10.*std::log10((std::max)(power/8.,1.e-12)));
            const float target=v1BandTarget(bandDb);
            const float texture=v1FluxTarget(flux,8);
            v1UpdateLane(target,texture,dt,state_.level[group],state_.texture[group]);
        }
        const float master=lightMasterFromDb(rmsDb);
        v1UpdateMaster(master,dt,state_.master);
        // Settle exactly, rather than retaining denormals as perpetual motion.
        if(state_.master < 1.e-7f) state_.master=0.f;
        for(unsigned i=0;i<8;++i){
            if(state_.level[i]<1.e-7f) state_.level[i]=0.f;
            if(state_.texture[i]<1.e-7f) state_.texture[i]=0.f;
        }
        state_.valid=true;previousValid_=true;return state_;
    }
};
inline float lightFreshness(double age) noexcept {
    if(!std::isfinite(age)||age<-.005||age>=1.5)return 0.f;
    if(age<=.25)return 1.f;
    const double t=std::clamp((age-.25)/1.25,0.,1.);
    return float((1.-t*t*(3.-2.*t))*std::exp(-2.*t));
}
} // namespace

namespace nekorox::component::collection {
// UI-side sampling of an immutable publication, not a new audio processor.
inline LightBands presentLights(const LightBands& source,
    std::uint64_t serial, std::uint64_t recording,
    std::uint64_t expectedSerial, std::uint64_t expectedRecording,
    double ageSeconds, float gain) noexcept {
    if(!source.valid || !serial || !recording || serial!=expectedSerial ||
        recording!=expectedRecording || !std::isfinite(source.master) ||
        !std::isfinite(gain) || gain<=0.f) return {};
    LightBands result=source;
    for(unsigned i=0;i<8;++i){
        if(!std::isfinite(result.level[i]) || !std::isfinite(result.texture[i]))return {};
        result.level[i]=std::clamp(result.level[i],0.f,1.5f);
        result.texture[i]=std::clamp(result.texture[i],0.f,2.f);
    }
    result.master=std::clamp(result.master,0.f,1.f)*
        lightFreshness(ageSeconds)*std::clamp(gain,0.f,1.f);
    if(result.master<1.e-7f) result.master=0.f;
    return result;
}
} // namespace
