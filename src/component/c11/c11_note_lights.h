#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "../collection/c9_light_features.h"

namespace goden::c11 {
constexpr std::size_t noteCount = 120; // E0..D#10 inclusive; not 108.
constexpr std::size_t focusCount = 12;
constexpr float minibandMaximumDegrees = 5.0f;
constexpr float minibandMaximumRadians =
    minibandMaximumDegrees * 3.14159265358979323846f / 180.0f;
constexpr float minibandMinimumSemitones = 4.25f;
constexpr float minibandMaximumSemitones = 6.0f;
inline float sat(float x) noexcept { return std::clamp(x, 0.0f, 1.0f); }
inline float safe(float x) noexcept { return std::isfinite(x) ? x : 0.0f; }
inline float smooth(float lo, float hi, float x) noexcept {
    const float t = sat((x-lo)/(hi-lo)); return t*t*(3.0f-2.0f*t);
}
inline float approach(float old, float target, float dt, float up, float down) noexcept {
    const float tau = target > old ? up : down;
    return old + (target-old) * (-std::expm1(-dt/tau));
}
inline float noteHz(std::size_t i) noexcept {
    // Numeric note coordinates are not MIDI messages; values above 127 are valid here.
    return 440.0f * std::exp2((float(i)+16.0f-69.0f)/12.0f);
}
struct NoteFrame {
    std::uint64_t serial=0, recording=0, sourceSequence=0;
    std::array<float,noteCount> level{}, prominence{}, confidence{}, flux{}, loudness{};
    // Already measured log power, retained for C12's presentation transfer.
    // No second FFT or change to the existing musical feature calculations.
    std::array<float,noteCount> absoluteDb = [] {
        std::array<float,noteCount> values;values.fill(-120.f);return values;
    }();
    float rmsDb=-120.0f;
};
struct Focus {
    // All coordinates are normalized to the local Album field, not desktop pixels.
    float u=0, spread=.05f, energy=0, texture=0;
    float seed=0, confidence=0, note=-1, reserved=0;
};
struct Miniband {
    // Energy/texture are musical. Angle is an actual screen-space tilt in
    // radians and is bounded to +/-5 degrees. Half-width is normalized to the
    // Album field; every entry maps 1:1 to E0..D#10 by array index.
    float energy=0, texture=0, angle=0;
    // Absolute energy for C12. Salience/flux remain available to other consumers.
    float loudness=0;
    float absoluteDb=-120.f;
    float halfWidth=minibandMinimumSemitones/float(noteCount);
};
struct NoteLights {
    std::array<Focus,focusCount> focus{};
    std::array<Miniband,noteCount> miniband{};
    float master=0;
    std::uint64_t sequence=0;
};
class NoteProjector {
    static constexpr std::size_t tapCapacity = 16384;
    struct Kernel {unsigned first=0,count=0,offset=0;float confidence=0;};
    std::array<Kernel,noteCount> kernels_{};
    std::array<float,tapCapacity> weights_{};
    std::array<float,noteCount> level_{},baselineDb_{},previousDb_{},activity_{};
    std::size_t bins_=0,fftSize_=0;
    float rate_=0,globalBaseline_=-120,previousGlobal_=-120;
    std::uint64_t serial_=0,recording_=0,lastSequence_=0,planBuilds_=0;
    bool planValid_=false,historyValid_=false;
    NoteFrame last_{};
    static float median(float* first,std::size_t count) noexcept {
        if(!count)return -120.f;
        auto* middle=first+count/2;
        std::nth_element(first,middle,first+count);
        return *middle;
    }
    bool prepare(std::size_t bins,std::size_t fftSize,float rate) noexcept {
        if(planValid_&&bins==bins_&&fftSize==fftSize_&&rate==rate_)return true;
        kernels_={};std::size_t used=0;
        const float binHz=rate/float(fftSize);
        const float top=(std::min)(.48f*rate,float(bins-1)*binHz);
        for(std::size_t i=0;i<noteCount;++i){
            const float f=noteHz(i);if(f>top)continue;
            const float spacing=f*(std::exp2(1.f/12.f)-1.f);
            const float sigma=(std::max)(.6f*spacing,binHz);
            const unsigned first=unsigned((std::max)(1,int(std::floor((f-2*sigma)/binHz))));
            const unsigned end=unsigned((std::min)(int(bins)-1,int(std::ceil((f+2*sigma)/binHz))));
            if(end<first)continue;
            const unsigned count=end-first+1;
            if(used+count>weights_.size()){planValid_=false;return false;}
            float sum=0;
            for(unsigned k=0;k<count;++k){
                const float d=(float(first+k)*binHz-f)/sigma;
                weights_[used+k]=std::exp(-.5f*d*d);sum+=weights_[used+k];
            }
            for(unsigned k=0;k<count;++k)weights_[used+k]/=(std::max)(sum,1e-12f);
            kernels_[i]={first,count,unsigned(used),sat(spacing/(1.5f*binHz))};
            used+=count;
        }
        bins_=bins;fftSize_=fftSize;rate_=rate;planValid_=true;
        ++planBuilds_;reset();return true;
    }
public:
    void reset() noexcept {
        level_={};activity_={};baselineDb_.fill(-120.f);previousDb_.fill(-120.f);
        globalBaseline_=previousGlobal_=-120;
        historyValid_=false;serial_=recording_=lastSequence_=0;last_={};
    }
    void discontinuity(bool keepEnvelope) noexcept {
        if(!keepEnvelope){reset();return;}
        historyValid_=false;activity_.fill(0.f);lastSequence_=0;last_={};
        // Keep levels and the FFT projection plan, suppress the first
        // recovered variation exactly as the V1 LightFeatureModel does.
    }
    std::uint64_t planBuilds() const noexcept {return planBuilds_;}
    NoteFrame ingest(const float* power,std::size_t bins,std::size_t fftSize,
        float sampleRate,float rmsDb,float dt,std::uint64_t serial,
        std::uint64_t recording,std::uint64_t sequence) noexcept {
        using namespace nekorox::component::collection;
        NoteFrame out{};out.serial=serial;out.recording=recording;
        out.sourceSequence=sequence;out.rmsDb=std::isfinite(rmsDb)?rmsDb:-120.f;
        if(!power||bins<2||fftSize<2||fftSize>8192||bins>fftSize/2+1||
           !std::isfinite(sampleRate)||sampleRate<8000||sampleRate>768000||
           !std::isfinite(rmsDb)||!std::isfinite(dt)||dt<=0||dt>.5f||!serial||!recording)return out;
        if(serial_&&(serial<serial_||(serial==serial_&&recording!=recording_)))return out;
        if(!prepare(bins,fftSize,sampleRate))return out;
        if(serial_==serial&&recording_==recording){
            if(sequence==lastSequence_&&lastSequence_)return last_;
            if(sequence<lastSequence_)return out;
        }else{reset();serial_=serial;recording_=recording;}
        dt=(std::min)(dt,.12f);
        std::array<float,noteCount> db{},globalValues{};db.fill(-120);
        std::size_t supported=0;
        for(std::size_t i=0;i<noteCount;++i){
            const auto& k=kernels_[i];if(!k.count)continue;
            double p=0;
            for(unsigned j=0;j<k.count;++j)
                p+=double(weights_[k.offset+j])*(std::max)(0.f,safe(power[k.first+j]));
            db[i]=std::clamp(float(10.*std::log10((std::max)(p,1e-12))),-120.f,24.f);
            out.absoluteDb[i]=db[i];
            globalValues[supported++]=db[i];out.confidence[i]=k.confidence;
        }
        const float global=median(globalValues.data(),supported);
        const bool previous=historyValid_;
        if(!previous){baselineDb_=previousDb_=db;globalBaseline_=previousGlobal_=global;}
        const float commonStep=global-previousGlobal_;
        const float commonTrend=global-globalBaseline_;
        const float audible=smooth(-88.f,-60.f,rmsDb);
        for(std::size_t i=0;i<noteCount;++i){
            if(!kernels_[i].count){level_[i]=activity_[i]=0;continue;}
            // Salience is measured in LOG POWER before any visual compression.
            // A loud distorted mix cannot erase a local contrast by saturating level[].
            std::array<float,40> neighbor{};std::size_t count=0;
            const int hole=std::clamp(int(std::ceil(1.f/(std::max)(.08f,out.confidence[i]))),1,12);
            const int radius=(std::max)(6,(std::min)(19,hole*2+2));
            for(int d=-radius;d<=radius;++d){
                const int j=int(i)+d;
                if(std::abs(d)<=hole||j<0||j>=int(noteCount)||!kernels_[j].count)continue;
                neighbor[count++]=db[std::size_t(j)];
            }
            const float local=count?median(neighbor.data(),count):global;
            const float excess=(std::max)(0.f,db[i]-local-1.25f);
            const float contrast=-std::expm1(-excess/8.f);
            const float globalExcess=-std::expm1(-(std::max)(0.f,db[i]-global-4.f)/12.f);
            const float novelty=previous?
                -std::expm1(-(std::max)(0.f,db[i]-baselineDb_[i]-commonTrend-.8f)/6.f):0.f;
            const float movement=previous?std::abs(db[i]-previousDb_[i]-commonStep):0.f;
            const float activity=-std::expm1(-(std::max)(0.f,movement-.35f)/((std::max)(dt,.005f)*280.f));
            activity_[i]=approach(activity_[i],activity,dt,.018f,.09f);
            const float presence=audible*smooth(-102.f,-72.f,db[i]);
            const float absolute=-std::expm1(-(std::max)(0.f,db[i]+90.f)/40.f);
            const float magnitude=(std::max)(0.f,(db[i]+88.f)/62.f);
            out.loudness[i]=smooth(-102.f,-78.f,db[i])*
                (-std::expm1(-1.4f*std::pow(magnitude,1.8f)));
            level_[i]=approach(level_[i],presence*absolute,dt,.035f,.20f);
            // Global contrast only reinforces LOCAL contrast; empty high bins do
            // not make all occupied bands look individually prominent.
            const float salience=std::pow(contrast,.82f)*(.88f+.12f*globalExcess);
            const float target=presence*(.022f*absolute+1.08f*salience+
                .23f*novelty+.12f*activity_[i]*salience);
            out.level[i]=level_[i];
            out.prominence[i]=std::clamp(target,0.f,1.45f);
            out.flux[i]=activity_[i]*presence*(.35f+.65f*salience);
            baselineDb_[i]+=(-std::expm1(-dt/1.0f))*(db[i]-baselineDb_[i]);
            previousDb_[i]=db[i];
        }
        globalBaseline_+=(-std::expm1(-dt/1.0f))*(global-globalBaseline_);
        previousGlobal_=global;historyValid_=true;
        serial_=serial;recording_=recording;lastSequence_=sequence;last_=out;
        return out;
    }
};
class FocusTracker {
    std::array<Miniband,noteCount> minibands_{};
    std::uint64_t serial_=0,recording_=0,lastSequence_=0;
    float master_=0;
public:
    void reset() noexcept {minibands_={};serial_=recording_=lastSequence_=0;master_=0;}
    NoteLights update(const NoteFrame& frame,float dt) noexcept {
        NoteLights out{};
        if(!std::isfinite(dt)||dt<=0||dt>.5f||!frame.serial||!frame.recording)return out;
        if(serial_&&(frame.serial<serial_||(frame.serial==serial_&&frame.recording!=recording_)))return out;
        if(frame.serial!=serial_||frame.recording!=recording_){reset();serial_=frame.serial;recording_=frame.recording;}
        if(frame.sourceSequence<lastSequence_)return out;
        if(frame.sourceSequence!=lastSequence_){
            for(std::size_t i=0;i<noteCount;++i){
                auto& band=minibands_[i];
                if(frame.confidence[i]<=0){band={};band.halfWidth=0;continue;}
                const float raw=std::clamp(safe(frame.prominence[i]),0.f,1.45f);
                const float target=raw*smooth(.009f,.030f,raw);
                band.energy=approach(band.energy,target,dt,.022f,.125f);
                band.texture=approach(band.texture,sat(safe(frame.flux[i])),dt,.020f,.12f);
                band.loudness=sat(safe(frame.loudness[i]));
                band.absoluteDb=std::isfinite(frame.absoluteDb[i])?
                    std::clamp(frame.absoluteDb[i],-120.f,24.f):-120.f;
                // Keep current C12 geometry: width in semitones, +/-5 degree
                // physical tilt, same stable phase and 110 ms tilt smoothing.
                const float stable=std::sin((float(i)+.5f)*2.399963229728653f);
                const float left=i>0?frame.level[i-1]:frame.level[i];
                const float right=i+1<noteCount?frame.level[i+1]:frame.level[i];
                const float balance=(right-left)/((std::max)(.08f,left+right));
                const float requested=std::clamp(minibandMaximumRadians*(
                    stable*(.55f+.35f*sat(band.texture))+.35f*balance),
                    -minibandMaximumRadians,minibandMaximumRadians);
                band.angle=approach(band.angle,requested,dt,.11f,.11f);
                band.halfWidth=(minibandMinimumSemitones+(1.f-sat(frame.confidence[i]))*
                    (minibandMaximumSemitones-minibandMinimumSemitones))/float(noteCount);
                // Unsupported frequencies must not emit the V1 per-lane pedestal.
                if(frame.confidence[i]<=0)band.halfWidth=0;
            }
            nekorox::component::collection::v1UpdateMaster(
                nekorox::component::collection::lightMasterFromDb(frame.rmsDb),dt,master_);
            lastSequence_=frame.sourceSequence;
        }
        out.miniband=minibands_;out.master=master_;out.sequence=lastSequence_;
        return out;
    }
};
} // namespace goden::c11
