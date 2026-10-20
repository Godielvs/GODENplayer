#pragma once
#include "../c11/c11_note_lights.h"
#include "../c11/c11_art_profile.h"

namespace nekorox::component::a4 {
// Presentation-only envelopes. Never feed these values back into shared analysis.
// The source sequence also holds this state during pause, stale input and redraws.
class ClovisResponse final {
    std::array<float,8> broad_{};
    std::array<float,goden::c11::noteCount> notes_{};
    std::array<float,goden::c11::noteCount> fast_{};
    std::uint64_t sequence_=0;
    double sampledAt_=-1;
    float calm_=0;
public:
    static float noteTargetDb(float db) noexcept {
        return std::isfinite(db)?collection::v1BandTarget(db):0.f;
    }
    static float activity(const goden::c11::NoteLights& notes,
                          const collection::LightBands& broad) noexcept {
        using goden::c11::smooth;
        float occupied=0,variation=0,supported=0;
        for(const auto& band:notes.miniband){
            if(!(band.halfWidth>0))continue;
            const float presence=smooth(.045f,.20f,goden::c11::safe(band.energy));
            occupied+=presence;
            variation+=presence*goden::c11::sat(goden::c11::safe(band.texture));
            supported+=1;
        }
        const float coverage=occupied/(std::max)(supported,1.f);
        const float flux=variation/(std::max)(occupied,1.f)*std::sqrt(coverage);
        return (std::max)({smooth(.18f,.55f,goden::c11::safe(broad.master)),
            smooth(.16f,.50f,coverage),smooth(.12f,.45f,flux)});
    }
    void advance(const goden::c11::NoteLights& notes,
                 const collection::LightBands& broad,float freshness,double now) noexcept {
        if(!std::isfinite(now))return;
        if(!notes.sequence||!broad.valid||!(freshness>0)||!std::isfinite(freshness)){
            sampledAt_=now;return; // Missing audio is not a calm musical passage.
        }
        if(notes.sequence==sequence_)return;
        const float dt=sampledAt_<0||now<=sampledAt_||now-sampledAt_>.5?
            1.f/30.f:float((std::min)(now-sampledAt_,.1));
        const float target=1.f-activity(notes,broad);
        if(!sequence_)calm_=target;
        else calm_=goden::c11::approach(calm_,target,dt,1.5f,.25f);
        for(std::size_t i=0;i<broad_.size();++i)
            broad_[i]=goden::c11::approach(broad_[i],
                std::clamp(goden::c11::safe(broad.level[i]),0.f,1.5f),dt,.080f,.280f);
        for(std::size_t i=0;i<notes_.size();++i){
            if(!(notes.miniband[i].halfWidth>0)){notes_[i]=fast_[i]=0;continue;}
            // Match Clovis Light's level law and attack/release, using this
            // note's measured dB rather than its already compressed loudness.
            fast_[i]=noteTargetDb(notes.miniband[i].absoluteDb);
            notes_[i]=goden::c11::approach(notes_[i],
                fast_[i],dt,.035f,.180f);
        }
        sequence_=notes.sequence;sampledAt_=now;
    }
    float calmTexture() const noexcept {return calm_;}
    template<class Gpu> void apply(Gpu& gpu) const noexcept {
        for(std::size_t i=0;i<broad_.size();++i){
            auto& packed=gpu.broadLevel[i/4];
            switch(i%4){case 0:packed.x=broad_[i];break;case 1:packed.y=broad_[i];break;
                case 2:packed.z=broad_[i];break;default:packed.w=broad_[i];break;}
        }
        for(std::size_t i=0;i<notes_.size();++i){
            gpu.miniband[i].x=notes_[i];gpu.miniband[i].y=fast_[i];
        }
    }
};
inline goden::c11::Color3 clovisDominantPigment(const goden::c11::ArtProfile& profile) noexcept {
    const auto count=(std::min)(profile.count,std::uint32_t(profile.pigments.size()));
    if(!count)return {};
    std::size_t best=0;float weight=-1;
    // Near-black is the space around the light, not an emitting pigment. First
    // choose the most prevalent chromatic ink; monochrome art uses its largest
    // non-black neutral. A coherent red outline on black therefore stays red.
    for(unsigned pass=0;pass<2;++pass){
        for(std::size_t i=0;i<count;++i){
            const auto& p=profile.pigments[i];const auto lab=goden::c11::lab(p.linear);
            if(goden::c11::luminance(p.linear)<.003f ||
                (pass==0&&std::hypot(lab.y,lab.z)<.025f))continue;
            if(p.broadWeight>weight){weight=p.broadWeight;best=i;}
        }
        if(weight>=0)break;
    }
    return profile.pigments[best].linear;
}
} // namespace nekorox::component::a4
