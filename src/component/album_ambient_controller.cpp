#include "album_ambient_controller.h"
#include "a4/a4_preferences.h"
#include "collection/collection_bridge.h"
#include "realtime_analysis_hub.h"
#include "performance_telemetry.h"
#include "rollout_flags.h"
#include "runtime.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <windows.h>

namespace nekorox::component {
namespace {
std::atomic<std::uint64_t> nextLifetime{1};
std::uint64_t nowTicks() noexcept {
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}
float angleDistance(float a, float b) noexcept {
    return std::abs(std::remainder(a-b, 6.2831853071795864769f));
}
}
AlbumAmbientController::AlbumAmbientController() noexcept
    : lifetimeToken_(nextLifetime.fetch_add(1, std::memory_order_relaxed))
{
    LARGE_INTEGER f{};
    if(QueryPerformanceFrequency(&f) && f.QuadPart>0)
        qpcFrequency_=static_cast<std::uint64_t>(f.QuadPart);
}
AlbumAmbientController::~AlbumAmbientController(){shutdown();}

void AlbumAmbientController::setUserPaused(bool paused,std::uint64_t now) noexcept
{
    if(paused==userPaused_)return;
    const auto current=sample(now);
    const auto seconds=qpcSeconds(now);
    const auto raw=flowSignal_.sample(seconds,playbackActive_,true,muted_);
    for(std::size_t b=0;b<3;++b)
        pausePhaseOffset_[b]=std::remainder(current.phase[b]-raw.phase[b],6.28318530718f);
    userPaused_=paused;
    pauseTail_=paused && visible_ && current.availability>1.f/4096.f;
    pausePose_=current;pauseStarted_=seconds;
    pauseRate_.stop(seconds);
    if(pauseTail_){
        pauseRate_.set(1.0,seconds,0.0);
        pauseRate_.set(0.0,seconds,4.0);
        physicalMotion_=true;
    }
    ++visualRevision_;
}

void AlbumAmbientController::sync(const AlbumAmbientEligibility& e,
    std::uint64_t stream, double displayHz, std::uint64_t now) noexcept
{
    const bool modeChanged=mode_!=e.mode;
    mode_=e.mode; visible_=e.visible && e.albumMode;
    playbackActive_=e.playing; muted_=e.muted;
    if(modeChanged) ++visualRevision_;
    expectedStreamGeneration_=stream;
    // Clovis requests the existing collection analyser only while this owner
    // is eligible. The three-band Flow filter bank and its phases stay intact.
    const bool alloySelected=a4::style()==a4::Style::spectralAlloy;
    const bool wantSpectralLights=alloySelected &&
        a4::materialStyle()!=a4::MaterialStyle::legacy;
    const bool wantAlloyV2Notes=alloySelected &&
        a4::materialStyle()==a4::MaterialStyle::spectralAlloyV2;
    if(wantSpectralLights!=spectralLights_ || wantAlloyV2Notes!=alloyV2Notes_){
        spectralLights_=wantSpectralLights;
        alloyV2Notes_=wantAlloyV2Notes;
        if(leaseActive_){
            const auto mask=kAnalysisFeatureAlbumBands |
                (spectralLights_?kAnalysisFeatureCollection:0U) |
                (alloyV2Notes_?kAnalysisFeatureAlloyV2Notes:0U);
            if(!RealtimeAnalysisHub::instance().setDemand(AnalysisOwner::albumAmbient,
                    lifetimeToken_,mask,0.0,Runtime::instance().latestRealtimeSequence())){
                release(now,AlbumAmbientState::staticFallback);
            }
        }
    }
    const auto hz=ambientBackgroundHz(displayHz);
    textureSchedule_.setPeriod(std::max<std::uint64_t>(1,
        static_cast<std::uint64_t>(std::llround(double(qpcFrequency_)/hz))));
    // A stream discontinuity is not permission to retry a failed shader.
    // Explicit preference changes and resource reconstruction call retryRendering.
    AlbumAmbientFallback fallback=AlbumAmbientFallback::none;
    if(!rolloutFlagEnabled(RolloutFlag::albumAmbientA1)) fallback=AlbumAmbientFallback::rolloutDisabled;
    else if(e.mode==AlbumAmbientMode::off) fallback=AlbumAmbientFallback::disabled;
    else if(e.reducedMotion) fallback=AlbumAmbientFallback::reducedMotion;
    else if(!e.hardware) fallback=AlbumAmbientFallback::softwareRasterizer;
    else if(!e.hasArtwork) fallback=AlbumAmbientFallback::noArtwork;
    else if(!visible_) fallback=AlbumAmbientFallback::hidden;
    else if(renderUnavailable_) fallback=AlbumAmbientFallback::unavailable;
    const bool enabled=fallback==AlbumAmbientFallback::none && e.wantsAudio();
    tailAllowed_=visible_ && !e.bookletOpen &&
        (fallback==AlbumAmbientFallback::none || fallback==AlbumAmbientFallback::disabled);
    if(enabled && !leaseActive_) {
        acquire(Runtime::instance().latestRealtimeSequence());
        state_=leaseActive_?AlbumAmbientState::waitingSource:AlbumAmbientState::staticFallback;
        sourceFreshnessQpc_=now; sourceStale_=false; textureSchedule_.reset(now);
        sourceRecovery_.activate(qpcSeconds(now));
    } else if(!enabled && leaseActive_) {
        auto state=AlbumAmbientState::decay;
        if(e.bookletOpen)state=AlbumAmbientState::bookletFrozen;
        else if(!visible_)state=AlbumAmbientState::hidden;
        else if(!tailAllowed_)state=AlbumAmbientState::staticFallback;
        release(now,state);
    }
    if(!leaseActive_ && !tailAllowed_) physicalMotion_=false;
    if(!leaseActive_ && !physicalMotion_ && !e.bookletOpen)
        state_=!visible_?AlbumAmbientState::hidden:
            e.mode==AlbumAmbientMode::off?AlbumAmbientState::offStatic:AlbumAmbientState::staticFallback;
    publishDiagnostics(fallback);
}
void AlbumAmbientController::acquire(std::uint64_t start) noexcept
{
    // Do not reset FlowSignal: phases survive view/lease and stream changes.
    lastPublicationRevision_=0;lastIngestAccepted_=false;
    Runtime::instance().setRealtimeConsumerActive(RealtimeConsumer::albumAmbient,true);
    const auto features=kAnalysisFeatureAlbumBands |
        (spectralLights_?kAnalysisFeatureCollection:0U) |
        (alloyV2Notes_?kAnalysisFeatureAlloyV2Notes:0U);
    leaseActive_=RealtimeAnalysisHub::instance().setDemand(
        AnalysisOwner::albumAmbient,lifetimeToken_,features,0.0,start);
    if(!leaseActive_)Runtime::instance().setRealtimeConsumerActive(RealtimeConsumer::albumAmbient,false);
    ++visualRevision_;
}
void AlbumAmbientController::release(std::uint64_t now,AlbumAmbientState state) noexcept
{
    flowSignal_.suspend(qpcSeconds(now));
    if(leaseActive_){
        (void)RealtimeAnalysisHub::instance().setOwnerActive(AnalysisOwner::albumAmbient,lifetimeToken_,false);
        Runtime::instance().setRealtimeConsumerActive(RealtimeConsumer::albumAmbient,false);
    }
    leaseActive_=false;state_=state;
    sourceRecovery_.reset();
    physicalMotion_=tailAllowed_ && flowSignal_.sample(qpcSeconds(now),false,true,false).needsFrame;
    ++visualRevision_;
}
bool AlbumAmbientController::reconcilePose(std::uint64_t now) noexcept
{
    const auto pose=sample(now);
    bool changed=std::abs(pose.gain-lastScheduledPose_.gain)>=1.0f/4096.0f ||
        std::abs(pose.availability-lastScheduledPose_.availability)>=1.0f/4096.0f;
    for(std::size_t i=0;i<kAmbientBandCount;++i)
        changed=changed || std::abs(pose.drive[i]-lastScheduledPose_.drive[i])>=1.0f/4096.0f ||
            std::abs(pose.transient[i]-lastScheduledPose_.transient[i])>=1.0f/4096.0f ||
            std::abs(pose.absoluteEnergy[i]-lastScheduledPose_.absoluteEnergy[i])>=1.0f/4096.0f ||
            std::abs(pose.relativeFast[i]-lastScheduledPose_.relativeFast[i])>=1.0f/4096.0f ||
            std::abs(pose.relativeSlow[i]-lastScheduledPose_.relativeSlow[i])>=1.0f/4096.0f ||
            std::abs(pose.growth[i]-lastScheduledPose_.growth[i])>=1.0f/4096.0f ||
            std::abs(pose.attack[i]-lastScheduledPose_.attack[i])>=1.0f/4096.0f ||
            angleDistance(pose.phase[i],lastScheduledPose_.phase[i])>=1.0f/4096.0f;
    changed=changed || pose.lights.valid!=lastScheduledPose_.lights.valid ||
        std::abs(pose.lights.master-lastScheduledPose_.lights.master)>=1.f/4096.f;
    for(unsigned i=0;i<8;++i)changed=changed ||
        std::abs(pose.lights.level[i]-lastScheduledPose_.lights.level[i])>=1.f/4096.f ||
        std::abs(pose.lights.texture[i]-lastScheduledPose_.lights.texture[i])>=1.f/4096.f;
    changed=changed || std::abs(pose.alloyFreshness-
        lastScheduledPose_.alloyFreshness)>=1.f/4096.f ||
        std::abs(pose.alloyBroad.master-
            lastScheduledPose_.alloyBroad.master)>=1.f/4096.f ||
        pose.alloyNotes.sequence!=lastScheduledPose_.alloyNotes.sequence;
    for(unsigned i=0;i<8;++i)changed=changed ||
        std::abs(pose.alloyBroad.level[i]-lastScheduledPose_.alloyBroad.level[i])>=1.f/4096.f ||
        std::abs(pose.alloyBroad.texture[i]-lastScheduledPose_.alloyBroad.texture[i])>=1.f/4096.f;
    for(std::size_t i=0;i<goden::c11::focusCount;++i)changed=changed ||
        std::abs(pose.alloyNotes.focus[i].energy-
            lastScheduledPose_.alloyNotes.focus[i].energy)>=1.f/4096.f ||
        std::abs(pose.alloyNotes.focus[i].texture-
            lastScheduledPose_.alloyNotes.focus[i].texture)>=1.f/4096.f ||
        pose.alloyNotes.focus[i].note!=lastScheduledPose_.alloyNotes.focus[i].note;
    const bool moving=(leaseActive_ || tailAllowed_) &&
        (flowPose_.needsFrame || (pauseTail_ && pauseRate_.moving(qpcSeconds(now))));
    if(changed || moving!=physicalMotion_){
        physicalMotion_=moving;lastScheduledPose_=pose;++visualRevision_;return true;
    }
    return false;
}
bool AlbumAmbientController::pump(std::uint64_t now) noexcept
{
    if(!leaseActive_)return physicalMotion_?reconcilePose(now):false;
    const auto stream=Runtime::instance().streamGeneration();
    if(stream!=expectedStreamGeneration_){
        expectedStreamGeneration_=stream;lastPublicationRevision_=0;
        sourceFreshnessQpc_=now;sourceStale_=false;lastIngestAccepted_=false;
        sourceRecovery_.activate(qpcSeconds(now));
        flowSignal_.suspend(qpcSeconds(now)); // retain phase, decay until admitted PCM
        state_=AlbumAmbientState::waitingSource;
    }
    Runtime::instance().pumpVisualisation();
    const auto snapshot=RealtimeAnalysisHub::instance().ambientSnapshot();
    const bool fresh=snapshot && validAmbientFeatures(*snapshot) &&
        snapshot->streamGeneration==expectedStreamGeneration_ && snapshot->sourceEndQpc!=0 &&
        now>=snapshot->sourceEndQpc && ambientSampleFresh(qpcSeconds(now-snapshot->sourceEndQpc));
    if(fresh){
        sourceFreshnessQpc_=snapshot->sourceEndQpc;
        if(snapshot->publicationRevision!=lastPublicationRevision_){
            lastPublicationRevision_=snapshot->publicationRevision;
            ++diagnostics_.featurePublications;
            for(std::size_t i=0;i<kAmbientBandCount;++i){
                flowFeatures_.power[i]=snapshot->bandPower[i];
                flowFeatures_.absoluteEnergy[i]=snapshot->absoluteEnergy[i];
                flowFeatures_.relativeFast[i]=snapshot->relativeFast[i];
                flowFeatures_.relativeSlow[i]=snapshot->relativeSlow[i];
                flowFeatures_.growth[i]=snapshot->growth[i];
                flowFeatures_.attack[i]=snapshot->attack[i];
            }
            flowFeatures_.gate=snapshot->gate;flowFeatures_.rich=true;
            flowFeatures_.sourceSeconds=qpcSeconds(snapshot->sourceEndQpc);
            flowFeatures_.playbackSerial=snapshot->playbackSerial;
            lastIngestAccepted_=flowSignal_.ingest(flowFeatures_);
            if(lastIngestAccepted_){sourceStale_=false;state_=AlbumAmbientState::active;}
        }
    }else if(now>=sourceFreshnessQpc_ && qpcSeconds(now-sourceFreshnessQpc_)>=.250){
        sourceStale_=true;state_=AlbumAmbientState::decay;
    }
    if(sourceRecovery_.observe(qpcSeconds(now),fresh) &&
        Runtime::instance().recoverVisualisationSource()){
        sourceRecovery_.commit();
        expectedStreamGeneration_=Runtime::instance().streamGeneration();
        lastPublicationRevision_=0;lastIngestAccepted_=false;
        sourceFreshnessQpc_=now;state_=AlbumAmbientState::waitingSource;
        flowSignal_.suspend(qpcSeconds(now));
    }
    // Freshness/prediction evolves even when the immutable publication did not.
    const bool changed=reconcilePose(now);
    publishDiagnostics(sourceStale_?AlbumAmbientFallback::sourceStale:AlbumAmbientFallback::none);
    return changed;
}
AlbumAmbientPose AlbumAmbientController::sample(std::uint64_t now) noexcept
{
    AlbumAmbientPose pose;pose.publicationRevision=visualRevision_;
    flowPose_=flowSignal_.sample(qpcSeconds(now),playbackActive_,
        leaseActive_ || (tailAllowed_ && visible_),muted_);
    pose.phase=flowPose_.phase;
    for(std::size_t b=0;b<3;++b)pose.phase[b]+=pausePhaseOffset_[b];
    if(leaseActive_ || (tailAllowed_ && visible_)){
        pose.drive=flowPose_.level;pose.transient=flowPose_.transient;
        pose.absoluteEnergy=flowPose_.absoluteEnergy;
        pose.relativeFast=flowPose_.relativeFast;
        pose.relativeSlow=flowPose_.relativeSlow;
        pose.growth=flowPose_.growth;pose.attack=flowPose_.attack;
        pose.gate=flowPose_.gate;
        pose.availability=flowPose_.availability;
        pose.gain=pose.availability*(mode_==AlbumAmbientMode::subtle?.60f:1.f);
    }
    if(spectralLights_ && leaseActive_ && playbackActive_ && !muted_){
        const auto frame=collection::Bridge::instance().latest();
        if(frame && frame->valid && frame->sourceQpc && now>=frame->sourceQpc){
            const auto age=qpcSeconds(now-frame->sourceQpc);
            pose.lights=collection::presentLights(frame->lights,
                frame->serial,frame->recording,Runtime::instance().playbackSerial(),
                Runtime::instance().recordingIdentityHash(),
                age,
                mode_==AlbumAmbientMode::subtle?.60f:1.f);
            if(alloyV2Notes_ && frame->serial==Runtime::instance().playbackSerial() &&
                    frame->recording==Runtime::instance().recordingIdentityHash()){
                pose.alloyBroad=frame->lights;
                pose.alloyNotes=frame->noteLights;
                pose.alloyFreshness=collection::lightFreshness(age)*
                    (mode_==AlbumAmbientMode::subtle?.60f:1.f);
            }
        }
    }
    if(pauseTail_ && tailAllowed_ && visible_){
        const auto revision=pose.publicationRevision;
        pose=pausePose_;pose.publicationRevision=revision;
        const auto seconds=qpcSeconds(now);
        const auto speed=static_cast<float>(pauseRate_.sample(seconds));
        const auto travel=static_cast<float>(pauseRate_.integral(pauseStarted_,seconds));
        for(std::size_t b=0;b<3;++b){
            const float baseRate=b==0?.11f:b==1?.20f:.34f;
            const float gainRate=b==0?.40f:b==1?.68f:.95f;
            pose.phase[b]=pausePose_.phase[b]+travel*pausePose_.availability*
                (baseRate+gainRate*pausePose_.drive[b]);
            pose.drive[b]*=speed;pose.transient[b]*=speed;
            pose.absoluteEnergy[b]*=speed;pose.growth[b]*=speed;pose.attack[b]*=speed;
            pose.relativeFast[b]=1.f+(pose.relativeFast[b]-1.f)*speed;
            pose.relativeSlow[b]=1.f+(pose.relativeSlow[b]-1.f)*speed;
        }
        pose.gain*=speed;pose.gate*=speed;pose.availability*=speed;
        pose.lights.master*=speed;
        pose.alloyFreshness*=speed;
    }
    pose.sinePhase=std::sin(pose.phase[0]);pose.cosinePhase=std::cos(pose.phase[0]);
    return pose;
}
bool AlbumAmbientController::textureDue(std::uint64_t now) const noexcept
{return textureSchedule_.due(visualRevision_!=presentedRevision_,now);}
void AlbumAmbientController::deferTexture(std::uint64_t now,std::uint32_t reasons) noexcept
{
    if(visualRevision_==presentedRevision_)return;
    const auto changed=textureSchedule_.defer(true,now);
    if(changed || deferReasonMask_!=reasons){
        ++deferTransitions_;deferReasonMask_=reasons;
        PerformanceTelemetry::instance().event(
            nekorox::PerformanceEventKindV2::cacheReason,
            TelemetrySurface::artwork,0xA4D0U,reasons,
            visualRevision_,presentedRevision_);
    }
}
void AlbumAmbientController::resumeTexture(std::uint64_t now) noexcept
{
    if(!textureSchedule_.resume(visualRevision_!=presentedRevision_,now))return;
    ++deferTransitions_;deferReasonMask_=0;
    PerformanceTelemetry::instance().event(
        nekorox::PerformanceEventKindV2::cacheReason,
        TelemetrySurface::artwork,0xA4D1U,0,
        visualRevision_,presentedRevision_);
}
void AlbumAmbientController::observeTexturePolicy(std::uint32_t reasons) noexcept
{
    if(textureSchedule_.deferred() || deferReasonMask_==reasons)return;
    deferReasonMask_=reasons;
    PerformanceTelemetry::instance().event(
        nekorox::PerformanceEventKindV2::cacheReason,
        TelemetrySurface::artwork,0xA4D2U,reasons,
        visualRevision_,presentedRevision_);
}
void AlbumAmbientController::texturePresented(std::uint64_t now,bool produced,
    bool accepted,std::uint64_t captured) noexcept
{
    ++textureAttempts_;
    if(!captured)captured=visualRevision_;
    captured=std::min(captured,visualRevision_);
    if(produced){producedRevision_=std::max(producedRevision_,captured);
        lastTextureProductionQpc_=now;}
    if(!produced || !accepted){++rejectedPresents_;return;}
    presentedRevision_=std::max(presentedRevision_,std::min(captured,visualRevision_));
    ++acceptedPresents_;
    ++diagnostics_.textureUpdates;
    textureSchedule_.accepted(now);deferReasonMask_=0;
    publishAlbumAmbientDiagnostics(diagnostics_);
}
void AlbumAmbientController::renderUnavailable(std::uint64_t now) noexcept
{
    renderUnavailable_=true;unavailableStreamGeneration_=expectedStreamGeneration_;
    tailAllowed_=false;if(leaseActive_)release(now,AlbumAmbientState::staticFallback);
    physicalMotion_=false;presentedRevision_=visualRevision_;
    publishDiagnostics(AlbumAmbientFallback::unavailable);
}
void AlbumAmbientController::retryRendering() noexcept
{renderUnavailable_=false;unavailableStreamGeneration_=0;++visualRevision_;}
void AlbumAmbientController::suspendPresentation(std::uint64_t now) noexcept
{
    if(pauseTail_){
        const auto shown=sample(now);
        const auto raw=flowSignal_.sample(qpcSeconds(now),false,true,false);
        for(std::size_t b=0;b<3;++b)pausePhaseOffset_[b]=shown.phase[b]-raw.phase[b];
    }
    pauseTail_=false;
    tailAllowed_=false;visible_=false;
    release(now,AlbumAmbientState::hidden);
    physicalMotion_=false;
}
void AlbumAmbientController::shutdown() noexcept
{
    suspendPresentation(nowTicks());
    userPaused_=false;sourceStale_=false;renderUnavailable_=false;
    // Object lifetime ends in the destructor; a hidden Album may reuse this instance.
}
bool AlbumAmbientController::animationActive(std::uint64_t) const noexcept
{return visualRevision_!=presentedRevision_;}
AlbumAmbientProgress AlbumAmbientController::progress() const noexcept
{
    return {visualRevision_,producedRevision_,presentedRevision_,
        textureSchedule_.deadlineQpc(),lastTextureProductionQpc_,
        textureAttempts_,acceptedPresents_,rejectedPresents_,
        deferTransitions_,deferReasonMask_,textureSchedule_.deferred()};
}
std::uint64_t AlbumAmbientController::nextPumpQpc(std::uint64_t now) const noexcept
{return now+std::max<std::uint64_t>(1,qpcFrequency_/50U);}
void AlbumAmbientController::publishDiagnostics(AlbumAmbientFallback f) noexcept
{diagnostics_.mode=mode_;diagnostics_.fallback=f;publishAlbumAmbientDiagnostics(diagnostics_);}
double AlbumAmbientController::qpcSeconds(std::uint64_t ticks) const noexcept
{return double(ticks)/double(std::max<std::uint64_t>(1,qpcFrequency_));}
} // namespace nekorox::component
