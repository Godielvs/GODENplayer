#include "analysis_controller.h"
#include "worker_failure.h"

#include "analysis_worker_host.h"
#include "database_service.h"
#include "packet_cost_controller.h"
#include "runtime.h"
#include "sdk_compat.h"
#include "text_util.h"
#include "worker_reaper.h"
#include "nekorox/analysis_source_revision.h"
#include "nekorox/track_identity.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace nekorox::component {
namespace {

constexpr SIZE_T kWorkerMemoryLimit = 512ULL * 1024ULL * 1024ULL;

std::uint64_t analysisQpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart > 0
        ? static_cast<std::uint64_t>(value.QuadPart)
        : 0;
}

std::uint64_t analysisQpcFrequency() noexcept
{
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
            ? static_cast<std::uint64_t>(value.QuadPart)
            : 1ULL;
    }();
    return frequency;
}

} // namespace

struct AnalysisController::WorkerRun {
    AnalysisController* owner = nullptr;
    std::uint64_t requestId = 0;
    AnalysisWorkerHandles worker;
    std::thread reader;
    std::atomic<bool> cancelled{false};
    std::filesystem::path sourcePath;
    std::string sourceTrackId;
    bool sourceValidated = false;
};

AnalysisController& AnalysisController::instance()
{
    static AnalysisController controller;
    return controller;
}

AnalysisController::~AnalysisController()
{
    stop();
}

bool AnalysisController::start(const std::filesystem::path& localTrack)
{
    if (!accepting_.load(std::memory_order_acquire)) return false;
    cancel();
    cacheGeneration_.fetch_add(1, std::memory_order_acq_rel);
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(localTrack, fileError)) {
        setLaunchError(localTrack, L"Deep analysis requires a readable local file.");
        return false;
    }
    lastProgressPostQpc_.store(0, std::memory_order_release);
    lastProgressStage_.store(
        static_cast<std::uint16_t>(AnalysisStage::queued),
        std::memory_order_release);
    if (retiring_.load(std::memory_order_acquire) != 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingTrack_ = localTrack;
            pendingStart_ = true;
            state_ = {};
            state_.status = DeepAnalysisStatus::running;
            state_.trackPath = localTrack;
            state_.stage = AnalysisStage::queued;
        }
        postListenAnalysisRefresh();
        return true;
    }
    return startNow(localTrack);
}

bool AnalysisController::startNow(const std::filesystem::path& localTrack)
{
    if (!accepting_.load(std::memory_order_acquire)) return false;
    auto run = std::make_unique<WorkerRun>();
    run->owner = this;
    run->requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    run->sourcePath = localTrack;
    const auto requestId = run->requestId;
    PacketCostController::instance().beginDeepAnalysis(requestId);
    std::wstring launchError;
    if (!launchAnalysisWorker(L"analyze", run->requestId, localTrack,
            kWorkerMemoryLimit, run->worker, launchError, true)) {
        finishRun(std::move(run));
        PacketCostController::instance().endDeepAnalysis(requestId);
        setLaunchError(localTrack, std::move(launchError));
        return false;
    }

    WorkerRun* active = nullptr;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.status = DeepAnalysisStatus::running;
        state_.requestId = run->requestId;
        state_.trackPath = localTrack;
        resultJson_.clear();
        active_ = std::move(run);
        active = active_.get();
        // Publish ownership and the joinable thread atomically with respect to
        // cancel/reap. The reader never performs filesystem I/O under this lock.
        active->reader = std::thread([this, active] { readerMain(active); });
    } catch (...) {
        std::unique_ptr<WorkerRun> failed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.get() == active) {
                failed = std::move(active_);
                state_.status = DeepAnalysisStatus::error;
                state_.error = L"The analysis reader thread could not start.";
            }
        }
        finishRun(std::move(failed));
        PacketCostController::instance().endDeepAnalysis(requestId);
        postListenAnalysisRefresh();
        return false;
    }
    postListenAnalysisRefresh();
    return true;
}

void AnalysisController::resumePending()
{
    std::filesystem::path track;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pendingStart_ || !accepting_.load(std::memory_order_relaxed) ||
            active_ != nullptr || retiring_.load(std::memory_order_relaxed) != 0) {
            return;
        }
        track = std::move(pendingTrack_);
        pendingTrack_.clear();
        pendingStart_ = false;
    }
    startNow(track);
}

