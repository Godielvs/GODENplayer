#include "packet_cost_controller.h"
#include "worker_failure.h"

#include "database_service.h"
#include "runtime.h"
#include "sdk_compat.h"
#include "text_util.h"
#include "workspace_notifications.h"
#include "worker_reaper.h"

#include <cassert>
#include <utility>

namespace nekorox::component {
namespace {

constexpr SIZE_T kWorkerMemoryLimit = 512ULL * 1024ULL * 1024ULL;

void postPacketCostRefresh()
{
    postListenRefresh();
    notifyNativePanelsPlaybackChanged();
}

std::string sourceRevision(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return {};
    return std::to_string(size) + ":" +
        std::to_string(modified.time_since_epoch().count());
}

} // namespace

struct PacketCostController::WorkerRun {
    PacketCostController* owner = nullptr;
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
    std::filesystem::path trackPath;
    std::string sourceRevision;
    std::string sourceTrackId;
    AnalysisWorkerHandles worker;
    std::thread reader;
    std::atomic<bool> cancelled{false};
    bool retireCounted = false;
};

PacketCostController& PacketCostController::instance()
{
    static PacketCostController controller;
    return controller;
}

PacketCostController::~PacketCostController()
{
    stop();
}

void PacketCostController::onTrackChanged(
    const std::filesystem::path& localTrack)
{
    if (!accepting_.load(std::memory_order_acquire)) return;
    const auto revision = sourceRevision(localTrack);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.trackPath == localTrack &&
            state_.sourceRevision == revision) return;
    }
    cancelWorker();
    const auto generation = generation_.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.trackPath = localTrack;
        state_.sourceRevision = revision;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(localTrack, fileError)) {
        postPacketCostRefresh();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.status = PacketCostStatus::loading;
    }
    loadCache(generation, localTrack, revision);
    postPacketCostRefresh();
}

void PacketCostController::beginDeepAnalysis(std::uint64_t requestId)
{
    if (requestId == 0) return;
    bool shouldCancel = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        deepAnalysisRequestId_ = requestId;
        shouldCancel = active_ != nullptr;
        const auto hasFinalResult =
            state_.status == PacketCostStatus::ready ||
            state_.status == PacketCostStatus::unsupported;
        if (!hasFinalResult &&
            (shouldCancel || state_.status == PacketCostStatus::running)) {
            deferredForDeepAnalysis_ = true;
            state_.status = PacketCostStatus::loading;
            state_.requestId = 0;
            state_.error.clear();
        }
    }
    if (shouldCancel) cancelWorker();
    postPacketCostRefresh();
}

void PacketCostController::endDeepAnalysis(std::uint64_t requestId)
{
    std::filesystem::path localTrack;
    std::string revision;
    std::uint64_t generation = 0;
    bool resume = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requestId == 0 || deepAnalysisRequestId_ != requestId) return;
        deepAnalysisRequestId_ = 0;
        const auto pendingCurrent = pendingStart_ &&
            pendingGeneration_ == generation_.load(std::memory_order_relaxed) &&
            pendingTrack_ == state_.trackPath &&
            pendingSourceRevision_ == state_.sourceRevision;
        resume = (deferredForDeepAnalysis_ || pendingCurrent) &&
            state_.status != PacketCostStatus::ready &&
            state_.status != PacketCostStatus::unsupported &&
            !state_.trackPath.empty();
        deferredForDeepAnalysis_ = false;
        // A retiring worker may have delivered its wake while Deep held the
        // launch gate. Transfer the pending request to a fresh cache lookup;
        // a later reaper callback must not launch the old identity meanwhile.
        pendingStart_ = false;
        pendingGeneration_ = 0;
        pendingTrack_.clear();
        pendingSourceRevision_.clear();
        if (resume) {
            state_.status = PacketCostStatus::loading;
            localTrack = state_.trackPath;
            revision = state_.sourceRevision;
            generation = generation_.load(std::memory_order_relaxed);
        }
    }
    if (resume) loadCache(generation, localTrack, std::move(revision));
    postPacketCostRefresh();
}

