#pragma once
#include "collection_renderer.h"
#include "collection_preferences.h"
#include "collection_bridge.h"
#include "c8_source_guard.h"
#include "curated/curated_renderer.h"
#include "realtime_analysis_hub.h"
#include "runtime.h"
#include <atomic>
namespace nekorox::component::collection {
// A compatibility facade, not a second presentation clock or renderer window.
// The existing Album lifecycle drives exactly one backend at a time.
class VisualMux final {
    curated::Renderer legacy_;Renderer collection_;curated::Diagnostics compatible_{};
    std::uint64_t token_=0;bool leased_=false;std::shared_ptr<const AudioFrame> lastAudioFrame_;
    C8SourceGuard c8Source_;
    AudioIdentity renderedIdentity_{};
    bool resumeWaiting_=false;
    double resumeStart_=0,resumeFreshSince_=-1;
    std::uint64_t resumeSourceAfterQpc_=0;
    AudioHistoryLease historyLease_;
    AudioHistoryReader historyReader_;ID3D11Device* historyDevice_=nullptr;
    std::uint64_t historyRestart_=0;
    unsigned historyPreset_=0;bool historyMusic_=false,historyConfigured_=false;
    static bool usesAudioTexture(const Selection&s)noexcept{
        const auto all=presets();if(s.preset>=all.size())return false;const auto&p=all[s.preset];
        for(unsigned i=0;i<p.count;++i)for(const auto input:p.passes[i].inputs)if(input==8)return true;
        return false;
    }
    void resetHistoryReader()noexcept{historyReader_.reset();historyDevice_=nullptr;historyRestart_=0;
        historyPreset_=0;historyMusic_=false;historyConfigured_=false;}
    void configureHistoryReader(AudioIdentity identity,ID3D11Device*device,const Selection&s)noexcept{
        if(historyConfigured_&&historyReader_.identity()==identity&&historyDevice_==device&&historyPreset_==s.preset&&
            historyMusic_==s.music&&historyRestart_==s.restart)return;
        historyReader_.reset(identity);historyDevice_=device;historyPreset_=s.preset;historyMusic_=s.music;
        historyRestart_=s.restart;historyConfigured_=true;
    }
    void demand(bool active,bool caustics=false)noexcept{
        const auto s=selection();const auto all=presets();active=active&&
            (caustics?!legacy_.failed():(selected()&&s.preset<all.size()&&
            (s.music||all[s.preset].audioOriginal)&&!collection_.failed()));
        historyLease_.set(active&&!caustics&&usesAudioTexture(s));
        if(active==leased_)return;
        if(active){Runtime::instance().setRealtimeConsumerActive(RealtimeConsumer::albumAmbient,true);
            leased_=RealtimeAnalysisHub::instance().setDemand(AnalysisOwner::albumAmbient,token_,kAnalysisFeatureCollection,0,Runtime::instance().latestRealtimeSequence());
            if(!leased_)Runtime::instance().setRealtimeConsumerActive(RealtimeConsumer::albumAmbient,false);
        }else{RealtimeAnalysisHub::instance().removeDemand(AnalysisOwner::albumAmbient,token_);Runtime::instance().setRealtimeConsumerActive(RealtimeConsumer::albumAmbient,false);leased_=false;}
    }
    void sync()noexcept{const auto&d=collection_.diagnostics();compatible_.referenceFrames=d.frame;compatible_.simulationDispatches=d.dispatches+d.draws;
        compatible_.renderedFrames=d.produced;compatible_.resetCount=d.resetCount;compatible_.outputRevision=d.produced;compatible_.drawnRevision=d.drawn;compatible_.acceptedRevision=d.accepted;
        compatible_.bytes=d.bytes;compatible_.droppedQueries=d.queryDrops;compatible_.gridWidth=d.width;compatible_.gridHeight=d.height;compatible_.gpuP95Us=d.gpuP95Us;
        compatible_.prepareLastUs=d.cpuUs;compatible_.lastHr=d.result;compatible_.ready=d.ready;compatible_.budgetPaused=d.budget;compatible_.active=d.active;}
public:
    LRESULT qa(unsigned field,LPARAM value) noexcept {
        if(field==100)return value>=0&&static_cast<std::size_t>(value)<presets().size()?handleMenu(47000U+static_cast<UINT>(value)):FALSE;
        if(field==101)return value==0||value==1?handleMenu(48000U+static_cast<UINT>(value)):FALSE;
        if(field==102){if(value==75)return handleMenu(48009);if(value==100)return handleMenu(48008);
            return value==125||value==175?handleMenu(value==125?48002U:48003U):FALSE;}
        if(field==104)return value==0||value==1?handleMenu(value?48006U:48007U):FALSE;
        if(field==103){retry();return TRUE;}
        const auto&d=collection_.diagnostics();
        // QA reads run on the owner UI thread. Bracket a multi-field sample with
        // frame (2) and raw sequence (51); reject it if either changes mid-read.
        // Keep these below 100: the host reserves that range for UI mutations.
        const auto&submitted=collection_.submittedConstantsForProbe();
        const auto*raw=lastAudioFrame_.get();
        const auto fixed=[](float v){return static_cast<LRESULT>(v*1000000.f);};
        const auto mean=[](const std::array<float,64>&a,unsigned begin,unsigned end){
            float sum=0;for(unsigned b=begin;b<end;++b)sum+=a[b];return sum/float(end-begin);};
        const auto high=[](const std::array<float,64>&a,unsigned begin,unsigned end){
            float peak=0;for(unsigned b=begin;b<end;++b)peak=std::max(peak,a[b]);return peak;};
        if(field>=90&&field<=95){
            if(value<0||value>=64)return -1;
            const auto b=static_cast<unsigned>(value);
            if(field<=92)return raw?fixed(field==90?raw->level[b]:field==91?raw->hit[b]:raw->relative[b]):0;
            const auto&a=field==93?submitted.levels:field==94?submitted.onsets:submitted.relatives;
            const auto&v=a[b/4];return fixed(b%4==0?v.x:b%4==1?v.y:b%4==2?v.z:v.w);
        }
        switch(field){
        case 0:return selected()?1:0;case 1:return selection().preset;
        case 2:return static_cast<LRESULT>(d.frame);case 3:return static_cast<LRESULT>(d.produced);
        case 4:return static_cast<LRESULT>(d.drawn);case 5:return static_cast<LRESULT>(d.accepted);
        case 6:return static_cast<LRESULT>(d.resetCount);case 7:return static_cast<LRESULT>(d.resizeCount);
        case 8:return static_cast<LRESULT>(d.bytes);case 9:return d.ready;case 10:return d.active;
        case 11:return d.budget;case 12:return d.deferred;case 13:return static_cast<LRESULT>(d.result);
        case 14:return static_cast<LRESULT>(d.cpuUs*1000.0);case 15:return static_cast<LRESULT>(d.gpuP95Us*1000.0);
        case 16:return d.width;case 17:return d.height;case 18:return static_cast<LRESULT>(d.audioRows);
        case 19:return static_cast<LRESULT>(Bridge::instance().drops());case 20:return static_cast<LRESULT>(d.queryDrops);
        case 21:return d.gpuTimingAvailable;case 22:return static_cast<LRESULT>(d.audioSequenceApplied);
        case 23:return static_cast<LRESULT>(RealtimeAnalysisHub::instance().activeFeatureMask());
        case 24:return static_cast<LRESULT>(d.audioRowsSkipped);case 25:return static_cast<LRESULT>(d.audioRestoreFence);
        case 26:return static_cast<LRESULT>(d.audioReadCalls);case 27:return d.audioRestorePending;
        case 28:return static_cast<LRESULT>(historyReader_.cursor());case 29:return static_cast<LRESULT>(historyReader_.plannedThrough());
        case 30:return static_cast<LRESULT>(historyReader_.generation());case 31:return static_cast<LRESULT>(historyReader_.resourceEpoch());
        case 32:return historyReader_.staged();
        case 33:return static_cast<LRESULT>(c8Source_.ageMilliseconds*1000.0);
        case 34:return static_cast<LRESULT>(c8Source_.freshnessGain*1000000.f);
        case 35:return static_cast<LRESULT>(c8Source_.recoveries);
        case 36:return lastAudioFrame_?static_cast<LRESULT>(lastAudioFrame_->softGapCount):0;
        case 37:return lastAudioFrame_?static_cast<LRESULT>(lastAudioFrame_->hardResetCount):0;
        case 38:return static_cast<LRESULT>(d.resizeAttempts);
        case 39:return static_cast<LRESULT>(d.rejectedResizes);
        case 40:return static_cast<LRESULT>(d.resizeLastHr);
        case 41:return static_cast<LRESULT>(d.pressureTier);
        case 42:return static_cast<LRESULT>(d.pressureEvents);
        case 43:return static_cast<LRESULT>(d.nominalWidth);
        case 44:return static_cast<LRESULT>(d.nominalHeight);
        case 45:return static_cast<LRESULT>(d.pressureResumeSeconds*1000.0);
        case 46:return static_cast<LRESULT>(collection_.measurementEpoch());
        case 47:return static_cast<LRESULT>(d.gpuWindowEpoch);
        case 48:return d.gpuTimingAvailable && d.gpuP95Us>0.0 && d.gpuWindowEpoch==collection_.measurementEpoch();
        case 49:return static_cast<LRESULT>(d.explicitRetries);
        case 50:return 1; // Signal diagnostic schema version.
        case 51:return raw?static_cast<LRESULT>(raw->sequence):0;
        case 52:return raw?static_cast<LRESULT>(raw->serial):0;
        case 53:return raw?static_cast<LRESULT>(raw->recording):0;
        case 54:return raw&&raw->valid;
        case 55:return raw?static_cast<LRESULT>(raw->rmsDb*1000.f):-120000;
        case 56:return raw?fixed(raw->gate):0;
        case 57:return raw?static_cast<LRESULT>(raw->adaptationEpoch):0;
        case 58:return static_cast<LRESULT>(c8Source_.staleObservations);
        case 59:return static_cast<LRESULT>(c8Source_.identityRejects);
        case 60:case 61:case 62:{const unsigned group=field-60,begin=group==0?0:group==1?15:43,end=group==0?15:group==1?43:64;
            return raw?fixed(mean(raw->level,begin,end)):0;}
        case 63:case 64:case 65:{const unsigned group=field-63,begin=group==0?0:group==1?15:43,end=group==0?15:group==1?43:64;
            return raw?fixed(mean(raw->relative,begin,end)):0;}
        case 66:case 67:case 68:{const unsigned group=field-66,begin=group==0?0:group==1?15:43,end=group==0?15:group==1?43:64;
            return raw?fixed(high(raw->hit,begin,end)):0;}
        case 70:return fixed(submitted.bands.x);case 71:return fixed(submitted.bands.y);
        case 72:return fixed(submitted.bands.z);case 73:return fixed(submitted.bands.w);
        case 74:return fixed(submitted.attacks.x);case 75:return fixed(submitted.attacks.y);case 76:return fixed(submitted.attacks.z);
        case 77:return fixed(submitted.options.x);case 78:return fixed(submitted.size.z);
        case 79:return fixed(submitted.motion.w);case 80:return fixed(submitted.options.w);
        case 81:case 82:case 83:case 84:{const auto&p=submitted.palette[field-81];return fixed(.2126f*p.x+.7152f*p.y+.0722f*p.z);}
        case 85:return raw?static_cast<LRESULT>(raw->endSample):0;
        case 86:return raw?static_cast<LRESULT>(raw->rate):0;
        case 96:return raw?static_cast<LRESULT>(raw->sourceQpc):0;
        case 97:return raw&&raw->gap;
        case 98:return resumeWaiting_;
        default:return -1;
        }
    }
    VisualMux(){static std::atomic<std::uint64_t>next{0xC400000000000001ULL};token_=next.fetch_add(1,std::memory_order_relaxed);}
    ~VisualMux(){demand(false);}
    bool prepare(ID3D11Device*d,ID3D11DeviceContext*c,ID2D1DeviceContext*dc,std::uint64_t epoch,D2D1_SIZE_F v,const curated::Input&input,double now,bool allow=true)noexcept{
        if(!selected()){
            const bool musicalWater=input.preset==curated::Preset::caustics&&curated::expressive(input);
            demand(input.active&&input.playing&&musicalWater,musicalWater);
            collection_.active(false,now);auto water=input;
            if(musicalWater){
                if(leased_)lastAudioFrame_=Bridge::instance().latest();
                const AudioIdentity expected{Runtime::instance().playbackSerial(),Runtime::instance().recordingIdentityHash()};
                LARGE_INTEGER stamp{},frequency{};QueryPerformanceCounter(&stamp);QueryPerformanceFrequency(&frequency);
                const double age=lastAudioFrame_&&frequency.QuadPart>0?
                    double(stamp.QuadPart-std::int64_t(lastAudioFrame_->sourceQpc))/double(frequency.QuadPart):999.;
                const auto*audio=c8Source_.sample(lastAudioFrame_.get(),expected.serial,expected.recording,age,now);
                if(audio&&audio->valid){water.levels=audio->level;water.onsets=audio->hit;water.relatives=audio->relative;}
            }
            return legacy_.prepare(d,c,dc,epoch,v,water,now,allow);
        }
        legacy_.setActivity(false,now);const bool hadLease=leased_;
        demand(input.active&&input.playing);collection_.transport(input.playing,input.active,now);
        auto&bridge=Bridge::instance();const bool readAudio=allow&&(collection_.due(now)||!collection_.diagnostics().ready||collection_.diagnostics().preset!=selection().preset);
        const AudioIdentity expected{Runtime::instance().playbackSerial(),Runtime::instance().recordingIdentityHash()};
        if(!hadLease&&leased_&&collection_.diagnostics().ready&&
                collection_.diagnostics().preset==selection().preset&&renderedIdentity_==expected){
            resumeWaiting_=true;resumeStart_=now;resumeFreshSince_=-1;
            LARGE_INTEGER resumed{};QueryPerformanceCounter(&resumed);
            resumeSourceAfterQpc_=static_cast<std::uint64_t>(resumed.QuadPart);
        }
        if(!leased_||renderedIdentity_!=expected)resumeWaiting_=false;
        if(leased_)lastAudioFrame_=bridge.latest();
        auto latest=lastAudioFrame_;LARGE_INTEGER c8Now{},c8Frequency{};
        QueryPerformanceCounter(&c8Now);QueryPerformanceFrequency(&c8Frequency);
        const double c8Age=latest&&c8Frequency.QuadPart>0?
            double(c8Now.QuadPart-std::int64_t(latest->sourceQpc))/double(c8Frequency.QuadPart):999.0;
        const AudioFrame*audio=c8Source_.sample(latest.get(),expected.serial,expected.recording,c8Age,now);
        if(resumeWaiting_){
            if(audio&&latest&&latest->sourceQpc>resumeSourceAfterQpc_&&c8Age<=.100){
                if(resumeFreshSince_<0)resumeFreshSince_=now;
            }
            else resumeFreshSince_=-1;
            if(now-resumeStart_>=.250||(resumeFreshSince_>=0&&now-resumeFreshSince_>=.080))resumeWaiting_=false;
        }
        auto chosen=selection();
        for(unsigned i=0;i<4;++i)chosen.palette[i]={input.colors[i].r,input.colors[i].g,input.colors[i].b,1.f};
        configureHistoryReader(expected,d,chosen);const RetainedAudioRows::Batch*history=nullptr;
        if(leased_&&readAudio&&!resumeWaiting_&&usesAudioTexture(chosen)&&expected.valid()){
            history=historyReader_.stage(retainedAudioRows());
        }
        HistoryApplyReceipt receipt{};const auto ok=collection_.prepare(d,c,dc,epoch,v,chosen,audio,expected,history,
            historyReader_.generation(),&receipt,now,allow&&!resumeWaiting_);
        (void)historyReader_.commit(receipt);
        if(ok&&!resumeWaiting_&&collection_.producedThisFrame())renderedIdentity_=expected;
        if(collection_.failed())demand(false);sync();return ok;
    }
    bool draw(ID2D1RenderTarget*t,const D2D1_RECT_F&r,float o=1)noexcept{const bool result=selected()?collection_.draw(t,r,o):legacy_.draw(t,r,o);sync();return result;}
    void setActivity(bool a,double n)noexcept{if(selected()){legacy_.setActivity(false,n);if(!a){demand(false);lastAudioFrame_.reset();}collection_.active(a,n);}else{if(!a)demand(false);collection_.active(false,n);legacy_.setActivity(a,n);}sync();}
    void presented(bool a)noexcept{if(selected())collection_.presented(a);else legacy_.presented(a);sync();}
    void suspend(double n)noexcept{lastAudioFrame_.reset();demand(false);collection_.active(false,n);legacy_.suspend(n);sync();}
    void invalidateBitmap()noexcept{collection_.invalidateBitmap();legacy_.invalidateBitmap();}
    void retry()noexcept{if(selected()){resetHistoryReader();collection_.retry();}else legacy_.retry();sync();}
    void reset()noexcept{demand(false);c8Source_.reset();resumeWaiting_=false;renderedIdentity_={};resetHistoryReader();collection_.reset();legacy_.reset();sync();}
    bool needsFrame()const noexcept{return selected()?collection_.needsFrame():legacy_.needsFrame();}
    bool asynchronous()const noexcept{return selected()&&collection_.asynchronous();}
    // Last prepared eight-region light state for the curated/Clovis renderer.
    // The legacy AlbumAmbientController does not publish this path's lights.
    [[nodiscard]] const LightBands& appliedLights() const noexcept {
        static const LightBands empty{};
        return lastAudioFrame_ ? lastAudioFrame_->lights : empty;
    }
    bool due(double n)const noexcept{return selected()?collection_.due(n):legacy_.due(n);}
    double deadline()const noexcept{return selected()?collection_.deadline():legacy_.deadline();}
    bool failed()const noexcept{return selected()?collection_.failed():legacy_.failed();}
    bool producedThisFrame()const noexcept{return selected()?collection_.producedThisFrame():legacy_.producedThisFrame();}
    const curated::Diagnostics& diagnostics()const noexcept{return selected()?compatible_:legacy_.diagnostics();}
    ID3D11Texture2D*outputTexture()const noexcept{return selected()?collection_.outputTexture():legacy_.outputTexture();}
};
} // namespace
