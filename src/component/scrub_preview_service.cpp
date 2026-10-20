#include "scrub_preview_service.h"

#include "rollout_flags.h"
#include "scrub_output_adapter.h"
#include "sdk_compat.h"
#include "nekorox/pcm_frame_carry.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace nekorox::component {
namespace {
// A target change prepares at most 30 ms, then the same decoder keeps reading
// successive blocks. The old 80 ms one-shot grain made rapid pointer input
// repeatedly throw away work and left a stationary pointer starved.
constexpr double kGrainSeconds = 0.030;
constexpr double kOverlapSeconds = 0.020;
constexpr std::size_t kMaximumPcmBytes = 8U << 20U;

void applyTransition(std::vector<float>& pcm, std::size_t frames,
    std::uint32_t channels, std::uint32_t rate,
    const std::vector<float>& previousTail, bool sessionEdge,
    bool reposition)
{
    if (frames == 0 || channels == 0 || rate == 0) return;
    const auto overlap = (std::min)(frames, static_cast<std::size_t>(
        std::llround(rate * kOverlapSeconds)));
    const auto previousFrames = previousTail.size() / channels;
    const auto blendFrames = reposition
        ? (std::min)(overlap, previousFrames) : 0U;
    const auto edgeFrames = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(rate * 0.008)));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        auto envelope = 0.50118723f;
        if (sessionEdge && frame < edgeFrames) {
            auto amount = static_cast<float>(frame) /
                static_cast<float>(edgeFrames);
            amount = amount * amount * (3.0f - 2.0f * amount);
            envelope *= amount;
        }
        for (std::size_t channel = 0; channel < channels; ++channel)
            pcm[frame * channels + channel] *= envelope;
    }
    for (std::size_t frame = 0; frame < blendFrames; ++frame) {
        const auto gains = scrubPreviewCrossfadeGains(frame, blendFrames);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            pcm[frame * channels + channel] =
                previousTail[(previousFrames - blendFrames + frame) * channels + channel] *
                    gains.outgoing + pcm[frame * channels + channel] * gains.incoming;
        }
    }
}
}

struct ScrubPreviewService::Impl {
    struct Target {
        std::uint64_t session = 0;
        std::uint64_t revision = 0;
        metadb_handle_ptr track;
        double seconds = 0.0;
        ScrubOutputRoute route;
        bool reposition = true;
    };

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    bool closeRequested = false;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t targetRevision = 0;
    std::uint64_t outputOpenCount = 0;
    std::uint64_t decoderOpenCount = 0;
    std::uint64_t flushCount = 0;
    std::uint64_t queuedFrames = 0;
    std::uint64_t submittedCursor = 0;
    std::uint64_t staleDrops = 0;
    std::uint64_t acceptedBlocks = 0;
    std::optional<Target> pending;
    std::shared_ptr<abort_callback_impl> activeTargetAbort;
    ScrubPreviewSnapshot state;
    metadb_handle_ptr activeTrack;
    ScrubOutputRoute activeRoute;

    void publish(const Target& target, ScrubPreviewState next,
        std::string reason = {})
    {
        std::lock_guard lock(mutex);
        if (target.session != sessionGeneration ||
            target.revision != targetRevision || stopping) return;
        state = {next, target.revision, std::move(reason), target.session,
            target.revision, outputOpenCount, decoderOpenCount, flushCount,
            queuedFrames, submittedCursor, staleDrops, acceptedBlocks};
    }

    void ensureWorker()
    {
        if (running) return;
        stopping = false;
        running = true;
        worker = std::thread([this] { run(); });
    }

