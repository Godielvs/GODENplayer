#include "waveform_cache_service.h"

#include <foobar2000/SDK/input.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace nekorox::component {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kRequestDwell = std::chrono::milliseconds(12);
constexpr auto kProgressPublishInterval = std::chrono::milliseconds(33);
constexpr std::size_t kProgressPublishMinimumBins = 24;
constexpr std::size_t kInitialPreviewMinimumBins = 8;

struct DecodeRequest {
    std::uint64_t generation = 0;
    WaveformCacheKey key;
    WaveformCacheService::SnapshotCallback callback;
    Clock::time_point eligibleAt{};
};

double boundedProgress(std::uint64_t scanned, std::uint64_t total) noexcept
{
    if (total == 0) return 0.0;
    return std::clamp(
        static_cast<double>(scanned) / static_cast<double>(total),
        0.0, 1.0);
}

std::uint64_t expectedFrames(double durationSeconds, std::uint32_t sampleRate)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0 ||
        sampleRate == 0) {
        return 0;
    }
    const auto frames =
        static_cast<long double>(durationSeconds) * sampleRate;
    if (frames >=
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::llround(frames));
}

} // namespace

struct WaveformCacheService::Impl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    std::uint64_t nextGeneration = 0;
    std::optional<DecodeRequest> pending;
    std::shared_ptr<abort_callback_impl> activeAbort;
    std::uint64_t activeGeneration = 0;
    std::shared_ptr<WaveformPersistenceHook> persistence;
    SnapshotPtr current = std::make_shared<WaveformCacheSnapshot>();

    void publish(
        WaveformCacheSnapshot value,
        const SnapshotCallback& callback,
        bool requireCurrent = true)
    {
        auto shared = std::make_shared<WaveformCacheSnapshot>(std::move(value));
        {
            std::lock_guard lock(mutex);
            if (requireCurrent &&
                shared->generation == nextGeneration) {
                current = shared;
            }
        }
        if (callback) {
            try {
                callback(std::move(shared));
            } catch (...) {
                // A consumer callback must not terminate the decode worker.
            }
        }
    }

    bool generationIsCurrent(std::uint64_t generation) const
    {
        std::lock_guard lock(mutex);
        return !stopping && generation == nextGeneration;
    }

    void publishState(
        const DecodeRequest& request,
        WaveformCacheState state,
        std::shared_ptr<const WaveformPyramidV1> pyramid = {},
        double progress = 0.0,
        std::string error = {},
        bool requireCurrent = true)
    {
        WaveformCacheSnapshot value;
        value.generation = request.generation;
        value.key = request.key;
        value.state = state;
        value.progress = progress;
        value.pyramid = std::move(pyramid);
        value.error = std::move(error);
        publish(std::move(value), request.callback, requireCurrent);
    }

    void decode(const DecodeRequest& request,
        const std::shared_ptr<abort_callback_impl>& aborter,
        const std::shared_ptr<WaveformPersistenceHook>& persistenceHook)
    {
        if (filesystem::g_is_remote_or_unrecognized(
                request.key.path.c_str())) {
            publishState(request, WaveformCacheState::failed, {}, 0.0,
                "WaveSeek supports local tracks only");
            return;
        }

        publishState(request, WaveformCacheState::decoding);
        service_ptr_t<input_decoder> decoder;
        service_ptr_t<file> fileHint;
        input_entry::g_open_for_decoding(
            decoder,
            fileHint,
            request.key.path.c_str(),
            *aborter);
        decoder->initialize(
            request.key.subsong,
            input_flag_no_seeking | input_flag_no_looping,
            *aborter);

        audio_chunk_impl chunk;
        std::vector<float> convertedSamples;
        std::unique_ptr<WaveformPyramidBuilder> builder;
        auto lastPublish = Clock::now();
        std::size_t lastPublishedBins = 0;
        std::string appendError;
        bool reachedEnd = false;

        const auto decodeNextChunk = [&]() {
            if (!decoder->run(chunk, *aborter)) {
                reachedEnd = true;
                return false;
            }
            aborter->check();
            const auto sampleRate = chunk.get_sample_rate();
            const auto channels = chunk.get_channel_count();
            const auto frameCount = chunk.get_sample_count();
            if (sampleRate == 0 || channels == 0 || frameCount == 0 ||
                frameCount >
                    std::numeric_limits<std::size_t>::max() / channels) {
                throw std::runtime_error(
                    "decoder produced an invalid PCM chunk");
            }
            if (builder == nullptr) {
                builder = std::make_unique<WaveformPyramidBuilder>(
                    request.generation,
                    expectedFrames(request.key.durationSeconds, sampleRate));
            }
            const auto sampleCount =
                static_cast<std::size_t>(frameCount) * channels;
            convertedSamples.resize(sampleCount);
            const auto* source = chunk.get_data();
            std::transform(source, source + sampleCount,
                convertedSamples.begin(), [](audio_sample sample) {
                    return static_cast<float>(sample);
                });
            if (!builder->appendInterleaved(
                    convertedSamples,
                    frameCount, channels, sampleRate, appendError)) {
                throw std::runtime_error(appendError);
            }
            return true;
        };

        // Decode a tiny sequential prefix before any fingerprint/SQLite
        // access. This guarantees a real first visual frame even when the
        // persistent cache lives on a cold or fragmented disk.
        while ((builder == nullptr ||
                builder->completedBaseBins() < kInitialPreviewMinimumBins) &&
               decodeNextChunk()) {
        }
        if (builder == nullptr || builder->scannedFrames() == 0) {
            throw std::runtime_error("decoder produced no PCM");
        }

        auto progressive = std::make_shared<WaveformPyramidV1>(
            builder->snapshot(false));
        auto progressiveProgress = boundedProgress(
            progressive->scannedFrames, progressive->totalFrames);
        publishState(request, WaveformCacheState::decoding,
            progressive, progressiveProgress);
        lastPublishedBins = builder->completedBaseBins();
        lastPublish = Clock::now();

        // Persistence is an optimization, not a gate for first paint. A
        // complete hit replaces the preview atomically; a miss continues the
        // already-open sequential decoder from the same position.
        if (persistenceHook != nullptr) {
            publishState(request, WaveformCacheState::loadingCache,
                progressive, progressiveProgress);
            std::vector<std::byte> encoded;
            std::string cacheError;
            bool found = false;
            try {
                found = persistenceHook->load(
                    request.key, encoded, cacheError);
            } catch (const std::exception& exception) {
                cacheError = exception.what();
            } catch (...) {
                cacheError = "waveform persistence load failed";
            }
            aborter->check();
            if (found) {
                WaveformPyramidV1 cached;
                std::string decodeError;
                if (decodeWaveformPyramid(encoded, cached, decodeError) &&
                    cached.complete()) {
                    cached.generation = request.generation;
                    publishState(request, WaveformCacheState::complete,
                        std::make_shared<WaveformPyramidV1>(
                            std::move(cached)),
                        1.0);
                    return;
                }
            }
            // Cache errors are deliberately non-fatal: a clean streaming
            // decode repairs stale or corrupt data through the store hook.
            publishState(request, WaveformCacheState::decoding,
                progressive, progressiveProgress);
        }

        while (!reachedEnd && decodeNextChunk()) {

            const auto now = Clock::now();
            const auto completedBins = builder->completedBaseBins();
            if (now - lastPublish >= kProgressPublishInterval &&
                completedBins >=
                    lastPublishedBins + kProgressPublishMinimumBins) {
                auto progressive =
                    std::make_shared<WaveformPyramidV1>(
                        builder->snapshot(false));
                publishState(request, WaveformCacheState::decoding,
                    progressive,
                    boundedProgress(
                        progressive->scannedFrames,
                        progressive->totalFrames));
                lastPublish = now;
                lastPublishedBins = completedBins;
            }
        }
        aborter->check();

        auto completed = std::make_shared<WaveformPyramidV1>(
            builder->finalize());
        if (!generationIsCurrent(request.generation)) {
            publishState(request, WaveformCacheState::cancelled,
                completed,
                boundedProgress(
                    completed->scannedFrames, completed->totalFrames),
                {}, false);
            return;
        }

        if (persistenceHook != nullptr) {
            std::vector<std::byte> encoded;
            std::string encodeError;
            if (encodeWaveformPyramid(*completed, encoded, encodeError)) {
                std::string storeError;
                try {
                    persistenceHook->store(
                        request.key, encoded, storeError);
                } catch (...) {
                    // Persistence is a cache optimization, never a reason to
                    // discard a successfully decoded waveform.
                }
            }
        }
        aborter->check();
        publishState(request, WaveformCacheState::complete,
            std::move(completed), 1.0);
    }

    void run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            DecodeRequest request;
            std::shared_ptr<abort_callback_impl> aborter;
            std::shared_ptr<WaveformPersistenceHook> persistenceHook;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    return stopping || pending.has_value();
                });
                if (stopping) break;

                const auto eligibleAt = pending->eligibleAt;
                const auto eligibleGeneration = pending->generation;
                if (Clock::now() < eligibleAt) {
                    changed.wait_until(lock, eligibleAt, [&] {
                        return stopping || !pending.has_value() ||
                            pending->generation != eligibleGeneration;
                    });
                    if (stopping) break;
                    if (!pending.has_value() ||
                        pending->generation != eligibleGeneration) {
                        continue;
                    }
                }

                request = std::move(*pending);
                pending.reset();
                aborter = std::make_shared<abort_callback_impl>();
                activeAbort = aborter;
                activeGeneration = request.generation;
                persistenceHook = persistence;
            }

            try {
                decode(request, aborter, persistenceHook);
            } catch (const exception_aborted&) {
                publishState(request, WaveformCacheState::cancelled,
                    {}, 0.0, {}, false);
            } catch (const std::exception& exception) {
                if (aborter->is_aborting()) {
                    publishState(request, WaveformCacheState::cancelled,
                        {}, 0.0, {}, false);
                } else {
                    publishState(request, WaveformCacheState::failed,
                        {}, 0.0, exception.what());
                }
            } catch (...) {
                publishState(request,
                    aborter->is_aborting()
                        ? WaveformCacheState::cancelled
                        : WaveformCacheState::failed,
                    {}, 0.0,
                    aborter->is_aborting()
                        ? std::string{}
                        : std::string{"waveform decoding failed"});
            }

            {
                std::lock_guard lock(mutex);
                if (activeGeneration == request.generation) {
                    activeAbort.reset();
                    activeGeneration = 0;
                }
            }
        }
    }
};

