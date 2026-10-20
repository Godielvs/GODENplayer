#pragma once

#include <windows.h>

namespace nekorox::component {

// Window-local identity: an asynchronous WinEvent can update the shared
// display cache before the owning window processes WM_WINDOWPOSCHANGED.
class WindowMonitorIdentity final {
public:
    bool update(HMONITOR monitor, UINT dpi) noexcept
    {
        if (monitor_ == monitor && dpi_ == dpi) return false;
        monitor_ = monitor;
        dpi_ = dpi;
        return true;
    }
    void reset() noexcept { monitor_ = nullptr; dpi_ = 0; }
private:
    HMONITOR monitor_ = nullptr;
    UINT dpi_ = 0;
};

} // namespace nekorox::component