    bool decodeGrain(input_decoder& decoder, const Target& target,
        PcmFrameCarry& carry,
        abort_callback& aborter, std::vector<float>& pcm,
        std::uint32_t& rate, std::uint32_t& channels, std::uint32_t& mask)
    {
        if (target.reposition) {
            decoder.seek((std::max)(0.0, target.seconds), aborter);
            carry.clear();
        }
        std::size_t wantedFrames = 0;
        for (;;) {
            if (carry.empty()) {
                carry.clear();
                audio_chunk_impl chunk;
                if (!decoder.run(chunk, aborter)) break;
                aborter.check();
                const auto chunkRate = chunk.get_sample_rate();
                const auto chunkChannels = chunk.get_channel_count();
                const auto chunkMask = chunk.get_channel_config();
                const auto frames = static_cast<std::size_t>(
                    chunk.get_sample_count());
                if (chunkRate == 0 || chunkChannels == 0 ||
                    chunkChannels > 32 || frames == 0 ||
                    !carry.replace(chunk.get_data(), frames, chunkRate,
                        chunkChannels, chunkMask, kMaximumPcmBytes)) {
                    carry.clear();
                    throw std::runtime_error("decoder returned invalid preview PCM");
                }
            }
            if (rate == 0) {
                rate = carry.rate();
                channels = carry.channels();
                mask = carry.mask();
                wantedFrames = static_cast<std::size_t>(std::llround(rate * kGrainSeconds));
                if (wantedFrames * channels * sizeof(float) > kMaximumPcmBytes)
                    throw std::runtime_error("preview PCM cache limit exceeded");
                pcm.reserve(wantedFrames * channels);
            }
            if (rate != carry.rate() || channels != carry.channels() ||
                mask != carry.mask()) break;
            const auto haveFrames = pcm.size() / channels;
            (void)carry.appendTo(pcm, wantedFrames - haveFrames);
            if (pcm.size() / channels >= wantedFrames) break;
        }
        return !pcm.empty();
    }