void AnalysisController::cancel()
{
    std::unique_ptr<WorkerRun> run;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run = std::move(active_);
        changed = run != nullptr || pendingStart_;
        // A retiring worker may have a replacement queued behind it. Cancel
        // owns that request too, including while no worker is currently active.
        pendingStart_ = false;
        pendingTrack_.clear();
        if (changed && state_.status == DeepAnalysisStatus::running) {
            state_.status = DeepAnalysisStatus::cancelled;
            state_.error.clear();
        }
    }
    if (run != nullptr) {
        const auto requestId = run->requestId;
        run->cancelled.store(true, std::memory_order_release);
        finishRun(std::move(run), true);
        PacketCostController::instance().endDeepAnalysis(requestId);
    }
    if (changed) postListenAnalysisRefresh();
}

void AnalysisController::stop()
{
    shutdownAndDrain();
}

void AnalysisController::cancelViewRequest()
{
    cacheGeneration_.fetch_add(1, std::memory_order_acq_rel);
    cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    pendingStart_ = false;
    pendingTrack_.clear();
    state_ = {};
    resultJson_.clear();
}

void AnalysisController::shutdownAndDrain()
{
    accepting_.store(false, std::memory_order_release);
    cacheGeneration_.fetch_add(1, std::memory_order_acq_rel);
    cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    pendingStart_ = false;
    pendingTrack_.clear();
    state_ = {};
    resultJson_.clear();
}

void AnalysisController::onTrackChanged(const std::filesystem::path& localTrack)
{
    if (!accepting_.load(std::memory_order_acquire)) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = state_.trackPath != localTrack;
    }
    if (!changed) return;
    cancel();
    const auto generation = cacheGeneration_.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.trackPath = localTrack;
        resultJson_.clear();
    }
    std::error_code fileError;
    if (std::filesystem::is_regular_file(localTrack, fileError)) {
        DatabaseService::instance().loadAnalysis(localTrack,
            [this, generation](AnalysisCachePayload payload) {
                onCacheLoaded(generation, std::move(payload));
            });
    }
}

DeepAnalysisSnapshot AnalysisController::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void AnalysisController::readerMain(WorkerRun* run)
{
    // Freeze the source before the suspended child can decode it. Read sharing
    // permits the decoder but refuses edits/replacement for this run. Neither
    // hashing nor filesystem metadata access holds the UI state mutex.
    struct SourceReadGuard {
        HANDLE handle;
        ~SourceReadGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } source{CreateFileW(run->sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (source.handle != INVALID_HANDLE_VALUE)
        run->sourceTrackId = analysisSourceIdentity(run->sourcePath);
    bool ready = !run->sourceTrackId.empty() &&
        !run->cancelled.load(std::memory_order_acquire) &&
        ResumeThread(run->worker.startThread) != static_cast<DWORD>(-1);
    bool terminalFrame = false;
    if (!ready) {
        TerminateProcess(run->worker.process, ERROR_CANCELLED);
        const std::string error = "The analysis source could not be frozen or the worker could not start. Retry with a readable file.";
        AnalysisFrameHeaderV1 failure;
        failure.kind = AnalysisMessageKind::error;
        failure.requestId = run->requestId;
        applyFrame(failure, {reinterpret_cast<const std::byte*>(error.data()),
            reinterpret_cast<const std::byte*>(error.data() + error.size())});
        terminalFrame = true;
    }
    while (ready) {
        AnalysisFrameHeaderV1 header;
        std::vector<std::byte> payload;
        std::string protocolError;
        if (!readAnalysisWorkerFrame(run->worker.output, run->requestId,
                header, payload, protocolError)) {
            if (protocolError.empty()) break;
            AnalysisFrameHeaderV1 synthetic;
            synthetic.kind = AnalysisMessageKind::error;
            synthetic.requestId = run->requestId;
            applyFrame(synthetic, std::vector<std::byte>(
                reinterpret_cast<const std::byte*>(protocolError.data()),
                reinterpret_cast<const std::byte*>(
                    protocolError.data() + protocolError.size())));
            terminalFrame = true;
            break;
        }
        if (header.kind == AnalysisMessageKind::complete) {
            run->sourceValidated = !run->sourceTrackId.empty() &&
                analysisSourceIdentity(run->sourcePath) == run->sourceTrackId;
        }
        applyFrame(header, payload);
        if (header.kind == AnalysisMessageKind::complete ||
            header.kind == AnalysisMessageKind::error) {
            terminalFrame = true;
            break;
        }
    }
    WaitForSingleObject(run->worker.process, 2000);
    if (!terminalFrame && !run->cancelled.load(std::memory_order_acquire)) {
        DWORD exitCode = 0;
        GetExitCodeProcess(run->worker.process, &exitCode);
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.get() == run && state_.status == DeepAnalysisStatus::running) {
            state_.status = DeepAnalysisStatus::error;
            state_.error = workerExitDiagnostic(L"Analysis", exitCode);
        }
    }
    postListenAnalysisRefresh();
    const auto requestId = run->requestId;
    if (!run->cancelled.load(std::memory_order_acquire)) {
        fb2k::inMainThread([this, requestId] {
            reapCompleted(requestId);
            PacketCostController::instance().endDeepAnalysis(requestId);
        });
    }
}

void AnalysisController::reapCompleted(std::uint64_t requestId)
{
    std::unique_ptr<WorkerRun> run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ == nullptr || active_->requestId != requestId) return;
        run = std::move(active_);
    }
    finishRun(std::move(run));
}

