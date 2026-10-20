#pragma once
#include "a4_effect.h"
#include "a4_gpu_timing.h"
#include "clovis_response.h"
#include "album_ambient_controller.h"
#include <memory>
#include <chrono>
#include <wrl/client.h>

namespace nekorox::component::a4 {
struct SceneGeometry {
    D2D1_RECT_F cover{}, tracks{}, lyrics{};
    D2D1_RECT_F bookletAlbum{}, bookletWritten{};
    float bookletProgress=0.0f;
    float coverOpacity=1.0f;
    bool coverOptics=true;
    std::uint64_t coverRevision=0;
};
class Pipeline final {
public:
    struct Diagnostics {
        std::uint64_t fieldUpdates=0, sceneCompositions=0, resourceBytes=0;
        std::uint64_t fieldRevision=0, reusedScenes=0;
        HRESULT lastHr=S_OK;
        bool ready=false, low=false, fieldProduced=false, latched=false;
        double gpuFieldP95Us=0.0, gpuInclusiveP95Us=0.0;
        double gpuDecorativeP95Us=0.0, gpuBlurP95Us=0.0, gpuSceneP95Us=0.0, gpuFinishP95Us=0.0;
        double cpuStageSumUs=0.0;
        std::uint64_t budgetEpoch=1,budgetSamples=0,budgetWindows=0,queryDrops=0,queryDisjoint=0;
        std::uint64_t filmUpdates=0,finishedReuses=0;
        unsigned effectiveTier=0;
        bool measured=false;
        // Legacy inclusive field reports GPU-timeline p95, not A4 cost.
        double cpuPrepareLastUs=0.0;
    };
    // Both prepare() and compose() run OUTSIDE the owner's BeginDraw/EndDraw.
    // They restore target, DPI, antialias state and transform on every exit.
    void beginMeasurement(ID3D11Device*, ID3D11DeviceContext*, bool eligible);
    void endMeasurement() noexcept;
    bool setQualityMode(QualityMode mode) noexcept;
    bool prepare(ID2D1DeviceContext*, std::uint64_t resourceEpoch,
        D2D1_SIZE_F viewportDip, float dpi,
        const std::shared_ptr<const Material>&, const AlbumAmbientPose&,
        bool updateField, bool low, float grainAmplitude, double nowSeconds) noexcept;
    using CoverDraw = bool(*)(void*,ID2D1DeviceContext*,const D2D1_RECT_F&,float);
    bool compose(ID2D1DeviceContext*, ID2D1Bitmap* cover,
        const SceneGeometry&, bool coverTransitionActive,
        void* transitionOwner, CoverDraw drawTransition);
    // Only this call runs INSIDE the owner's BeginDraw. No retargeting.
    bool drawToOwner(ID2D1DeviceContext*) noexcept;
    bool pointer(float normalizedX, float normalizedY, double nowSeconds) noexcept;
    [[nodiscard]] bool transitionActive(double nowSeconds) const noexcept;
    [[nodiscard]] const Diagnostics& diagnostics() const noexcept {return diagnostics_;}
    void retry() noexcept;
    bool resourcesPending() const noexcept {return resourcesPending_;}
    // Non-persistent diagnostic cap: can only REMOVE work, never bypass a budget.
    bool diagnosticCap(unsigned tier) noexcept {
        if(tier<1U||tier>2U)return false;
        const bool changed=budget_.requireAtLeast(static_cast<QualityTier>(tier));
        if(changed){fieldReady_=sceneValid_=finishedValid_=false;publishBudget();}
        return changed;
    }
    unsigned retryMilliseconds(double now) const noexcept {
        if(retry_.latched||budget_.latched()||diagnostics_.ready||!std::isfinite(now))return 0;
        return static_cast<unsigned>(std::clamp(std::ceil((retry_.notBefore-now)*1000.0),1.0,2000.0));
    }
    void reset() noexcept; // GPU only; phase/material/grain epoch survive
private:
    bool ensure(ID2D1DeviceContext*, D2D1_SIZE_F, float, bool) noexcept;
    Constants constants() const noexcept;
    HRESULT draw(ID2D1DeviceContext*, Pass, const Constants&) noexcept;
    HRESULT pane(ID2D1DeviceContext*, D2D1_RECT_F, float opacity) noexcept;
    bool finishScene(ID2D1DeviceContext*);
    void publishBudget() noexcept;
    void fail(HRESULT, double now) noexcept;
    Material sampleMaterial(double now) const noexcept;
    void samplePointer(double now) noexcept;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    std::array<Microsoft::WRL::ComPtr<ID2D1Effect>,5> effect_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> black_,placeholder_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> field_,scene_,detail_;
    std::shared_ptr<const Material> material_;
    Material sourceMaterial_{},renderMaterial_{};
    std::uint64_t epoch_=0,detailHash_=~std::uint64_t{0},grainFrame_=0,materialIdentity_=0;
    std::uint64_t sceneFieldRevision_=0,sceneGrainRevision_=0;
    D2D1_SIZE_F viewport_{};
    float scale_=1.0f,grainAmplitude_=0.0f;
    float pointerX_=0.0f,pointerY_=0.0f,pointerFromX_=0.0f,pointerFromY_=0.0f;
    float pointerTargetX_=0.0f,pointerTargetY_=0.0f;
    unsigned width_=0,height_=0;
    Fit fieldSize_{};
    AlbumAmbientPose pose_{};
    ClovisResponse clovisResponse_{};
    SceneGeometry sceneGeometry_{};
    double materialStarted_=-1.0,lastBuild_=-1.0,now_=0.0,pointerStarted_=-1.0;
    bool fieldReady_=false,sceneValid_=false,materialAdmitted_=false;
    bool materialBlend_=false,sceneWasBooklet_=false,sceneHasContent_=false;
    bool coldFrame_=true,fieldBuiltThisFrame_=false,finishedValid_=false;
    bool resourcesPending_=false;
    // Full is the installed default. Reduced tiers are explicit user choices;
    // GPU timings are diagnostic and never downgrade the shader automatically.
    DecorativeBudget budget_{false};
    QualityMode qualityMode_=QualityMode::full;
    Retry retry_{};
    GpuTiming timing_;
    Diagnostics diagnostics_{};
};
} // namespace nekorox::component::a4
