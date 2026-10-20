// Real D3D11/DComp/phosphor and threadpool waits. Only device selection,
// display data, fault injection and telemetry are supplied by this fixture.
// Baseline containment drains a misordered wait before CloseHandle: record the
// bug without deliberately performing an undefined Windows handle teardown.
#include "crt_xy_surface.h"
#include "display_environment.h"
#include "frame_clock.h"
#include "interaction_latency_guard.h"
#include "render_device_hub.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace nekorox;
using namespace nekorox::component;
using Microsoft::WRL::ComPtr;
namespace {
DeviceLease offered;
std::uint64_t recovery = 1;
constexpr UINT readyMessage = WM_APP + 53;
bool failComposition = false, failSwapChain = false;
bool persistentSwapChainFailure = false;
unsigned swapChainAttempts = 0;
std::atomic<bool> blockCallback{false}, callbackRunning{false};
HANDLE callbackEntered = nullptr, callbackRelease = nullptr;
unsigned violations = 0, checkedCloses = 0, acquisitions = 0;
std::uint32_t drainedGeneration = 0, drainedSerial = 0;
const char* phase = "initial";

void require(bool ok, const char* detail)
{
    if (!ok) throw std::runtime_error(detail);
}
void expect(bool ok, const char* detail)
{
    if (!ok) { ++violations; std::cout << "FAIL [" << phase << "]: " << detail << '\n'; }
}
DeviceLease makeDevice(D3D_DRIVER_TYPE type)
{
    DeviceLease lease;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &lease.device, nullptr, &lease.immediateContext)), "Create D3D device");
    lease.backend = type == D3D_DRIVER_TYPE_WARP
        ? SurfaceBackend::warpDxgi : SurfaceBackend::hardwareDxgi;
    return lease;
}
}

