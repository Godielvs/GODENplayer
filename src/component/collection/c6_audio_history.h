#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>

namespace nekorox::component::c6 {
struct AudioIdentity {
    std::uint64_t serial=0, recording=0;
    [[nodiscard]] bool valid() const noexcept { return serial!=0 && recording!=0; }
    friend bool operator==(AudioIdentity,AudioIdentity)=default;
};
enum class HistoryReadMode : std::uint8_t { incremental, restore };

// Written by the analysis worker, never by the audio callback. The UI only
// try-locks while copying a bounded batch; it never waits behind the producer.
// Publication sequence is independent of analyzer reset/source sequence.
template<class RowData, std::size_t Capacity=512, std::size_t BatchCapacity=64>
class AudioRowHistory final {
public:
    struct Row {
        RowData pixels{};
        std::uint64_t publication=0, sourceSequence=0;
        double sourceSeconds=0; // Optional sample-clock metadata, not FFT data.
        bool discontinuity=false;
    };
    struct Batch {
        std::array<Row,BatchCapacity> rows{};
        std::size_t count=0;
        std::uint64_t serial=0, recording=0, skippedObservations=0;
        std::uint64_t oldestAvailable=0,newestAvailable=0,restoreFence=0;
        bool busy=false, oldIdentity=false,complete=false;
    };
    void publish(std::uint64_t serial, std::uint64_t recording,
        std::uint64_t sourceSequence, bool gap, const RowData& pixels,double sourceSeconds=0) noexcept {
        if (!serial || !recording || !sourceSequence) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (serial_ && (serial<serial_ || (serial==serial_ && recording!=recording_))) return;
        if (serial!=serial_ || recording!=recording_) {
            serial_=serial; recording_=recording; head_=count_=0;
        }
        if(count_ && rows_[(head_+Capacity-1)%Capacity].sourceSequence==sourceSequence && !gap) return;
        auto& row=rows_[head_]; row.pixels=pixels;
        row.publication=++publication_; row.sourceSequence=sourceSequence;
        row.sourceSeconds=sourceSeconds;
        row.discontinuity=gap;
        head_=(head_+1)%Capacity; count_=std::min(count_+1,Capacity);
    }
    bool read(AudioIdentity expected,std::uint64_t cursor,
        HistoryReadMode mode,std::uint64_t restoreFence,Batch& out) noexcept {
        out.count=0;out.serial=out.recording=out.skippedObservations=0;
        out.oldestAvailable=out.newestAvailable=out.restoreFence=0;
        out.busy=out.oldIdentity=out.complete=false;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){out.busy=true; return false;}
        out.serial=serial_;out.recording=recording_;
        const AudioIdentity retained{serial_,recording_};
        if(!expected.valid()||retained!=expected){out.oldIdentity=count_!=0;return true;}
        if(!count_)return true;
        const auto first=(head_+Capacity-count_)%Capacity;
        out.oldestAvailable=rows_[first].publication;
        out.newestAvailable=rows_[(head_+Capacity-1)%Capacity].publication;
        out.restoreFence=mode==HistoryReadMode::restore
            ?(restoreFence?restoreFence:out.newestAvailable)
            :out.newestAvailable;
        std::size_t begin=0;
        while(begin<count_ && rows_[(first+begin)%Capacity].publication<=cursor)++begin;
        if(begin<count_){
            const auto oldest=rows_[(first+begin)%Capacity].publication;
            out.skippedObservations=cursor&&oldest>cursor+1?oldest-cursor-1:0;
            for(std::size_t i=begin;i<count_&&out.count<BatchCapacity;++i){
                const auto&row=rows_[(first+i)%Capacity];
                if(row.publication>out.restoreFence)break;
                out.rows[out.count++]=row;
            }
        }
        const auto last=out.count?out.rows[out.count-1].publication:cursor;
        if(mode==HistoryReadMode::restore&&last<out.restoreFence&&
            out.oldestAvailable>out.restoreFence){
            // The frozen restore range was overwritten before this reader could
            // copy it. Finish deterministically and report the exact omission;
            // new publications never extend the restore target indefinitely.
            out.skippedObservations=out.restoreFence-last;
            out.complete=true;
        }else out.complete=last>=out.restoreFence;
        return true;
    }
private:
    std::mutex mutex_;
    std::array<Row,Capacity> rows_{};
    std::size_t head_=0,count_=0;
    std::uint64_t serial_=0,recording_=0,publication_=0;
};
} // namespace nekorox::component::c6
