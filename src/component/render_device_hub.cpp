#include "render_device_hub.h"
#include "device_failure.h"
#include "display_environment.h"

#include <dxgi1_2.h>

#include <array>
#include <algorithm>
#include <chrono>

namespace nekorox::component {
#if defined(NEKOROX_RENDER_HUB_TEST)
// Standalone worker probe only: production still uses DXGI/D3D directly.
namespace testing {
HRESULT enumerateAdapter(IDXGIFactory1*, UINT, IDXGIAdapter1**) noexcept;
HRESULT describeAdapter(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*) noexcept;
bool allowWarpCreation() noexcept;
HRESULT hardwareCreationFailure() noexcept;
}
#endif
namespace {

bool sameLuid(const LUID& left, const LUID& right) noexcept
{
    return left.HighPart == right.HighPart &&
        left.LowPart == right.LowPart;
}

enum class AdapterLookupStatus { found, missing, failed };
struct AdapterLookup {
    AdapterLookupStatus status = AdapterLookupStatus::failed;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
};

AdapterLookup adapterForLuid(const LUID& wanted) noexcept
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()))) ||
            !factory) return {};
    bool incompleteEnumeration = false;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
#if defined(NEKOROX_RENDER_HUB_TEST)
        const auto result = testing::enumerateAdapter(
            factory.Get(), index, adapter.ReleaseAndGetAddressOf());
#else
        const auto result = factory->EnumAdapters1(
            index, adapter.ReleaseAndGetAddressOf());
#endif
        if (result == DXGI_ERROR_NOT_FOUND)
            return {incompleteEnumeration ? AdapterLookupStatus::failed :
                AdapterLookupStatus::missing, {}};
        // Only a complete enumeration proves absence. A transient DXGI error
        // must neither dereference a null adapter nor poison the missing cache.
        if (FAILED(result) || !adapter) return {};
        DXGI_ADAPTER_DESC1 description{};
#if defined(NEKOROX_RENDER_HUB_TEST)
        const auto described = testing::describeAdapter(adapter.Get(), &description);
#else
        const auto described = adapter->GetDesc1(&description);
#endif
        if (FAILED(described)) {
            // Another adapter may still be usable. Keep searching, but never
            // treat this incomplete scan as evidence that our LUID is absent.
            incompleteEnumeration = true;
            continue;
        }
        if (sameLuid(description.AdapterLuid, wanted))
            return {AdapterLookupStatus::found, std::move(adapter)};
    }
}

HRESULT createHardwareDevice(IDXGIAdapter1* adapter,
    Microsoft::WRL::ComPtr<ID3D11Device>& device) noexcept
{
    if (adapter == nullptr) return E_INVALIDARG;
#if defined(NEKOROX_RENDER_HUB_TEST)
    const auto injected = testing::hardwareCreationFailure();
    if (FAILED(injected)) return injected;
#endif
    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    auto result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
        device.ReleaseAndGetAddressOf(), &created,
        context.ReleaseAndGetAddressOf());
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data() + 1, 1,
            D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), &created,
            context.ReleaseAndGetAddressOf());
    }
    if (FAILED(result)) return result;
    return device ? device->GetDeviceRemovedReason() : E_FAIL;
}

bool createWarpDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device) noexcept
{
#if defined(NEKOROX_RENDER_HUB_TEST)
    if (!testing::allowWarpCreation()) return false;
#endif
    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
        device.ReleaseAndGetAddressOf(), &created,
        context.ReleaseAndGetAddressOf());
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data() + 1, 1,
            D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), &created,
            context.ReleaseAndGetAddressOf());
    }
    return SUCCEEDED(result) && device &&
        SUCCEEDED(device->GetDeviceRemovedReason());
}

BOOL CALLBACK invalidateOwnedWindow(HWND window, LPARAM) noexcept
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId()) invalidateGraphicsRecovery(window);
    return TRUE;
}

} // namespace

RenderDeviceHub& RenderDeviceHub::instance()
{
    static RenderDeviceHub hub;
    return hub;
}

RenderDeviceHub::~RenderDeviceHub()
{
    if (recoveryWorker_.joinable()) {
        recoveryWorker_.request_stop();
        recoveryChanged_.notify_all();
        recoveryWorker_.join();
    }
}

void RenderDeviceHub::initialize() noexcept
{
    D2D1_FACTORY_OPTIONS options{};
    D2D1CreateFactory(
        D2D1_FACTORY_TYPE_MULTI_THREADED,
        __uuidof(ID2D1Factory1),
        &options,
        reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf()));
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
    try {
        recoveryWorker_ = std::jthread(
            [this](std::stop_token token) { recoveryLoop(token); });
    } catch (...) {
        // Factories and WARP remain usable. Recovery is optional and a failed
        // thread creation must not terminate this noexcept initialization.
    }
}

