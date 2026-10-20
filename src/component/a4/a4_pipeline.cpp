#include "a4_pipeline.h"
#include "a4_preferences.h"
#include <d2d1effects.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace nekorox::component::a4 {
using Microsoft::WRL::ComPtr;
namespace {
struct Restore {
    ID2D1DeviceContext* c;
    ComPtr<ID2D1Image> target;
    float x=96,y=96;
    D2D1_MATRIX_3X2_F transform{};
    D2D1_ANTIALIAS_MODE aa{};
    explicit Restore(ID2D1DeviceContext* p):c(p) {
        c->GetTarget(target.GetAddressOf());c->GetDpi(&x,&y);
        c->GetTransform(&transform);aa=c->GetAntialiasMode();
    }
    ~Restore(){c->SetTarget(target.Get());c->SetDpi(x,y);c->SetTransform(transform);c->SetAntialiasMode(aa);}
};
HRESULT bitmap(ID2D1DeviceContext* c,ComPtr<ID2D1Bitmap1>& out,
    unsigned w,unsigned h,DXGI_FORMAT format)
{
    if(out && out->GetPixelSize().width==w && out->GetPixelSize().height==h &&
        out->GetPixelFormat().format==format)return S_FALSE;
    const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(format,D2D1_ALPHA_MODE_PREMULTIPLIED),96.0f,96.0f);
    ComPtr<ID2D1Bitmap1> next;
    const auto hr=c->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,&properties,next.GetAddressOf());
    if(SUCCEEDED(hr))out=std::move(next);
    return hr;
}
D2D1_RECT_F physical(D2D1_RECT_F r,float s){return {r.left*s,r.top*s,r.right*s,r.bottom*s};}
Float4 rect4(D2D1_RECT_F r){return {r.left,r.top,r.right,r.bottom};}
unsigned idx(Pass p){return static_cast<unsigned>(p);}
bool sameRect(D2D1_RECT_F a,D2D1_RECT_F b){return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;}
bool sameScene(const SceneGeometry& a,const SceneGeometry& b){
    return sameRect(a.cover,b.cover)&&sameRect(a.tracks,b.tracks)&&sameRect(a.lyrics,b.lyrics)&&
        sameRect(a.bookletAlbum,b.bookletAlbum)&&sameRect(a.bookletWritten,b.bookletWritten)&&
        a.bookletProgress==b.bookletProgress&&a.coverOpacity==b.coverOpacity&&
        a.coverOptics==b.coverOptics&&a.coverRevision==b.coverRevision;
}
std::uint64_t bytes(ID2D1Bitmap* p,unsigned bpp){if(!p)return 0;auto s=p->GetPixelSize();return std::uint64_t(s.width)*s.height*bpp;}
goden::c11::ArtProfile alloyProfile(const Material& material) noexcept {
    if(material.alloyV2.valid)return material.alloyV2;
    goden::c11::ArtProfile profile;
    profile.valid=true;profile.count=3;profile.meanLuma=material.meanLuma;
    profile.contrast=material.contrast;profile.roughness=material.roughness;
    profile.hardness=std::clamp(1.f-material.roughness,0.f,1.f);
    profile.anisotropy=material.anisotropy;profile.orientation=material.orientation;
    constexpr float weights[3]{.48f,.32f,.20f};
    for(unsigned i=0;i<3;++i){const auto color=material.colors[i];
        profile.pigments[i].linear={color.x,color.y,color.z};
        profile.pigments[i].broadWeight=weights[i];
        profile.pigments[i].accentWeight=weights[i];
        profile.pigments[i].area=weights[i];}
    return profile;
}
}

