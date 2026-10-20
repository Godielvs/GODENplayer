#pragma once
#include "curated_core.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace nekorox::component::curated {
struct Diagnostics {
    std::uint64_t referenceFrames=0, simulationDispatches=0, renderedFrames=0;
    std::uint64_t resetCount=0, outputRevision=0, drawnRevision=0, acceptedRevision=0;
    std::uint64_t bytes=0, droppedQueries=0;
    std::uint32_t gridWidth=0,gridHeight=0;
    double gpuP95Us=0, prepareLastUs=0, discardedCatchupSeconds=0;
    HRESULT lastHr=S_OK;
    bool ready=false, budgetPaused=false, active=false, gpuTimingAvailable=false;
};
// Owned by AlbumModeRenderer, called only on the owning presentation thread.
// No independent device, thread, swapchain, decoder or runtime compiler.
class Renderer final {
public:
    bool prepare(ID3D11Device*, ID3D11DeviceContext*, ID2D1DeviceContext*,
        std::uint64_t d2dEpoch, D2D1_SIZE_F viewport, const Input&, double now,
        bool allowWork=true) noexcept;
    bool draw(ID2D1RenderTarget*,const D2D1_RECT_F&,float opacity=1) noexcept;
    void setActivity(bool active,double now) noexcept;
    void presented(bool accepted) noexcept;
    void suspend(double now) noexcept;
    void invalidateBitmap() noexcept;
    void retry() noexcept;
    void reset() noexcept;
    bool needsFrame()const noexcept;
    bool due(double now)const noexcept;
    double deadline()const noexcept;
    bool producedThisFrame()const noexcept{return produced_;}
    bool failed()const noexcept{return FAILED(diagnostics_.lastHr);}
    const Diagnostics& diagnostics()const noexcept{return diagnostics_;}
    ID3D11Texture2D* outputTexture()const noexcept{return output_.texture.Get();}
    // Read only by the standalone diagnostic executable; never read back on UI.
    ID3D11Texture2D* diagnosticStateTexture()const noexcept {
        return state_[preset_==Preset::caustics?1U:stateIndex_].texture.Get();
    }
    ID3D11Buffer* diagnosticPhotonsBuffer()const noexcept {return photons_.Get();}
private:
    using Com=Microsoft::WRL::ComPtr<IUnknown>;
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Texture {Ptr<ID3D11Texture2D> texture;Ptr<ID3D11ShaderResourceView> srv;Ptr<ID3D11UnorderedAccessView> uav;};
    struct Query {Ptr<ID3D11Query> disjoint,start,end;std::uint64_t epoch=0;bool pending=false;};
    bool create(ID3D11Device*,ID3D11DeviceContext*,Preset,Mode,float,double);
    bool texture(Texture&,DXGI_FORMAT,std::uint32_t,std::uint32_t);
    bool bitmap(ID2D1DeviceContext*,std::uint64_t);
    bool upload(const Input&);
    void bindAndDispatch(unsigned shader,ID3D11ShaderResourceView* state,
        ID3D11UnorderedAccessView* target,ID3D11UnorderedAccessView* image);
    void step(const Input&);
    void resolve(const Input&,unsigned steps);
    void collectQueries()noexcept;
    int beginQuery()noexcept;
    void endQuery(int)noexcept;
    void error(HRESULT,double)noexcept;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11ComputeShader> shader_[9];
    Ptr<ID3D11Buffer> constants_,photons_;
    Ptr<ID3D11UnorderedAccessView> photonsUav_;
    Ptr<ID3D11SamplerState> repeat_;
    Texture state_[2],raw_,output_;
    Ptr<ID2D1Bitmap1> bitmap_;
    Ptr<ID2D1DeviceContext> bitmapContext_;
    std::uint64_t bitmapEpoch_=0;
    std::array<Query,6> queries_{};
    std::array<double,30> timings_{};
    unsigned timingCount_=0,warmup_=8,expensiveWindows_=0;
    bool queryAvailable_=true;
    double budgetPauseUntil_=0;
    Extent extent_{};
    Preset preset_=Preset::caustics;
    Mode allocationMode_=Mode::music;
    Input lastInput_{};
    std::array<collection::BandSlew,3> musicalSlew_{};
    double lastSlew_=0;
    Timeline timeline_{};
    std::uint64_t frame_=0,lastRestart_=0;
    std::uint64_t queryEpoch_=1;
    unsigned stateIndex_=0;
    bool initialized_=false,produced_=false,pending_=false,activeWanted_=false;
    unsigned failures_=0;
    double retryAt_=0,lastNow_=0;
    Diagnostics diagnostics_{};
};
} // namespace