    void run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::uint64_t openSession = 0;
        metadb_handle_ptr openTrack;
        std::unique_ptr<ScrubOutputStream> output;
        service_ptr_t<input_decoder> decoder;
        PcmFrameCarry decoderCarry;
        std::vector<float> previousTail;
        std::uint32_t previousRate = 0;
        std::uint32_t previousChannels = 0;
        std::uint32_t previousMask = 0;
        for (;;) {
            Target target;
            bool haveTarget = false;
            bool closeOutput = false;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    return stopping || closeRequested || pending.has_value();
                });
                if (stopping) break;
                if (closeRequested) {
                    closeRequested = false;
                    closeOutput = true;
                }
                if (pending) {
                    target = std::move(*pending);
                    pending.reset();
                    haveTarget = true;
                }
            }
            if (closeOutput) {
                if (output) {
                    output->stop();
                    std::lock_guard lock(mutex);
                    ++flushCount;
                    queuedFrames = 0;
                }
                output.reset();
                decoder.release();
                decoderCarry.clear();
                openTrack.release();
                openSession = 0;
                previousTail.clear();
                previousRate = 0;
                previousChannels = 0;
                previousMask = 0;
            }
            if (!haveTarget) continue;
            try {
                auto aborter = std::make_shared<abort_callback_impl>();
                {
                    std::lock_guard lock(mutex);
                    if (stopping || target.session != sessionGeneration ||
                        target.revision != targetRevision) continue;
                    activeTargetAbort = aborter;
                }
                struct AbortRegistration {
                    Impl* owner;
                    std::shared_ptr<abort_callback_impl> aborter;
                    ~AbortRegistration() {
                        std::lock_guard lock(owner->mutex);
                        if (owner->activeTargetAbort == aborter)
                            owner->activeTargetAbort.reset();
                    }
                } registration{this, aborter};
                if (scrubPreviewNeedsSessionOpen(openSession, target.session,
                        openTrack.is_valid() && openTrack == target.track,
                        output != nullptr, decoder.is_valid())) {
                    if (output) output->stop();
                    output.reset();
                    decoder.release();
                    decoderCarry.clear();
                    previousTail.clear();
                    previousRate = 0;
                    previousChannels = 0;
                    previousMask = 0;
                    std::string error;
                    output = openScrubOutput(target.route, error);
                    if (!output) {
                        publish(target, ScrubPreviewState::silentSelection, std::move(error));
                        continue;
                    }
                    service_ptr_t<file> fileHint;
                    input_entry::g_open_for_decoding(decoder, fileHint,
                        target.track->get_path(), *aborter);
                    decoder->initialize(target.track->get_subsong_index(),
                        input_flag_no_looping, *aborter);
                    aborter->check();
                    openSession = target.session;
                    openTrack = target.track;
                    {
                        std::lock_guard lock(mutex);
                        ++outputOpenCount;
                        ++decoderOpenCount;
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    if (stopping || target.session != sessionGeneration ||
                        target.revision != targetRevision) continue;
                }
                std::vector<float> pcm;
                std::uint32_t rate = 0, channels = 0, mask = 0;
                if (!decodeGrain(*decoder, target, decoderCarry, *aborter, pcm,
                        rate, channels, mask)) {
                    publish(target, ScrubPreviewState::failed,
                        "decoder produced no preview audio");
                    continue;
                }
                const auto formatChanged = previousRate != 0 &&
                    (previousRate != rate || previousChannels != channels ||
                        previousMask != mask);
                if (formatChanged) {
                    output->flush();
                    previousTail.clear();
                    std::lock_guard lock(mutex);
                    ++flushCount;
                    queuedFrames = 0;
                }
                const auto limits = scrubDeliveryLimits(rate, channels);
                const auto availableFrames = pcm.size() / channels;
                const auto deliveryFrames = std::min(
                    availableFrames, limits.maximumQueuedFrames);
                pcm.resize(deliveryFrames * channels);
                applyTransition(pcm, deliveryFrames, channels, rate,
                    previousTail, previousTail.empty(), target.reposition);
                std::size_t submittedFrames = 0;
                previousChannels = channels;
                previousRate = rate;
                previousMask = mask;
                std::string error;
                for (std::size_t offset = 0; offset < deliveryFrames;
                    offset += limits.blockFrames) {
                    {
                        std::lock_guard lock(mutex);
                        if (stopping || closeRequested ||
                            target.session != sessionGeneration ||
                            target.revision != targetRevision) {
                            ++staleDrops;
                            break;
                        }
                    }
                    const auto frames = std::min(
                        limits.blockFrames, deliveryFrames - offset);
                    const auto samples = std::span<const float>(
                        pcm.data() + offset * channels, frames * channels);
                    if (!output->submit(
                            {samples, frames, rate, channels, mask}, error)) break;
                    submittedFrames += frames;
                    {
                        std::lock_guard lock(mutex);
                        // The SDK reports readiness rather than an exact
                        // consumed cursor. Keep this as a bounded latency
                        // estimate, never a monotonic submitted counter.
                        queuedFrames = std::min<std::uint64_t>(
                            limits.maximumQueuedFrames, frames);
                        submittedCursor += frames;
                        ++acceptedBlocks;
                    }
                }
                if (submittedFrames != 0) {
                    const auto tailFrames = std::min(submittedFrames,
                        static_cast<std::size_t>(std::llround(
                            rate * kOverlapSeconds)));
                    const auto end = pcm.begin() + submittedFrames * channels;
                    previousTail.assign(end - tailFrames * channels, end);
                    output->forcePlay();
                }
                if (!error.empty()) {
                    publish(target, ScrubPreviewState::failed, std::move(error));
                    continue;
                }
                if (submittedFrames >= limits.blockFrames * 2U)
                    publish(target, ScrubPreviewState::ready);
                if (submittedFrames != 0) {
                    std::lock_guard lock(mutex);
                    if (!stopping && !closeRequested &&
                            target.session == sessionGeneration &&
                            target.revision == targetRevision && !pending) {
                        // Stationary pointer: advance from the selected
                        // region instead of waiting for another mouse event.
                        target.seconds += static_cast<double>(submittedFrames) /
                            std::max<std::uint32_t>(1U, rate);
                        target.reposition = false;
                        pending = target;
                        changed.notify_one();
                    }
                }
            } catch (const exception_aborted&) {
            } catch (const std::exception& exception) {
                publish(target, ScrubPreviewState::failed, exception.what());
            } catch (...) {
                publish(target, ScrubPreviewState::failed, "preview worker failed");
            }
        }
        if (output) output->stop();
    }
};

ScrubPreviewService& ScrubPreviewService::instance()
{
    static ScrubPreviewService service;
    return service;
}

