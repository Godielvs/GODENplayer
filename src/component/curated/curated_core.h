#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include "../collection/collection_motion.h"

namespace nekorox::component::curated {
enum class Preset : std::uint32_t { caustics=0, lagrangian=1 };
enum class Mode : std::uint32_t { original=0, music=1 };
enum class Response : std::uint32_t { expressive=0, legacyC2=1 };
enum class Palette : std::uint32_t { original=0, artwork=1 };
inline bool selectedStyle(unsigned style) noexcept { return style==2U||style==3U||style==4U; }
inline Preset presetForStyle(unsigned style) noexcept { return style==3U?Preset::lagrangian:Preset::caustics; }
struct Color {float r=.2f,g=.2f,b=.2f,a=1.f;};
struct Input {
    Preset preset=Preset::caustics; Mode mode=Mode::music; Palette palette=Palette::original;
    std::array<float,3> band{}, transient{};
    // Immutable shared analysis, populated only for expressive Caustics.
    std::array<float,64> levels{},onsets{},relatives{};
    std::array<Color,4> colors{Color{.15f,.15f,.15f,1},Color{.1f,.1f,.1f,1},Color{.25f,.25f,.25f,1},Color{.006f,.006f,.006f,1}};
    float intensity=1.f;
    Response response=Response::expressive;
    float speed=2.f; // used only by Caustics Music; Original always remains 1x
    float projectionSteps=1.f; // internal host count, not an audio parameter
    float substepOffset=0.f; // seconds inside the current wave reference frame
    bool active=false;
    bool playing=true; // distinct from visible/eligible; pause has a visual tail
    std::uint64_t restartRevision=0;
};
struct Extent {std::uint32_t width=0,height=0;};
inline Extent gridFor(Preset preset,Mode mode,float) noexcept {
    if(preset==Preset::lagrangian&&mode==Mode::original)return {512,512};
    // Simulation resolution is a bounded preset policy, not the first HWND
    // size observed. Presentation still targets the exact Album viewport, so
    // resize never recompiles shaders or resets state merely to chase aspect.
    return {512,288};
}
// One reference frame = one original frame (16 substeps for the wave field).
// The original websites are refresh-dependent. C1 fixes the reference at 60 Hz;
// the caustics source's frame/120 phase expression remains unchanged.
// Fixed integration step. Rate changes the number of reference steps, never
// the solver's dt. No time passes while hidden/paused; no catch-up backlog.
class Timeline {
    collection::CoalescedClock clock_{};
    double rate_=1.0;
public:
    static constexpr double referenceStep=1.0/60.0,renderStep=1.0/30.0;
    void setRate(double rate)noexcept{if(std::isfinite(rate))rate_=std::clamp(rate,.5,2.0);}
    void setTransport(bool playing,bool eligible,double now)noexcept{
        if(!eligible)clock_.suspend(now);else clock_.transport(playing,true,now);
    }
    void setActive(bool value,double now)noexcept{setTransport(value,value,now);}
    bool due(double now)const noexcept{return clock_.due(now);}
    unsigned advance(double now)noexcept{return clock_.advance(now,30,rate_);}
    void defer(double now)noexcept{clock_.defer(now,30);}
    void resume(double now)noexcept{
        if(clock_.deferred){clock_.deferred=false;clock_.anchor=now;clock_.debt=0;clock_.next=std::min(clock_.next,now);}
    }
    void reset(double now)noexcept{clock_.reset(now);}
    double deadline()const noexcept{return clock_.next;}
    double droppedSeconds()const noexcept{return clock_.dropped;}
    bool active()const noexcept{return clock_.running;}
    bool deferred()const noexcept{return clock_.deferred;}
};

inline float finite01(float v) noexcept{return std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;}
inline float finiteColor(float v) noexcept{return std::isfinite(v)?std::clamp(v,0.f,4.f):0.f;}
struct MusicSignals {
    std::array<float,3> absolute{};
    std::array<float,3> relativeFast{1.f,1.f,1.f};
    std::array<float,3> relativeSlow{1.f,1.f,1.f};
    std::array<float,3> growth{};
    std::array<float,3> attack{};
    float gate=0.f,intensity=0.f;
};
struct MusicBandBinding {
    float absolute=0.f,relativeFast=0.f,relativeSlow=0.f,growth=0.f,attack=0.f;
    float continuousGain=1.f,impulseGain=1.f;
};
struct PresetBindings {std::array<MusicBandBinding,3> band{};};
inline constexpr PresetBindings kCausticsBindings{{
    MusicBandBinding{.62f,.18f,.10f,.10f,.78f,1.00f,1.00f},
    MusicBandBinding{.48f,.24f,.18f,.10f,.72f,.86f,.82f},
    MusicBandBinding{.38f,.28f,.14f,.20f,.82f,.72f,.92f}}};
inline constexpr PresetBindings kWaveBindings{{
    MusicBandBinding{.54f,.16f,.20f,.10f,.84f,.90f,1.00f},
    MusicBandBinding{.44f,.24f,.20f,.12f,.76f,.82f,.88f},
    MusicBandBinding{.32f,.30f,.16f,.22f,.88f,.68f,.96f}}};
struct BoundMusic {std::array<float,3> continuous{},impulse{};float intensity=0.f;};
inline BoundMusic bindMusicLegacy(Preset preset,const MusicSignals& signal) noexcept {
    BoundMusic result;
    const auto gate=finite01(signal.gate);
    if(gate<=0.f)return result; // relative neutral (about 1) is never force by itself
    const auto& bindings=preset==Preset::caustics?kCausticsBindings:kWaveBindings;
    for(std::size_t i=0;i<3;++i){
        const auto relativeFast=finite01((std::clamp(signal.relativeFast[i],0.f,4.f)-1.f)/1.5f);
        const auto relativeSlow=finite01((std::clamp(signal.relativeSlow[i],0.f,4.f)-1.f)/1.5f);
        const auto growth=finite01(signal.growth[i]);
        const auto& b=bindings.band[i];
        result.continuous[i]=gate*finite01(b.continuousGain*(
            b.absolute*finite01(signal.absolute[i])+b.relativeFast*relativeFast+
            b.relativeSlow*relativeSlow+b.growth*growth));
        result.impulse[i]=gate*finite01(b.impulseGain*(
            b.attack*finite01(signal.attack[i])+(1.f-b.attack)*growth));
    }
    result.intensity=gate*finite01(signal.intensity);
    return result;
}
inline float signedFinite(float value,float lo,float hi,float neutral=0.f) noexcept {
    return std::isfinite(value)?std::clamp(value,lo,hi):neutral;
}
inline BoundMusic bindMusic(Preset preset,const MusicSignals& s,
    Response response=Response::expressive) noexcept {
    if(response==Response::legacyC2)return bindMusicLegacy(preset,s);
    BoundMusic r;const float gate=finite01(s.gate);if(gate==0.f)return r;
    for(unsigned i=0;i<3;++i){
        const float absolute=std::pow(finite01(s.absolute[i]),1.15f);
        const float fast=std::log2(std::max(.125f,signedFinite(s.relativeFast[i],.125f,4.f,1.f)));
        const float slow=std::log2(std::max(.125f,signedFinite(s.relativeSlow[i],.125f,4.f,1.f)));
        const float contrast=1.f+.28f*std::tanh(.55f*fast+.30f*slow);
        r.continuous[i]=gate*finite01(absolute*contrast);
        const float onset=.8f*finite01(s.attack[i])+.2f*std::max(0.f,signedFinite(s.growth[i],-1.f,1.f));
        r.impulse[i]=gate*finite01(onset);

    }
    r.intensity=gate*finite01(s.intensity);return r;
}
inline bool expressive(const Input& in) noexcept {
    return in.mode==Mode::music&&in.response==Response::expressive;
}
inline double simulationRate(const Input& in) noexcept {
    return in.preset==Preset::caustics&&in.mode==Mode::music
        ?double(signedFinite(in.speed,.5f,2.f,2.f)):1.0;
}
inline std::uint64_t acceptedRevisionAfterPresent(std::uint64_t accepted,
    std::uint64_t drawn,std::uint64_t produced,bool presentAccepted) noexcept {
    if(!presentAccepted||drawn<=accepted)return accepted;
    return std::min(drawn,produced);
}
struct alignas(16) Constants {
    std::array<std::uint32_t,4> dimensions{}; // width, height, referenceFrame, flags
    std::array<float,4> bands{};
    std::array<float,4> attacks{};
    std::array<float,4> timing{};
    std::array<Color,4> colors{};
    std::array<std::array<float,4>,16> levels{},onsets{},relatives{};
};
static_assert(sizeof(Constants)==896);
static_assert(alignof(Constants)==16);
inline Constants makeConstants(const Input& in,Extent e,std::uint64_t frame)noexcept{
    Constants c;
    const auto visibleFrame=std::uint32_t(frame&0xffffffffULL);
    c.dimensions={e.width,e.height,visibleFrame,
        (in.mode==Mode::music?1U:0U)|(in.palette==Palette::artwork?2U:0U)|
        (in.response==Response::legacyC2?4U:0U)|
        (expressive(in)&&in.preset==Preset::caustics?8U:0U)};
    for(unsigned i=0;i<3;++i){c.bands[i]=finite01(in.band[i]);c.attacks[i]=finite01(in.transient[i]);}
    c.bands[3]=finite01(in.intensity);
    c.attacks[3]=signedFinite(in.projectionSteps,1.f,6.f,1.f);
    c.timing={float(double(frame)/60.)+signedFinite(in.substepOffset,0.f,1.f/60.f),
        expressive(in)&&in.preset==Preset::lagrangian?1.f/480.f:1.f/60.f,.398f,.814f};
    for(unsigned i=0;i<4;++i)c.colors[i]={finiteColor(in.colors[i].r),finiteColor(in.colors[i].g),finiteColor(in.colors[i].b),1};
    if(expressive(in)&&in.preset==Preset::caustics){
        for(unsigned i=0;i<64;++i){
            c.levels[i/4][i%4]=signedFinite(in.levels[i],0.f,4.f);
            c.onsets[i/4][i%4]=signedFinite(in.onsets[i],0.f,4.f);
            c.relatives[i/4][i%4]=signedFinite(in.relatives[i],-1.f,1.f);
        }
    }
    return c;
}
// Derivative of the selected source's action is affine in predicted q2.
// Used ONLY in Music mode. Original retains the finite-difference Newton loop.
inline double wavePrediction(double q,double v,double left,double right,double up,double down,double k=1.,double a=.814)noexcept{
    return .5*(left+right+up+down)-(q-v)-k*a*std::sin(a*q);
}
} // namespace