ID2D1Factory1* RenderDeviceHub::d2dFactory() noexcept
{
    std::call_once(initialization_, [this] { initialize(); });
    return d2dFactory_.Get();
}

IDWriteFactory* RenderDeviceHub::writeFactory() noexcept
{
    std::call_once(initialization_, [this] { initialize(); });
    return writeFactory_.Get();
}

bool RenderDeviceHub::tryAcquireExisting(HWND window, DeviceLease& lease) noexcept
{
    std::call_once(initialization_, [this] { initialize(); });
    lease = {};
    const auto display = DisplayEnvironmentService::instance().snapshot(window);
    const auto luid = display.adapterLuid;

    bool request = false;
    bool available = false;
    {
        std::lock_guard lock(devicesMutex_);
        pruneUnavailableDevices();
        auto found = std::find_if(devices_.begin(), devices_.end(),
            [&](const DeviceEntry& entry) {
                return !entry.softwareRasterizer &&
                    sameLuid(entry.adapterLuid, luid);
            });
        if (found == devices_.end()) {
            found = std::find_if(devices_.begin(), devices_.end(),
                [](const DeviceEntry& entry) {
                    return entry.softwareRasterizer;
                });
        }
        if (found != devices_.end()) {
            lease.device = found->device;
            lease.device->GetImmediateContext(
                lease.immediateContext.ReleaseAndGetAddressOf());
            lease.backend = found->softwareRasterizer
                ? SurfaceBackend::warpDxgi : SurfaceBackend::hardwareDxgi;
            lease.epoch = recoveryEpoch_.load(std::memory_order_acquire);
            lease.adapterLuid = found->adapterLuid;
            lease.topologyGeneration = display.topologyGeneration;
            available = static_cast<bool>(lease);
        }
        const auto hasHardware = std::any_of(
            devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
                return !entry.softwareRasterizer &&
                    sameLuid(entry.adapterLuid, luid);
            });
        request = !hasHardware &&
            !std::any_of(recoveries_.begin(), recoveries_.end(),
            [&](const RecoveryEntry& entry) {
                return entry.active && sameLuid(entry.adapterLuid, luid);
            });
    }
    if (request) requestRecovery(luid, display.topologyGeneration);
    return available;
}

bool RenderDeviceHub::acquire(
    HWND window,
    Microsoft::WRL::ComPtr<ID3D11Device>& device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>& immediateContext,
    bool& softwareRasterizer) noexcept
{
    DeviceLease lease;
    if (!tryAcquireExisting(window, lease)) {
        device.Reset();
        immediateContext.Reset();
        softwareRasterizer = false;
        return false;
    }
    device = std::move(lease.device);
    immediateContext = std::move(lease.immediateContext);
    softwareRasterizer = lease.backend == SurfaceBackend::warpDxgi;
    return device != nullptr && immediateContext != nullptr;
}

