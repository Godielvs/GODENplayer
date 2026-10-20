#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <windows.h>

namespace nekorox::component {

inline constexpr wchar_t kDawVisualMappingName[] =
    L"Local\\GodenPlayer.DawPreview.Visual.v2";
inline constexpr wchar_t kDawVisualWakeEventName[] =
    L"Local\\GodenPlayer.DawPreview.Wake.v2";
inline constexpr std::uint32_t kDawVisualMagic = 0x32564447U;
inline constexpr std::size_t kDawVisualSlotCount = 8U;
inline constexpr std::size_t kDawVisualMaximumFrames = 2048U;
inline constexpr std::size_t kDawVisualMaximumChannels = 2U;

struct DawVisualChunk {
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    std::uint64_t positionFrames = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t frameCount = 0;
    bool discontinuity = false;
    std::array<float,
        kDawVisualMaximumFrames * kDawVisualMaximumChannels> samples{};
};

struct DawOperationTokenV2 {
    std::uint32_t ownerPid = 0;
    std::uint64_t startupEpoch = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return ownerPid != 0 && startupEpoch != 0 && generation != 0 &&
            sequence != 0;
    }
};

class DawVisualBridge final {
public:
    static DawVisualBridge& instance();

    [[nodiscard]] DawOperationTokenV2 begin() noexcept;
    void end(const DawOperationTokenV2& token) noexcept;
    void publish(
        const DawOperationTokenV2& token,
        std::span<const float> interleaved,
        std::uint32_t frameCount,
        std::uint32_t sampleRate,
        std::uint64_t positionFrames) noexcept;
    [[nodiscard]] bool active() const noexcept;
    bool readLatest(
        std::uint64_t& cursor,
        DawVisualChunk& output) const noexcept;

private:
    DawVisualBridge() noexcept;
    ~DawVisualBridge();
    DawVisualBridge(const DawVisualBridge&) = delete;
    DawVisualBridge& operator=(const DawVisualBridge&) = delete;

    struct SharedState;
    HANDLE mapping_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    SharedState* shared_ = nullptr;
    std::uint64_t startupEpoch_ = 0;
};

} // namespace nekorox::component
