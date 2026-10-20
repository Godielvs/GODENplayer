#pragma once

#include <windows.h>

#include <cstdint>

namespace nekorox::component {

enum class SurfaceBackend : unsigned char {
    hardwareDxgi,
    warpDxgi,
    gdiFallback,
    offscreenSoftware,
};

enum class FrameResult : unsigned char {
    accepted,
    notReady,
    occluded,
    deviceLost,
    failed,
};

struct SurfaceBindingKey {
    std::uint64_t lifetime = 0;
    std::uint64_t swapchainEpoch = 0;
    HANDLE source = nullptr;

    [[nodiscard]] bool operator==(const SurfaceBindingKey&) const = default;
    [[nodiscard]] bool valid() const noexcept
    {
        return lifetime != 0 && swapchainEpoch != 0 && source != nullptr;
    }
};

struct FrameTicket {
    SurfaceBindingKey binding{};
    std::uint64_t geometryRevision = 0;
    std::uint64_t contentRevision = 0;

    [[nodiscard]] bool operator==(const FrameTicket&) const = default;
};

struct FrameOutcome {
    FrameResult state = FrameResult::failed;
    HRESULT drawHr = E_PENDING;
    HRESULT presentHr = E_PENDING;
    std::uint64_t resourceEpoch = 0;
    std::uint64_t sizeRevision = 0;
    bool backingValid = false;

    [[nodiscard]] bool accepted() const noexcept
    {
        return state == FrameResult::accepted;
    }
};

struct PreparedFrame {
    std::uint64_t revision = 0;
    std::uint64_t deviceEpoch = 0;

    [[nodiscard]] bool operator==(const PreparedFrame&) const = default;
};

} // namespace nekorox::component
