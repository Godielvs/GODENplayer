#pragma once
#include "collection_audio_history.h"
#include <memory>
#include "collection_graph.h"
#include "visual_refinement_state.h"
#include "c8_visual_policy.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <array>
#include <memory>

namespace nekorox::component::collection {
#ifndef GODEN_SHADER_RENDER_PROCESS
class RemoteFrameClient;
#endif
struct Diagnostics {
    std::uint64_t frame=0,dispatches=0,draws=0,produced=0,drawn=0,accepted=0,resetCount=0,resizeCount=0;
    std::uint64_t bytes=0,queryDrops=0,audioRows=0,audioSequenceApplied=0;
    std::uint64_t audioRowsSkipped=0,audioRestoreFence=0,audioReadCalls=0;
    unsigned width=0,height=0,preset=0;bool ready=false,active=false,deferred=false,budget=false,gpuTimingAvailable=false;
    bool audioRestorePending=false;
    double cpuUs=0,gpuP95Us=0;HRESULT result=S_OK;
    std::uint64_t resizeAttempts=0,rejectedResizes=0;
    HRESULT resizeLastHr=S_OK;
    unsigned pressureTier=0,nominalWidth=0,nominalHeight=0;
    std::uint64_t pressureEvents=0,gpuWindowEpoch=0;
    double pressureResumeSeconds=0.0;
    std::uint64_t explicitRetries=0;
};
// Contains no Runtime/foobar dependency. Production and native probe call this
// exact class. The owner must call it outside a D2D BeginDraw and on the existing
// presentation thread. No immediate-context concurrency, no runtime compiler.
class Renderer final {
#ifndef GODEN_SHADER_RENDER_PROCESS
    std::unique_ptr<RemoteFrameClient> remote_;
    double remotePollAt_=0;
#endif
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    struct Tex {Com<ID3D11Texture2D> resource;Com<ID3D11ShaderResourceView> srv;Com<ID3D11RenderTargetView> rtv;Com<ID3D11UnorderedAccessView> uav;Extent size{};};
    struct Pair {Tex tex[2];unsigned current=0;};
    struct Program {Com<ID3D11PixelShader> ps;Com<ID3D11ComputeShader> cs;};
    struct Query {Com<ID3D11Query> disjoint,begin,end;std::uint64_t epoch=0;bool pending=false;};
    Com<ID3D11Device> device_;Com<ID3D11DeviceContext> context_;
    Com<ID3D11VertexShader> vertex_;Com<ID3D11PixelShader> resample_;Com<ID3D11Buffer> constants_,storage_,photons_;
    Com<ID3D11UnorderedAccessView> storageUav_,photonsUav_;
    Com<ID3D11SamplerState> sampler_,clampSampler_;
    Com<ID3D11RasterizerState> rasterizer_;
    Com<ID3D11BlendState> blend_;Com<ID3D11DepthStencilState> depth_;
    std::array<Pair,8> outputs_{};Tex audio_,noise_,keyboard_,zero_;
    std::array<Program,8> programs_{};
    std::array<Query,6> queries_{};std::array<double,30> samples_{};unsigned sampleCount_=0,warmup_=6;
    Com<ID2D1Bitmap1> bitmap_;std::array<Com<ID2D1Bitmap1>,2> bitmapSides_{};Com<ID2D1DeviceContext> bitmapContext_;
    std::uint64_t bitmapEpoch_=0,resourceEpoch_=1,audioResourceEpoch_=0,lastAudio_=0,lastRestart_=0,audioSerial_=0,audioRecording_=0;
    unsigned audioHead_=0,audioCount_=0;Selection selection_{};Extent extent_{},wanted_{};
    double resizeSince_=0,now_=0,stepTime_=0,stepDelta_=1.0/60.0,outputDelta_=1.0/30.0;
    std::array<double,4> musicalPhase_{};
    C8MotionState c8Motion_;
    StormState storm_;
    RadianceMotionState radianceMotion_;
    SpectrumScrollState spectrumScroll_;
    std::uint32_t stormSeedForProbe_=0;
    double resizeRetryAfter_=0;
    unsigned resizeAttemptCount_=0,resizeFailureProbe_=0;
    unsigned rayCount_=8192;
    // C7: frequency coordinates of the retained FFT rows, not a visual gain.
    // Retain across pause and same-device resize; a new valid source refreshes it.
    float spectrumNyquistHz_=24000.f;
    bool playing_=true,physicsOnly_=false,audioTextureUsed_=false;
    bool initialized_=false,pending_=false,onceDone_=false,wantedActive_=false;
    Timeline timeline_;Diagnostics diag_{};
    // Read-only QA receipt of the constants submitted to the visible Main pass.
    // Retaining CPU data avoids GPU readback, logging, or work on the audio thread.
    Constants submittedConstants_{};
    bool makeTexture(Tex&,Extent,DXGI_FORMAT,bool target,bool uav);
    bool allocate(ID3D11Device*,ID3D11DeviceContext*,const Selection&,Extent,double,Tex* retainedAudio=nullptr);
    bool synchronizeAudioIdentity(AudioIdentity);
    void uploadAudio(const RetainedAudioRows::Batch*,AudioIdentity,std::uint64_t,HistoryApplyReceipt&);
    bool ensureBitmap(ID2D1DeviceContext*,std::uint64_t);
    void step(const AudioFrame*);
    bool resizeTargets(Extent,ID2D1DeviceContext*,std::uint64_t);
    void collect();int queryBegin();void queryEnd(int);
    void recordGpuWindow(double p95Us) noexcept;
    void fail(HRESULT) noexcept;
public:
    Renderer();~Renderer();
    bool prepare(ID3D11Device*,ID3D11DeviceContext*,ID2D1DeviceContext*,std::uint64_t,
        D2D1_SIZE_F,const Selection&,const AudioFrame*,AudioIdentity,
        const RetainedAudioRows::Batch*,std::uint64_t historyGeneration,
        HistoryApplyReceipt*,double now,bool allowWork=true) noexcept;
    bool draw(ID2D1RenderTarget*,D2D1_RECT_F,float opacity=1.f) noexcept;
    void active(bool value,double now) noexcept;
    void transport(bool playing,bool eligible,double now) noexcept;
    void presented(bool accepted)noexcept;
    void retry()noexcept;
    // Native probe only; not exported through window messages or preferences.
    void injectResizeFailureForProbe(unsigned nth)noexcept{resizeFailureProbe_=nth;}
    void setStormSeedForProbe(std::uint32_t seed)noexcept{stormSeedForProbe_=seed;storm_.seed(seed);}
    void invalidateBitmap()noexcept;
    void reset()noexcept;
    bool needsFrame()const noexcept;
    bool due(double now)const noexcept;
    double deadline()const noexcept;
    bool failed()const noexcept{return FAILED(diag_.result);}
    bool producedThisFrame()const noexcept{return producedThisFrame_;}
    const Diagnostics& diagnostics()const noexcept{return diag_;}
    const Constants& submittedConstantsForProbe()const noexcept{return submittedConstants_;}
    std::uint64_t measurementEpoch()const noexcept{return resourceEpoch_;}
    ID3D11Texture2D* intermediateForProbe(unsigned slot)const noexcept{return slot<8?outputs_[slot].tex[outputs_[slot].current].resource.Get():nullptr;}
    ID3D11Texture2D* outputTexture()const noexcept;
    bool responsePendingForProbe()const noexcept;
    bool asynchronous()const noexcept;
private:
 bool producedThisFrame_=false;
};
} // namespace
