// Compile with NEKOROX_RENDER_HUB_TEST and the real render_device_hub.cpp.
// Only adapter enumeration, WARP failure injection and display snapshots are
// controlled. The worker, synchronization, cache and D3D devices are real.
#include "render_device_hub.h"
#include "display_environment.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace nekorox::component;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
enum class Enumeration { missing, real, failOnce, nullOnce, descriptionFailOnce, failAlways, blockedMissing };
std::atomic<Enumeration> enumeration{Enumeration::missing};
std::atomic<unsigned> lookups{0}, warpAttempts{0}, warpFailures{0}, wrongThread{0};
std::atomic<bool> allowWarp{true};
std::atomic<HRESULT> hardwareFailure{S_OK};
std::atomic<unsigned> hardwareAttempts{0};
std::atomic<std::uint64_t> displayTopology{1};
LUID displayAdapter{};
DWORD testThread = 0;
std::mutex blockMutex;
std::condition_variable blockChanged;
bool enteredBlock = false, releaseBlock = false;
std::mutex timesMutex;
std::vector<Clock::time_point> lookupTimes;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void await(Predicate predicate, const char* message) {
    const auto end = Clock::now() + 8s;
    while (!predicate()) {
        require(Clock::now() < end, message);
        std::this_thread::sleep_for(5ms);
    }
}
void reset(Enumeration mode) {
    enumeration = mode; lookups = 0; warpAttempts = 0; warpFailures = 0; wrongThread = 0;
    allowWarp = true; displayTopology = 1;
    hardwareFailure = S_OK; hardwareAttempts = 0;
    { std::lock_guard lock(blockMutex); enteredBlock = releaseBlock = false; }
    { std::lock_guard lock(timesMutex); lookupTimes.clear(); }
}
struct UnblockOnExit {
    void release() noexcept {
        { std::lock_guard lock(blockMutex); releaseBlock = true; }
        blockChanged.notify_all();
    }
    ~UnblockOnExit() { release(); }
};
}

namespace nekorox::component {
struct RenderDeviceHubTestPeer {
    struct State {
        unsigned active = 0, dormant = 0, hardware = 0, warp = 0;
        std::uint64_t topology = 0;
        bool worker = false;
    };
    static auto create() { return std::unique_ptr<RenderDeviceHub>(new RenderDeviceHub); }
    static State state(RenderDeviceHub& hub) {
        std::lock_guard lock(hub.devicesMutex_);
        State result;
        result.worker = hub.recoveryWorker_.joinable();
        result.topology = hub.topologyGeneration_;
        for (const auto& entry : hub.recoveries_) {
            if (entry.active) ++result.active;
            else if (entry.missingAtTopology) ++result.dormant;
        }
        for (const auto& device : hub.devices_) {
            if (device.softwareRasterizer) ++result.warp;
            else ++result.hardware;
        }
        return result;
    }
};
DisplayEnvironmentService& DisplayEnvironmentService::instance() noexcept {
    static DisplayEnvironmentService value; return value;
}
DisplayEnvironmentSnapshotV1 DisplayEnvironmentService::snapshot(HWND) noexcept {
    DisplayEnvironmentSnapshotV1 value;
    value.adapterLuid = displayAdapter; value.topologyGeneration = displayTopology.load();
    return value;
}
WindowPresentationIdentityV1 DisplayEnvironmentService::windowIdentity(HWND) const noexcept {
    WindowPresentationIdentityV1 value; value.topologyGeneration = displayTopology.load(); return value;
}
std::uint64_t DisplayEnvironmentService::topologyGeneration() const noexcept { return displayTopology.load(); }
namespace testing {
HRESULT hardwareCreationFailure() noexcept {
    if (GetCurrentThreadId() == testThread) ++wrongThread;
    ++hardwareAttempts;
    return hardwareFailure.exchange(S_OK);
}
HRESULT enumerateAdapter(IDXGIFactory1* factory, UINT index, IDXGIAdapter1** out) noexcept {
    if (GetCurrentThreadId() == testThread) ++wrongThread;
    *out = nullptr;
    if (index == 0) {
        ++lookups;
        std::lock_guard lock(timesMutex); lookupTimes.push_back(Clock::now());
    }
    const auto mode = enumeration.load();
    if (mode == Enumeration::missing) return DXGI_ERROR_NOT_FOUND;
    if (mode == Enumeration::failAlways) return E_FAIL;
    if (mode == Enumeration::failOnce || mode == Enumeration::nullOnce) {
        enumeration = Enumeration::real;
        return mode == Enumeration::failOnce ? E_FAIL : S_OK;
    }
    if (mode == Enumeration::blockedMissing) {
        std::unique_lock lock(blockMutex); enteredBlock = true; blockChanged.notify_all();
        blockChanged.wait_for(lock, 8s, [] { return releaseBlock; });
        return DXGI_ERROR_NOT_FOUND; // Deliberately stale after a topology wake.
    }
    return factory->EnumAdapters1(index, out);
}
bool allowWarpCreation() noexcept {
    if (GetCurrentThreadId() == testThread) ++wrongThread;
    const bool allowed = allowWarp.load();
    ++warpAttempts; if (!allowed) ++warpFailures;
    return allowed;
}
HRESULT describeAdapter(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1* out) noexcept {
    const auto result = adapter->GetDesc1(out);
    if (SUCCEEDED(result) && enumeration == Enumeration::descriptionFailOnce &&
            out->AdapterLuid.HighPart == displayAdapter.HighPart &&
            out->AdapterLuid.LowPart == displayAdapter.LowPart) {
        enumeration = Enumeration::real; return E_FAIL;
    }
    return result;
}
}
}