void PacketCostController::loadCache(std::uint64_t generation,
    const std::filesystem::path& localTrack, std::string revision)
{
    DatabaseService::instance().loadPacketCost(localTrack,
        [this, generation, revision = std::move(revision)](
                PacketCostCachePayload payload) mutable {
            onCacheLoaded(generation, std::move(revision),
                std::move(payload));
        });
}

void PacketCostController::stop()
{
    shutdownAndDrain();
}

void PacketCostController::shutdownAndDrain()
{
    accepting_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    cancelWorker();
    std::lock_guard<std::mutex> lock(mutex_);
    pendingStart_ = false;
    pendingTrack_.clear();
    pendingSourceRevision_.clear();
    state_ = {};
    deepAnalysisRequestId_ = 0;
    deferredForDeepAnalysis_ = false;
}

std::shared_ptr<const PacketCostResult> PacketCostController::readyResult() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.status == PacketCostStatus::ready && state_.result &&
            state_.result->supported
        ? state_.result : std::shared_ptr<const PacketCostResult>{};
}

std::shared_ptr<const PacketCostResult> PacketCostController::readyResultForTrack(
    const std::filesystem::path& track) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.status == PacketCostStatus::ready && state_.result &&
            state_.result->supported && state_.trackPath == track
        ? state_.result : std::shared_ptr<const PacketCostResult>{};
}

PacketCostDiagnostics PacketCostController::diagnostics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    PacketCostDiagnostics result;
    result.generation = generation_.load(std::memory_order_relaxed);
    result.activeRequestId = active_ ? active_->requestId : 0;
    result.retiring = retiring_.load(std::memory_order_relaxed);
    result.pendingStart = pendingStart_;
    return result;
}

void PacketCostController::cancelWorker()
{
    std::unique_ptr<WorkerRun> run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run = std::move(active_);
    }
    if (run == nullptr) return;
    run->cancelled.store(true, std::memory_order_release);
    finishRun(std::move(run), true);
}

void PacketCostController::startWorker(std::uint64_t generation,
    const std::filesystem::path& localTrack,
    const std::string& revision)
{
    if (generation != generation_.load(std::memory_order_acquire)) return;
    std::string sourceTrackId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ != nullptr || state_.trackPath != localTrack ||
            state_.sourceRevision != revision ||
            state_.status == PacketCostStatus::ready ||
            state_.status == PacketCostStatus::unsupported) {
            return;
        }
        if (deepAnalysisRequestId_ != 0) {
            deferredForDeepAnalysis_ = true;
            state_.status = PacketCostStatus::loading;
            return;
        }
        if (retiring_.load(std::memory_order_relaxed) != 0) {
            pendingGeneration_ = generation;
            pendingTrack_ = localTrack;
            pendingSourceRevision_ = revision;
            pendingStart_ = true;
            return;
        }
        sourceTrackId = state_.sourceTrackId;
    }
    auto run = std::make_unique<WorkerRun>();
    run->owner = this;
    run->requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    run->generation = generation;
    run->trackPath = localTrack;
    run->sourceRevision = revision;
    run->sourceTrackId = std::move(sourceTrackId);
    std::wstring launchError;
    if (!launchAnalysisWorker(L"cost", run->requestId, localTrack,
            kWorkerMemoryLimit, run->worker, launchError)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == generation_.load(std::memory_order_relaxed) &&
                state_.trackPath == localTrack) {
                state_.status = PacketCostStatus::error;
                state_.error = std::move(launchError);
            }
        }
        postPacketCostRefresh();
        return;
    }
    bool abortLaunch = false;
    WorkerRun* active = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_.load(std::memory_order_relaxed) ||
            state_.trackPath != localTrack ||
            state_.sourceRevision != revision ||
            deepAnalysisRequestId_ != 0) {
            if (state_.trackPath == localTrack && deepAnalysisRequestId_ != 0) {
                deferredForDeepAnalysis_ = true;
                state_.status = PacketCostStatus::loading;
            }
            abortLaunch = true;
        } else {
            state_.status = PacketCostStatus::running;
            state_.requestId = run->requestId;
            state_.error.clear();
            active_ = std::move(run);
            active = active_.get();
        }
    }
    if (abortLaunch) {
        run->cancelled.store(true, std::memory_order_release);
        finishRun(std::move(run), true);
        return;
    }
    try {
        active->reader = std::thread([this, active] { readerMain(active); });
    } catch (...) {
        std::unique_ptr<WorkerRun> failed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.get() == active) failed = std::move(active_);
            state_.status = PacketCostStatus::error;
            state_.error = L"The packet-cost reader thread could not start.";
        }
        finishRun(std::move(failed));
        postPacketCostRefresh();
        return;
    }
    postPacketCostRefresh();
}

