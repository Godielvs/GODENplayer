#pragma once

#include <d3d11.h>
#include <d2d1.h>

namespace nekorox::component {

inline void invalidateGraphicsRecovery(HWND window) noexcept
{
    // Product roots clip their children. Invalidating only the root can leave
    // a paused child on its failed/software target until input arrives. Queue
    // paints for the subtree without running UI drawing on the recovery worker.
    if (window != nullptr) {
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

inline bool dxgiDeviceLost(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_HUNG ||
        result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

inline bool confirmedDeviceLost(ID3D11Device* device, HRESULT result) noexcept
{
    return dxgiDeviceLost(result) ||
        (result == D2DERR_RECREATE_TARGET && device != nullptr &&
            FAILED(device->GetDeviceRemovedReason()));
}

} // namespace nekorox::component