namespace nekorox::component {
struct CrtSurfaceTestPeer {
    static void containClose(CrtXySurface& surface)
    {
        bool unsafe;
        {
            std::lock_guard lock(surface.frameWaitMutex_);
            unsafe = surface.waitAdmissionOpen_ || surface.frameWaitArmed_ ||
                surface.readyMessagePending_ || callbackRunning.load();
        }
        ++checkedCloses;
        expect(!unsafe, "retirement must invalidate/drain wait and mailbox before closing handle");
        expect(drainedGeneration == surface.generation() && drainedSerial == surface.nextArmSerial_,
            "wait drain must complete for the current generation/arm before close");
        // A failing baseline must still be safe to run on the user's machine.
        if (unsafe) surface.disarmFrameWait(true);
    }
    static void drained(CrtXySurface& surface)
    {
        drainedGeneration = surface.generation(); drainedSerial = surface.nextArmSerial_;
    }
    static HANDLE armControlledWait(CrtXySurface& surface)
    {
        surface.disarmFrameWait(true);
        require(surface.frameLatencyHandle_ && surface.swapChain_, "Live swapchain required");
        CloseHandle(surface.frameLatencyHandle_);
        auto handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(handle != nullptr, "Create controlled latency event");
        surface.frameLatencyHandle_ = handle;
        surface.armFrameWait();
        std::lock_guard lock(surface.frameWaitMutex_);
        require(surface.frameWaitArmed_, "Controlled wait armed");
        return handle;
    }
    static void disarm(CrtXySurface& surface) { surface.disarmFrameWait(true); }
    static void armPacing(CrtXySurface& surface)
    {
        surface.disarmFrameWait(true);
        require(surface.armPaceTimer(surface.qpcFrequency_ / 10U), "Arm 100ms pacing timer");
    }
    static bool pending(CrtXySurface& surface)
    {
        std::lock_guard lock(surface.frameWaitMutex_);
        return surface.readyMessagePending_;
    }
    static bool waiting(CrtXySurface& surface)
    {
        std::lock_guard lock(surface.frameWaitMutex_);
        return surface.frameWaitArmed_ || surface.readyMessagePending_;
    }
    static bool hasChain(CrtXySurface& surface) { return surface.swapChain_ != nullptr; }
    static HANDLE handle(CrtXySurface& surface) { return surface.frameLatencyHandle_; }
    static ID3D11Texture2D* phosphor(CrtXySurface& surface) { return surface.phosphorTextures_[0].Get(); }
    static void noDeadline(CrtXySurface& surface) { surface.nextPresentQpc_ = 0; }
};
void crtTestBeforeLatencyClose(CrtXySurface& surface) { CrtSurfaceTestPeer::containClose(surface); }
void crtTestWaitDrained(CrtXySurface& surface) { CrtSurfaceTestPeer::drained(surface); }
bool crtTestFailComposition() { return std::exchange(failComposition, false); }
bool crtTestFailSwapChain()
{
    ++swapChainAttempts;
    return std::exchange(failSwapChain, false) || persistentSwapChainFailure;
}
void crtTestCallbackBeforePost()
{
    if (!blockCallback.exchange(false)) return;
    callbackRunning = true;
    SetEvent(callbackEntered);
    WaitForSingleObject(callbackRelease, 5000);
    callbackRunning = false;
}
RenderDeviceHub& RenderDeviceHub::instance() { static RenderDeviceHub value; return value; }
RenderDeviceHub::~RenderDeviceHub() = default;
bool RenderDeviceHub::acquire(HWND, ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context, bool& software) noexcept
{
    ++acquisitions;
    device = offered.device; context = offered.immediateContext;
    software = offered.backend == SurfaceBackend::warpDxgi;
    return bool(offered);
}
bool RenderDeviceHub::isDeviceCompatibleWithWindow(ID3D11Device* device, HWND) noexcept { return device != nullptr; }
void RenderDeviceHub::invalidate(ID3D11Device*) noexcept { ++recovery; }
std::uint64_t RenderDeviceHub::recoveryEpoch() const noexcept { return recovery; }
DisplayEnvironmentService& DisplayEnvironmentService::instance() noexcept { static DisplayEnvironmentService value; return value; }
std::uint64_t DisplayEnvironmentService::topologyGeneration() const noexcept { return 1; }
DisplayEnvironmentSnapshotV1 DisplayEnvironmentService::snapshot(HWND) noexcept
{
    DisplayEnvironmentSnapshotV1 result; result.refreshNumerator = 165; return result;
}
std::uint32_t DisplayEnvironmentSnapshotV1::refreshMilliHz() const noexcept { return 165000; }
InteractionLatencyGuard& InteractionLatencyGuard::instance() noexcept { static InteractionLatencyGuard value; return value; }
bool InteractionLatencyGuard::shouldYieldRender() const noexcept { return false; }
void WindowFrameClock::reportPresentOutcome(bool) noexcept {}
PerformanceTelemetry::PerformanceTelemetry() noexcept = default;
PerformanceTelemetry::~PerformanceTelemetry() = default;
PerformanceTelemetry& PerformanceTelemetry::instance() noexcept { static PerformanceTelemetry value; return value; }
std::uint64_t PerformanceTelemetry::beginSpan(TelemetrySurface, std::uint64_t, std::uint64_t) noexcept { return 0; }
void PerformanceTelemetry::endSpan(TelemetrySurface, std::uint64_t, std::uint64_t, std::uint64_t) noexcept {}
void PerformanceTelemetry::present(TelemetrySurface, HRESULT, bool) noexcept {}
}

