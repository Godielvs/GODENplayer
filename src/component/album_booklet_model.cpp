#include "album_booklet_model.h"

#include <algorithm>

namespace nekorox::component {

AlbumBookletLayout computeAlbumBookletLayout(float width, float height,
    AlbumPanelVisibility visibility) noexcept
{
    width = std::max(0.0f, width);
    height = std::max(0.0f, height);
    AlbumBookletLayout out;
    out.backdrop = D2D1::RectF(0, 0, width, height);
    const auto designed = goden::c11::layoutAlbum(
        width, height, visibility, true);
    const auto native = [](const goden::c11::Rect& value) noexcept {
        return D2D1::RectF(value.l, value.t, value.r, value.b);
    };
    out.artwork = native(designed.art);
    out.albumDetails = native(designed.tracks);
    out.writtenDetails = native(designed.lyrics);
    out.close = native(designed.close);
    out.tracksToggle = native(designed.tracksToggle);
    out.lyricsToggle = native(designed.lyricsToggle);
    out.panelsMenu = native(designed.panelsMenu);
    out.visualMenu = native(designed.preset);
    out.narrow = width < 980.0f;
    return out;
}

float AlbumBookletModel::sampleRaw(Clock::time_point now) const noexcept
{
    if (duration_.count() <= 0) return open_ ? 1.0f : 0.0f;
    const auto elapsed = std::chrono::duration<double>(now - started_).count();
    const auto t = std::clamp(static_cast<float>(elapsed /
        std::chrono::duration<double>(duration_).count()), 0.0f, 1.0f);
    const auto smooth = t * t * (3.0f - 2.0f * t);
    const auto destination = open_ ? 1.0f : 0.0f;
    return start_ + (destination - start_) * smooth;
}

float AlbumBookletModel::sample(Clock::time_point now) noexcept
{
    presented_ = sampleRaw(now);
    return presented_;
}

void AlbumBookletModel::setOpen(bool open, Clock::time_point now,
    std::chrono::milliseconds duration) noexcept
{
    presented_ = sampleRaw(now);
    start_ = presented_;
    open_ = open;
    started_ = now;
    duration_ = duration;
    if (duration.count() == 0) presented_ = open ? 1.0f : 0.0f;
}

bool AlbumBookletModel::active(Clock::time_point now) const noexcept
{
    const auto value = sampleRaw(now);
    return open_ ? value < 0.999f : value > 0.001f;
}

void AlbumBookletModel::closeImmediately() noexcept
{
    open_ = false;
    presented_ = 0.0f;
    start_ = 0.0f;
    duration_ = std::chrono::milliseconds(0);
}

} // namespace nekorox::component
