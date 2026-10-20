// Test-only cache/notification seam around the actual NativeD2DSurface.
// Real D3D11 hardware and WARP devices, D2D targets and DComp swapchains are
// used; no driver is disabled and the user's player is never touched.
#include "native_d2d_surface.h"
#include "render_device_hub.h"
#include "display_environment.h"
#include "performance_telemetry.h"
#include "device_failure.h"
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace nekorox::component;
namespace {
ComPtr<ID2D1Factory1> factory;
DeviceLease offered;
std::uint64_t recovery = 1, topology = 1;
unsigned acquisitions = 0;
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Graphics HRESULT " + std::to_string(hr)); }
DeviceLease makeDevice(D3D_DRIVER_TYPE driver) {
    DeviceLease result;
    check(D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&result.device,nullptr,&result.immediateContext));
    result.backend = driver == D3D_DRIVER_TYPE_WARP ? SurfaceBackend::warpDxgi : SurfaceBackend::hardwareDxgi;
    return result;
}
void paint(NativeD2DSurface& surface) {
    auto* target=surface.target();target->BeginDraw();target->Clear(D2D1::ColorF(.12f,.25f,.37f));
    const auto result=surface.present(target->EndDraw());
    require(result==FrameResult::accepted || result==FrameResult::notReady || result==FrameResult::occluded,"Paint failed");
}
}
namespace nekorox::component {
RenderDeviceHub& RenderDeviceHub::instance(){static RenderDeviceHub value;return value;}
RenderDeviceHub::~RenderDeviceHub()=default;
ID2D1Factory1* RenderDeviceHub::d2dFactory() noexcept{return factory.Get();}
bool RenderDeviceHub::tryAcquireExisting(HWND,DeviceLease& lease) noexcept {
    ++acquisitions;lease=offered;lease.epoch=recovery;lease.topologyGeneration=topology;return bool(lease);
}
bool RenderDeviceHub::isDeviceCompatibleWithWindow(ID3D11Device* device,HWND) noexcept{return device!=nullptr;}
void RenderDeviceHub::invalidate(ID3D11Device*) noexcept{++recovery;}
std::uint64_t RenderDeviceHub::recoveryEpoch() const noexcept{return recovery;}
DisplayEnvironmentService& DisplayEnvironmentService::instance() noexcept{static DisplayEnvironmentService value;return value;}
std::uint64_t DisplayEnvironmentService::topologyGeneration() const noexcept{return topology;}
PerformanceTelemetry::PerformanceTelemetry() noexcept=default;
PerformanceTelemetry::~PerformanceTelemetry()=default;
PerformanceTelemetry& PerformanceTelemetry::instance() noexcept{static PerformanceTelemetry value;return value;}
std::uint64_t PerformanceTelemetry::beginSpan(TelemetrySurface,std::uint64_t,std::uint64_t) noexcept{return 0;}
void PerformanceTelemetry::endSpan(TelemetrySurface,std::uint64_t,std::uint64_t,std::uint64_t) noexcept{}
}
int main() {
    HWND window=nullptr;
    NativeD2DSurface surface;
    try {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,factory.GetAddressOf()));
        auto warp=makeDevice(D3D_DRIVER_TYPE_WARP), hardware=makeDevice(D3D_DRIVER_TYPE_HARDWARE);
        window=CreateWindowExW(0,L"STATIC",L"Native surface recovery probe",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            40,40,320,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"Create test window");
        const auto nested=CreateWindowExW(0,L"STATIC",L"Nested clipped host",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
            0,0,160,60,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        const auto child=CreateWindowExW(0,L"STATIC",L"Paused panel",WS_CHILD|WS_VISIBLE,
            0,0,80,30,nested,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(nested && child,"Create child fixture");
        ShowWindow(window,SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        ValidateRect(child,nullptr);ValidateRect(nested,nullptr);ValidateRect(window,nullptr);
        InvalidateRect(window,nullptr,FALSE);
        require(!GetUpdateRect(child,nullptr,FALSE),"Control: root-only invalidation should not reach clipped child");
        invalidateGraphicsRecovery(window);
        require(GetUpdateRect(child,nullptr,FALSE)!=FALSE,"Recovery must wake paused descendants");
        offered=warp;require(surface.ensure(window,96),"Create WARP surface");
        require(surface.backend()==SurfaceBackend::warpDxgi,"Initial backend must be WARP");
        paint(surface);const auto originalBinding=surface.bindingKey();
        const auto resource=surface.resourceEpoch();const auto initialAcquisitions=acquisitions;
        for(int i=0;i<500;++i)require(surface.ensure(window,96),"Steady WARP ensure");
        require(acquisitions==initialAcquisitions,"Steady WARP must not poll device cache");
        ++recovery;require(surface.ensure(window,96),"Unrelated recovery with WARP only");
        require(surface.bindingKey()==originalBinding && surface.resourceEpoch()==resource,"Unrelated epoch must keep WARP target");
        const auto probed=acquisitions;
        for(int i=0;i<500;++i)require(surface.ensure(window,96),"WARP stays usable");
        require(acquisitions==probed,"WARP cache miss must not poll per frame");
        // Simulate a paused panel's next paint after the hub publishes hardware.
        // No resize or explicit reset is allowed to rescue the transition.
        offered=hardware;++recovery;
        require(surface.ensure(window,96),"Ensure after hardware recovery");
        require(surface.backend()==SurfaceBackend::hardwareDxgi,"Recovered hardware must replace healthy WARP");
        require(surface.d3dDevice()==hardware.device.Get(),"Use offered hardware device");
        require(surface.bindingKey()!=originalBinding && surface.resourceEpoch()>resource,"Promote invalidates targets/readiness");
        paint(surface);const auto hardwareBinding=surface.bindingKey();const auto stable=acquisitions;
        ++recovery;for(int i=0;i<500;++i)require(surface.ensure(window,96),"Steady hardware ensure");
        require(surface.bindingKey()==hardwareBinding && acquisitions==stable,"Unrelated recovery must not rebuild healthy hardware");
        // A new WARP scene can also recover during continuous presentation.
        surface.reset();offered=warp;++recovery;require(surface.ensure(window,96),"Second WARP scene");
        for(int i=0;i<6;++i){require(surface.ensure(window,96),"WARP playing ensure");paint(surface);}
        offered=hardware;++recovery;require(surface.ensure(window,96),"Playing promotion");
        require(surface.backend()==SurfaceBackend::hardwareDxgi,"Playing hardware promotion");paint(surface);
        const auto resizedBinding=surface.bindingKey();SetWindowPos(window,nullptr,0,0,480,160,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        require(surface.resize(),"Resize recovered target");require(surface.bindingKey()==resizedBinding,"Resize preserves swapchain binding");paint(surface);
        // Display topology may make another cached adapter available without
        // publishing a new recovery epoch (for example after reconnect).
        surface.reset();offered=warp;++recovery;require(surface.ensure(window,96),"Topology WARP scene");
        const auto beforeTopology=surface.bindingKey();offered=hardware;++topology;
        require(surface.ensure(window,96),"Topology-only promotion");
        require(surface.backend()==SurfaceBackend::hardwareDxgi && surface.bindingKey()!=beforeTopology,
            "Topology-only change must promote cached hardware");paint(surface);
        surface.reset();DestroyWindow(window);window=nullptr;
        std::cout<<"Native surface recovery PASS: WARP idle/playing -> hardware; cache miss and hardware stability; resize; topology-only promotion. acquisitions="<<acquisitions<<'\n';
        return 0;
    } catch(const std::exception& error) {
        surface.reset();if(window)DestroyWindow(window);std::cerr<<error.what()<<'\n';return 1;
    }
}
