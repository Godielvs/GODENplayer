// Real DisplayEnvironment window/message path + real RenderDeviceHub worker.
// Enumeration can hide the adapter; no display/driver or player is changed.
#include "render_device_hub.h"
#include "display_environment.h"
#include "frame_clock.h"
#include <dxgi1_2.h>
#include <dbt.h>
#include <iostream>
#include <stdexcept>
#include <cwchar>
using namespace nekorox::component;
using namespace std::chrono_literals;
namespace {
std::atomic<bool> hideAdapters{true};
std::atomic<unsigned> lookups{0}, environmentWakes{0}, preferenceRefreshes{0};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
HWND ownEnvironmentWindow() {
    HWND found = nullptr;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        wchar_t name[128]{};
        if (pid == GetCurrentProcessId() && GetClassNameW(window,name,128) &&
                std::wcscmp(name,L"GODENplayer.DisplayEnvironment.v1") == 0) {
            *reinterpret_cast<HWND*>(data) = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
template<class Predicate> void await(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, "Timed out awaiting worker");
        std::this_thread::sleep_for(5ms);
    }
}
}
namespace nekorox::component {
struct RenderDeviceHubTestPeer {
    static bool hasWorker(RenderDeviceHub& hub) { return hub.recoveryWorker_.joinable(); }
    static bool dormant(RenderDeviceHub& hub) {
        std::lock_guard lock(hub.devicesMutex_);
        for (const auto& entry : hub.recoveries_)
            if (!entry.active && entry.missingAtTopology) return true;
        return false;
    }
    static bool hardware(RenderDeviceHub& hub) {
        std::lock_guard lock(hub.devicesMutex_);
        for (const auto& device : hub.devices_)
            if (!device.softwareRasterizer) return true;
        return false;
    }
};
// Only downstream clock/telemetry are stubbed. Neither replaces display/hub.
void WindowFrameClock::environmentChanged() noexcept { ++environmentWakes; }
void refreshSystemMotionPreference() noexcept { ++preferenceRefreshes; }
PerformanceTelemetry::PerformanceTelemetry() noexcept = default;
PerformanceTelemetry::~PerformanceTelemetry() = default;
PerformanceTelemetry& PerformanceTelemetry::instance() noexcept { static PerformanceTelemetry value; return value; }
std::uint64_t PerformanceTelemetry::beginSpan(TelemetrySurface,std::uint64_t,std::uint64_t) noexcept { return 0; }
void PerformanceTelemetry::endSpan(TelemetrySurface,std::uint64_t,std::uint64_t,std::uint64_t) noexcept {}
namespace testing {
HRESULT enumerateAdapter(IDXGIFactory1* factory, UINT index, IDXGIAdapter1** out) noexcept {
    if (index == 0) ++lookups;
    if (hideAdapters) { *out = nullptr; return DXGI_ERROR_NOT_FOUND; }
    return factory->EnumAdapters1(index, out);
}
bool allowWarpCreation() noexcept { return true; }
HRESULT hardwareCreationFailure() noexcept { return S_OK; }
HRESULT describeAdapter(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1* out) noexcept { return adapter->GetDesc1(out); }
}
}
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HWND window = nullptr;
    auto& display = DisplayEnvironmentService::instance();
    try {
        display.start();
        const auto environment = ownEnvironmentWindow();
        DWORD pid=0; GetWindowThreadProcessId(environment,&pid);
        require(environment && pid == GetCurrentProcessId(), "Own environment window unavailable");
        const auto beforePreferences = preferenceRefreshes.load();
        const auto beforeSettingsWakes = environmentWakes.load();
        SendMessageW(environment, WM_SETTINGCHANGE, SPI_SETCLIENTAREAANIMATION, 0);
        require(preferenceRefreshes == beforePreferences + 1 &&
            environmentWakes > beforeSettingsWakes, "Own settings message did not refresh preferences/presentation");
        std::cout << "PASS: own WM_SETTINGCHANGE -> preferences -> presentation; no OS setting changed" << std::endl;
        auto& hub = RenderDeviceHub::instance();
        SendMessageW(environment,WM_DISPLAYCHANGE,32,MAKELPARAM(1920,1080));
        require(!RenderDeviceHubTestPeer::hasWorker(hub), "Topology initialized the graphics worker");
        window = CreateWindowExW(0,L"STATIC",L"Hub environment test",WS_OVERLAPPEDWINDOW,
            20,20,320,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window != nullptr, "Test window unavailable");
        display.registerPersistentRoot(window);
        DeviceLease lease; (void)hub.tryAcquireExisting(window,lease);
        await([&] { return RenderDeviceHubTestPeer::dormant(hub); });
        require(lookups == 1 && hub.tryAcquireExisting(window,lease) && lease.backend == SurfaceBackend::warpDxgi,
            "Initial missing adapter not using WARP");
        for (const UINT event : {WM_DISPLAYCHANGE, WM_DEVICECHANGE}) {
            const auto topology = display.topologyGeneration(), epoch = hub.recoveryEpoch();
            const auto beforeLookups = lookups.load(), beforeWakes = environmentWakes.load();
            hideAdapters = false;
            SendMessageW(environment,event,event == WM_DEVICECHANGE ? DBT_DEVNODES_CHANGED : 32,0);
            require(display.topologyGeneration() > topology && environmentWakes > beforeWakes,
                "Real environment path did not advance topology/notify presentation");
            await([&] { return RenderDeviceHubTestPeer::hardware(hub); });
            require(hub.tryAcquireExisting(window,lease) && lease.backend == SurfaceBackend::hardwareDxgi &&
                lease.epoch > epoch && lookups == beforeLookups + 1,
                "Real environment message did not restore hardware");
            std::cout << "PASS: native " << (event == WM_DISPLAYCHANGE ? "WM_DISPLAYCHANGE" : "WM_DEVICECHANGE")
                << " -> topology -> dormant worker -> hardware" << std::endl;
            if (event == WM_DISPLAYCHANGE) {
                hideAdapters = true; hub.invalidate(lease.device.Get());
                await([&] { return RenderDeviceHubTestPeer::dormant(hub); });
            }
        }
        display.unregisterPersistentRoot(window); DestroyWindow(window); window = nullptr;
        display.stop(); std::cout << "REAL environment integration PASS" << std::endl;
        CoUninitialize(); return 0;
    } catch (const std::exception& error) {
        if (window) { display.unregisterPersistentRoot(window); DestroyWindow(window); }
        display.stop(); std::cerr << "FAIL: " << error.what() << std::endl; CoUninitialize(); return 1;
    }
}
