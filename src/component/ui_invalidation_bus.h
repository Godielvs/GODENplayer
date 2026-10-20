#pragma once

#include <cstdint>

#include <windows.h>

namespace nekorox::component {

enum class UiInvalidationSurface : std::uint8_t {
    nativePanels = 0,
    workspace = 1,
    waveSeek = 2,
    listen = 3,
    masterDsp = 4,
    count,
};

enum class UiDirty : std::uint32_t {
    none = 0,
    semantic = 1U << 0U,
    position = 1U << 1U,
    analysis = 1U << 2U,
    artwork = 1U << 3U,
    theme = 1U << 4U,
    input = 1U << 5U,
    volume = 1U << 6U,
};

[[nodiscard]] constexpr UiDirty operator|(UiDirty left, UiDirty right) noexcept
{
    return static_cast<UiDirty>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr UiDirty operator&(UiDirty left, UiDirty right) noexcept
{
    return static_cast<UiDirty>(
        static_cast<std::uint32_t>(left) &
        static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool any(UiDirty value) noexcept
{
    return value != UiDirty::none;
}

// A process-wide post-once invalidation router. Window membership changes are
// rare and publish an immutable registry snapshot. Hot update paths only load
// that snapshot, OR dirty bits and post when a window transitions from clean
// to dirty; they never copy HWND vectors or post while holding a mutex.
class UiInvalidationBus final {
public:
    static UiInvalidationBus& instance();

    void registerWindow(
        UiInvalidationSurface surface,
        HWND window,
        UINT message);
    void unregisterWindow(
        UiInvalidationSurface surface,
        HWND window) noexcept;

    void publish(
        UiInvalidationSurface surface,
        UiDirty dirty) noexcept;

    [[nodiscard]] UiDirty consume(
        UiInvalidationSurface surface,
        HWND window) noexcept;

private:
    UiInvalidationBus();
    ~UiInvalidationBus();
    UiInvalidationBus(const UiInvalidationBus&) = delete;
    UiInvalidationBus& operator=(const UiInvalidationBus&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace nekorox::component
