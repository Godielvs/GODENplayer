#pragma once
// Put include/ and integration/ on the include path, or adjust paths on copy.
#include "goden_pixel_assets.generated.hpp"
#include "ui_design_system.h"
namespace nekorox::component {
[[nodiscard]] constexpr goden::pixel::IconId pixelIconFor(UiGlyph glyph) noexcept {
    using I=goden::pixel::IconId;
    switch(glyph) {
    case UiGlyph::stop:return I::Stop;
    case UiGlyph::previous:return I::Previous;
    case UiGlyph::play:return I::Play;
    case UiGlyph::pause:return I::Pause;
    case UiGlyph::next:return I::Next;
    case UiGlyph::repeat:return I::Repeat;
    case UiGlyph::shuffle:return I::Shuffle;
    case UiGlyph::keepPlaying:return I::KeepPlaying;
    case UiGlyph::volumeMuted:return I::VolumeMuted;
    case UiGlyph::volumeLow:return I::VolumeLow;
    case UiGlyph::volumeMedium:return I::VolumeMedium;
    case UiGlyph::volumeHigh:return I::VolumeHigh;
    case UiGlyph::windowMinimize:return I::WindowMinimize;
    case UiGlyph::windowMaximize:return I::WindowMaximize;
    case UiGlyph::windowRestore:return I::WindowRestore;
    case UiGlyph::windowClose:return I::WindowClose;
    case UiGlyph::listen:return I::Listen;
    case UiGlyph::nowPlaying:return I::NowPlaying;
    case UiGlyph::queue:return I::Queue;
    case UiGlyph::favoriteOutline:return I::FavoriteOutline;
    case UiGlyph::favoriteFilled:return I::FavoriteFilled;
    // Existing collapse/expand mean fold up/down, NOT fullscreen focus.
    case UiGlyph::collapse:return I::ChevronUp;
    case UiGlyph::expand:return I::ChevronDown;
    case UiGlyph::menu:return I::Menu;
    case UiGlyph::menuCheck:return I::MenuCheck;
    case UiGlyph::menuSubmenu:return I::MenuSubmenu;
    }
    return I::Count; // missing enum value must be caught in qualification
}
} // namespace nekorox::component