void Pipeline::beginMeasurement(ID3D11Device* device,ID3D11DeviceContext* context,bool eligible)
{
    timing_.beginFrame(device,context,eligible&&!budget_.latched()&&!retry_.latched,budget_.epoch());
    fieldBuiltThisFrame_=false;coldFrame_=false;
}
void Pipeline::publishBudget() noexcept
{
    const auto& s=budget_.snapshot();
    diagnostics_.effectiveTier=static_cast<unsigned>(s.tier);
    diagnostics_.budgetEpoch=s.epoch;diagnostics_.budgetSamples=s.accepted;
    diagnostics_.budgetWindows=s.completedWindows;
    diagnostics_.gpuDecorativeP95Us=s.decorativeP95Us;
    diagnostics_.gpuFieldP95Us=s.stageP95Us[0];diagnostics_.gpuBlurP95Us=s.stageP95Us[1];
    diagnostics_.gpuSceneP95Us=s.stageP95Us[2];diagnostics_.gpuFinishP95Us=s.stageP95Us[3];
    diagnostics_.measured=s.completedWindows!=0 && s.decorativeP95Us>0;
    diagnostics_.low=s.tier!=QualityTier::full;
    diagnostics_.latched=budget_.latched()||retry_.latched;
    diagnostics_.queryDrops=timing_.dropped();diagnostics_.queryDisjoint=timing_.disjoint();
}
void Pipeline::endMeasurement() noexcept
{
    timing_.endFrame();
    QualifiedGpuSample sample;
    while(timing_.pop(sample)) {

        diagnostics_.cpuStageSumUs=0;
        for(const auto value:sample.cpuUs)diagnostics_.cpuStageSumUs+=value;
        if(budget_.submit(sample)) {
            fieldReady_=sceneValid_=finishedValid_=false;
            // No full-tier query can count in the new quality epoch.
        }
    }
    diagnostics_.gpuInclusiveP95Us=timing_.inclusiveP95Us();
    publishBudget();
}
void Pipeline::retry() noexcept
{
    retry_={};fieldReady_=sceneValid_=finishedValid_=false;
    budget_.restart(true);timing_.reset();
    diagnostics_.gpuInclusiveP95Us=0;publishBudget();
}
void Pipeline::fail(HRESULT value,double now) noexcept
{
    diagnostics_.lastHr=value;diagnostics_.ready=false;
    diagnostics_.fieldProduced=false;sceneValid_=false;fieldReady_=false;finishedValid_=false;
    timing_.qualify(false,0);
    retry_.fail(now);diagnostics_.latched=retry_.latched;
}

bool Pipeline::ensure(ID2D1DeviceContext* c,D2D1_SIZE_F size,float dpi,bool low) noexcept
{
    if(!c||!(size.width>0)||!(size.height>0)||!std::isfinite(size.width)||
        !std::isfinite(size.height)||!std::isfinite(dpi)||dpi<48||dpi>768)return false;
    const auto scale=dpi/96.f;
    const double wd=std::ceil(double(size.width)*scale),hd=std::ceil(double(size.height)*scale);
    if(wd<1||hd<1||wd>8192||hd>8192||wd*hd>16777216)return false;
    const auto w=static_cast<unsigned>(wd),h=static_cast<unsigned>(hd);
    const auto wanted=fit(w,h,low);
    const bool layoutChanged=width_!=w||height_!=h||scale_!=scale||
        viewport_.width!=size.width||viewport_.height!=size.height;
    if(layoutChanged){budget_.restart(false);coldFrame_=true;finishedValid_=false;sceneHasContent_=false;sceneValid_=false;fieldReady_=false;sceneWasBooklet_=false;}
    if(wanted.width!=fieldSize_.width||wanted.height!=fieldSize_.height)fieldReady_=false;
    viewport_=size;scale_=scale;width_=w;height_=h;fieldSize_=wanted;
    resourcesPending_=false;
    for(unsigned i=0;i<effect_.size();++i){
        if(!effect_[i]) {
            diagnostics_.lastHr=createEffect(c,static_cast<Pass>(i),effect_[i].GetAddressOf());
            if(FAILED(diagnostics_.lastHr)) return false;
            // One shader registration/load per UI turn; never publish a partial scene.
            resourcesPending_=true;
            return false;
        }
    }
    if(!black_&&FAILED(diagnostics_.lastHr=c->CreateSolidColorBrush(D2D1::ColorF(0.f,0.f,0.f,1.f),black_.GetAddressOf())))return false;
    if(!placeholder_&&FAILED(diagnostics_.lastHr=c->CreateSolidColorBrush(D2D1::ColorF(.12f,.13f,.13f,1.f),placeholder_.GetAddressOf())))return false;
    diagnostics_.lastHr=bitmap(c,field_,wanted.width,wanted.height,DXGI_FORMAT_R16G16B16A16_FLOAT);
    if(FAILED(diagnostics_.lastHr))return false;
    if(diagnostics_.lastHr==S_OK){resourcesPending_=true;return false;}
    diagnostics_.lastHr=bitmap(c,scene_,w,h,DXGI_FORMAT_B8G8R8A8_UNORM);
    if(FAILED(diagnostics_.lastHr))return false;
    if(diagnostics_.lastHr==S_OK){resourcesPending_=true;return false;}
    diagnostics_.low=low;
    diagnostics_.resourceBytes=bytes(field_.Get(),8)+bytes(scene_.Get(),4)+bytes(detail_.Get(),4);
    return true;
}