bool RenderDeviceHub::isDeviceCompatibleWithWindow(
    ID3D11Device* device,
    HWND window) noexcept
{
    if (device == nullptr || window == nullptr || !IsWindow(window))
        return false;

    auto& display = DisplayEnvironmentService::instance();
    const auto identity = display.windowIdentity(window);
    const auto topology = identity.topologyGeneration;
    auto root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) root = window;
    {
        std::lock_guard lock(devicesMutex_);
        const auto cached = std::find_if(
            compatibility_.begin(), compatibility_.end(),
            [&](const CompatibilityEntry& entry) {
                return entry.device == device && entry.root == root &&
                    entry.topologyGeneration == topology &&
                    entry.rootLifetimeToken == identity.rootLifetimeToken &&
                    entry.windowPresentationRevision ==
                        identity.windowPresentationRevision;
            });
        if (cached != compatibility_.end()) return cached->compatible;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> currentAdapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> currentAdapter1;
    if (FAILED(device->QueryInterface(
            __uuidof(IDXGIDevice),
            reinterpret_cast<void**>(dxgiDevice.ReleaseAndGetAddressOf()))) ||
        FAILED(dxgiDevice->GetAdapter(
            currentAdapter.ReleaseAndGetAddressOf())) ||
        FAILED(currentAdapter.As(&currentAdapter1))) {
        return false;
    }

    DXGI_ADAPTER_DESC1 currentDescription{};
    if (FAILED(currentAdapter1->GetDesc1(&currentDescription))) return false;
    bool compatible =
        (currentDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

    if (!compatible) {
        const auto wantedDescription = display.snapshot(window).adapterLuid;
        compatible = sameLuid(wantedDescription, LUID{}) || sameLuid(
            currentDescription.AdapterLuid, wantedDescription);
    }
    {
        std::lock_guard lock(devicesMutex_);
        compatibility_.erase(std::remove_if(
            compatibility_.begin(), compatibility_.end(),
            [&](const CompatibilityEntry& entry) {
                return entry.root == root &&
                    (entry.topologyGeneration != topology ||
                    entry.rootLifetimeToken != identity.rootLifetimeToken ||
                     entry.windowPresentationRevision !=
                        identity.windowPresentationRevision);
            }), compatibility_.end());
        compatibility_.push_back({device, root, topology,
            identity.rootLifetimeToken,
            identity.windowPresentationRevision, compatible});
    }
    return compatible;
}

void RenderDeviceHub::invalidate(ID3D11Device* device) noexcept
{
    if (device == nullptr) return;
    LUID failedLuid{};
    bool failedHardware = false;
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        for (const auto& entry : devices_) {
            if (entry.device.Get() == device && !entry.softwareRasterizer) {
                failedLuid = entry.adapterLuid;
                failedHardware = true;
                break;
            }
        }
        devices_.erase(
            std::remove_if(
                devices_.begin(), devices_.end(), [&](const DeviceEntry& entry) {
                    return entry.device.Get() == device;
                }),
            devices_.end());
        compatibility_.erase(std::remove_if(
            compatibility_.begin(), compatibility_.end(),
            [&](const CompatibilityEntry& entry) {
                return entry.device == device;
            }), compatibility_.end());
    }
    recoveryEpoch_.fetch_add(1, std::memory_order_acq_rel);
    if (failedHardware) requestRecovery(failedLuid,
        DisplayEnvironmentService::instance().topologyGeneration());
    EnumWindows(invalidateOwnedWindow, 0);
}

std::uint64_t RenderDeviceHub::recoveryEpoch() const noexcept
{
    return recoveryEpoch_.load(std::memory_order_acquire);
}

