#include "daw_visual_bridge.h"

#include <algorithm>
#include <cstring>

namespace nekorox::component {

struct alignas(64) DawVisualBridge::SharedState {
    struct alignas(64) Slot {
        volatile LONG64 sequence = 0;
        std::uint64_t generation = 0;
        std::uint64_t positionFrames = 0;
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::uint32_t frameCount = 0;
        std::uint32_t reserved = 0;
        std::array<float,
            kDawVisualMaximumFrames * kDawVisualMaximumChannels> samples{};
    };

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t slotCount = 0;
    std::uint32_t maximumFrames = 0;
    std::uint32_t ownerPid = 0;
    std::uint32_t reserved = 0;
    std::uint64_t ownerStartupEpoch = 0;
    volatile LONG active = 0;
    volatile LONG64 generation = 0;
    volatile LONG64 latestSequence = 0;
    std::array<Slot, kDawVisualSlotCount> slots{};
};

DawVisualBridge& DawVisualBridge::instance()
{
    static DawVisualBridge bridge;
    return bridge;
}

DawVisualBridge::DawVisualBridge() noexcept
{
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    startupEpoch_ = static_cast<std::uint64_t>(qpc.QuadPart) ^
        (static_cast<std::uint64_t>(GetTickCount64()) << 17U) ^
        static_cast<std::uint64_t>(GetCurrentProcessId());
    if (startupEpoch_ == 0) startupEpoch_ = 1;
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedState)), kDawVisualMappingName);
    if (mapping_ == nullptr) return;
    const auto existed = GetLastError() == ERROR_ALREADY_EXISTS;
    shared_ = static_cast<SharedState*>(MapViewOfFile(
        mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (shared_ == nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return;
    }
    if (!existed || shared_->magic != kDawVisualMagic ||
            shared_->version != 2U) {
        std::memset(shared_, 0, sizeof(*shared_));
        shared_->magic = kDawVisualMagic;
        shared_->version = 2U;
        shared_->slotCount = static_cast<std::uint32_t>(
            kDawVisualSlotCount);
        shared_->maximumFrames = static_cast<std::uint32_t>(
            kDawVisualMaximumFrames);
    }
    wakeEvent_ = CreateEventW(
        nullptr, TRUE, FALSE, kDawVisualWakeEventName);
}

DawVisualBridge::~DawVisualBridge()
{
    // Session owners release activity through end(token). A process that only
    // observed the shared mapping must not stop another process's preview.
    if (shared_ != nullptr) UnmapViewOfFile(shared_);
    if (wakeEvent_ != nullptr) CloseHandle(wakeEvent_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
}

DawOperationTokenV2 DawVisualBridge::begin() noexcept
{
    DawOperationTokenV2 token{};
    if (shared_ == nullptr) return token;
    token.ownerPid = GetCurrentProcessId();
    token.startupEpoch = startupEpoch_;
    token.generation = static_cast<std::uint64_t>(
        InterlockedIncrement64(&shared_->generation));
    // The operation sequence is intentionally carried outside the shared
    // layout so V2 readers keep their stable offsets. Generation is already a
    // monotonic, process-owned operation sequence.
    token.sequence = token.generation;
    shared_->ownerPid = token.ownerPid;
    shared_->ownerStartupEpoch = token.startupEpoch;
    MemoryBarrier();
    InterlockedExchange(&shared_->active, 1);
    if (wakeEvent_ != nullptr) SetEvent(wakeEvent_);
    return token;
}

void DawVisualBridge::end(const DawOperationTokenV2& token) noexcept
{
    if (shared_ == nullptr || !token) return;
    const auto current = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(&shared_->generation, 0, 0));
    if (shared_->ownerPid != token.ownerPid ||
            shared_->ownerStartupEpoch != token.startupEpoch ||
            current != token.generation) {
        return;
    }
    InterlockedExchange(&shared_->active, 0);
    if (wakeEvent_ != nullptr) ResetEvent(wakeEvent_);
}

void DawVisualBridge::publish(
    const DawOperationTokenV2& token,
    std::span<const float> interleaved,
    std::uint32_t frameCount,
    std::uint32_t sampleRate,
    std::uint64_t positionFrames) noexcept
{
    if (shared_ == nullptr || !token || frameCount == 0 || sampleRate == 0)
        return;
    const auto currentGeneration = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(&shared_->generation, 0, 0));
    if (InterlockedCompareExchange(&shared_->active, 0, 0) == 0 ||
            shared_->ownerPid != token.ownerPid ||
            shared_->ownerStartupEpoch != token.startupEpoch ||
            currentGeneration != token.generation) {
        return;
    }
    frameCount = std::min<std::uint32_t>(frameCount,
        static_cast<std::uint32_t>(kDawVisualMaximumFrames));
    const auto sampleCount = static_cast<std::size_t>(frameCount) * 2U;
    if (interleaved.size() < sampleCount) return;

    const auto sequence = static_cast<std::uint64_t>(
        InterlockedIncrement64(&shared_->latestSequence));
    auto& slot = shared_->slots[sequence % kDawVisualSlotCount];
    InterlockedExchange64(&slot.sequence, 0);
    slot.generation = token.generation;
    slot.positionFrames = positionFrames;
    slot.sampleRate = sampleRate;
    slot.channels = 2U;
    slot.frameCount = frameCount;
    std::memcpy(slot.samples.data(), interleaved.data(),
        sampleCount * sizeof(float));
    MemoryBarrier();
    if (InterlockedCompareExchange(&shared_->active, 0, 0) == 0 ||
            shared_->ownerPid != token.ownerPid ||
            shared_->ownerStartupEpoch != token.startupEpoch ||
            static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &shared_->generation, 0, 0)) != token.generation) {
        return;
    }
    InterlockedExchange64(
        &slot.sequence, static_cast<LONG64>(sequence));
}

bool DawVisualBridge::active() const noexcept
{
    return shared_ != nullptr && shared_->magic == kDawVisualMagic &&
        InterlockedCompareExchange(&shared_->active, 0, 0) != 0;
}

bool DawVisualBridge::readLatest(
    std::uint64_t& cursor,
    DawVisualChunk& output) const noexcept
{
    if (!active()) return false;
    const auto activeGeneration = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(&shared_->generation, 0, 0));
    const auto latest = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(&shared_->latestSequence, 0, 0));
    if (latest == 0 || latest == cursor) return false;
    const auto& slot = shared_->slots[latest % kDawVisualSlotCount];
    const auto before = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&slot.sequence), 0, 0));
    if (before != latest || slot.generation != activeGeneration ||
            slot.channels != 2U || slot.frameCount == 0 ||
            slot.frameCount > kDawVisualMaximumFrames) {
        return false;
    }
    output.sequence = latest;
    output.generation = slot.generation;
    output.positionFrames = slot.positionFrames;
    output.sampleRate = slot.sampleRate;
    output.channels = slot.channels;
    output.frameCount = slot.frameCount;
    output.discontinuity = cursor != 0 && latest != cursor + 1U;
    std::memcpy(output.samples.data(), slot.samples.data(),
        static_cast<std::size_t>(slot.frameCount) * slot.channels *
            sizeof(float));
    MemoryBarrier();
    const auto after = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&slot.sequence), 0, 0));
    if (after != before) return false;
    cursor = latest;
    return true;
}

} // namespace nekorox::component
