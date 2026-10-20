#pragma once

#include "render_surface_contracts.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <thread>
#include <vector>

namespace nekorox::component {

struct DeviceLease {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext;
    SurfaceBackend backend = SurfaceBackend::gdiFallback;
    std::uint64_t epoch = 0;
    std::uint64_t topologyGeneration = 0;
    LUID adapterLuid{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return device != nullptr && immediateContext != nullptr;
    }
};

// Process-wide immutable graphics factories. Render targets, contexts and
// swap chains remain surface-owned; no render mutex is introduced.
class RenderDeviceHub final {
public:
    static RenderDeviceHub& instance();
    ~RenderDeviceHub();

    ID2D1Factory1* d2dFactory() noexcept;
    IDWriteFactory* writeFactory() noexcept;

    // UI-safe cache probe. A miss schedules device creation on the recovery
    // worker and returns immediately; it never calls D3D11CreateDevice.
    [[nodiscard]] bool tryAcquireExisting(HWND window, DeviceLease& lease) noexcept;

    // Compatibility wrapper for legacy surfaces. It is now a cache-only
    // probe: a miss schedules recovery and returns false; it never creates a
    // D3D device on the calling/UI thread.
    bool acquire(
        HWND window,
        Microsoft::WRL::ComPtr<ID3D11Device>& device,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>& immediateContext,
        bool& softwareRasterizer) noexcept;

    // Returns whether an existing hardware device belongs to the adapter that
    // currently presents `window`. WARP is compatible with every monitor and
    // is replaced through the normal recovery epoch once hardware is stable.
    [[nodiscard]] bool isDeviceCompatibleWithWindow(
        ID3D11Device* device,
        HWND window) noexcept;

    void invalidate(ID3D11Device* device) noexcept;
    // Cache-only notification; does not initialize factories or create devices.
    // A conclusively absent/unsupported adapter sleeps until this advances.
    void notifyTopologyChanged(std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t recoveryEpoch() const noexcept;

private:
    RenderDeviceHub() = default;
#if defined(NEKOROX_RENDER_HUB_TEST)
    friend struct RenderDeviceHubTestPeer;
#endif
    void initialize() noexcept;
    void recoveryLoop(std::stop_token stopToken) noexcept;
    void requestRecovery(const LUID& adapterLuid, std::uint64_t topology) noexcept;
    void pruneUnavailableDevices() noexcept; // devicesMutex_ must be held

    std::once_flag initialization_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    struct DeviceEntry {
        LUID adapterLuid{};
        bool softwareRasterizer = false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
    };
    struct RecoveryEntry {
        LUID adapterLuid{};
        std::uint32_t attempt = 0;
        std::chrono::steady_clock::time_point due{};
        bool active = false;
        // Complete absence or DXGI_ERROR_UNSUPPORTED; transient failures must
        // never enter this negative hardware cache.
        std::uint64_t missingAtTopology = 0;
    };
    struct CompatibilityEntry {
        ID3D11Device* device = nullptr;
        HWND root = nullptr;
        std::uint64_t topologyGeneration = 0;
        std::uint64_t rootLifetimeToken = 0;
        std::uint64_t windowPresentationRevision = 0;
        bool compatible = false;
    };
    std::mutex devicesMutex_;
    std::condition_variable_any recoveryChanged_;
    std::vector<DeviceEntry> devices_;
    std::vector<RecoveryEntry> recoveries_;
    std::vector<CompatibilityEntry> compatibility_;
    std::uint64_t topologyGeneration_ = 0; // protected by devicesMutex_
    std::jthread recoveryWorker_;
    std::atomic<std::uint64_t> recoveryEpoch_{1};
};

} // namespace nekorox::component
