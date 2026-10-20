#pragma once

#include "nekorox/contracts.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nekorox::component {

struct AnalysisCachePayload;

enum class DeepAnalysisStatus : std::uint8_t {
    idle = 0,
    running = 1,
    complete = 2,
    cancelled = 3,
    error = 4,
};

struct DeepAnalysisSnapshot {
    DeepAnalysisStatus status = DeepAnalysisStatus::idle;
    std::uint64_t requestId = 0;
    std::filesystem::path trackPath;
    AnalysisStage stage = AnalysisStage::queued;
    float stageProgress = 0.0f;
    float overallProgress = 0.0f;
    AnalysisMediaInfoV1 media{};
    std::shared_ptr<const std::vector<AnalysisWaveformBinV1>> waveform;
    std::shared_ptr<const std::string> resultJson;
    std::wstring error;
};

class AnalysisController final {
public:
    static AnalysisController& instance();

    bool start(const std::filesystem::path& localTrack);
    void cancel();
    void cancelViewRequest();
    void shutdownAndDrain();
    void stop();
    void onTrackChanged(const std::filesystem::path& localTrack);
    DeepAnalysisSnapshot snapshot() const;

private:
    struct WorkerRun;

    AnalysisController() = default;
    ~AnalysisController();
    AnalysisController(const AnalysisController&) = delete;
    AnalysisController& operator=(const AnalysisController&) = delete;

    void readerMain(WorkerRun* run);
    bool startNow(const std::filesystem::path& localTrack);
    void resumePending();
    void onRunRetired() noexcept;
    void reapCompleted(std::uint64_t requestId);
    void applyFrame(const AnalysisFrameHeaderV1& header,
        const std::vector<std::byte>& payload);
    void finishRun(
        std::unique_ptr<WorkerRun> run,
        bool asynchronous = false) noexcept;
    static void reapWorker(void* context) noexcept;
    void setLaunchError(const std::filesystem::path& track, std::wstring error);
    void onCacheLoaded(
        std::uint64_t generation,
        AnalysisCachePayload payload);

    mutable std::mutex mutex_;
    std::unique_ptr<WorkerRun> active_;
    DeepAnalysisSnapshot state_;
    std::string resultJson_;
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::atomic<std::uint64_t> cacheGeneration_{0};
    std::atomic<std::uint64_t> lastProgressPostQpc_{0};
    std::atomic<std::uint16_t> lastProgressStage_{
        static_cast<std::uint16_t>(AnalysisStage::queued)};
    std::atomic<bool> accepting_{true};
    std::atomic<std::uint32_t> retiring_{0};
    std::filesystem::path pendingTrack_;
    bool pendingStart_ = false;
};

[[nodiscard]] const wchar_t* analysisStageLabel(AnalysisStage stage) noexcept;

} // namespace nekorox::component
