#pragma once
#include "curated_core.h"
#include <windows.h>
namespace nekorox::component::curated {
Response response() noexcept;
float causticsSpeed() noexcept;
Mode mode() noexcept;
Palette palette() noexcept;
std::uint64_t restartRevision() noexcept;
bool setMode(Mode) noexcept;
bool setPalette(Palette) noexcept;
void restart() noexcept;
void appendMenu(HMENU,bool selected) noexcept;
bool handleMenu(UINT command) noexcept;
}