WaveformCacheService::WaveformCacheService()
    : impl_(std::make_unique<Impl>())
{
}

WaveformCacheService::~WaveformCacheService()
{
    stop();
}

void WaveformCacheService::start()
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) return;
    impl_->stopping = false;
    impl_->running = true;
    impl_->worker = std::thread([impl = impl_.get()] {
        impl->run();
    });
}

void WaveformCacheService::stop()
{
    std::thread worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->stopping = true;
        ++impl_->nextGeneration;
        impl_->pending.reset();
        if (impl_->activeAbort != nullptr) impl_->activeAbort->abort();
        worker = std::move(impl_->worker);
        impl_->running = false;
    }
    impl_->changed.notify_all();
    if (worker.joinable()) worker.join();

    auto idle = std::make_shared<WaveformCacheSnapshot>();
    std::lock_guard lock(impl_->mutex);
    impl_->activeAbort.reset();
    impl_->activeGeneration = 0;
    impl_->current = std::move(idle);
}

std::uint64_t WaveformCacheService::request(
    const metadb_handle_ptr& track,
    SnapshotCallback callback)
{
    if (track.is_empty()) return 0;

    DecodeRequest request;
    request.key.path = track->get_path();
    request.key.subsong = track->get_subsong_index();
    request.key.durationSeconds = track->get_length();
    request.callback = std::move(callback);
    request.eligibleAt = Clock::now() + kRequestDwell;

    SnapshotPtr dwelling;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running || impl_->stopping) return 0;
        request.generation = ++impl_->nextGeneration;
        if (impl_->activeAbort != nullptr) impl_->activeAbort->abort();
        impl_->pending = request;

        auto value = std::make_shared<WaveformCacheSnapshot>();
        value->generation = request.generation;
        value->key = request.key;
        value->state = WaveformCacheState::dwelling;
        impl_->current = value;
        dwelling = std::move(value);
    }
    impl_->changed.notify_all();
    if (request.callback) {
        try {
            request.callback(std::move(dwelling));
        } catch (...) {
        }
    }
    return request.generation;
}

void WaveformCacheService::cancel()
{
    std::optional<DecodeRequest> cancelledPending;
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->nextGeneration;
        if (impl_->pending.has_value()) {
            cancelledPending = std::move(impl_->pending);
            impl_->pending.reset();
        }
        if (impl_->activeAbort != nullptr) impl_->activeAbort->abort();

        auto value = std::make_shared<WaveformCacheSnapshot>();
        value->generation = impl_->nextGeneration;
        value->state = WaveformCacheState::idle;
        impl_->current = std::move(value);
    }
    impl_->changed.notify_all();
    if (cancelledPending.has_value() && cancelledPending->callback) {
        auto value = std::make_shared<WaveformCacheSnapshot>();
        value->generation = cancelledPending->generation;
        value->key = cancelledPending->key;
        value->state = WaveformCacheState::cancelled;
        try {
            cancelledPending->callback(std::move(value));
        } catch (...) {
        }
    }
}

void WaveformCacheService::setPersistenceHook(
    std::shared_ptr<WaveformPersistenceHook> hook)
{
    std::lock_guard lock(impl_->mutex);
    impl_->persistence = std::move(hook);
}

WaveformCacheService::SnapshotPtr WaveformCacheService::snapshot() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->current;
}

} // namespace nekorox::component