namespace {
void pump(CrtXySurface& surface, HWND window, unsigned milliseconds)
{
    auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.hwnd == window && message.message == readyMessage)
                surface.present(static_cast<std::uint32_t>(message.wParam), static_cast<std::uint32_t>(message.lParam));
            else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 2, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
}
void beam(CrtXySurface& surface)
{
    const CrtBeamSegment segments[] = {
        {-.65f,-.65f,.65f,.65f,.3f,0}, {-.65f,.65f,.65f,-.65f,.3f,0}
    };
    surface.submit(segments);
}
void drawingResumes(CrtXySurface& surface, HWND window)
{
    const auto frames = surface.stats().presentedFrames;
    beam(surface);
    const auto deadline = GetTickCount64() + 500;
    do { pump(surface, window, 5); }
    while (surface.stats().presentedFrames == frames && GetTickCount64() < deadline);
    expect(surface.stats().presentedFrames > frames, "drawing resumes using actual readiness messages");
    if (surface.stats().presentedFrames == frames) {
        std::cout << "  frames=" << frames << " waiting=" << CrtSurfaceTestPeer::waiting(surface)
            << " chain=" << CrtSurfaceTestPeer::hasChain(surface) << " generation=" << surface.generation() << '\n';
    }
}
void staleRejected(CrtXySurface& surface, std::uint32_t oldGeneration)
{
    const auto frames = surface.stats().presentedFrames;
    CrtSurfaceTestPeer::noDeadline(surface);
    surface.present(oldGeneration, 0);
    expect(surface.stats().presentedFrames == frames, "obsolete unsolicited message rejected");
}
void run(HWND window, D3D_DRIVER_TYPE type)
{
    offered = makeDevice(type); ++recovery;
    phase = "initial";
    auto owner = std::make_unique<CrtXySurface>(); // large bounded segment queue
    auto& surface = *owner;
    surface.attach(window, readyMessage);
    RECT viewport{0,0,240,160};
    surface.setViewport(viewport); surface.setActive(true);
    require(surface.active() && CrtSurfaceTestPeer::hasChain(surface), "Create real CRT surface");
    drawingResumes(surface, window);

    // A position-only composition failure bypassed the normal resize drain.
    phase = "composition failure";
    CrtSurfaceTestPeer::armControlledWait(surface);
    auto before = surface.generation();
    failComposition = true; ++viewport.left; ++viewport.right;
    surface.setViewport(viewport);
    expect(surface.generation() != before, "composition failure retires generation");
    staleRejected(surface, before); drawingResumes(surface, window);

    // Display notification can arrive with a serial of zero while a wait is armed.
    phase = "unsolicited epoch";
    CrtSurfaceTestPeer::armControlledWait(surface);
    before = surface.generation(); ++recovery;
    surface.displayEnvironmentChanged(true); pump(surface, window, 40);
    expect(surface.generation() != before, "unsolicited epoch recovery retires generation");
    staleRejected(surface, before); drawingResumes(surface, window);

    // Callback is already running (past the mutex) when teardown begins.
    phase = "in-flight callback";
    auto controlled = CrtSurfaceTestPeer::armControlledWait(surface);
    ResetEvent(callbackEntered); ResetEvent(callbackRelease); blockCallback = true;
    SetEvent(controlled);
    require(WaitForSingleObject(callbackEntered, 2000) == WAIT_OBJECT_0, "Callback entered");
    before = surface.generation();
    std::jthread unblock([] { Sleep(80); SetEvent(callbackRelease); });
    failComposition = true; ++viewport.left; ++viewport.right;
    surface.setViewport(viewport);
    expect(!callbackRunning, "callback completes before teardown returns");
    expect(surface.generation() != before, "in-flight callback epoch invalidated");
    unblock.join(); staleRejected(surface, before); drawingResumes(surface, window);

    // Mailbox already delivered: the obsolete queued serial must not survive.
    phase = "queued callback";
    controlled = CrtSurfaceTestPeer::armControlledWait(surface);
    SetEvent(controlled);
    auto deadline = GetTickCount64() + 2000;
    while (!CrtSurfaceTestPeer::pending(surface) && GetTickCount64() < deadline) Sleep(1);
    require(CrtSurfaceTestPeer::pending(surface), "Readiness mailbox delivered");
    before = surface.generation(); failComposition = true;
    ++viewport.left; ++viewport.right; surface.setViewport(viewport);
    expect(surface.generation() != before, "queued callback epoch invalidated");
    staleRejected(surface, before); drawingResumes(surface, window);

    // Successful ResizeBuffers is not retirement; simulation and waitable
    // object must survive. Compare composite pixels without advancing the
    // simulation, as well as the identity of its phosphor texture.
    phase = "successful resize";
    CrtSurfaceCapture pixelsBefore, pixelsAfter;
    require(surface.captureBgra(pixelsBefore), "Capture pre-resize CRT");
    ComPtr<ID3D11Texture2D> texture = CrtSurfaceTestPeer::phosphor(surface);
    auto latency = CrtSurfaceTestPeer::handle(surface);
    const auto closesBeforeResize = checkedCloses;
    before = surface.generation();
    viewport.right += 20; viewport.bottom += 20; surface.setViewport(viewport);
    viewport.right -= 20; viewport.bottom -= 20; surface.setViewport(viewport);
    require(surface.captureBgra(pixelsAfter), "Capture post-resize CRT");
    expect(surface.generation() == before && CrtSurfaceTestPeer::handle(surface) == latency,
        "successful resize preserves generation and latency handle");
    expect(checkedCloses == closesBeforeResize, "resize does not retire any latency handle");
    expect(CrtSurfaceTestPeer::phosphor(surface) == texture.Get() && pixelsBefore.bgra == pixelsAfter.bgra,
        "successful resize preserves phosphor pixels");
    drawingResumes(surface, window);

    phase = "resize during pacing";
    // A consumed latency credit followed by a still-pending pacing timer.
    // Keep latency unsignalled so swapping back to it cannot hide the bug.
    CrtSurfaceTestPeer::armControlledWait(surface);
    CrtSurfaceTestPeer::armPacing(surface);
    before = surface.generation();
    viewport.right += 1; surface.setViewport(viewport);
    viewport.bottom += 1; surface.setViewport(viewport);
    expect(surface.generation() == before, "pacing resize keeps generation");
    drawingResumes(surface, window);

    phase = "resize with pacing callback in flight";
    CrtSurfaceTestPeer::armControlledWait(surface);
    ResetEvent(callbackEntered); ResetEvent(callbackRelease); blockCallback = true;
    CrtSurfaceTestPeer::armPacing(surface);
    require(WaitForSingleObject(callbackEntered, 2000) == WAIT_OBJECT_0, "Pacing callback entered");
    std::jthread unblockPacing([] { Sleep(80); SetEvent(callbackRelease); });
    viewport.right += 1; surface.setViewport(viewport);
    unblockPacing.join();
    expect(!callbackRunning, "resize drains in-flight pacing callback");
    drawingResumes(surface, window);

    // Hide/show preserves the simulation but actually retires the swapchain.
    phase = "hide show";
    texture = CrtSurfaceTestPeer::phosphor(surface);
    surface.setActive(false);
    expect(!CrtSurfaceTestPeer::waiting(surface), "hidden CRT does not wait");
    expect(CrtSurfaceTestPeer::phosphor(surface) == texture.Get(), "hide preserves phosphor");
    surface.setActive(true); drawingResumes(surface, window);
    pump(surface, window, 800);
    const auto frames = surface.stats().presentedFrames;
    pump(surface, window, 120);
    expect(surface.stats().presentedFrames == frames && !CrtSurfaceTestPeer::waiting(surface),
        "settled CRT quiesces without continuous polling");

    // Independently observe a transient recreation failure. This is a separate
    // liveness condition from handle lifetime and must not be rescued by UI.
    phase = "transient recreation";
    CrtSurfaceTestPeer::armControlledWait(surface);
    ++recovery; failSwapChain = true;
    surface.displayEnvironmentChanged(true); pump(surface, window, 20);
    expect(!CrtSurfaceTestPeer::hasChain(surface), "one-shot swapchain fault exercised");
    pump(surface, window, 650);
    expect(CrtSurfaceTestPeer::hasChain(surface), "transient swapchain failure retries without external redraw");
    drawingResumes(surface, window);

    // Persistent failure must not spin. Hiding cancels the recovery timer.
    phase = "persistent failure";
    surface.setActive(false); persistentSwapChainFailure = true;
    const auto attempts = swapChainAttempts;
    surface.setActive(true); pump(surface, window, 580);
    expect(swapChainAttempts - attempts >= 2 && swapChainAttempts - attempts <= 3,
        "persistent failures retry at most four times per second");
    surface.setActive(false); const auto hiddenAttempts = swapChainAttempts;
    pump(surface, window, 600);
    expect(swapChainAttempts == hiddenAttempts && !CrtSurfaceTestPeer::waiting(surface),
        "hidden surface cancels resource retries");
    persistentSwapChainFailure = false;
    surface.setActive(true); drawingResumes(surface, window);

    // Initial async device-cache miss also resumes without a resize or owner
    // calling setActive again. No blocking device creation is added to UI.
    phase = "device cache miss";
    surface.setActive(false); ++recovery;
    auto savedDevice = offered; offered = {};
    surface.setActive(true); pump(surface, window, 20);
    expect(surface.active() && !CrtSurfaceTestPeer::hasChain(surface), "cache miss retains active recovery intent");
    offered = savedDevice; pump(surface, window, 400);
    expect(CrtSurfaceTestPeer::hasChain(surface), "initial device cache miss recovers independently");
    drawingResumes(surface, window);

    phase = "owner repeats viewport while device unavailable";
    surface.setActive(false); ++recovery; offered = {};
    const auto requests = acquisitions;
    const auto repeatedStart = GetTickCount64();
    surface.setActive(true);
    unsigned ownerCalls = 0;
    do {
        for (unsigned i = 0; i < 10; ++i) {
            surface.setViewport(viewport); surface.setActive(true); ++ownerCalls;
        }
        pump(surface, window, 2);
    } while (GetTickCount64() - repeatedStart < 600);
    const auto elapsed = GetTickCount64() - repeatedStart;
    std::cout << "Owner burst: calls=" << ownerCalls << " elapsedMs=" << elapsed
        << " acquisitions=" << acquisitions - requests << '\n';
    expect(acquisitions - requests >= 2 && acquisitions - requests <= 1 + elapsed / 250,
        "repeated owner paints respect shared recovery deadline");
    offered = savedDevice; pump(surface, window, 400);
    expect(CrtSurfaceTestPeer::hasChain(surface), "owner paint burst does not postpone retry indefinitely");
    drawingResumes(surface, window);

    phase = "real DXGI waits";
    // Restore a failed baseline explicitly only after its liveness assertions;
    // keep the independent real-handle cases from cascading that prior failure.
    surface.setActive(false); surface.setActive(true);
    require(CrtSurfaceTestPeer::hasChain(surface), "Restore independent real-handle fixture");
    for (unsigned i = 0; i < 5; ++i) {
        beam(surface); ++recovery; surface.displayEnvironmentChanged(true);
        pump(surface, window, 40); drawingResumes(surface, window);
    }

    surface.shutdown();
    expect(!CrtSurfaceTestPeer::waiting(surface), "shutdown drains readiness");
    pump(surface, window, 10);
    std::cout << (type == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware")
        << ": frames=" << frames << " closes=" << checkedCloses << " violations=" << violations << '\n';
}
}
int main()
{
    HWND window = nullptr;
    try {
        callbackEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        callbackRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(callbackEntered && callbackRelease, "Create callback fixture events");
        window = CreateWindowExW(0, L"STATIC", L"CRT recovery qualification",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 40,40,320,240,
            nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window != nullptr, "Create fixture window");
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
        run(window, D3D_DRIVER_TYPE_HARDWARE);
        run(window, D3D_DRIVER_TYPE_WARP);
        expect(checkedCloses >= 20, "retirement observations must be exercised");
        DestroyWindow(window); window = nullptr;
        CloseHandle(callbackEntered); CloseHandle(callbackRelease);
        std::cout << (violations ? "FAIL" : "PASS") << ": CRT recovery; acquisitions="
            << acquisitions << " checkedCloses=" << checkedCloses << " violations=" << violations << '\n';
        return violations ? 1 : 0;
    } catch (const std::exception& error) {
        SetEvent(callbackRelease);
        if (window) DestroyWindow(window);
        std::cerr << "Fixture error: " << error.what() << '\n';
        return 2;
    }
}