Material Pipeline::sampleMaterial(double now) const noexcept
{
    Material result=material_?*material_:Material{};
    if(materialBlend_){
        const float t=smooth(0.f,.62f,float(now-materialStarted_));
        for(unsigned i=0;i<4;++i)result.colors[i]=mix(sourceMaterial_.colors[i],result.colors[i],t);
        result.roughness=sourceMaterial_.roughness+(result.roughness-sourceMaterial_.roughness)*t;
        result.anisotropy=sourceMaterial_.anisotropy+(result.anisotropy-sourceMaterial_.anisotropy)*t;
        result.dispersion=sourceMaterial_.dispersion+(result.dispersion-sourceMaterial_.dispersion)*t;
        // Orientation is pi-periodic; take its short arc.
        result.orientation=sourceMaterial_.orientation+
            std::remainder(result.orientation-sourceMaterial_.orientation,3.14159265358979323846f)*t;
        const auto from=alloyProfile(sourceMaterial_);
        const auto to=alloyProfile(result);
        auto blended=to;blended.count=std::max(from.count,to.count);
        const auto blend=[](float a,float b,float amount){return a+(b-a)*amount;};
        blended.edgeDensity=blend(from.edgeDensity,to.edgeDensity,t);
        blended.contrast=blend(from.contrast,to.contrast,t);
        blended.orientation=from.orientation+
            std::remainder(to.orientation-from.orientation,3.14159265358979323846f)*t;
        blended.anisotropy=blend(from.anisotropy,to.anisotropy,t);
        blended.meanLuma=blend(from.meanLuma,to.meanLuma,t);
        blended.neutralFraction=blend(from.neutralFraction,to.neutralFraction,t);
        blended.roughness=blend(from.roughness,to.roughness,t);
        blended.hardness=blend(from.hardness,to.hardness,t);
        for(std::size_t i=0;i<blended.pigments.size();++i){
            const auto& a=from.pigments[i%std::max<std::uint32_t>(1,from.count)];
            const auto& b=to.pigments[i%std::max<std::uint32_t>(1,to.count)];
            auto& p=blended.pigments[i];
            p.linear={blend(a.linear.x,b.linear.x,t),blend(a.linear.y,b.linear.y,t),
                blend(a.linear.z,b.linear.z,t)};
            p.area=blend(a.area,b.area,t);p.broadWeight=blend(a.broadWeight,b.broadWeight,t);
            p.accentWeight=blend(a.accentWeight,b.accentWeight,t);
            p.salience=blend(a.salience,b.salience,t);
        }
        result.alloyV2=blended;
    }
    return result;
}
void Pipeline::samplePointer(double now) noexcept
{
    const auto t=pointerStarted_<0?1.f:smooth(0.f,.10f,float(now-pointerStarted_));
    pointerX_=pointerFromX_+(pointerTargetX_-pointerFromX_)*t;
    pointerY_=pointerFromY_+(pointerTargetY_-pointerFromY_)*t;
}
bool Pipeline::pointer(float x,float y,double now) noexcept
{
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(now))return false;
    x=std::clamp(x,-1.f,1.f);y=std::clamp(y,-1.f,1.f);
    if(x==pointerTargetX_&&y==pointerTargetY_)return false;
    samplePointer(now);pointerFromX_=pointerX_;pointerFromY_=pointerY_;
    pointerTargetX_=x;pointerTargetY_=y;pointerStarted_=now;sceneValid_=false;
    return true;
}
bool Pipeline::transitionActive(double now) const noexcept
{
    if(!diagnostics_.ready||retry_.latched||budget_.latched())return false;
    return (materialBlend_&&now-materialStarted_<.62)||
        (pointerStarted_>=0&&now-pointerStarted_<.10);
}
Constants Pipeline::constants() const noexcept
{
    Constants c{};
    c.viewport={viewport_.width,viewport_.height,scale_,float(grainFrame_%1048576)};
    c.output={float(width_),float(height_),0,0};
    c.levels={pose_.drive[0],pose_.drive[1],pose_.drive[2],pose_.gain};
    c.hits={pose_.transient[0],pose_.transient[1],pose_.transient[2],0};
    c.phase={pose_.phase[0],pose_.phase[1],pose_.phase[2],float((materialIdentity_%1000003)*.000006283)};
    c.color0=vec(renderMaterial_.colors[0]);c.color1=vec(renderMaterial_.colors[1]);
    c.color2=vec(renderMaterial_.colors[2]);c.color3=vec(renderMaterial_.colors[3]);
    c.material={renderMaterial_.roughness,renderMaterial_.anisotropy,renderMaterial_.dispersion,renderMaterial_.detailMean};
    c.pointer={pointerX_,pointerY_,renderMaterial_.orientation,
        static_cast<float>(materialStyle())};
    c.tuning={grainAmplitude_,14,diagnostics_.low?1.8f:3.0f,1};
    if(pose_.lights.valid){
        for(unsigned i=0;i<2;++i){
            const unsigned b=i*4;
            c.lightLevel[i]={pose_.lights.level[b],pose_.lights.level[b+1],
                pose_.lights.level[b+2],pose_.lights.level[b+3]};
            c.lightTexture[i]={pose_.lights.texture[b],pose_.lights.texture[b+1],
                pose_.lights.texture[b+2],pose_.lights.texture[b+3]};
        }
        c.lightControl={std::clamp(pose_.lights.master,0.f,1.f),1.f,0.f,0.f};
    }
    c.lightControl.z=float(fieldSize_.width);c.lightControl.w=float(fieldSize_.height);
    const auto profile=alloyProfile(renderMaterial_);
    c.alloyV2=goden::c11::makeGpu(profile,pose_.alloyNotes,
        float(fieldSize_.width)/std::max(1.f,float(fieldSize_.height)),now_,
        pose_.alloyFreshness,pose_.alloyBroad.level,pose_.alloyBroad.texture,
        pose_.alloyBroad.master);
    if(materialStyle()==MaterialStyle::spectralAlloyV2){
        clovisResponse_.apply(c.alloyV2);
        const auto pigment=clovisDominantPigment(profile);
        // C12-only payload: accent[0].xyz is the emitting dominant pigment.
        // Other materials keep the original packing and shared analysis.
        c.alloyV2.accent[0].x=pigment.x;c.alloyV2.accent[0].y=pigment.y;
        c.alloyV2.accent[0].z=pigment.z;
        c.output.z=clovisResponse_.calmTexture();
    }
    // Refined C12 uses the current artwork and validated, gap-safe analysis feed.
    return c;
}
HRESULT Pipeline::draw(ID2D1DeviceContext* c,Pass pass,const Constants& params) noexcept
{
    auto* effect=effect_[idx(pass)].Get();
    const auto hr=updateEffect(effect,params);
    if(FAILED(hr))return hr;
    if(pass==Pass::glass||pass==Pass::cover){
        ComPtr<ID2D1Image> output;effect->GetOutput(output.GetAddressOf());
        const D2D1_RECT_F source{params.rect.x,params.rect.y,params.rect.z,params.rect.w};
        const D2D1_POINT_2F origin{source.left,source.top};
        c->DrawImage(output.Get(),&origin,&source,D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_SOURCE_OVER);
    }else c->DrawImage(effect);
    return S_OK; // success of the command; callers still MUST check EndDraw.
}