void AnalysisController::applyFrame(const AnalysisFrameHeaderV1& header,
    const std::vector<std::byte>& payload)
{
    bool persist = false;
    std::filesystem::path persistPath;
    AnalysisMediaInfoV1 persistMedia{};
    std::shared_ptr<const std::vector<AnalysisWaveformBinV1>> persistWaveform;
    std::string persistJson;
    std::string persistTrackId;
    bool stageChanged = false;
    auto publishedStage = AnalysisStage::queued;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ == nullptr || active_->requestId != header.requestId) return;
        switch (header.kind) {
        case AnalysisMessageKind::progress:
            if (payload.size() == sizeof(AnalysisProgressV1)) {
                AnalysisProgressV1 progress;
                std::memcpy(&progress, payload.data(), sizeof(progress));
                stageChanged = state_.stage != progress.stage;
                publishedStage = progress.stage;
                state_.stage = progress.stage;
                state_.stageProgress = std::clamp(progress.stageProgress, 0.0f, 1.0f);
                state_.overallProgress = std::max(state_.overallProgress,
                    std::clamp(progress.overallProgress, 0.0f, 1.0f));
            }
            break;
        case AnalysisMessageKind::partialResult:
            if (payload.size() >= sizeof(AnalysisMediaInfoV1)) {
                AnalysisMediaInfoV1 media;
                std::memcpy(&media, payload.data(), sizeof(media));
                const auto expected = sizeof(media) +
                    static_cast<std::size_t>(media.waveformBinCount) *
                        sizeof(AnalysisWaveformBinV1);
                if (expected == payload.size()) {
                    auto waveform = std::make_shared<
                        std::vector<AnalysisWaveformBinV1>>(media.waveformBinCount);
                    if (!waveform->empty()) {
                        std::memcpy(waveform->data(),
                            payload.data() + sizeof(media),
                            waveform->size() * sizeof(waveform->front()));
                    }
                    state_.media = media;
                    state_.waveform = std::move(waveform);
                }
            }
            break;
        case AnalysisMessageKind::complete:
            if (!active_->sourceValidated) {
                resultJson_.clear();
                state_.resultJson.reset();
                state_.waveform.reset();
                state_.media = {};
                state_.status = DeepAnalysisStatus::error;
                state_.error = L"The audio file changed during analysis. Retry with the current file.";
                break;
            }
            resultJson_.assign(reinterpret_cast<const char*>(payload.data()),
                payload.size());
            state_.resultJson =
                std::make_shared<const std::string>(resultJson_);
            state_.status = DeepAnalysisStatus::complete;
            state_.stage = AnalysisStage::complete;
            state_.stageProgress = 1.0f;
            state_.overallProgress = 1.0f;
            state_.error.clear();
            persist = true;
            persistPath = state_.trackPath;
            persistMedia = state_.media;
            persistWaveform = state_.waveform;
            persistJson = resultJson_;
            persistTrackId = active_->sourceTrackId;
            break;
        case AnalysisMessageKind::error: {
            const std::string text(reinterpret_cast<const char*>(payload.data()),
                payload.size());
            state_.status = DeepAnalysisStatus::error;
            resultJson_.clear();
            state_.resultJson.reset();
            state_.waveform.reset();
            state_.media = {};
            state_.error = utf8ToWide(text);
            break;
        }
        default:
            break;
        }
    }
    if (persist) {
        DatabaseService::instance().storeAnalysis(std::move(persistPath),
            std::move(persistTrackId), persistMedia,
            std::move(persistWaveform), std::move(persistJson));
    }
    bool publish = header.kind != AnalysisMessageKind::progress ||
        stageChanged;
    if (!publish) {
        const auto now = analysisQpcNow();
        const auto last = lastProgressPostQpc_.load(
            std::memory_order_acquire);
        publish = now >= last + analysisQpcFrequency() / 30U;
    }
    if (publish) {
        lastProgressPostQpc_.store(
            analysisQpcNow(), std::memory_order_release);
        lastProgressStage_.store(
            static_cast<std::uint16_t>(header.kind ==
                    AnalysisMessageKind::progress
                ? publishedStage
                : AnalysisStage::complete),
            std::memory_order_release);
        postListenAnalysisRefresh();
    }
}

