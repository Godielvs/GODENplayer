#pragma once
#include "collection_audio.h"
#include <atomic>
#include <memory>
namespace nekorox {struct RealtimeFrameV1;}
namespace nekorox::component::collection {
// Publications are immutable. Multicast readers own independent cursors.
// No race-prone seqlock over non-atomic PCM, and no hidden model thread.
class Bridge final {
    Analyzer analyzer_;
    std::array<std::shared_ptr<AudioFrame>,160> pool_{};
    std::atomic<std::shared_ptr<const AudioFrame>> latest_;
    std::atomic<std::uint64_t> sequence_{0},drops_{0};
    std::size_t cursor_=0;std::uint64_t qpcFrequency_=1;
    Bridge();
public:
    static Bridge& instance();
    void ingest(const ::nekorox::RealtimeFrameV1&,bool discontinuity,
        bool notesEnabled=false)noexcept;
    std::shared_ptr<const AudioFrame> latest()const noexcept{return latest_.load(std::memory_order_acquire);}
    std::uint64_t drops()const noexcept{return drops_.load(std::memory_order_relaxed);}
};
} // namespace