bool Pipeline::prepare(ID2D1DeviceContext* c,std::uint64_t epoch,D2D1_SIZE_F size,float dpi,
    const std::shared_ptr<const Material>& material,const AlbumAmbientPose& pose,
    bool requested,bool low,float grain,double now) noexcept
{
    diagnostics_.fieldProduced=false;
    const auto cpuBegin=std::chrono::steady_clock::now();
    struct CpuGuard {double& output;std::chrono::steady_clock::time_point start;
        ~CpuGuard(){output=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();}
    } cpuGuard{diagnostics_.cpuPrepareLastUs,cpuBegin};
    resourcesPending_=false;
    if(!c||!std::isfinite(now))return false;
    if(context_.Get()!=c||epoch_!=epoch){reset();context_=c;epoch_=epoch;}
    now_=now;
    if(budget_.latched()||!retry_.due(now))return false;
    low=low||budget_.tier()!=QualityTier::full;
    try{
        if(!ensure(c,size,dpi,low)){if(!resourcesPending_)fail(FAILED(diagnostics_.lastHr)?diagnostics_.lastHr:E_INVALIDARG,now);return false;}
        const auto wanted=material&&material->valid?material:std::shared_ptr<const Material>{};
        const auto identity=wanted?(wanted->hash ^
            (wanted->contentRevision*0x9e3779b97f4a7c15ULL)):0;
        if(!materialAdmitted_||identity!=materialIdentity_){
            coldFrame_=true;
            const bool had=materialAdmitted_;
            sourceMaterial_=sampleMaterial(now);
            material_=wanted;materialIdentity_=identity;
            materialAdmitted_=true;materialStarted_=now;materialBlend_=had;
            fieldReady_=false;sceneValid_=false;
        }
        const auto oldX=pointerX_,oldY=pointerY_;
        samplePointer(now);if(oldX!=pointerX_||oldY!=pointerY_)sceneValid_=false;
        const float effectiveGrain=grainAmplitudeForTier(budget_.tier(),grain);
        if(grainAmplitude_!=effectiveGrain){grainAmplitude_=effectiveGrain;finishedValid_=false;sceneValid_=false;}
        const bool blendCompleted=materialBlend_&&now-materialStarted_>=.62;
        if(blendCompleted)materialBlend_=false;
        const bool blendDue=materialBlend_ && (lastBuild_<0||now-lastBuild_>=1.0/30.0);
        if(!requested&&fieldReady_&&!blendDue&&!blendCompleted)return true;
        renderMaterial_=sampleMaterial(now);
        pose_=pose; // optics uses the exact pose of the retained light field
        if(materialStyle()==MaterialStyle::spectralAlloyV2)
            clovisResponse_.advance(pose.alloyNotes,pose.alloyBroad,pose.alloyFreshness,now);
        if(pose.availability>1.f/4096.f)++grainFrame_;
        if(!detail_||detailHash_!=identity){
            std::array<std::uint8_t,64*64*4> pixels{};
            for(unsigned i=0;i<4096;++i){pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=renderMaterial_.detail[i];pixels[i*4+3]=255;}
            auto p=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            ComPtr<ID2D1Bitmap1> next;
            const auto hr=c->CreateBitmap(D2D1::SizeU(64,64),pixels.data(),256,&p,next.GetAddressOf());
            if(FAILED(hr)){fail(hr,now);return false;}
            detail_=std::move(next);detailHash_=identity;
        }
        Restore saved(c);c->SetDpi(96,96);c->SetTransform(D2D1::Matrix3x2F::Identity());
        auto p=constants();p.output={float(fieldSize_.width),float(fieldSize_.height),0,0};
        timing_.beginField();
        c->SetTarget(field_.Get());c->BeginDraw();c->Clear(D2D1::ColorF(0.f,0.f,0.f,1.f));
        auto hr=draw(c,Pass::field,p);const auto end=c->EndDraw();
        if(SUCCEEDED(hr))hr=end;
        timing_.endField();
        if(FAILED(hr)){fail(hr,now);return false;}
        fieldBuiltThisFrame_=true;
        fieldReady_=true;sceneValid_=finishedValid_=false;lastBuild_=now;
        diagnostics_.ready=true;diagnostics_.fieldProduced=true;diagnostics_.lastHr=S_OK;
        ++diagnostics_.fieldRevision;++diagnostics_.fieldUpdates;retry_.success();
        return true;
    }catch(...){fail(E_OUTOFMEMORY,now);return false;}
}
HRESULT Pipeline::pane(ID2D1DeviceContext* c,D2D1_RECT_F r,float opacity) noexcept
{
    if(r.right<=r.left||r.bottom<=r.top||opacity<=0)return S_OK;
    if(budget_.tier()==QualityTier::fieldOnly) {
        black_->SetOpacity(.58f*std::clamp(opacity,0.f,1.f));
        c->FillRectangle(physical(r,scale_),black_.Get());black_->SetOpacity(1.0f);
        return S_OK;
    }
    auto p=constants();p.rect=rect4(physical(r,scale_));
    p.input0={float(fieldSize_.width),float(fieldSize_.height),0,0};p.tuning.w=std::clamp(opacity,0.f,1.f);
    // Spectral Alloy panes sample the crisp field directly.  There is no
    // Gaussian prefilter anywhere in the normal or reduced-quality path.
    effect_[idx(Pass::glass)]->SetInput(0,field_.Get());
    return draw(c,Pass::glass,p);
}
bool Pipeline::compose(ID2D1DeviceContext* c,ID2D1Bitmap* cover,
    const SceneGeometry& geometry,bool coverTransitionActive,
    void* transitionOwner,CoverDraw transition)
{
    if(!c||!fieldReady_||!scene_)return false;
    const bool booklet=geometry.bookletProgress>.001f;
    const bool opening=booklet&&!sceneWasBooklet_;
    sceneWasBooklet_=booklet;
    if(sceneValid_&&!coverTransitionActive&&!opening&&sameScene(sceneGeometry_,geometry)&&
        sceneFieldRevision_==diagnostics_.fieldRevision&&sceneGrainRevision_==grainFrame_){
        ++diagnostics_.reusedScenes;
        timing_.qualify(false,0);
        return finishScene(c);
    }
    timing_.beginStage(GpuStage::scene);
    Restore saved(c);c->SetTarget(scene_.Get());c->SetDpi(96,96);
    c->SetTransform(D2D1::Matrix3x2F::Identity());c->BeginDraw();
    c->Clear(D2D1::ColorF(0.f,0.f,0.f,1.f));
    // The background is evaluated at the native target resolution.  This
    // avoids upscaling a small field texture and confines film grain to this
    // draw, before glass, cover and typography are composited.
    auto p=constants();
    p.input0={float(fieldSize_.width),float(fieldSize_.height),0,0};
    effect_[idx(Pass::encode)]->SetInput(0,field_.Get());
    auto hr=draw(c,Pass::encode,p);
    if(SUCCEEDED(hr))hr=pane(c,geometry.tracks,1.0f-geometry.bookletProgress);
    if(SUCCEEDED(hr))hr=pane(c,geometry.lyrics,1.0f-geometry.bookletProgress);
    if(booklet){
        const float amount=std::clamp(geometry.bookletProgress,0.f,1.f);
        const auto paneOpacity=std::clamp((amount-.45f)/.55f,0.f,1.f);
        if(SUCCEEDED(hr))hr=pane(c,geometry.bookletAlbum,paneOpacity);
        if(SUCCEEDED(hr))hr=pane(c,geometry.bookletWritten,paneOpacity);
    }
    bool coverDone=false;
    if(coverTransitionActive&&transition&&geometry.coverOpacity>.999f){
        // Existing transition expects DIP geometry and the actual output DPI.
        c->SetDpi(scale_*96,scale_*96);
        coverDone=transition(transitionOwner,c,geometry.cover,geometry.coverOpacity);
        c->SetDpi(96,96);c->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    if(!coverDone&&cover&&geometry.coverOpacity>0 &&
            (budget_.tier()!=QualityTier::full || !geometry.coverOptics)) {
        const auto destination=physical(geometry.cover,scale_);
        c->DrawBitmap(cover,&destination,std::clamp(geometry.coverOpacity,0.f,1.f),D2D1_INTERPOLATION_MODE_LINEAR);
        coverDone=true;
    }
    if(!coverDone&&cover&&geometry.coverOpacity>0){
        p=constants();p.rect=rect4(physical(geometry.cover,scale_));
        const auto dip=cover->GetSize();const auto pixels=cover->GetPixelSize();
        p.input0={dip.width,dip.height,float(pixels.width),float(pixels.height)};
        p.input1={64,64,0,0};p.tuning.w=geometry.coverOptics?1.f:0.f;
        effect_[idx(Pass::cover)]->SetInput(0,cover);effect_[idx(Pass::cover)]->SetInput(1,detail_.Get());
        effect_[idx(Pass::cover)]->SetInput(2,field_.Get());
        // Opacity is applied to the result, not to the source sampled as a lens.
        const auto lp=D2D1::LayerParameters(physical(geometry.cover,scale_),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),std::clamp(geometry.coverOpacity,0.f,1.f));
        const bool translucent=geometry.coverOpacity<.999f;
        if(translucent)c->PushLayer(lp,nullptr);
        if(SUCCEEDED(hr))hr=draw(c,Pass::cover,p);
        if(translucent)c->PopLayer();
    }else if(!coverDone&&!cover){
        const auto r=physical(geometry.cover,scale_);
        if(r.right>r.left&&r.bottom>r.top)c->FillRectangle(r,placeholder_.Get());
    }
    const auto end=c->EndDraw();if(SUCCEEDED(hr))hr=end;
    timing_.endStage(GpuStage::scene);
    if(FAILED(hr)){fail(hr,now_);return false;}
    sceneGeometry_=geometry;sceneFieldRevision_=diagnostics_.fieldRevision;sceneGrainRevision_=grainFrame_;
    sceneHasContent_=true;sceneValid_=true;finishedValid_=false;
    diagnostics_.lastHr=S_OK;++diagnostics_.sceneCompositions;
    if(grainAmplitude_>0)++diagnostics_.filmUpdates;
    diagnostics_.resourceBytes=bytes(field_.Get(),8)+bytes(scene_.Get(),4)+bytes(detail_.Get(),4);
    const bool finished=finishScene(c);
    unsigned required=stageBit(GpuStage::field)|stageBit(GpuStage::scene);
    timing_.qualify(finished && fieldBuiltThisFrame_ && !coldFrame_ && !materialBlend_ &&
        !coverTransitionActive && !opening && geometry.bookletProgress<=.001f,required);
    return finished;
}
bool Pipeline::finishScene(ID2D1DeviceContext* c)
{
    (void)c;
    // Grain is now part of the background draw.  Keeping a post-composite
    // finish pass would put noise over artwork and glass again.
    finishedValid_=false;
    return sceneValid_&&scene_;
}
bool Pipeline::drawToOwner(ID2D1DeviceContext* c) noexcept
{
    if(!c||!sceneValid_||!scene_)return false;
    D2D1_MATRIX_3X2_F old{};c->GetTransform(&old);
    c->SetTransform(D2D1::Matrix3x2F::Scale(1.f/scale_,1.f/scale_)*old);
    // The complete cached scene already contains background-only grain.
    // Scroll/lyrics repaint copies it without re-running a native shader.
    c->DrawBitmap(scene_.Get());
    c->SetTransform(old);
    return true;
}
void Pipeline::reset() noexcept
{
    for(auto& e:effect_)e.Reset();
    black_.Reset();placeholder_.Reset();
    timing_.reset();budget_.restart(false);
    field_.Reset();scene_.Reset();detail_.Reset();context_.Reset();
    epoch_=0;detailHash_=~std::uint64_t{0};width_=height_=0;fieldSize_={};
    fieldReady_=sceneValid_=sceneWasBooklet_=sceneHasContent_=false;retry_={};
    diagnostics_.ready=false;diagnostics_.latched=false;diagnostics_.resourceBytes=0;
    finishedValid_=false;coldFrame_=true;fieldBuiltThisFrame_=false;
    publishBudget();
    // No reset of material admission, grain epoch, pointer or CPU signal.
}
bool Pipeline::setQualityMode(QualityMode mode) noexcept
{
    const auto raw=static_cast<unsigned>(mode);
    if(raw>static_cast<unsigned>(QualityMode::fieldOnly))mode=QualityMode::full;
    if(mode==QualityMode::automatic)mode=QualityMode::full;
    const auto tier=mode==QualityMode::light?QualityTier::light:
        mode==QualityMode::fieldOnly?QualityTier::fieldOnly:QualityTier::full;
    if(!budget_.configure(false,tier)){qualityMode_=mode;return false;}
    qualityMode_=mode;
    fieldReady_=sceneValid_=finishedValid_=false;
    publishBudget();
    return true;
}
} // namespace nekorox::component::a4