void PacketCostController::resumePending()
{
    std::filesystem::path track;
    std::string revision;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pendingStart_ || !accepting_.load(std::memory_order_relaxed) ||
            active_ != nullptr || retiring_.load(std::memory_order_relaxed) != 0 ||
            deepAnalysisRequestId_ != 0) {
            return;
        }
        generation = pendingGeneration_;
        track = std::move(pendingTrack_);
        revision = std::move(pendingSourceRevision_);
        pendingTrack_.clear();
        pendingSourceRevision_.clear();
        pendingStart_ = false;
    }
    startWorker(generation, track, revision);
}

void PacketCostController::reapCompleted(std::uint64_t requestId)
{
    std::unique_ptr<WorkerRun> run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ == nullptr || active_->requestId != requestId) return;
        run = std::move(active_);
    }
    finishRun(std::move(run));
}

void PacketCostController::readerMain(WorkerRun* run)
{
    bool terminal = false;
    bool terminalNotified = false;
    bool sourceChanged = false;
    for (;;) {
        AnalysisFrameHeaderV1 header;
        std::vector<std::byte> payload;
        std::string protocolError;
        if (!readAnalysisWorkerFrame(run->worker.output, run->requestId,
                header, payload, protocolError)) {
            if (!protocolError.empty() &&
                !run->cancelled.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_.get() == run) {
                    state_.status = PacketCostStatus::error;
                    state_.error = utf8ToWide(protocolError);
                }
            }
            break;
        }
        if (header.kind == AnalysisMessageKind::complete) {
            PacketCostResult parsed;
            std::string parseError;
            if (!deserializePacketCost(payload, parsed, parseError)) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_.get() == run) {
                    state_.status = PacketCostStatus::error;
                    state_.error = utf8ToWide(parseError);
                }
            } else {
                const auto currentRevision = sourceRevision(run->trackPath);
                auto result = std::make_shared<PacketCostResult>(
                    std::move(parsed));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (active_.get() != run ||
                        run->generation !=
                            generation_.load(std::memory_order_relaxed) ||
                        state_.sourceRevision != run->sourceRevision ||
                        currentRevision != run->sourceRevision) {
                        sourceChanged = currentRevision != run->sourceRevision;
                        break;
                    }
                    state_.status = result->supported
                        ? PacketCostStatus::ready
                        : PacketCostStatus::unsupported;
                    state_.result = result;
                    state_.error = result->supported
                        ? std::wstring{} : utf8ToWide(result->error);
                }
                DatabaseService::instance().storePacketCost(
                    run->trackPath, run->sourceTrackId, std::move(result));
            }
            terminal = true;
            postPacketCostRefresh();
            terminalNotified = true;
            break;
        }
        if (header.kind == AnalysisMessageKind::error) {
            const std::string workerError(
                reinterpret_cast<const char*>(payload.data()), payload.size());
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.get() == run) {
                state_.status = PacketCostStatus::error;
                state_.error = utf8ToWide(workerError);
            }
            terminal = true;
            postPacketCostRefresh();
            terminalNotified = true;
            break;
        }
    }
    WaitForSingleObject(run->worker.process, 2000);
    if (!terminal && !run->cancelled.load(std::memory_order_acquire)) {
        DWORD exitCode = 0;
        GetExitCodeProcess(run->worker.process, &exitCode);
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.get() == run && state_.status == PacketCostStatus::running) {
            state_.status = PacketCostStatus::error;
            state_.error = workerExitDiagnostic(L"Packet-cost", exitCode);
        }
    }
    if (!terminalNotified) postPacketCostRefresh();
    const auto requestId = run->requestId;
    const auto generation = run->generation;
    const auto trackPath = run->trackPath;
    if (!run->cancelled.load(std::memory_order_acquire)) {
        fb2k::inMainThread([this, requestId, generation, sourceChanged, trackPath] {
            reapCompleted(requestId);
            if (!sourceChanged) return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Playback may have moved on while this completion waited for
                // the UI thread. Only its original request may retry the file.
                if (!accepting_.load(std::memory_order_relaxed) ||
                    generation != generation_.load(std::memory_order_relaxed) ||
                    state_.trackPath != trackPath) {
                    return;
                }
            }
            onTrackChanged(trackPath);
        });
    }
}

