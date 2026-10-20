#pragma once

#include "album_mode_view.h"

#include <d2d1.h>

#include <chrono>

namespace nekorox::component {

struct AlbumBookletLayout {
    D2D1_RECT_F backdrop{};
    D2D1_RECT_F artwork{};
    D2D1_RECT_F albumDetails{};
    D2D1_RECT_F writtenDetails{};
    D2D1_RECT_F close{};
    D2D1_RECT_F tracksToggle{};
    D2D1_RECT_F lyricsToggle{};
    D2D1_RECT_F panelsMenu{};
    D2D1_RECT_F visualMenu{};
    bool narrow = false;
};

[[nodiscard]] AlbumBookletLayout computeAlbumBookletLayout(
    float widthDip, float heightDip,
    AlbumPanelVisibility visibility = {}) noexcept;

class AlbumBookletModel final {
public:
    using Clock = std::chrono::steady_clock;

    void setOpen(bool open, Clock::time_point now,
        std::chrono::milliseconds duration) noexcept;
    [[nodiscard]] float sample(Clock::time_point now) noexcept;
    [[nodiscard]] bool openDestination() const noexcept { return open_; }
    [[nodiscard]] bool active(Clock::time_point now) const noexcept;
    void closeImmediately() noexcept;

private:
    float sampleRaw(Clock::time_point now) const noexcept;

    bool open_ = false;
    float presented_ = 0.0f;
    float start_ = 0.0f;
    Clock::time_point started_{};
    std::chrono::milliseconds duration_{0};
};

} // namespace nekorox::component
