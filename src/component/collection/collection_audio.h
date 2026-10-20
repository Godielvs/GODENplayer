#pragma once
// GODEN C4. Fixed-storage stereo power analysis, owned by the existing analysis
// worker. No allocations in ingest(), no dependency on paint cadence.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <span>
#include <limits>
#include "c9_light_features.h"
#include "../c11/c11_note_lights.h"

namespace nekorox::component::collection {
inline constexpr unsigned kBands=64, kAudioWidth=512, kHistoryRows=512;
struct alignas(16) V4 { float x=0,y=0,z=0,w=0; };
struct AudioFrame {
    std::uint64_t sequence=0,serial=0,recording=0,sourceQpc=0,firstSample=0,endSample=0;
    std::uint32_t rate=0; bool valid=false, gap=false;
    float gate=0, rmsDb=-120;
    LightBands lights{};
    goden::c11::NoteLights noteLights{};
    std::array<float,kBands> level{},hit{},relative{};
    std::array<V4,kAudioWidth> row{}; // R weighted FFT, G left waveform [.0,1], B raw FFT, A gate
    // Immutable source diagnostics; UI never reads mutable Analyzer fields.
    std::uint64_t adaptationEpoch=0,softGapCount=0,hardResetCount=0;
};
class Analyzer {
    static constexpr unsigned MaxN=8192;
    std::array<float,MaxN> left_{},right_{},window_{};
    std::array<std::complex<float>,MaxN> fftL_{},fftR_{};
    std::array<float,MaxN/2+1> powers_{};
    std::array<float,kBands> baseline_{},fast_{},hits_{},smooth_{};
    LightFeatureModel lightFeatures_;
    goden::c11::NoteProjector noteProjector_;
    goden::c11::FocusTracker focusTracker_;
    unsigned n_=2048,head_=0,filled_=0,hop_=960,since_=0,rate_=0,channels_=0;
    std::uint64_t serial_=0,recording_=0,nextSample_=0,sequence_=0;
    float windowSum_=1; bool baselineValid_=false,pendingGap_=true,notesEnabled_=false;
    // C8: a bounded same-source interruption invalidates FFT history, not the
    // musical adaptation. No missing PCM is fabricated or interpolated.
    bool suppressRecoveredOnset_=false;
    std::uint64_t lastBlockEndQpc_=0,adaptationEpoch_=0,softGapCount_=0,hardResetCount_=0;
    // Fixed frequency geometry and row weighting depend only on rate/FFT
    // size. Keep their original float operation order; musical observations,
    // envelopes and FFT cadence remain live and unchanged.
    struct BandPlan { float center=0,half=0; unsigned first=0,last=0; };
    struct RowPlan { float k=0,weight=0; unsigned a=0,b=0; };
    std::array<BandPlan,kBands> bandPlan_{};
    std::array<RowPlan,kAudioWidth> rowPlan_{};
    unsigned planRate_=0,planN_=0;
    void prepareGeometry() noexcept {
        if(planRate_==rate_ && planN_==n_)return;
        const float top=std::min(18000.f,float(rate_)*.48f),binHz=float(rate_)/float(n_);
        for(unsigned b=0;b<kBands;++b){
            const float lo=25.f*std::pow(top/25.f,float(b)/kBands),hi=25.f*std::pow(top/25.f,float(b+1)/kBands);
            const float center=std::sqrt(lo*hi),half=std::max((hi-lo)*.5f,binHz);
            const unsigned first=unsigned(std::max(1.f,std::floor((center-half)/binHz)));
            const unsigned last=std::min(n_/2,unsigned(std::ceil((center+half)/binHz)));
            bandPlan_[b]={center,half,first,last};
        }
        for(unsigned x=0;x<kAudioWidth;++x){
            float k=float(x)*float(n_/2)/float(kAudioWidth-1);
            unsigned a=std::min(n_/2,unsigned(k)),b=std::min(n_/2,a+1);
            rowPlan_[x]={k,std::pow(std::max(float(x)/511.f,.012f),.18f),a,b};
        }
        planRate_=rate_;planN_=n_;
    }
    static float safe(float x) noexcept { return std::isfinite(x)?std::clamp(x,-8.f,8.f):0.f; }
    static float db(float p) noexcept {return std::max(-120.f,10.f*std::log10(std::max(p,1.e-12f)));}
    static void fft(std::complex<float>* a,unsigned n) noexcept {
        for(unsigned i=1,j=0;i<n;++i){unsigned bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(a[i],a[j]);}
        for(unsigned len=2;len<=n;len<<=1){const float angle=-6.283185307179586f/float(len);
            const std::complex<float> root(std::cos(angle),std::sin(angle));
            for(unsigned i=0;i<n;i+=len){std::complex<float>w(1,0);for(unsigned j=0;j<len/2;++j){auto u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=root;}}}
    }
    void configure(unsigned rate,unsigned channels,std::uint64_t serial,std::uint64_t recording,std::uint64_t first,bool keepAdaptation=false) noexcept {
        lightFeatures_.discontinuity(keepAdaptation);
        noteProjector_.discontinuity(keepAdaptation);
        rate_=rate;channels_=channels;serial_=serial;recording_=recording;nextSample_=first;
        n_=2048;while(n_<MaxN && float(n_)<float(rate)*2048.f/48000.f)n_*=2;
        prepareGeometry();
        hop_=std::max(1U,rate/50U);head_=filled_=since_=0;pendingGap_=true;
        left_.fill(0);right_.fill(0);hits_.fill(0);
        suppressRecoveredOnset_=keepAdaptation;
        if(keepAdaptation){++softGapCount_;}
        else{baselineValid_=false;smooth_.fill(0);baseline_.fill(-120);fast_.fill(-120);
            noteProjector_.reset();focusTracker_.reset();
            ++adaptationEpoch_;++hardResetCount_;}
        windowSum_=0;for(unsigned i=0;i<n_;++i){window_[i]=.5f-.5f*std::cos(6.28318530718f*float(i)/float(n_-1));windowSum_+=window_[i];}
    }
    AudioFrame analyze(std::uint64_t qpc) noexcept {
        AudioFrame out;out.sequence=++sequence_;out.serial=serial_;out.recording=recording_;out.sourceQpc=qpc;
        out.endSample=nextSample_;out.firstSample=nextSample_>=n_?nextSample_-n_:0;out.rate=rate_;out.gap=pendingGap_;pendingGap_=false;out.valid=true;
        double rms=0;for(unsigned i=0;i<n_;++i){unsigned j=(head_+i)%n_;float l=left_[j],r=right_[j];rms+=(double(l)*l+double(r)*r)*.5;fftL_[i]={l*window_[i],0};fftR_[i]={r*window_[i],0};}
        out.rmsDb=db(float(rms/n_));out.gate=std::clamp((out.rmsDb+72.f)/14.f,0.f,1.f);out.gate=out.gate*out.gate*(3-2*out.gate);
        fft(fftL_.data(),n_);fft(fftR_.data(),n_);
        const float norm=2.f/(windowSum_*windowSum_);
        for(unsigned k=0;k<=n_/2;++k)powers_[k]=(std::norm(fftL_[k])+std::norm(fftR_[k]))*.5f*norm;
        const float binHz=float(rate_)/float(n_),dt=float(hop_)/rate_;
        if(notesEnabled_){
            const auto noteFrame=noteProjector_.ingest(powers_.data(),n_/2+1,n_,
                float(rate_),out.rmsDb,dt,serial_,recording_,out.sequence);
            out.noteLights=focusTracker_.update(noteFrame,dt);
        }
        std::array<float,kBands> lightDb{};
        for(unsigned b=0;b<kBands;++b){
            const auto& plan=bandPlan_[b];
            // Triangular frequency integration, narrow bands interpolate instead
            // of becoming empty below FFT bin spacing. Levels are power, not L+R.
            const float center=plan.center,half=plan.half;
            const unsigned first=plan.first,last=plan.last;
            float sum=0,weight=0;for(unsigned k=first;k<=last;++k){float w=std::max(0.f,1.f-std::abs(float(k)*binHz-center)/half);sum+=powers_[k]*w;weight+=w;}
            const float value=db(sum/std::max(weight,.001f));
            lightDb[b]=value;
            if(!baselineValid_){baseline_[b]=value;fast_[b]=value;}
            float old=fast_[b];float tau=value>old?.009f:.14f;
            fast_[b]+=(-std::expm1(-dt/tau))*(value-fast_[b]);
            baseline_[b]+=(-std::expm1(-dt/(value>baseline_[b]?2.5f:5.f)))*(value-baseline_[b]);
            // No additive relative baseline: a quiet band stays quiet even
            // after it has become the "normal" level for this song.
            const float magnitude=std::pow(std::max(0.f,(fast_[b]+74.f)/62.f),1.65f);
            const float contrast=1.f+.32f*std::tanh((fast_[b]-baseline_[b])/4.5f);
            const float bandGate=std::clamp((value+92.f)/28.f,0.f,1.f)*out.gate;
            const float onset=suppressRecoveredOnset_?0.f:
                std::clamp((fast_[b]-old-.85f)/8.f,0.f,1.f)*std::sqrt(magnitude);
            hits_[b]=std::max(onset,hits_[b]*std::exp(-dt/.13f));
            // Smooth shoulder instead of clipping at -18 dB: compressed bands retain
            // headroom for small musical differences in the loud regime.
            const float desired=(-std::expm1(-1.35f*magnitude*contrast))*bandGate;
            const float musicalTau=desired>smooth_[b]?.050f:.200f;
            smooth_[b]+=(-std::expm1(-dt/musicalTau))*(desired-smooth_[b]);
            out.level[b]=smooth_[b];
            out.hit[b]=hits_[b]*bandGate;
            out.relative[b]=std::clamp((fast_[b]-baseline_[b])/12.f,-1.f,1.f)*bandGate;

        }
        out.lights=lightFeatures_.observe(lightDb,out.rmsDb,dt);
        baselineValid_=true;suppressRecoveredOnset_=false;
        out.adaptationEpoch=adaptationEpoch_;out.softGapCount=softGapCount_;out.hardResetCount=hardResetCount_;
        for(unsigned x=0;x<kAudioWidth;++x){const auto& plan=rowPlan_[x];float k=plan.k;unsigned a=plan.a,b=plan.b;float p=powers_[a]+(powers_[b]-powers_[a])*(k-float(a));
            float raw=std::clamp((db(p)+90.f)/78.f,0.f,1.f)*out.gate;
            float weighted=std::clamp(raw*plan.weight*1.75f,0.f,1.f);
            unsigned sample=(head_+n_-std::min(n_,kAudioWidth)+x)%n_;
            out.row[x]={weighted,.5f+.5f*std::clamp(left_[sample],-1.f,1.f),raw,out.gate};
        }
        return out;
    }
public:
    void setNotesEnabled(bool enabled) noexcept {
        if(enabled==notesEnabled_)return;
        notesEnabled_=enabled;
        if(!enabled){noteProjector_.reset();focusTracker_.reset();}
    }
    template<class Publish>
    void ingest(std::span<const float> pcm,unsigned rate,unsigned channels,
        std::uint64_t serial,std::uint64_t recording,std::uint64_t first,
        std::uint64_t endQpc,std::uint64_t frequency,bool discontinuity,Publish&& publish) noexcept {
        if(rate<8000||rate>192000||channels==0||channels>32||!serial||!recording||pcm.empty()||pcm.size()%channels||!frequency)return;
        if(serial_ && (serial<serial_ || (serial==serial_&&recording!=recording_)))return;
        const auto count=pcm.size()/channels;
        if(count>std::numeric_limits<std::uint64_t>::max()-first)return;
        // Within a stream, replayed blocks cannot restart the adaptation.
        if(!discontinuity&&serial==serial_&&recording==recording_&&first+count<=nextSample_)return;
        const bool sameSource=rate_==rate&&channels_==channels&&serial_==serial&&recording_==recording;
        if(discontinuity||!sameSource||nextSample_!=first){
            const bool forward=first>=nextSample_;
            const auto missed=forward?first-nextSample_:std::numeric_limits<std::uint64_t>::max();
            const double blockSeconds=double(count)/double(rate);
            const bool timeBound=lastBlockEndQpc_&&endQpc>=lastBlockEndQpc_&&
                double(endQpc-lastBlockEndQpc_)/double(frequency)<=blockSeconds+.35;
            const bool preserve=baselineValid_&&sameSource&&forward&&timeBound&&
                missed<=std::uint64_t(double(rate)*.35);
            configure(rate,channels,serial,recording,first,preserve);
        }
        const auto frames=pcm.size()/channels;
        for(std::size_t i=0;i<frames;++i){
            left_[head_]=safe(pcm[i*channels]);right_[head_]=channels==1?left_[head_]:safe(pcm[i*channels+1]);
            // Deliberately L/R only; do not relabel a multichannel downmix.
            head_=(head_+1)%n_;filled_=std::min(n_,filled_+1);++nextSample_;++since_;
            if(filled_==n_&&since_>=hop_){since_=0;const auto remain=frames-1-i;
                const auto offset=std::uint64_t(double(remain)*double(frequency)/rate);
                publish(analyze(endQpc>offset?endQpc-offset:0));}
        }
            lastBlockEndQpc_=endQpc;
    }
};
} // namespace
