#include "../src/component/c11/c11_note_lights.h"
#include "../src/component/c11/c11_alloy_gpu.h"
#include "c13_reference_v1.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>

namespace {
void requireC13(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
}

void runC13ResponseTests() {
    using namespace goden::c11;
    namespace current=nekorox::component::collection;
    requireC13(noteCount==120,"C13 note grid is not 120 semitones");
    requireC13(minibandMaximumDegrees==5,"C13 miniband tilt is not five degrees");
    requireC13(sizeof(AlloyGpu)==2304,"C13 AlloyGpu ABI changed");
    requireC13(offsetof(AlloyGpu,miniband)==320,"C13 miniband ABI offset changed");
    requireC13(offsetof(AlloyGpu,broadLevel)==2240,"C13 broad-level ABI offset changed");

    // Factoring the shared law must not change Original V1 response.
    c13_reference_v1::LightFeatureModel old;
    current::LightFeatureModel now;
    std::array<float,64> bands{};
    std::mt19937 rng(713);
    for(int frame=0;frame<2000;++frame){
        const float rms=-70.f+float(rng()%7000)/100.f;
        for(auto& band:bands)band=-120.f+float(rng()%13200)/100.f;
        if(frame%100==0){old.discontinuity(true);now.discontinuity(true);}
        if(frame==1311){old.reset();now.reset();}
        const float dt=frame%3==0?1.f/60.f:.02f;
        const auto a=old.observe(bands,rms,dt);
        const auto b=now.observe(bands,rms,dt);
        requireC13(std::abs(a.master-b.master)<1e-7f,"C13 changed V1 master transfer");
        requireC13(a.valid==b.valid,"C13 changed V1 validity");
        for(int i=0;i<8;++i){
            requireC13(std::abs(a.level[i]-b.level[i])<1e-7f,"C13 changed V1 level transfer");
            requireC13(std::abs(a.texture[i]-b.texture[i])<1e-7f,"C13 changed V1 variation transfer");
        }
    }


    // A broadband plateau must not erase local timbre differences at high level.
    NoteProjector projector;FocusTracker tracker;
    std::array<float,1025> powers{};NoteFrame frame{};NoteLights lights{};
    for(unsigned publication=1;publication<=150;++publication){
        powers.fill(.002f);
        frame=projector.ingest(powers.data(),powers.size(),2048,48000,-8,.02f,1,1,publication);
        lights=tracker.update(frame,.02f);
    }
    float flat=0;for(const auto& b:lights.miniband)flat=std::max(flat,b.energy);
    requireC13(flat<.03f,"C12 lit a uniform broadband plateau as prominent notes");
    for(unsigned publication=151;publication<=250;++publication){
        powers[42]=powers[43]=powers[44]=.04f;
        frame=projector.ingest(powers.data(),powers.size(),2048,48000,-8,.02f,1,1,publication);
        lights=tracker.update(frame,.02f);
    }
    const auto peak=std::size_t(std::max_element(frame.prominence.begin(),frame.prominence.end())-frame.prominence.begin());
    requireC13(noteHz(peak)>750&&noteHz(peak)<1300,"C12 prominence lost the local spectral peak");
    requireC13(lights.miniband[peak].energy>flat+.3f,"C12 lost timbre contrast in dense music");
    requireC13(projector.planBuilds()==1,"C12 rebuilt the frequency plan per publication");
    const auto repeated=projector.ingest(powers.data(),powers.size(),2048,48000,-8,.02f,1,1,250);
    requireC13(repeated.level==frame.level&&repeated.flux==frame.flux&&repeated.prominence==frame.prominence,
        "C12 reintegrated a duplicate publication");
    const auto repeatedLights=tracker.update(repeated,.02f);
    for(int i=0;i<120;++i){
        requireC13(repeatedLights.miniband[i].angle==lights.miniband[i].angle&&
            repeatedLights.miniband[i].energy==lights.miniband[i].energy,"C12 reintegrated a duplicate frame");
        requireC13(std::abs(lights.miniband[i].angle)<=minibandMaximumRadians,"C12 escaped its tilt limit");
    }
    requireC13(projector.ingest(nullptr,0,0,48000,0,.02,1,1,251).level[60]==0,"C12 accepted null FFT input");
    requireC13(projector.ingest(powers.data(),powers.size(),2048,48000,0,.02,0,1,251).level[60]==0,"C12 accepted invalid serial");
    requireC13(projector.ingest(powers.data(),powers.size(),2048,48000,0,.02,1,2,251).level[60]==0,"C12 accepted mismatched recording");
    requireC13(projector.ingest(powers.data(),powers.size(),2048,48000,0,.02,1,1,249).level[60]==0,"C12 accepted stale publication");
    const auto plans=projector.planBuilds();projector.discontinuity(true);powers.fill(.05f);
    const auto recovered=projector.ingest(powers.data(),powers.size(),2048,48000,-12,.02,1,1,251);
    requireC13(projector.planBuilds()==plans,"C12 soft gap rebuilt the FFT plan");
    for(float v:recovered.flux)requireC13(v==0,"C12 invented an attack after a soft gap");
    requireC13(recovered.level[60]>0,"C12 discarded the preserved envelope after a gap");
    NoteProjector low;std::array<float,257> lowPower{};lowPower.fill(.001f);
    const auto lowFrame=low.ingest(lowPower.data(),lowPower.size(),512,8000,-10,.02,2,2,1);
    FocusTracker lowTracker;const auto lowLights=lowTracker.update(lowFrame,.02f);
    requireC13(lowFrame.confidence[119]==0,"C12 fabricated high-note confidence above Nyquist");
    requireC13(lowLights.miniband[119].halfWidth==0&&lowLights.miniband[119].energy==0&&
        lowLights.miniband[119].texture==0,"C12 emitted unsupported notes");
    // Confidence loss also retires an already bright note, not merely an idle one.
    frame.confidence[peak]=0;frame.sourceSequence=300;
    const auto retired=tracker.update(frame,.02f);
    requireC13(retired.miniband[peak].halfWidth==0&&retired.miniband[peak].energy==0&&
        retired.miniband[peak].texture==0,"C12 retained an unsupported active note");
    ArtProfile art{};art.valid=true;art.count=3;art.roughness=.4f;art.hardness=.7f;art.anisotropy=.6f;
    const auto baseline=makeGpu(art,lights,2.,10.,1.);
    requireC13(baseline.material.x==.4f&&baseline.material.y==.7f&&baseline.material.z==.6f&&
        std::abs(baseline.motion.x-.1f)<1e-6f,"C12 material payload was not restored");
    const auto later=makeGpu(art,lights,2.,1e12,1.);
    requireC13(std::isfinite(later.motion.x)&&std::abs(later.motion.x)<=3.142f,"C12 material phase became unbounded");
    NoteLights none;const auto noNotes=makeGpu(art,none,2,10,1);
    for(const auto& band:noNotes.miniband)requireC13(band.w==0,"C12 packed width without a publication");
    const auto unsupported=makeGpu(art,lowLights,2,10,1);
    requireC13(unsupported.miniband[119].w==0,"C12 GPU enabled an unsupported note");
    for(int i=0;i<1000;++i){bands.fill(-120);const auto silence=now.observe(bands,-120,.02f);
        if(i==999)requireC13(silence.master==0,"V1 master did not close on silence");}
}