void AnalysisController::onCacheLoaded(
    std::uint64_t generation, AnalysisCachePayload payload)
{
    if (generation != cacheGeneration_.load(std::memory_order_acquire)) return;
    if (!payload.error.empty()) {
        FB2K_console_formatter() << "GODENplayer Core analysis cache load: "
                                << payload.error.c_str();
        return;
    }
    if (!payload.found) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != cacheGeneration_.load(std::memory_order_relaxed) ||
            state_.trackPath != payload.trackPath ||
            state_.status == DeepAnalysisStatus::running) {
            return;
        }
        state_.status = DeepAnalysisStatus::complete;
        state_.stage = AnalysisStage::complete;
        state_.stageProgress = 1.0f;
        state_.overallProgress = 1.0f;
        state_.media = payload.media;
        state_.waveform = std::move(payload.waveform);
        state_.error.clear();
        resultJson_ = std::move(payload.resultJson);
        state_.resultJson =
            std::make_shared<const std::string>(resultJson_);
    }
    postListenAnalysisRefresh();
}

void AnalysisController::finishRun(
    std::unique_ptr<WorkerRun> run, bool asynchronous) noexcept
{
    if (run == nullptr) return;
    if (asynchronous) {
        retiring_.fetch_add(1, std::memory_order_acq_rel);
        auto* context = run.get();
        if (WorkerReaper::instance().enqueue(
                context, &AnalysisController::reapWorker)) {
            run.release();
            return;
        }
        retiring_.fetch_sub(1, std::memory_order_acq_rel);
    }
    // Same cancellation boundary as reapWorker; queue saturation must not
    // turn cancellation into an unbounded read-pipe join on the UI thread.
    if(run->cancelled.load(std::memory_order_acquire))
        cancelAnalysisWorker(run->worker,100);
    if (run->reader.joinable()) run->reader.join();
    closeAnalysisWorker(run->worker);
}

void AnalysisController::reapWorker(void* context) noexcept
{
    std::unique_ptr<WorkerRun> run(static_cast<WorkerRun*>(context));
    if (run->cancelled.load(std::memory_order_acquire)) {
        // Track navigation is interactive. A worker already at its terminal
        // frame still gets a short cooperative window, while the Job Object
        // remains the deterministic cancellation boundary.
        cancelAnalysisWorker(run->worker, 100);
    }
    if (run->reader.joinable()) run->reader.join();
    closeAnalysisWorker(run->worker);
    run->owner->onRunRetired();
}

void AnalysisController::onRunRetired() noexcept
{
    retiring_.fetch_sub(1, std::memory_order_acq_rel);
    if (accepting_.load(std::memory_order_acquire)) {
        fb2k::inMainThread([this] { resumePending(); });
    }
}

void AnalysisController::setLaunchError(
    const std::filesystem::path& track, std::wstring error)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.status = DeepAnalysisStatus::error;
        state_.trackPath = track;
        state_.error = std::move(error);
        resultJson_.clear();
    }
    postListenAnalysisRefresh();
}

const wchar_t* analysisStageLabel(AnalysisStage stage) noexcept
{
    switch (stage) {
    case AnalysisStage::queued: return L"QUEUED";
    case AnalysisStage::decode: return L"DECODING";
    case AnalysisStage::waveform: return L"WAVEFORM";
    case AnalysisStage::features: return L"FEATURES";
    case AnalysisStage::bpm: return L"PULSE";
    case AnalysisStage::key: return L"HARMONIC CENTER";
    case AnalysisStage::beats: return L"BEAT GRID";
    case AnalysisStage::harmony: return L"HARMONY";
    case AnalysisStage::segmentation: return L"SEGMENTATION";
    case AnalysisStage::timbre: return L"TIMBRE";
    case AnalysisStage::dynamics: return L"DYNAMICS";
    case AnalysisStage::rhythm: return L"RHYTHM";
    case AnalysisStage::melody: return L"MELODY";
    case AnalysisStage::stabilization: return L"STABILIZING";
    case AnalysisStage::complete: return L"COMPLETE";
    default: return L"ANALYSIS";
    }
}

} // namespace nekorox::component
