#pragma once

#include "analysis_worker_host.h"
#include "nekorox/packet_cost.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace nekorox::component {

struct PacketCostCachePayload;

enum class PacketCostStatus : std::uint8_t {
    idle = 0,
    loading = 1,
    running = 2,
    ready = 3,
    unsupported = 4,
    error = 5,
};

struct PacketCostSnapshot {
    PacketCostStatus status = PacketCostStatus::idle;
    std::uint64_t requestId = 0;
    std::filesystem::path trackPath;
    std::string sourceRevision;
    std::string sourceTrackId;
    std::shared_ptr<const PacketCostResult> result;
    std::wstring error;
};

struct PacketCostDiagnostics {
    std::uint64_t generation = 0;
    std::uint64_t activeRequestId = 0;
    std::uint32_t retiring = 0;
    bool pendingStart = false;
};

class PacketCostController final {
public:
    static PacketCostController& instance();

    void onTrackChanged(const std::filesystem::path& localTrack);
    void beginDeepAnalysis(std::uint64_t requestId);
    void endDeepAnalysis(std::uint64_t requestId);
    void shutdownAndDrain();
    void stop();
    // Render callers keep the immutable result alive without copying the
    // controller's path, source revision or error strings on every frame.
    std::shared_ptr<const PacketCostResult> readyResult() const;
    std::shared_ptr<const PacketCostResult> readyResultForTrack(
        const std::filesystem::path& track) const;
    PacketCostDiagnostics diagnostics() const;

private:
    struct WorkerRun;

    PacketCostController() = default;
    ~PacketCostController();
    PacketCostController(const PacketCostController&) = delete;
    PacketCostController& operator=(const PacketCostController&) = delete;

    void cancelWorker();
    void loadCache(std::uint64_t generation,
        const std::filesystem::path& localTrack,
        std::string sourceRevision);
    void startWorker(std::uint64_t generation,
        const std::filesystem::path& localTrack,
        const std::string& sourceRevision);
    void readerMain(WorkerRun* run);
    void resumePending();
    void onRunRetired() noexcept;
    void reapCompleted(std::uint64_t requestId);
    void onCacheLoaded(std::uint64_t generation,
        std::string sourceRevision,
        PacketCostCachePayload payload);
    void finishRun(
        std::unique_ptr<WorkerRun> run,
        bool asynchronous = false) noexcept;
    static void reapWorker(void* context) noexcept;

    mutable std::mutex mutex_;
    std::unique_ptr<WorkerRun> active_;
    PacketCostSnapshot state_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::uint64_t deepAnalysisRequestId_ = 0;
    bool deferredForDeepAnalysis_ = false;
    std::atomic<bool> accepting_{true};
    std::atomic<std::uint32_t> retiring_{0};
    std::filesystem::path pendingTrack_;
    std::string pendingSourceRevision_;
    std::uint64_t pendingGeneration_ = 0;
    bool pendingStart_ = false;
};

} // namespace nekorox::component
