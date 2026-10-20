#pragma once

#include <array>
#include <cstdint>

#include <windows.h>

class goden_daw_visual_source_t final {
public:
    ~goden_daw_visual_source_t()
    {
        if (_Shared != nullptr) ::UnmapViewOfFile(_Shared);
        if (_Mapping != nullptr) ::CloseHandle(_Mapping);
        if (_Wake != nullptr) ::CloseHandle(_Wake);
    }

    HANDLE WakeHandle() noexcept
    {
        Ensure();
        return _Wake;
    }

    bool IsActive() noexcept
    {
        Ensure();
        return _Shared != nullptr && _Shared->Magic == Magic &&
            _Shared->Version == 2U &&
            _Shared->SlotCountValue == SlotCount &&
            _Shared->MaximumFramesValue == MaximumFrames &&
            ::InterlockedCompareExchange(&_Shared->Active, 0, 0) != 0;
    }

    bool ReadLatest(audio_chunk_impl& chunk, double& position) noexcept
    {
        if (!IsActive()) return false;
        const auto activeGeneration = static_cast<std::uint64_t>(
            ::InterlockedCompareExchange64(
                &_Shared->Generation, 0, 0));
        const auto latest = static_cast<std::uint64_t>(
            ::InterlockedCompareExchange64(
                &_Shared->LatestSequence, 0, 0));
        if (latest == 0 || latest == _Cursor) return false;
        const auto& slot = _Shared->Slots[latest % SlotCount];
        const auto before = static_cast<std::uint64_t>(
            ::InterlockedCompareExchange64(
                const_cast<volatile LONG64*>(&slot.Sequence), 0, 0));
        if (before != latest || slot.Generation != activeGeneration ||
            slot.Channels != 2U ||
            slot.FrameCount == 0 || slot.FrameCount > MaximumFrames ||
            slot.SampleRate == 0) return false;

        const auto sampleCount = static_cast<std::size_t>(
            slot.FrameCount) * slot.Channels;
        chunk.set_data_size(sampleCount);
        auto* output = chunk.get_data();
        for (std::size_t index = 0; index < sampleCount; ++index)
            output[index] = static_cast<audio_sample>(slot.Samples[index]);
        chunk.set_sample_count(slot.FrameCount);
        chunk.set_srate(slot.SampleRate);
        chunk.set_channels(slot.Channels);
        position = static_cast<double>(slot.PositionFrames) / slot.SampleRate;
        ::MemoryBarrier();
        const auto after = static_cast<std::uint64_t>(
            ::InterlockedCompareExchange64(
                const_cast<volatile LONG64*>(&slot.Sequence), 0, 0));
        if (after != before) return false;
        _Cursor = latest;
        return true;
    }

private:
    static constexpr wchar_t MappingName[] =
        L"Local\\GodenPlayer.DawPreview.Visual.v2";
    static constexpr wchar_t WakeName[] =
        L"Local\\GodenPlayer.DawPreview.Wake.v2";
    static constexpr std::uint32_t Magic = 0x32564447U;
    static constexpr std::size_t SlotCount = 8U;
    static constexpr std::size_t MaximumFrames = 2048U;

    struct alignas(64) Slot {
        volatile LONG64 Sequence = 0;
        std::uint64_t Generation = 0;
        std::uint64_t PositionFrames = 0;
        std::uint32_t SampleRate = 0;
        std::uint32_t Channels = 0;
        std::uint32_t FrameCount = 0;
        std::uint32_t Reserved = 0;
        std::array<float, MaximumFrames * 2U> Samples{};
    };
    struct alignas(64) SharedState {
        std::uint32_t Magic = 0;
        std::uint32_t Version = 0;
        std::uint32_t SlotCountValue = 0;
        std::uint32_t MaximumFramesValue = 0;
        std::uint32_t OwnerPid = 0;
        std::uint32_t Reserved = 0;
        std::uint64_t OwnerStartupEpoch = 0;
        volatile LONG Active = 0;
        volatile LONG64 Generation = 0;
        volatile LONG64 LatestSequence = 0;
        std::array<Slot, SlotCount> Slots{};
    };

    void Ensure() noexcept
    {
        // The Spectrum render thread may start before GODEN Core constructs
        // the producer. Creating the named event from either side guarantees
        // that a later DAW begin() can wake an already-sleeping consumer.
        // OpenEvent alone made DAW visualisation depend on component startup
        // order and was the source of the intermittent blank Spectrum.
        if (_Wake == nullptr)
            _Wake = ::CreateEventW(nullptr, TRUE, FALSE, WakeName);
        if (_Shared != nullptr) return;
        // InterlockedCompareExchange is used as an atomic acquire-load below.
        // Windows still implements that operation as a write-capable locked
        // instruction even when the comparand and replacement are zero, so a
        // FILE_MAP_READ view faults inside IsActive(). The producer creates a
        // private, read/write page-file mapping in this user session; give the
        // in-process Spectrum consumer a write-capable view as required by the
        // Interlocked contract.
        if (_Mapping == nullptr)
            _Mapping = ::OpenFileMappingW(
                FILE_MAP_READ | FILE_MAP_WRITE, FALSE, MappingName);
        if (_Mapping != nullptr)
            _Shared = static_cast<SharedState*>(::MapViewOfFile(
                _Mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                0, 0, sizeof(SharedState)));
    }

    HANDLE _Mapping = nullptr;
    HANDLE _Wake = nullptr;
    SharedState* _Shared = nullptr;
    std::uint64_t _Cursor = 0;
};
