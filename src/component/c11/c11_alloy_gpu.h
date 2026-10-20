#pragma once
#include <array>
#include <cstddef>
#include <type_traits>
#include "c11_art_profile.h"
#include "c11_note_lights.h"
namespace goden::c11 {
struct alignas(16) F4 {float x=0,y=0,z=0,w=0;};
struct alignas(16) AlloyGpu {
    F4 control;             // master, pigmentCount, fieldAspect, valid; refined C12 only
    F4 material;            // roughness, hardness, anisotropy, orientation
    F4 art;                 // meanLuma, contrast, edgeDensity, neutralFraction
    F4 motion;              // bounded material phase, gains, pause/freshnessGain
    std::array<F4,8> pigment;// linear RGB, normalized broad weight
    std::array<F4,8> accent; // accentWeight, area, salience, affinity to dominant pigment
    // One entry per E0..D#10 semitone: energy, texture, angle radians,
    // normalized half-width. Angle is already clamped to +/-5 degrees.
    std::array<F4,noteCount> miniband;
    std::array<F4,2> broadLevel;
    std::array<F4,2> broadTexture;
};
static_assert(std::is_trivially_copyable_v<AlloyGpu>);
static_assert(sizeof(AlloyGpu)==144*16);
static_assert(offsetof(AlloyGpu,pigment)==4*16);
static_assert(offsetof(AlloyGpu,miniband)==20*16);
static_assert(offsetof(AlloyGpu,broadLevel)==140*16);
inline AlloyGpu makeGpu(const ArtProfile& p,const NoteLights& lights,float aspect,
                        double time,float freshness,
                        const std::array<float,8>& broadLevel={},
                        const std::array<float,8>& broadTexture={},
                        float broadMaster=-1.f)noexcept{
    AlloyGpu g{};
    g.control={std::clamp(safe(broadMaster>=0?broadMaster:lights.master),0.f,2.f),float(p.count),(std::max)(.1f,aspect),p.valid?1.f:0.f};
    g.material={p.roughness,p.hardness,p.anisotropy,p.orientation};
    g.art={p.meanLuma,p.contrast,p.edgeDensity,p.neutralFraction};
    const float phase=std::isfinite(time)?float(std::remainder(time*.01,6.2831853071795864769)):0.f;
    g.motion={phase,1.f,1.f,std::clamp(safe(freshness),0.f,1.f)};
    const auto dominant=lab(p.pigments[0].linear);
    for(std::size_t i=0;i<8;++i){
        const auto& c=p.pigments[i];
        g.pigment[i]={c.linear.x,c.linear.y,c.linear.z,c.broadWeight};
        const auto perceptual=lab(c.linear);
        const float chromaDistance=std::hypot(perceptual.y-dominant.y,perceptual.z-dominant.z);
        g.accent[i]={c.accentWeight,c.area,c.salience,1.f-smooth(.025f,.14f,chromaDistance)};
    }
    for(std::size_t i=0;i<noteCount;++i){
        const auto& band=lights.miniband[i];
        g.miniband[i]={std::clamp(safe(band.energy),0.f,1.5f),std::clamp(safe(band.texture),0.f,2.f),
            std::clamp(safe(band.angle),-minibandMaximumRadians,
                minibandMaximumRadians),
            lights.sequence&&band.halfWidth>0?std::clamp(safe(band.halfWidth),
                minibandMinimumSemitones/float(noteCount),
                minibandMaximumSemitones/float(noteCount)):0.f};
    }
    for(std::size_t block=0;block<2;++block){
        const auto k=block*4;
        g.broadLevel[block]={broadLevel[k],broadLevel[k+1],broadLevel[k+2],broadLevel[k+3]};
        g.broadTexture[block]={broadTexture[k],broadTexture[k+1],broadTexture[k+2],broadTexture[k+3]};
    }
    return g;
}
} // namespace goden::c11