void RenderDeviceHub::pruneUnavailableDevices() noexcept
{
    for (auto iterator = devices_.begin(); iterator != devices_.end();) {
        if (!iterator->device || FAILED(iterator->device->GetDeviceRemovedReason())) {
            const auto device = iterator->device.Get();
            compatibility_.erase(std::remove_if(compatibility_.begin(),
                compatibility_.end(), [&](const CompatibilityEntry& entry) {
                    return entry.device == device;
                }), compatibility_.end());
            iterator = devices_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void RenderDeviceHub::notifyTopologyChanged(std::uint64_t generation) noexcept
{
    bool wake = false;
    {
        std::lock_guard lock(devicesMutex_);
        if (generation <= topologyGeneration_) return;
        topologyGeneration_ = generation;
        for (auto& entry : recoveries_) {
            if (entry.missingAtTopology == 0) continue;
            entry.missingAtTopology = 0;
            entry.attempt = 0;
            entry.due = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(16);
            entry.active = true;
            wake = true;
        }
    }
    if (wake) recoveryChanged_.notify_all();
}

void RenderDeviceHub::requestRecovery(
    const LUID& adapterLuid, std::uint64_t topology) noexcept
{
    // A newer snapshot is also a valid wake; a delayed snapshot cannot roll
    // back a generation already delivered by the environment window.
    notifyTopologyChanged(topology);
    {
        std::lock_guard lock(devicesMutex_);
        pruneUnavailableDevices();
        if (std::any_of(devices_.begin(), devices_.end(),
                [&](const DeviceEntry& entry) {
                    return !entry.softwareRasterizer &&
                        sameLuid(entry.adapterLuid, adapterLuid);
                })) return; // Hardware may have arrived since the cache miss.
        auto found = std::find_if(recoveries_.begin(), recoveries_.end(),
            [&](const RecoveryEntry& entry) {
                return sameLuid(entry.adapterLuid, adapterLuid);
            });
        if (found != recoveries_.end()) {
            if (found->active) return;
            if (found->missingAtTopology == topologyGeneration_ &&
                    found->missingAtTopology != 0 &&
                    std::any_of(devices_.begin(), devices_.end(),
                        [](const DeviceEntry& entry) {
                            return entry.softwareRasterizer;
                        })) return;
            // Even a missing adapter must retry fallback creation if WARP
            // failed or was invalidated. Retain the negative hardware lookup.
            found->attempt = 0;
            found->due = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(16);
            found->active = true;
        } else {
            recoveries_.push_back({adapterLuid, 0,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(16),
                true, 0});
        }
    }
    recoveryChanged_.notify_all();
}

void RenderDeviceHub::recoveryLoop(std::stop_token stopToken) noexcept
{
    constexpr std::array<int, 6> delays{16, 33, 66, 125, 250, 500};
    while (!stopToken.stop_requested()) {
        LUID luid{};
        std::uint64_t topology = 0;
        bool knownMissing = false;
        {
            std::unique_lock lock(devicesMutex_);
            recoveryChanged_.wait(lock, stopToken, [&] {
                return stopToken.stop_requested() ||
                    std::any_of(recoveries_.begin(), recoveries_.end(),
                        [](const RecoveryEntry& entry) { return entry.active; });
            });
            if (stopToken.stop_requested()) return;
            auto next = std::min_element(recoveries_.begin(), recoveries_.end(),
                [](const RecoveryEntry& left, const RecoveryEntry& right) {
                    if (!left.active) return false;
                    if (!right.active) return true;
                    return left.due < right.due;
                });
            if (next == recoveries_.end() || !next->active) continue;
            const auto now = std::chrono::steady_clock::now();
            if (next->due > now) {
                const auto selectedDue = next->due;
                recoveryChanged_.wait_until(lock, stopToken, selectedDue, [&] {
                    return stopToken.stop_requested() ||
                        std::any_of(recoveries_.begin(), recoveries_.end(),
                            [&](const RecoveryEntry& entry) {
                                return entry.active && entry.due < selectedDue;
                            });
                });
                continue;
            }
            luid = next->adapterLuid;
            topology = topologyGeneration_;
            knownMissing = next->missingAtTopology != 0 &&
                next->missingAtTopology == topology;
        }

        const auto lookup = knownMissing
            ? AdapterLookup{AdapterLookupStatus::missing, {}} : adapterForLuid(luid);
        Microsoft::WRL::ComPtr<ID3D11Device> recovered;
        const auto hardwareResult = lookup.status == AdapterLookupStatus::found
            ? createHardwareDevice(lookup.adapter.Get(), recovered) : E_FAIL;
        const auto success = SUCCEEDED(hardwareResult);
        bool needWarp = false;
        if (!success) {
            std::lock_guard lock(devicesMutex_);
            pruneUnavailableDevices();
            needWarp = std::none_of(devices_.begin(), devices_.end(),
                [](const DeviceEntry& value) { return value.softwareRasterizer; });
        }
        Microsoft::WRL::ComPtr<ID3D11Device> warp;
        const auto warpReady = needWarp && createWarpDevice(warp);
        bool published = false;
        {
            std::lock_guard lock(devicesMutex_);
            if (stopToken.stop_requested()) return;
            auto entry = std::find_if(recoveries_.begin(), recoveries_.end(),
                [&](const RecoveryEntry& value) {
                    return value.active && sameLuid(value.adapterLuid, luid);
                });
            if (entry == recoveries_.end()) continue;
            pruneUnavailableDevices();
            if (warpReady && std::none_of(devices_.begin(), devices_.end(),
                    [](const DeviceEntry& value) { return value.softwareRasterizer; })) {
                devices_.push_back({{}, true, warp});
                published = true;
            }
            if (topology != topologyGeneration_) {
                // An environment event can arrive while DXGI/D3D runs outside
                // the lock. Its wake must outlive this stale result, including
                // an old "missing" result. WARP above is adapter-independent.
                entry->missingAtTopology = 0;
                entry->attempt = 0;
                entry->due = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(16);
            } else if (success) {
                devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                    [&](const DeviceEntry& value) {
                        return !value.softwareRasterizer &&
                            sameLuid(value.adapterLuid, luid);
                    }), devices_.end());
                devices_.push_back({luid, false, recovered});
                entry->missingAtTopology = 0;
                entry->active = false;
                published = true;
            } else {
                // A conclusive lack of FL11 support is not a transient loss.
                // Keep WARP usable without polling a permanently unsupported
                // driver twice a second. Topology/driver changes wake it again.
                // OOM, removed devices and incomplete enumeration still retry.
                if (lookup.status == AdapterLookupStatus::missing ||
                        hardwareResult == DXGI_ERROR_UNSUPPORTED)
                    entry->missingAtTopology = topology;
                const bool hasWarp = std::any_of(devices_.begin(), devices_.end(),
                    [](const DeviceEntry& value) { return value.softwareRasterizer; });
                if (entry->missingAtTopology != 0 && hasWarp) {
                    entry->active = false;
                } else {
                    entry->attempt = std::min<std::uint32_t>(entry->attempt + 1U,
                        static_cast<std::uint32_t>(delays.size() - 1U));
                    entry->due = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(delays[entry->attempt]);
                }
            }
            if (published) recoveryEpoch_.fetch_add(1, std::memory_order_acq_rel);
        }
        if (published) EnumWindows(invalidateOwnedWindow, 0);
    }
}

} // namespace nekorox::component
