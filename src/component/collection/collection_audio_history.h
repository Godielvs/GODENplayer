#pragma once
#include "collection_audio.h"
#include "c6_audio_history.h"
#include <atomic>
#include <memory>
#include <utility>
namespace nekorox::component::collection {
using RetainedAudioRows=c6::AudioRowHistory<decltype(AudioFrame{}.row)>;
using AudioIdentity=c6::AudioIdentity;
using AudioHistoryReadMode=c6::HistoryReadMode;
struct HistoryApplyReceipt {
    AudioIdentity identity{};
    std::uint64_t readerGeneration=0,resourceEpoch=0,throughPublication=0;
    std::uint64_t skippedObservations=0;
    bool handled=false,restoreComplete=false;
};
// Transactional cursor shared by production and portable contract tests. A
// staged page remains stable across bitmap-only re-presentation and is retired
// only after the renderer reports that its texture upload (or explicit expiry)
// was handled. Display/Present acknowledgement is intentionally irrelevant.
class AudioHistoryReader final {
    std::unique_ptr<RetainedAudioRows::Batch> batch_;
    AudioIdentity identity_{};
    std::uint64_t generation_=1,cursor_=0,fence_=0,resourceEpoch_=0;
    bool restoring_=true,staged_=false;
public:
    void reset(AudioIdentity identity={}) noexcept {
        identity_=identity;cursor_=fence_=resourceEpoch_=0;restoring_=true;staged_=false;
        if(++generation_==0)generation_=1;
    }
    [[nodiscard]] const RetainedAudioRows::Batch* stage(RetainedAudioRows& source) noexcept {
        if(staged_)return batch_.get();
        if(!identity_.valid())return nullptr;
        if(!batch_){try{batch_=std::make_unique<RetainedAudioRows::Batch>();}catch(...){return nullptr;}}
        auto&batch=*batch_;const auto mode=restoring_?AudioHistoryReadMode::restore:AudioHistoryReadMode::incremental;
        if(!source.read(identity_,cursor_,mode,fence_,batch)||batch.busy||batch.oldIdentity||
            batch.serial!=identity_.serial||batch.recording!=identity_.recording)return nullptr;
        staged_=batch.count!=0||(batch.complete&&batch.skippedObservations!=0);
        return staged_?&batch:nullptr;
    }
    [[nodiscard]] bool commit(const HistoryApplyReceipt& receipt) noexcept {
        if(!staged_||!receipt.handled||receipt.identity!=identity_||
            receipt.readerGeneration!=generation_||receipt.resourceEpoch==0)return false;
        const auto&batch=*batch_;const auto through=batch.count?
            batch.rows[batch.count-1].publication:(batch.complete?batch.restoreFence:cursor_);
        if(receipt.throughPublication!=through||receipt.restoreComplete!=batch.complete||
            receipt.skippedObservations!=batch.skippedObservations)return false;
        if(resourceEpoch_&&resourceEpoch_!=receipt.resourceEpoch)return false;
        resourceEpoch_=receipt.resourceEpoch;cursor_=through;
        if(restoring_){if(!fence_)fence_=batch.restoreFence;
            if(receipt.restoreComplete){restoring_=false;fence_=0;}}
        staged_=false;return true;
    }
    [[nodiscard]] AudioIdentity identity()const noexcept{return identity_;}
    [[nodiscard]] std::uint64_t generation()const noexcept{return generation_;}
    [[nodiscard]] std::uint64_t cursor()const noexcept{return cursor_;}
    [[nodiscard]] std::uint64_t fence()const noexcept{return fence_;}
    [[nodiscard]] std::uint64_t resourceEpoch()const noexcept{return resourceEpoch_;}
    [[nodiscard]] std::uint64_t plannedThrough()const noexcept {
        if(!staged_)return cursor_;
        const auto&batch=*batch_;
        return batch.count?batch.rows[batch.count-1].publication:(batch.complete?batch.restoreFence:cursor_);
    }
    [[nodiscard]] bool restoring()const noexcept{return restoring_;}
    [[nodiscard]] bool staged()const noexcept{return staged_;}
};
inline RetainedAudioRows& retainedAudioRows() noexcept {
    static RetainedAudioRows history;
    return history;
}
inline std::atomic<unsigned> retainedAudioRowReaders{0};
class AudioHistoryLease final {
public:
    AudioHistoryLease() noexcept = default;
    AudioHistoryLease(const AudioHistoryLease&) = delete;
    AudioHistoryLease& operator=(const AudioHistoryLease&) = delete;
    AudioHistoryLease(AudioHistoryLease&& other) noexcept : active_(std::exchange(other.active_,false)) {}
    AudioHistoryLease& operator=(AudioHistoryLease&& other) noexcept {
        if(this!=&other){set(false);active_=std::exchange(other.active_,false);}
        return *this;
    }
    ~AudioHistoryLease(){set(false);}
    void set(bool active) noexcept {
        if(active==active_)return;
        if(active)retainedAudioRowReaders.fetch_add(1,std::memory_order_acq_rel);
        else retainedAudioRowReaders.fetch_sub(1,std::memory_order_acq_rel);
        active_=active;
    }
private:
    bool active_=false;
};
inline void retainAudioRows(const AudioFrame& frame) noexcept {
    if(frame.valid && retainedAudioRowReaders.load(std::memory_order_acquire)!=0)retainedAudioRows().publish(frame.serial,frame.recording,
        frame.sequence,frame.gap,frame.row,frame.rate?double(frame.endSample)/frame.rate:0.);
}
} // namespace nekorox::component::collection
