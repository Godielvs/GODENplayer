#pragma once
#include "a4_budget.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace nekorox::component::a4 {
// All calls on the owning UI thread. Six fixed query slots, DONOTFLUSH readback.
// Each stage encloses one complete D2D BeginDraw/EndDraw submission block.
// These are GPU-timeline ELAPSED intervals, not proof of exclusive GPU busy time.
// Inclusive brackets the owner and Present only for comparison; never budget it.
class GpuTiming final {
    using Query=Microsoft::WRL::ComPtr<ID3D11Query>;
    using Clock=std::chrono::steady_clock;
    struct Slot {
        Query disjoint,start,end;
        std::array<Query,stageCount> stageStart,stageEnd;
        QualifiedGpuSample sample{};
        std::array<Clock::time_point,stageCount> cpuStart{};
        unsigned openMask=0;
        bool pending=false;
    };
    std::array<Slot,6> slots_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::array<QualifiedGpuSample,12> ready_{};
    unsigned readyCount_=0;
    int active_=-1;
    std::uint64_t nextFrame_=0,dropped_=0,disjoint_=0,generation_=0;
    bool enabled_=false;
    double inclusive95_=0;
    std::array<double,120> inclusive_{};
    unsigned inclusiveHead_=0,inclusiveCount_=0,inclusiveNew_=0;
    void collect() noexcept {
        if(!context_) return;
        constexpr UINT flags=D3D11_ASYNC_GETDATA_DONOTFLUSH;
        for(auto& s:slots_) {
            if(!s.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            auto hr=context_->GetData(s.disjoint.Get(),&d,sizeof(d),flags);
            if(hr==S_FALSE) continue;
            if(FAILED(hr)||d.Disjoint||d.Frequency==0) {
                s.pending=false; ++disjoint_; continue;
            }
            UINT64 start=0,end=0;
            const auto a=context_->GetData(s.start.Get(),&start,sizeof(start),flags);
            const auto b=context_->GetData(s.end.Get(),&end,sizeof(end),flags);
            if(a==S_FALSE||b==S_FALSE) continue;
            bool invalid=FAILED(a)||FAILED(b)||end<start, wait=false;
            for(std::size_t i=0;i<stageCount;++i) {
                if((s.sample.presentMask&(1U<<i))==0) continue;
                UINT64 first=0,last=0;
                const auto x=context_->GetData(s.stageStart[i].Get(),&first,sizeof(first),flags);
                const auto y=context_->GetData(s.stageEnd[i].Get(),&last,sizeof(last),flags);
                wait=wait||x==S_FALSE||y==S_FALSE;
                invalid=invalid||FAILED(x)||FAILED(y)||last<first;
                if(x==S_OK&&y==S_OK&&last>=first)
                    s.sample.gpuUs[i]=double(last-first)*1e6/double(d.Frequency);
            }
            if(wait) continue;
            s.pending=false;
            if(invalid) {++disjoint_;continue;}
            s.sample.valid=true;
            s.sample.inclusiveUs=double(end-start)*1e6/double(d.Frequency);
            inclusive_[inclusiveHead_]=s.sample.inclusiveUs;inclusiveHead_=(inclusiveHead_+1U)%120U;
            inclusiveCount_=std::min(inclusiveCount_+1U,120U);
            if(++inclusiveNew_>=30U) {
                inclusiveNew_=0;auto values=inclusive_;
                std::sort(values.begin(),values.begin()+inclusiveCount_);
                inclusive95_=values[(inclusiveCount_*95U+99U)/100U-1U];
            }
            if(readyCount_<ready_.size()) ready_[readyCount_++]=s.sample;
            else ++dropped_;
        }
        std::sort(ready_.begin(),ready_.begin()+readyCount_,
            [](const auto& a,const auto& b){return a.frameId<b.frameId;});
    }
public:
    void reset() noexcept {
        endFrame(); slots_={}; readyCount_=0; device_.Reset();context_.Reset();
        active_=-1; enabled_=false;inclusive95_=0;inclusive_={};inclusiveHead_=inclusiveCount_=inclusiveNew_=0;
        // Frame IDs remain monotonic across a resource epoch.
    }
    void beginFrame(ID3D11Device* d,ID3D11DeviceContext* c,bool eligible,
        std::uint64_t epoch) noexcept {
        if(active_>=0) endFrame();
        if(device_.Get()!=d||context_.Get()!=c) {
            reset();device_=d;context_=c;enabled_=d&&c;++generation_;
            if(enabled_)for(auto& s:slots_) {
                D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
                if(FAILED(d->CreateQuery(&q,s.disjoint.GetAddressOf()))) {enabled_=false;break;}
                q.Query=D3D11_QUERY_TIMESTAMP;
                if(FAILED(d->CreateQuery(&q,s.start.GetAddressOf()))||
                    FAILED(d->CreateQuery(&q,s.end.GetAddressOf()))) {enabled_=false;break;}
                for(std::size_t i=0;i<stageCount;++i) {
                    if(FAILED(d->CreateQuery(&q,s.stageStart[i].GetAddressOf()))||
                       FAILED(d->CreateQuery(&q,s.stageEnd[i].GetAddressOf()))) {enabled_=false;break;}
                }
                if(!enabled_) break;
            }
        }
        if(!enabled_) return;
        collect(); if(!eligible) return;
        for(unsigned i=0;i<slots_.size();++i) if(!slots_[i].pending) {
            active_=static_cast<int>(i);auto& s=slots_[i];s.sample={};s.openMask=0;
            s.sample.frameId=++nextFrame_;s.sample.epoch=epoch;
            c->Begin(s.disjoint.Get());c->End(s.start.Get());return;
        }
        ++dropped_;
    }
    void qualify(bool eligible,unsigned requiredMask) noexcept {
        if(active_<0)return;auto& s=slots_[static_cast<unsigned>(active_)];
        s.sample.eligible=eligible;s.sample.requiredMask=requiredMask;
    }
    void beginStage(GpuStage stage) noexcept {
        if(active_<0)return;auto& s=slots_[static_cast<unsigned>(active_)];
        const auto i=static_cast<unsigned>(stage),bit=1U<<i;
        if((s.openMask|s.sample.presentMask)&bit){s.sample.eligible=false;return;}
        s.openMask|=bit;s.cpuStart[i]=Clock::now();context_->End(s.stageStart[i].Get());
    }
    void endStage(GpuStage stage) noexcept {
        if(active_<0)return;auto& s=slots_[static_cast<unsigned>(active_)];
        const auto i=static_cast<unsigned>(stage),bit=1U<<i;
        if(!(s.openMask&bit))return;
        context_->End(s.stageEnd[i].Get());
        s.sample.cpuUs[i]=std::chrono::duration<double,std::micro>(Clock::now()-s.cpuStart[i]).count();
        s.openMask&=~bit;s.sample.presentMask|=bit;
    }
    void beginField() noexcept {beginStage(GpuStage::field);}
    void endField() noexcept {endStage(GpuStage::field);}
    void endFrame() noexcept {
        if(active_<0)return;auto& s=slots_[static_cast<unsigned>(active_)];
        if(s.openMask) {s.sample.eligible=false;for(unsigned i=0;i<stageCount;++i)if(s.openMask&(1U<<i))endStage(static_cast<GpuStage>(i));}
        context_->End(s.end.Get());context_->End(s.disjoint.Get());s.pending=true;active_=-1;
    }
    bool pop(QualifiedGpuSample& value) noexcept {
        if(!readyCount_)return false;value=ready_[0];
        for(unsigned i=1;i<readyCount_;++i)ready_[i-1]=ready_[i];--readyCount_;return true;
    }
    double inclusiveP95Us()const noexcept{return inclusive95_;}
    std::uint64_t dropped()const noexcept{return dropped_;}
    std::uint64_t disjoint()const noexcept{return disjoint_;}
    std::uint64_t generation()const noexcept{return generation_;}
};
} // namespace nekorox::component::a4