ScrubPreviewService::ScrubPreviewService() : impl_(std::make_unique<Impl>()) {}
ScrubPreviewService::~ScrubPreviewService() { shutdown(); }

bool ScrubPreviewService::start(double targetSeconds) noexcept
{
    if (!rolloutFlagEnabled(RolloutFlag::auditionV1)) return false;
    try {
        metadb_handle_ptr track;
        if (!playback_control::get()->get_now_playing(track) || !track.is_valid())
            return false;
        auto route = probeScrubOutputRoute();
        std::lock_guard lock(impl_->mutex);
        impl_->ensureWorker();
        ++impl_->sessionGeneration;
        ++impl_->targetRevision;
        impl_->acceptedBlocks = 0;
        impl_->queuedFrames = 0;
        if (impl_->activeTargetAbort) impl_->activeTargetAbort->abort();
        impl_->activeTrack = track;
        impl_->activeRoute = route;
        impl_->state = {route.available ? ScrubPreviewState::awaiting :
            ScrubPreviewState::silentSelection, impl_->targetRevision, route.reason,
            impl_->sessionGeneration, impl_->targetRevision,
            impl_->outputOpenCount, impl_->decoderOpenCount, impl_->flushCount,
            impl_->queuedFrames, impl_->submittedCursor, impl_->staleDrops};
        if (route.available) impl_->pending = Impl::Target{impl_->sessionGeneration,
            impl_->targetRevision, track, targetSeconds, route, true};
        impl_->changed.notify_one();
        return route.available;
    } catch (...) { return false; }
}

void ScrubPreviewService::update(double targetSeconds) noexcept
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running || !impl_->activeTrack.is_valid() ||
        !impl_->activeRoute.available) return;
    ++impl_->targetRevision;
    if (impl_->activeTargetAbort) impl_->activeTargetAbort->abort();
    impl_->state = {ScrubPreviewState::awaiting, impl_->targetRevision, {},
        impl_->sessionGeneration, impl_->targetRevision,
        impl_->outputOpenCount, impl_->decoderOpenCount, impl_->flushCount,
        impl_->queuedFrames, impl_->submittedCursor, impl_->staleDrops};
    impl_->pending = Impl::Target{impl_->sessionGeneration, impl_->targetRevision,
        impl_->activeTrack, targetSeconds, impl_->activeRoute, true};
    impl_->changed.notify_one();
}

void ScrubPreviewService::cancel() noexcept
{
    std::lock_guard lock(impl_->mutex);
    ++impl_->sessionGeneration;
    ++impl_->targetRevision;
    impl_->pending.reset();
    if (impl_->activeTargetAbort) impl_->activeTargetAbort->abort();
    impl_->activeTrack.release();
    impl_->activeRoute = {};
    impl_->closeRequested = true;
    impl_->state = {ScrubPreviewState::idle, impl_->targetRevision, {},
        impl_->sessionGeneration, impl_->targetRevision,
        impl_->outputOpenCount, impl_->decoderOpenCount, impl_->flushCount,
        impl_->queuedFrames, impl_->submittedCursor, impl_->staleDrops};
    impl_->changed.notify_all();
}

void ScrubPreviewService::shutdown() noexcept
{
    std::thread worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->stopping = true;
        impl_->pending.reset();
        if (impl_->activeTargetAbort) impl_->activeTargetAbort->abort();
        worker = std::move(impl_->worker);
        impl_->running = false;
    }
    impl_->changed.notify_all();
    if (worker.joinable()) worker.join();
}

ScrubPreviewSnapshot ScrubPreviewService::snapshot() const
{
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->state;
    result.outputOpenCount = impl_->outputOpenCount;
    result.decoderOpenCount = impl_->decoderOpenCount;
    result.flushCount = impl_->flushCount;
    result.queuedFrames = impl_->queuedFrames;
    result.submittedCursor = impl_->submittedCursor;
    result.staleDrops = impl_->staleDrops;
    result.acceptedBlocks = impl_->acceptedBlocks;
    return result;
}

} // namespace nekorox::component