namespace {
using Peer = RenderDeviceHubTestPeer;
auto start() {
    auto hub = Peer::create(); DeviceLease lease;
    require(!hub->tryAcquireExisting(nullptr, lease), "Initial acquire must be cache-only");
    return hub;
}
DeviceLease waitFor(RenderDeviceHub& hub, SurfaceBackend backend) {
    await([&] {
        auto state = Peer::state(hub);
        return backend == SurfaceBackend::hardwareDxgi ? state.hardware > 0 : state.warp > 0;
    }, "Worker did not publish a device");
    DeviceLease lease;
    require(hub.tryAcquireExisting(nullptr, lease) && lease.backend == backend, "Wrong cached backend");
    require(SUCCEEDED(lease.device->GetDeviceRemovedReason()), "Published device is not healthy");
    require(wrongThread == 0, "Device lookup/creation ran on requesting thread");
    return lease;
}
void waitDormant(RenderDeviceHub& hub) {
    await([&] { auto s = Peer::state(hub); return s.active == 0 && s.dormant == 1; }, "Missing adapter did not sleep");
}
void chooseHardware() {
    ComPtr<IDXGIFactory1> factory;
    require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "Create real DXGI factory");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto hr = factory->EnumAdapters1(index, &adapter);
        require(hr != DXGI_ERROR_NOT_FOUND, "Probe needs a real hardware adapter");
        require(SUCCEEDED(hr) && adapter, "Enumerate real adapter");
        DXGI_ADAPTER_DESC1 description{};
        require(SUCCEEDED(adapter->GetDesc1(&description)), "Read real adapter");
        if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            displayAdapter = description.AdapterLuid;
            std::wcout << L"Hardware fixture: " << description.Description << std::endl; return;
        }
    }
}
void missingThenReconnect(unsigned soakSeconds) {
    reset(Enumeration::missing); auto hub = start();
    auto warp = waitFor(*hub, SurfaceBackend::warpDxgi); waitDormant(*hub);
    const auto epoch = hub->recoveryEpoch();
    for (int i = 0; i < 1000; ++i) {
        DeviceLease lease; require(hub->tryAcquireExisting(nullptr, lease), "Steady WARP cache miss");
    }
    require(lookups == 1 && Peer::state(*hub).active == 0, "Cache probes rearmed missing hardware");
    const auto begin = Clock::now();
    for (unsigned second = 0; second < soakSeconds; ++second) {
        std::this_thread::sleep_for(1s);
        require(lookups == 1 && warpAttempts == 1 && hub->recoveryEpoch() == epoch,
            "Dormant worker kept doing work");
        if ((second + 1) % 15 == 0) std::cout << "Dormancy soak " << second + 1 << "s: lookups=1 WARP=1" << std::endl;
    }
    std::cout << "Missing adapter: " << std::chrono::duration<double>(Clock::now()-begin).count()
        << "s, 1000 cache probes, lookups=" << lookups << ", WARP attempts=" << warpAttempts << std::endl;
    // Duplicate/old environment notifications must not create work.
    hub->notifyTopologyChanged(1); hub->notifyTopologyChanged(0);
    require(Peer::state(*hub).active == 0, "Unchanged topology woke a dormant entry");
    enumeration = Enumeration::real; displayTopology = 2; hub->notifyTopologyChanged(2);
    auto hardware = waitFor(*hub, SurfaceBackend::hardwareDxgi);
    require(hardware.epoch > warp.epoch && lookups == 2, "Reconnect did not publish a new epoch");
    displayTopology = 1; // Simulate an old snapshot delivered after the event.
    for (int i = 0; i < 1000; ++i) { DeviceLease lease; require(hub->tryAcquireExisting(nullptr, lease), "Hardware cache miss"); }
    require(Peer::state(*hub).topology == 2 && lookups == 2, "Stale snapshot regressed topology or retried hardware");
    std::cout << "PASS: event-driven hardware return, epochs, unchanged topology, steady hardware" << std::endl;
}
void warpFailureAndInvalidation() {
    reset(Enumeration::missing); allowWarp = false; auto hub = start();
    await([] { return warpFailures >= 3; }, "Failed WARP not retried");
    require(lookups == 1, "Fallback retries repeated absent hardware enumeration");
    allowWarp = true; auto first = waitFor(*hub, SurfaceBackend::warpDxgi); waitDormant(*hub);
    hub->invalidate(first.device.Get());
    DeviceLease absent; require(!hub->tryAcquireExisting(nullptr, absent), "Invalid WARP remained cached");
    auto second = waitFor(*hub, SurfaceBackend::warpDxgi); waitDormant(*hub);
    require(second.device.Get() != first.device.Get() && second.epoch > first.epoch && lookups == 1,
        "Dormant adapter did not recover fallback independently");
    // A newer snapshot can repair a missed event, including from dormancy.
    enumeration = Enumeration::real; displayTopology = 3;
    DeviceLease old; require(hub->tryAcquireExisting(nullptr, old), "Existing WARP unavailable on new topology");
    waitFor(*hub, SurfaceBackend::hardwareDxgi);
    require(Peer::state(*hub).topology == 3 && lookups == 2, "New snapshot failed to wake hardware");
    std::cout << "PASS: WARP failure/backoff, WARP invalidation, newer snapshot wake" << std::endl;
}
void transientLookup(Enumeration mode) {
    reset(mode); auto hub = start(); waitFor(*hub, SurfaceBackend::hardwareDxgi);
    require(lookups >= 2 && Peer::state(*hub).dormant == 0, "Transient enumeration error poisoned absence cache");
    std::cout << "PASS: transient " << (mode == Enumeration::failOnce ? "E_FAIL" :
        mode == Enumeration::nullOnce ? "S_OK/null" : "GetDesc1 failure") << " enumeration" << std::endl;
}
void topologyDuringLookup() {
    reset(Enumeration::blockedMissing); auto hub = start(); UnblockOnExit guard;
    { std::unique_lock lock(blockMutex); require(blockChanged.wait_for(lock, 5s, [] { return enteredBlock; }), "Lookup did not block"); }
    for (int i = 0; i < 1000; ++i) {
        DeviceLease absent; require(!hub->tryAcquireExisting(nullptr, absent), "Blocked lookup unexpectedly published a device");
    }
    require(Peer::state(*hub).active == 1 && lookups == 1, "Active recovery demand was not coalesced");
    enumeration = Enumeration::real; displayTopology = 2; hub->notifyTopologyChanged(2);
    // An older caller arriving after the event cannot consume it.
    displayTopology = 1; DeviceLease lease; (void)hub->tryAcquireExisting(nullptr, lease);
    guard.release(); waitFor(*hub, SurfaceBackend::hardwareDxgi);
    require(lookups == 2 && Peer::state(*hub).topology == 2 && Peer::state(*hub).dormant == 0,
        "Old missing result swallowed the topology wake");
    std::cout << "PASS: topology wake while lookup is blocked, stale missing result rejected" << std::endl;
}
void failureBackoffAndShutdown() {
    reset(Enumeration::failAlways); auto hub = start(); waitFor(*hub, SurfaceBackend::warpDxgi);
    await([] { std::lock_guard lock(timesMutex); return lookupTimes.size() >= 7; },
        "Transient failures did not keep bounded recovery alive");
    std::vector<Clock::time_point> times;
    { std::lock_guard lock(timesMutex); times = lookupTimes; }
    constexpr int delays[]{33,66,125,250,500,500};
    std::cout << "Transient retry intervals (ms):";
    for (unsigned i = 1; i <= 6; ++i) {
        const auto ms = std::chrono::duration<double,std::milli>(times[i]-times[i-1]).count();
        std::cout << ' ' << ms;
        require(ms >= delays[i-1] - 3, "Retry loop ran faster than backoff");
        require(ms <= delays[i-1] + 750, "Retry exceeded bounded backoff/scheduling allowance");
    }
    require(Peer::state(*hub).dormant == 0 && warpAttempts == 1, "Transient error stopped recovery/recreated WARP");
    const auto begin = Clock::now(); hub.reset();
    require(Clock::now()-begin < 1s, "Timed-wait shutdown did not respond to stop");
    std::cout << "\nPASS: bounded transient retries, shutdown while waiting" << std::endl;
    reset(Enumeration::missing); hub = start(); waitFor(*hub, SurfaceBackend::warpDxgi); waitDormant(*hub);
    const auto dormantBegin = Clock::now(); hub.reset();
    require(Clock::now()-dormantBegin < 1s, "Dormant shutdown did not respond to stop");
}
void unsupportedHardware() {
    reset(Enumeration::real); hardwareFailure = DXGI_ERROR_UNSUPPORTED;
    auto hub = start(); waitFor(*hub, SurfaceBackend::warpDxgi); waitDormant(*hub);
    const auto attempts = hardwareAttempts.load(), scans = lookups.load();
    for (int i = 0; i < 1000; ++i) {
        DeviceLease lease; require(hub->tryAcquireExisting(nullptr, lease), "Unsupported adapter lost WARP");
    }
    std::this_thread::sleep_for(1100ms);
    require(hardwareAttempts == attempts && lookups == scans,
        "Unsupported adapter polled despite a working fallback");
    displayTopology = 2; hub->notifyTopologyChanged(2);
    waitFor(*hub, SurfaceBackend::hardwareDxgi);
    require(hardwareAttempts == attempts + 1 && Peer::state(*hub).dormant == 0,
        "Topology change did not reconsider unsupported hardware");
    reset(Enumeration::real); hardwareFailure = E_OUTOFMEMORY;
    hub = start(); waitFor(*hub, SurfaceBackend::hardwareDxgi);
    require(hardwareAttempts >= 2 && Peer::state(*hub).dormant == 0,
        "Transient hardware creation error was cached as unsupported");
    std::cout << "PASS: unsupported FL11 sleeps with WARP, topology retries, transient OOM recovers" << std::endl;
}
}

int main(int argc, char** argv) {
    testThread = GetCurrentThreadId(); CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        chooseHardware();
        { auto hub = Peer::create(); hub->notifyTopologyChanged(2);
          require(!Peer::state(*hub).worker, "Environment notification initialized graphics"); }
        missingThenReconnect(argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 2);
        warpFailureAndInvalidation(); transientLookup(Enumeration::failOnce); transientLookup(Enumeration::nullOnce);
        transientLookup(Enumeration::descriptionFailOnce);
        topologyDuringLookup(); failureBackoffAndShutdown(); unsupportedHardware();
        std::cout << "REAL RenderDeviceHub worker PASS" << std::endl; CoUninitialize(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl; CoUninitialize(); return 1;
    }
}
