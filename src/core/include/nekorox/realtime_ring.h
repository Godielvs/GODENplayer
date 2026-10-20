#pragma once

#include "nekorox/contracts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace nekorox {

// A bounded latest-frame exchange. Slots have explicit ownership states so the
// audio producer never writes memory that a consumer is reading.
template <std::size_t Capacity>
class RealtimeRing final {
    static_assert(Capacity >= 3);

public:
    bool publish(
        const float* samples,
        std::size_t sampleCount,
        std::uint32_t sampleRate,
        std::uint16_t channels,
        std::uint32_t frameCount,
        std::uint64_t qpcTicks,
        double playbackSeconds,
        std::uint16_t flags,
        std::uint64_t streamGeneration = 0,
        std::uint64_t firstFrameIndex = 0,
        std::uint64_t playbackSerial = 0,
        std::uint64_t recordingIdentityHash = 0,
        std::uint32_t discontinuityReasonMask = 0) noexcept
    {
        if (samples == nullptr || sampleCount == 0 ||
            sampleCount > kRealtimeMaxSamples || channels == 0 ||
            channels > kRealtimeMaxChannels || frameCount == 0 ||
            frameCount > kRealtimeMaxFrames) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto sequence =
            nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        Slot& slot = slots_[(sequence - 1) % Capacity];

        auto state = slot.state.load(std::memory_order_acquire);
        while (state == SlotState::empty || state == SlotState::full) {
            if (slot.state.compare_exchange_weak(
                    state,
                    SlotState::writing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (state == SlotState::full) {
                    overwritten_.fetch_add(1, std::memory_order_relaxed);
                }

                auto& frame = slot.frame;
                frame.version = kRealtimeRingVersion;
                frame.sequence = sequence;
                frame.streamGeneration = streamGeneration;
                frame.playbackSerial = playbackSerial;
                frame.recordingIdentityHash = recordingIdentityHash;
                frame.firstFrameIndex = firstFrameIndex;
                frame.qpcTicks = qpcTicks;
                frame.playbackSeconds = playbackSeconds;
                frame.sampleRate = sampleRate;
                frame.channels = channels;
                frame.flags = flags;
                frame.discontinuityReasonMask = discontinuityReasonMask;
                frame.frameCount = frameCount;
                frame.sampleCount = static_cast<std::uint32_t>(sampleCount);
                std::copy_n(samples, sampleCount, frame.samples.begin());

                slot.sequence.store(sequence, std::memory_order_relaxed);
                slot.state.store(SlotState::full, std::memory_order_release);
                published_.store(sequence, std::memory_order_release);
                return true;
            }
        }

        // The indexed slot is being consumed. Dropping this producer frame is
        // bounded and safer than waiting in foobar's audio callback.
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool consumeLatest(
        RealtimeFrameV1& output,
        std::uint64_t& lastSequence,
        std::uint64_t* skipped = nullptr) noexcept
    {
        std::size_t bestIndex = Capacity;
        std::uint64_t bestSequence = lastSequence;

        for (std::size_t i = 0; i < Capacity; ++i) {
            if (slots_[i].state.load(std::memory_order_acquire) !=
                SlotState::full) {
                continue;
            }
            const auto sequence =
                slots_[i].sequence.load(std::memory_order_relaxed);
            if (sequence > bestSequence) {
                bestSequence = sequence;
                bestIndex = i;
            }
        }

        if (bestIndex == Capacity) {
            return false;
        }

        Slot& best = slots_[bestIndex];
        auto expected = SlotState::full;
        if (!best.state.compare_exchange_strong(
                expected,
                SlotState::reading,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        output = best.frame;
        best.state.store(SlotState::empty, std::memory_order_release);

        const auto missed = bestSequence > lastSequence
            ? bestSequence - lastSequence - 1
            : 0;
        lastSequence = bestSequence;
        if (skipped != nullptr) {
            *skipped = missed;
        }

        // Reclaim stale complete frames. CAS prevents touching a producer or
        // another consumer's active slot.
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (i == bestIndex) {
                continue;
            }
            if (slots_[i].sequence.load(std::memory_order_relaxed) <
                bestSequence) {
                auto stale = SlotState::full;
                slots_[i].state.compare_exchange_strong(
                    stale,
                    SlotState::empty,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
            }
        }
        return true;
    }

    bool consumeNext(
        RealtimeFrameV1& output,
        std::uint64_t& lastSequence,
        std::uint64_t* skipped = nullptr) noexcept
    {
        std::size_t bestIndex = Capacity;
        auto bestSequence = std::numeric_limits<std::uint64_t>::max();

        for (std::size_t i = 0; i < Capacity; ++i) {
            if (slots_[i].state.load(std::memory_order_acquire) !=
                SlotState::full) {
                continue;
            }
            const auto sequence =
                slots_[i].sequence.load(std::memory_order_relaxed);
            if (sequence > lastSequence && sequence < bestSequence) {
                bestSequence = sequence;
                bestIndex = i;
            }
        }

        if (bestIndex == Capacity) return false;

        Slot& best = slots_[bestIndex];
        auto expected = SlotState::full;
        if (!best.state.compare_exchange_strong(
                expected,
                SlotState::reading,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        output = best.frame;
        best.state.store(SlotState::empty, std::memory_order_release);

        const auto missed = bestSequence > lastSequence
            ? bestSequence - lastSequence - 1
            : 0;
        lastSequence = bestSequence;
        if (skipped != nullptr) *skipped = missed;

        // A consumer that starts late may leave frames older than its first
        // readable sequence behind. Reclaim only those stale slots; newer
        // frames remain queued so a UI timer can drain them in order.
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (i == bestIndex ||
                slots_[i].sequence.load(std::memory_order_relaxed) >=
                    bestSequence) {
                continue;
            }
            auto stale = SlotState::full;
            slots_[i].state.compare_exchange_strong(
                stale,
                SlotState::empty,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);
        }
        return true;
    }

    std::uint64_t publishedSequence() const noexcept
    {
        return published_.load(std::memory_order_acquire);
    }

    std::uint64_t overwrittenCount() const noexcept
    {
        return overwritten_.load(std::memory_order_relaxed);
    }

    std::uint64_t rejectedCount() const noexcept
    {
        return rejected_.load(std::memory_order_relaxed);
    }

private:
    enum class SlotState : std::uint8_t {
        empty,
        writing,
        full,
        reading,
    };

    struct Slot {
        std::atomic<SlotState> state{SlotState::empty};
        std::atomic<std::uint64_t> sequence{0};
        RealtimeFrameV1 frame{};
    };

    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> nextSequence_{0};
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> overwritten_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

// A bounded, multicast latest-frame bus. Unlike RealtimeRing, reading a frame
// does not reclaim it, so any number of consumers can keep an independent
// sequence cursor without stealing work from one another. The producer never
// waits: if the selected slot is currently being copied by a consumer, the
// frame is rejected and the next publication can proceed normally.
template <std::size_t Capacity>
class RealtimeFrameBus final {
    static_assert(Capacity >= 3);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    bool publish(
        const float* samples,
        std::size_t sampleCount,
        std::uint32_t sampleRate,
        std::uint16_t channels,
        std::uint32_t frameCount,
        std::uint64_t qpcTicks,
        double playbackSeconds,
        std::uint16_t flags,
        std::uint64_t streamGeneration = 0,
        std::uint64_t firstFrameIndex = 0,
        std::uint64_t playbackSerial = 0,
        std::uint64_t recordingIdentityHash = 0,
        std::uint32_t discontinuityReasonMask = 0) noexcept
    {
        if (samples == nullptr || sampleCount == 0 || sampleRate == 0 ||
            sampleCount > kRealtimeMaxSamples || channels == 0 ||
            channels > kRealtimeMaxChannels || frameCount == 0 ||
            frameCount > kRealtimeMaxFrames ||
            frameCount > (std::numeric_limits<std::size_t>::max)() / channels ||
            sampleCount != static_cast<std::size_t>(frameCount) * channels) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto previous = nextSequence_.load(std::memory_order_relaxed);
        if (previous == (std::numeric_limits<std::uint64_t>::max)()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto sequence = previous + 1U;
        nextSequence_.store(sequence, std::memory_order_relaxed);
        Slot& slot = slots_[(sequence - 1) % Capacity];
        std::uint32_t expected = 0;
        if (!slot.ownership.compare_exchange_strong(
                expected, kWriterBit,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto& frame = slot.frame;
        frame.version = kRealtimeRingVersion;
        frame.sequence = sequence;
        frame.streamGeneration = streamGeneration;
        frame.playbackSerial = playbackSerial;
        frame.recordingIdentityHash = recordingIdentityHash;
        frame.firstFrameIndex = firstFrameIndex;
        frame.qpcTicks = qpcTicks;
        frame.playbackSeconds = playbackSeconds;
        frame.sampleRate = sampleRate;
        frame.channels = channels;
        frame.flags = flags;
        frame.discontinuityReasonMask = discontinuityReasonMask;
        frame.frameCount = frameCount;
        frame.sampleCount = static_cast<std::uint32_t>(sampleCount);
        std::copy_n(samples, sampleCount, frame.samples.begin());

        slot.sequenceHint.store(sequence, std::memory_order_relaxed);
        slot.ownership.store(0, std::memory_order_release);
        published_.store(sequence, std::memory_order_release);
        return true;
    }

    bool consumeLatest(
        RealtimeFrameV1& output,
        std::uint64_t& lastSequence,
        std::uint64_t* skipped = nullptr) noexcept
    {
        for (std::size_t attempt = 0; attempt < Capacity; ++attempt) {
            std::size_t bestIndex = Capacity;
            std::uint64_t bestSequence = lastSequence;
            for (std::size_t index = 0; index < Capacity; ++index) {
                const auto sequence =
                    slots_[index].sequenceHint.load(std::memory_order_acquire);
                if (sequence > bestSequence) {
                    bestSequence = sequence;
                    bestIndex = index;
                }
            }
            if (bestIndex == Capacity) return false;
            if (copySlot(bestIndex, bestSequence, output)) {
                reportConsumption(bestSequence, lastSequence, skipped);
                return true;
            }
        }
        return false;
    }

    bool consumeNext(
        RealtimeFrameV1& output,
        std::uint64_t& lastSequence,
        std::uint64_t* skipped = nullptr) noexcept
    {
        for (std::size_t attempt = 0; attempt < Capacity; ++attempt) {
            std::size_t bestIndex = Capacity;
            auto bestSequence = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t index = 0; index < Capacity; ++index) {
                const auto sequence =
                    slots_[index].sequenceHint.load(std::memory_order_acquire);
                if (sequence > lastSequence && sequence < bestSequence) {
                    bestSequence = sequence;
                    bestIndex = index;
                }
            }
            if (bestIndex == Capacity) return false;
            if (copySlot(bestIndex, bestSequence, output)) {
                reportConsumption(bestSequence, lastSequence, skipped);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t publishedSequence() const noexcept
    {
        return published_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t rejectedCount() const noexcept
    {
        return rejected_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint32_t kWriterBit = 0x80000000U;
    static constexpr std::uint32_t kReaderMask = 0x7fffffffU;

    struct Slot {
        std::atomic<std::uint32_t> ownership{0};
        std::atomic<std::uint64_t> sequenceHint{0};
        RealtimeFrameV1 frame{};
    };

    bool copySlot(
        std::size_t index,
        std::uint64_t sequence,
        RealtimeFrameV1& output) noexcept
    {
        auto& slot = slots_[index];
        auto ownership = slot.ownership.load(std::memory_order_acquire);
        for (;;) {
            if ((ownership & kWriterBit) != 0 ||
                (ownership & kReaderMask) == kReaderMask) return false;
            if (slot.ownership.compare_exchange_weak(
                    ownership, ownership + 1U,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) break;
        }
        if (slot.sequenceHint.load(std::memory_order_acquire) != sequence) {
            slot.ownership.fetch_sub(1U, std::memory_order_release);
            return false;
        }
        const auto& frame = slot.frame;
        output.version = frame.version;
        output.sequence = frame.sequence;
        output.streamGeneration = frame.streamGeneration;
        output.playbackSerial = frame.playbackSerial;
        output.recordingIdentityHash = frame.recordingIdentityHash;
        output.firstFrameIndex = frame.firstFrameIndex;
        output.qpcTicks = frame.qpcTicks;
        output.playbackSeconds = frame.playbackSeconds;
        output.sampleRate = frame.sampleRate;
        output.channels = frame.channels;
        output.flags = frame.flags;
        output.discontinuityReasonMask = frame.discontinuityReasonMask;
        output.frameCount = frame.frameCount;
        output.sampleCount = frame.sampleCount;
        if (output.sampleCount <= output.samples.size()) {
            std::copy_n(frame.samples.begin(), output.sampleCount,
                output.samples.begin());
        }
        slot.ownership.fetch_sub(1U, std::memory_order_release);
        return output.sequence == sequence;
    }

    static void reportConsumption(
        std::uint64_t sequence,
        std::uint64_t& lastSequence,
        std::uint64_t* skipped) noexcept
    {
        const auto missed = sequence > lastSequence
            ? sequence - lastSequence - 1
            : 0;
        lastSequence = sequence;
        if (skipped != nullptr) *skipped = missed;
    }

    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> nextSequence_{0};
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

} // namespace nekorox
