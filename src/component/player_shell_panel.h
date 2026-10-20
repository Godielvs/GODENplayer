#pragma once

#include "sdk_compat.h"

namespace nekorox::component {

enum class PlayerShellMode : unsigned {
    normal = 0,
    album = 1,
};

[[nodiscard]] const GUID& playerShellPanelGuid() noexcept;
[[nodiscard]] UINT playerShellModeMessage() noexcept;
[[nodiscard]] PlayerShellMode playerShellMode() noexcept;
[[nodiscard]] bool requestPlayerShellMode(PlayerShellMode mode) noexcept;
void setPlayerShellMode(PlayerShellMode mode) noexcept;
void notifyPlayerShellsChanged() noexcept;

} // namespace nekorox::component