void PacketCostController::onCacheLoaded(
    std::uint64_t generation, std::string revision,
    PacketCostCachePayload payload)
{
    if (generation != generation_.load(std::memory_order_acquire)) return;
    if (!payload.trackPath.empty() &&
        sourceRevision(payload.trackPath) != revision) {
        onTrackChanged(payload.trackPath);
        return;
    }
    if (payload.found && payload.cost != nullptr) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_.load(std::memory_order_relaxed) ||
                state_.trackPath != payload.trackPath ||
                state_.sourceRevision != revision ||
                sourceRevision(payload.trackPath) != revision) {
                return;
            }
            state_.status = payload.cost->supported
                ? PacketCostStatus::ready
                : PacketCostStatus::unsupported;
            state_.sourceTrackId = std::move(payload.sourceTrackId);
            state_.result = std::move(payload.cost);
            state_.error = state_.result->supported
                ? std::wstring{} : utf8ToWide(state_.result->error);
        }
        postPacketCostRefresh();
        return;
    }
    if (!payload.error.empty()) {
        FB2K_console_formatter() << "GODENplayer Core packet-cost cache load: "
                                << payload.error.c_str();
    }
    bool deferred = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_.load(std::memory_order_relaxed) ||
            state_.trackPath != payload.trackPath ||
            state_.sourceRevision != revision) {
            return;
        }
        state_.sourceTrackId = std::move(payload.sourceTrackId);
        if (deepAnalysisRequestId_ != 0) {
            deferredForDeepAnalysis_ = true;
            state_.status = PacketCostStatus::loading;
            deferred = true;
        }
    }
    if (deferred) {
        postPacketCostRefresh();
        return;
    }
    startWorker(generation, payload.trackPath, revision);
}

void PacketCostController::finishRun(
    std::unique_ptr<WorkerRun> run, bool asynchronous) noexcept
{
    if (run == nullptr) return;
    if (asynchronous) {
        retiring_.fetch_add(1, std::memory_order_acq_rel);
        run->retireCounted = true;
        auto* context = run.get();
        if (WorkerReaper::instance().enqueue(
                context, &PacketCostController::reapWorker)) {
            run.release();
            return;
        }
        run->retireCounted = false;
        retiring_.fetch_sub(1, std::memory_order_acq_rel);
        if (run->cancelled.load(std::memory_order_acquire)) {
            cancelAnalysisWorker(run->worker, 250);
        }
    }
    if (run->reader.joinable()) run->reader.join();
    closeAnalysisWorker(run->worker);
}

void PacketCostController::onRunRetired() noexcept
{
    auto previous = retiring_.load(std::memory_order_acquire);
    while (previous != 0 && !retiring_.compare_exchange_weak(
            previous, previous - 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    assert(previous != 0 && "packet-cost retirement underflow prevented");
    if (previous == 0) {
        FB2K_console_formatter() <<
            "GODENplayer Core packet-cost retirement underflow prevented.";
        return;
    }
    if (accepting_.load(std::memory_order_acquire)) {
        fb2k::inMainThread([this] { resumePending(); });
    }
}

void PacketCostController::reapWorker(void* context) noexcept
{
    std::unique_ptr<WorkerRun> run(static_cast<WorkerRun*>(context));
    const auto counted = run->retireCounted;
    auto* owner = run->owner;
    assert(counted && "packet-cost reaper received an uncounted run");
    try {
        if (run->cancelled.load(std::memory_order_acquire)) {
            cancelAnalysisWorker(run->worker, 250);
        }
        if (run->reader.joinable()) run->reader.join();
        closeAnalysisWorker(run->worker);
    } catch (...) {
        FB2K_console_formatter() <<
            "GODENplayer Core packet-cost reaper cleanup failed.";
    }
    run->retireCounted = false;
    if (counted && owner != nullptr) owner->onRunRetired();
}

} // namespace nekorox::component
