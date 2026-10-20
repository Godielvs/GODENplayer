#pragma once
#include "collection_audio.h"
#include "collection_motion.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nekorox::component::collection {
struct Blob {const unsigned char* bytes=nullptr;std::size_t count=0;};
enum class Stage:std::uint8_t {fragment,compute};
// Input indexes 0..7 = latest committed pass version, 8 = audio, 9 = noise,
// 10 = zeroed keyboard texture. -1 = zero texture. Self reads the old side.
struct Pass {
    const char* key="";Stage stage=Stage::fragment;int output=7;
    unsigned scale=1,repeat=1,dispatch=0;bool once=false,fullFloat=false,final=false;
    std::array<int,11> inputs{};unsigned wrapMask=0;Blob original{},music{};
};
struct Preset {
    const char* id="";const wchar_t* title=L"";const Pass* passes=nullptr;unsigned count=0;
    unsigned maxEdge=768,alignment=1,rate=30;bool stateful=false,flipY=true,audioOriginal=false,experimental=false,storage=false;
};
std::span<const Preset> presets() noexcept;
Blob fullscreenVertexShader() noexcept;
Blob resamplePixelShader() noexcept;
struct Selection {unsigned preset=0;bool music=true;float intensity=1.25f;std::uint64_t restart=0;bool albumColors=true;std::array<V4,4> palette{V4{.22f,.22f,.22f,1},V4{.46f,.46f,.46f,1},V4{.12f,.12f,.12f,1},V4{.02f,.02f,.02f,1}};};
struct Extent {unsigned width=0,height=0; friend bool operator==(Extent,Extent)=default;};
inline Extent fitExtent(float width,float height,unsigned limit,unsigned alignment=1) noexcept {
    if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0)return {};
    alignment=std::clamp(alignment,1U,64U);limit=std::clamp(limit,64U,2048U);
    const double scale=std::min(1.0,double(limit)/std::max(double(width),double(height)));
    auto round=[&](double x){return std::max(alignment,(unsigned(std::max(1.0,std::floor(x))) /alignment)*alignment);};
    return {round(width*scale),round(height*scale)};
}
// Output dimensions are independent from simulation/camera/playback identity.
// On drag: preserve old graph. Commit the latest requested extent only after
// quiet; one reallocation, no shader recompilation and no unbounded catch-up.
class Timeline {
    CoalescedClock clock_;
public:
    void active(bool value,double now) noexcept {transport(value,value,now);}
    void transport(bool playing,bool eligible,double now) noexcept {
        if(!eligible){clock_.suspend(now);return;}
        clock_.transport(playing,true,now);
    }
    bool due(double now,unsigned hz=30)const noexcept {(void)hz;return clock_.due(now);}
    unsigned advance(double now,unsigned hz=30)noexcept{return clock_.advance(now,hz);}
    void defer(double now)noexcept{clock_.defer(now,30);}
    void reset(double now)noexcept{clock_.reset(now);}
    double deadline()const noexcept{return clock_.next;}
    double elapsed()const noexcept{return clock_.elapsed;}
    double delta()const noexcept{return clock_.lastDelta;}
    double droppedSeconds()const noexcept{return clock_.dropped;}
    bool active()const noexcept{return clock_.running;}
    double speed(double now)const noexcept{return clock_.rate.sample(now);}
};
struct alignas(16) Constants {
    V4 size,meta,bands,attacks,motion,options;
    std::array<V4,4> palette;
    std::array<V4,16> levels,onsets,relatives;
};
static_assert(sizeof(V4)==16 && sizeof(Constants)==928 && alignof(Constants)==16);
inline void packBands(std::array<V4,16>& to,const std::array<float,64>& from)noexcept{
    for(unsigned i=0;i<16;++i)to[i]={from[i*4],from[i*4+1],from[i*4+2],from[i*4+3]};
}
} // namespace
