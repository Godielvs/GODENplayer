#include "curated_renderer.h"
#include <dxgi.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>
#include "c1_velocity.generated.h"
#include "c1_pressure.generated.h"
#include "c1_caustics.generated.h"
#include "c1_caustics_image.generated.h"
#include "c1_wave_init.generated.h"
#include "c1_wave_step.generated.h"
#include "c1_wave_image.generated.h"
#include "c1_display.generated.h"
#include "c1_caustics_decay.generated.h"

namespace nekorox::component::curated {
namespace {
// D2D may retain the previous output in a pixel SRV. Remove the bindings
// explicitly before UAV writes, then restore all touched D3D state. No Flush.
struct ComputeScope {
 ID3D11DeviceContext* c;
 ID3D11ComputeShader* cs=nullptr;
 ID3D11ClassInstance* classes[256]{};UINT classCount=256;
 ID3D11Buffer* cb=nullptr;
 ID3D11ShaderResourceView* srvs[2]{};
 ID3D11UnorderedAccessView* uavs[3]{};
 ID3D11SamplerState* sampler=nullptr;
 ID3D11ShaderResourceView* ps[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
 explicit ComputeScope(ID3D11DeviceContext* context):c(context){
  c->CSGetShader(&cs,classes,&classCount);c->CSGetConstantBuffers(0,1,&cb);
  c->CSGetShaderResources(0,2,srvs);c->CSGetUnorderedAccessViews(0,3,uavs);c->CSGetSamplers(0,1,&sampler);
  c->PSGetShaderResources(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,ps);
  ID3D11ShaderResourceView* none[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
  c->PSSetShaderResources(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,none);
  unbind();
 }
 void unbind(){ID3D11ShaderResourceView* s[2]{};ID3D11UnorderedAccessView* u[3]{};
  c->CSSetShaderResources(0,2,s);c->CSSetUnorderedAccessViews(0,3,u,nullptr);}
 ~ComputeScope(){
  unbind();c->CSSetShader(cs,classes,classCount);c->CSSetConstantBuffers(0,1,&cb);
  c->CSSetShaderResources(0,2,srvs);c->CSSetUnorderedAccessViews(0,3,uavs,nullptr);c->CSSetSamplers(0,1,&sampler);
  c->PSSetShaderResources(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,ps);
  if(cs)cs->Release();if(cb)cb->Release();if(sampler)sampler->Release();
  for(auto* p:srvs)if(p)p->Release();for(auto* p:uavs)if(p)p->Release();
  for(UINT i=0;i<classCount;++i)if(classes[i])classes[i]->Release();
  for(auto* p:ps)if(p)p->Release();
 }
};
struct Blob {const void* data;SIZE_T size;};
const Blob shaders[]{
 {kC1Velocity,sizeof(kC1Velocity)},{kC1Pressure,sizeof(kC1Pressure)},
 {kC1Caustics,sizeof(kC1Caustics)},{kC1CausticsImage,sizeof(kC1CausticsImage)},
 {kC1WaveInit,sizeof(kC1WaveInit)},{kC1WaveStep,sizeof(kC1WaveStep)},
 {kC1WaveImage,sizeof(kC1WaveImage)},{kC1Display,sizeof(kC1Display)},{kC3CausticsDecay,sizeof(kC3CausticsDecay)}};
}
bool Renderer::texture(Texture& out,DXGI_FORMAT format,std::uint32_t w,std::uint32_t h){
 D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.Format=format;
 d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
 auto hr=device_->CreateTexture2D(&d,nullptr,out.texture.ReleaseAndGetAddressOf());
 if(SUCCEEDED(hr))hr=device_->CreateShaderResourceView(out.texture.Get(),nullptr,out.srv.ReleaseAndGetAddressOf());
 if(SUCCEEDED(hr))hr=device_->CreateUnorderedAccessView(out.texture.Get(),nullptr,out.uav.ReleaseAndGetAddressOf());
 diagnostics_.lastHr=hr;return SUCCEEDED(hr);
}
bool Renderer::create(ID3D11Device* device,ID3D11DeviceContext* context,Preset preset,Mode mode,float aspect,double now){
 auto oldResets=diagnostics_.resetCount;reset();diagnostics_.resetCount=oldResets+1;
 device_=device;context_=context;preset_=preset;allocationMode_=mode;extent_=gridFor(preset,mode,aspect);
 if(device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0){error(DXGI_ERROR_UNSUPPORTED,now);return false;}
 for(unsigned i=0;i<9;++i){
  auto hr=device->CreateComputeShader(shaders[i].data,shaders[i].size,nullptr,shader_[i].ReleaseAndGetAddressOf());
  if(FAILED(hr)){error(hr,now);return false;}
 }
 if(!texture(state_[0],DXGI_FORMAT_R32G32B32A32_FLOAT,extent_.width,extent_.height)||
    !texture(state_[1],DXGI_FORMAT_R32G32B32A32_FLOAT,extent_.width,extent_.height)||
    !texture(raw_,DXGI_FORMAT_R16G16B16A16_FLOAT,extent_.width,extent_.height)||
    !texture(output_,DXGI_FORMAT_R8G8B8A8_UNORM,extent_.width,extent_.height))return false;
 D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Constants);b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 auto hr=device->CreateBuffer(&b,nullptr,constants_.ReleaseAndGetAddressOf());
 if(FAILED(hr)){error(hr,now);return false;}
 b={};b.ByteWidth=extent_.width*extent_.height*4U*sizeof(std::uint32_t);
 b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=sizeof(std::uint32_t);
 hr=device->CreateBuffer(&b,nullptr,photons_.ReleaseAndGetAddressOf());
 D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_UNKNOWN;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
 u.Buffer.NumElements=extent_.width*extent_.height*4U;
 if(SUCCEEDED(hr))hr=device->CreateUnorderedAccessView(photons_.Get(),&u,photonsUav_.ReleaseAndGetAddressOf());
 if(FAILED(hr)){error(hr,now);return false;}
 D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
 s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;s.MaxLOD=0;s.ComparisonFunc=D3D11_COMPARISON_NEVER;
 hr=device->CreateSamplerState(&s,repeat_.ReleaseAndGetAddressOf());
 if(FAILED(hr)){error(hr,now);return false;}
  queryAvailable_=true;
  for(auto& q:queries_){D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
   hr=device->CreateQuery(&d,q.disjoint.ReleaseAndGetAddressOf());d.Query=D3D11_QUERY_TIMESTAMP;
   if(SUCCEEDED(hr))hr=device->CreateQuery(&d,q.start.ReleaseAndGetAddressOf());
   if(SUCCEEDED(hr))hr=device->CreateQuery(&d,q.end.ReleaseAndGetAddressOf());
   if(FAILED(hr)){queryAvailable_=false;queries_={};break;}
  }
  diagnostics_.gpuTimingAvailable=queryAvailable_;
 const float zeros[4]{};const UINT uzero[4]{};
 context->ClearUnorderedAccessViewFloat(state_[0].uav.Get(),zeros);
 context->ClearUnorderedAccessViewFloat(state_[1].uav.Get(),zeros);
 context->ClearUnorderedAccessViewFloat(raw_.uav.Get(),zeros);
 context->ClearUnorderedAccessViewFloat(output_.uav.Get(),zeros);
 context->ClearUnorderedAccessViewUint(photonsUav_.Get(),uzero);
 diagnostics_.bytes=std::uint64_t(extent_.width)*extent_.height*(16U*2U+8U+4U+16U)+sizeof(Constants);
  diagnostics_.gridWidth=extent_.width;diagnostics_.gridHeight=extent_.height;
  timeline_.reset(now);initialized_=true;diagnostics_.lastHr=S_OK;return true;
}
bool Renderer::bitmap(ID2D1DeviceContext* d2d,std::uint64_t epoch){
 if(bitmap_&&bitmapContext_.Get()==d2d&&bitmapEpoch_==epoch)return true;
 Ptr<IDXGISurface> surface;auto hr=output_.texture.As(&surface);
 if(SUCCEEDED(hr)){
  const auto p=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
    D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
  hr=d2d->CreateBitmapFromDxgiSurface(surface.Get(),&p,bitmap_.ReleaseAndGetAddressOf());
 }
 diagnostics_.lastHr=hr;if(FAILED(hr))return false;
 bitmapContext_=d2d;bitmapEpoch_=epoch;return true;
}
bool Renderer::upload(const Input& input){
 const auto c=makeConstants(input,extent_,frame_);context_->UpdateSubresource(constants_.Get(),0,nullptr,&c,0,0);
 ID3D11Buffer* p=constants_.Get();context_->CSSetConstantBuffers(0,1,&p);
 ID3D11SamplerState* s=repeat_.Get();context_->CSSetSamplers(0,1,&s);return true;
}
void Renderer::bindAndDispatch(unsigned shader,ID3D11ShaderResourceView* state,ID3D11UnorderedAccessView* target,ID3D11UnorderedAccessView* image){
 ID3D11ShaderResourceView* none[2]{};ID3D11UnorderedAccessView* nulls[3]{};
 context_->CSSetShaderResources(0,2,none);context_->CSSetUnorderedAccessViews(0,3,nulls,nullptr);
 ID3D11ShaderResourceView* inputs[2]{state,shader==7?raw_.srv.Get():nullptr};
 ID3D11UnorderedAccessView* outputs[3]{target,(shader==2||shader==3||shader==8)?photonsUav_.Get():nullptr,image};
 context_->CSSetShader(shader_[shader].Get(),nullptr,0);context_->CSSetShaderResources(0,2,inputs);
 context_->CSSetUnorderedAccessViews(0,3,outputs,nullptr);
 context_->Dispatch((extent_.width+15)/16,(extent_.height+15)/16,1);
 context_->CSSetShaderResources(0,2,none);context_->CSSetUnorderedAccessViews(0,3,nulls,nullptr);
 ++diagnostics_.simulationDispatches;
}
void Renderer::step(const Input& in){
 upload(in);
 if(preset_==Preset::caustics){
  bindAndDispatch(0,state_[1].srv.Get(),state_[0].uav.Get(),nullptr);
  bindAndDispatch(1,state_[0].srv.Get(),state_[1].uav.Get(),nullptr);
  if(!expressive(in)){
   bindAndDispatch(2,state_[1].srv.Get(),nullptr,nullptr);
   bindAndDispatch(3,nullptr,nullptr,raw_.uav.Get());
  }
 }else{
  if(frame_==0){bindAndDispatch(4,nullptr,state_[0].uav.Get(),state_[1].uav.Get());stateIndex_=0;}
  else {
   const unsigned substeps=expressive(in)?8U:16U;
   for(unsigned i=0;i<substeps;++i){
    if(expressive(in)) {auto sub=in;sub.substepOffset=float(i)/480.f;upload(sub);}
    const unsigned out=1-stateIndex_;
    bindAndDispatch(5,state_[stateIndex_].srv.Get(),state_[out].uav.Get(),nullptr);
    stateIndex_=out;
   }
  }
  if(!expressive(in))bindAndDispatch(6,state_[stateIndex_].srv.Get(),nullptr,raw_.uav.Get());
 }
 ++frame_;diagnostics_.referenceFrames=frame_;
}
void Renderer::resolve(const Input& input,unsigned steps){
 auto projection=input;projection.projectionSteps=float(std::clamp(steps,1U,6U));
 upload(projection);
 if(expressive(input)&&steps>0){
  if(preset_==Preset::caustics){
   // Pre-decay old stored counts by a^(n-1), then deposit S_n*D and
   // finish with one a decay. Exactly preserves frozen-field exposure.
   bindAndDispatch(8,nullptr,nullptr,nullptr);
   bindAndDispatch(2,state_[1].srv.Get(),nullptr,nullptr);
   bindAndDispatch(3,nullptr,nullptr,raw_.uav.Get());
  }else bindAndDispatch(6,state_[stateIndex_].srv.Get(),nullptr,raw_.uav.Get());
 }
 bindAndDispatch(7,nullptr,nullptr,output_.uav.Get());
 ++diagnostics_.renderedFrames;++diagnostics_.outputRevision;produced_=true;pending_=true;
}
int Renderer::beginQuery()noexcept{
 if(!queryAvailable_)return -1;
  for(unsigned i=0;i<queries_.size();++i)if(!queries_[i].pending){
   queries_[i].epoch=queryEpoch_;context_->Begin(queries_[i].disjoint.Get());context_->End(queries_[i].start.Get());return int(i);}
 ++diagnostics_.droppedQueries;return -1;
}
void Renderer::endQuery(int i)noexcept{if(i<0)return;auto& q=queries_[unsigned(i)];context_->End(q.end.Get());context_->End(q.disjoint.Get());q.pending=true;}
void Renderer::collectQueries()noexcept{
 if(!context_||!queryAvailable_)return;
  for(auto& q:queries_){if(!q.pending)continue;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 start=0,end=0;
  auto a=context_->GetData(q.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH);
  if(a==S_FALSE)continue;if(FAILED(a)||d.Disjoint||!d.Frequency){q.pending=false;continue;}
  auto b=context_->GetData(q.start.Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH);
  auto c=context_->GetData(q.end.Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH);
  if(b==S_FALSE||c==S_FALSE)continue;q.pending=false;
  if(FAILED(b)||FAILED(c)||end<start)continue;
  if(q.epoch!=queryEpoch_)continue;
  double us=double(end-start)*1e6/double(d.Frequency);
  if(warmup_){--warmup_;continue;}
  timings_[timingCount_++]=us;
  if(timingCount_==timings_.size()){
   auto sorted=timings_;std::sort(sorted.begin(),sorted.end());diagnostics_.gpuP95Us=sorted[28];timingCount_=0;
   // C1 presents at 30 Hz.  A 3 ms aspirational target is useful telemetry,
   // but it must not become an indefinite visual freeze on a real adapter.
   // Trip only after three windows exceed the complete frame opportunity,
   // then cool down briefly and resume from the latest pose.
   const auto frameBudgetUs=Timeline::renderStep*1e6;
   expensiveWindows_=diagnostics_.gpuP95Us>frameBudgetUs?expensiveWindows_+1:0;
   if(expensiveWindows_>=3){
    diagnostics_.budgetPaused=true;budgetPauseUntil_=lastNow_+.250;
    timeline_.setActive(false,lastNow_);
   }
  }
 }
}
void Renderer::error(HRESULT hr,double now)noexcept{
 diagnostics_.lastHr=FAILED(hr)?hr:E_FAIL;diagnostics_.ready=false;produced_=false;
 ++failures_;retryAt_=now+(failures_<3?.5:3600.);
 timeline_.setActive(false,now);pending_=false;
}
bool Renderer::prepare(ID3D11Device* device,ID3D11DeviceContext* context,ID2D1DeviceContext* d2d,
 std::uint64_t epoch,D2D1_SIZE_F viewport,const Input& requested,double now,bool allowWork)noexcept{
 auto start=std::chrono::steady_clock::now();
 struct TimeGuard{double& result;std::chrono::steady_clock::time_point start;~TimeGuard(){result=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();}}guard{diagnostics_.prepareLastUs,start};
 produced_=false;lastNow_=now;
 Input input=requested;
 const auto elapsed=lastSlew_>0?std::clamp(now-lastSlew_,0.0,.10):1.0/60.0;lastSlew_=now;
 if(expressive(input))for(unsigned i=0;i<3;++i)input.band[i]=musicalSlew_[i].step(input.band[i],elapsed);
  if(!device||!context||!d2d||!std::isfinite(now)||viewport.width<=0||viewport.height<=0)return false;
  try{
   const bool changed=!initialized_||device_.Get()!=device||context_.Get()!=context||preset_!=input.preset||allocationMode_!=input.mode||
     lastRestart_!=input.restartRevision||lastInput_.response!=input.response;
   if(FAILED(diagnostics_.lastHr)&&!changed)return false;
   if(!allowWork){
    activeWanted_=input.active;
    timeline_.setTransport(input.playing,input.active&&!diagnostics_.budgetPaused,now);
    timeline_.defer(now);
    if(changed){diagnostics_.ready=false;return false;}
    return diagnostics_.ready;
   } // explicit Retry unless a new resource epoch/preset supersedes the failure
  if(changed){
   if(now<retryAt_)return false;
   if(!create(device,context,input.preset,input.mode,viewport.width/viewport.height,now)){error(diagnostics_.lastHr,now);return false;}
   lastRestart_=input.restartRevision;
   ComputeScope save(context_.Get());step(input);resolve(input,1);lastInput_=input;
  }
  if(diagnostics_.budgetPaused&&now>=budgetPauseUntil_){
   diagnostics_.budgetPaused=false;budgetPauseUntil_=0;expensiveWindows_=0;
   timingCount_=0;warmup_=8;
  }
  activeWanted_=input.active;timeline_.setRate(simulationRate(input));
  timeline_.setTransport(input.playing,input.active&&!diagnostics_.budgetPaused,now);diagnostics_.active=timeline_.active();
  collectQueries();
  if(!bitmap(d2d,epoch)){error(diagnostics_.lastHr,now);return false;}
  bool colorChanged=lastInput_.palette!=input.palette||lastInput_.mode!=input.mode;
  for(unsigned i=0;i<4;++i)colorChanged=colorChanged||std::memcmp(&lastInput_.colors[i],&input.colors[i],sizeof(Color))!=0;
   if(!allowWork){timeline_.defer(now);diagnostics_.ready=true;return true;}
   timeline_.resume(now);
  const unsigned frames=timeline_.advance(now);
  if(frames||colorChanged){
   ComputeScope save(context_.Get());const int query=beginQuery();
   for(unsigned i=0;i<frames;++i)step(input);resolve(input,frames);endQuery(query);
  }
  lastInput_=input;diagnostics_.ready=true;diagnostics_.discardedCatchupSeconds=timeline_.droppedSeconds();
  diagnostics_.active=timeline_.active();return true;
 }catch(...){error(E_OUTOFMEMORY,now);return false;}
}
bool Renderer::draw(ID2D1RenderTarget* target,const D2D1_RECT_F& destination,float opacity)noexcept{
 if(!target||!bitmap_||!diagnostics_.ready)return false;
 const float dw=destination.right-destination.left,dh=destination.bottom-destination.top;
 if(dw<=0||dh<=0)return false;
 const auto sz=bitmap_->GetSize();D2D1_RECT_F dest=destination,src=D2D1::RectF(0,0,sz.width,sz.height);
 const bool referenceSquare=preset_==Preset::lagrangian&&lastInput_.mode==Mode::original;
 if(referenceSquare){float side=dh;dest.left+=(dw-side)*.5f;dest.top+=(dh-side)*.5f;dest.right=dest.left+side;dest.bottom=dest.top+side;}
 else{
  const float desired=dw/dh,current=sz.width/sz.height;
  if(current>desired){float w=sz.height*desired;src.left=(sz.width-w)*.5f;src.right=src.left+w;}
  else{float h=sz.width/desired;src.top=(sz.height-h)*.5f;src.bottom=src.top+h;}
 }
  target->DrawBitmap(bitmap_.Get(),dest,std::clamp(opacity,0.f,1.f),D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,src);
  diagnostics_.drawnRevision=diagnostics_.outputRevision;return true;
}
void Renderer::setActivity(bool a,double now)noexcept{activeWanted_=a;lastNow_=now;timeline_.setTransport(lastInput_.playing,a&&!diagnostics_.budgetPaused&&SUCCEEDED(diagnostics_.lastHr),now);diagnostics_.active=timeline_.active();}
void Renderer::presented(bool accepted)noexcept{
 if(!diagnostics_.ready)return;
 diagnostics_.acceptedRevision=acceptedRevisionAfterPresent(
  diagnostics_.acceptedRevision,diagnostics_.drawnRevision,
  diagnostics_.outputRevision,accepted);
 pending_=diagnostics_.acceptedRevision<diagnostics_.outputRevision;
}
void Renderer::suspend(double now)noexcept{setActivity(false,now);}
void Renderer::invalidateBitmap()noexcept{bitmap_.Reset();bitmapContext_.Reset();bitmapEpoch_=0;}
void Renderer::retry()noexcept{if(FAILED(diagnostics_.lastHr))initialized_=false;failures_=0;retryAt_=0;diagnostics_.budgetPaused=false;budgetPauseUntil_=0;diagnostics_.lastHr=S_OK;expensiveWindows_=0;timingCount_=0;warmup_=8;++queryEpoch_;for(auto& q:queries_)q.epoch=0;timeline_.setActive(activeWanted_,lastNow_);}
bool Renderer::needsFrame()const noexcept{
 return pending_||timeline_.active()||(activeWanted_&&diagnostics_.budgetPaused);
}
bool Renderer::due(double now)const noexcept{
 return pending_||timeline_.due(now)||(activeWanted_&&diagnostics_.budgetPaused&&now>=budgetPauseUntil_);
}
double Renderer::deadline()const noexcept{
 if(pending_)return lastNow_;
 if(activeWanted_&&diagnostics_.budgetPaused)return budgetPauseUntil_;
 return timeline_.deadline();
}
void Renderer::reset()noexcept{
 invalidateBitmap();for(auto& s:shader_)s.Reset();constants_.Reset();photons_.Reset();photonsUav_.Reset();repeat_.Reset();
 state_[0]={};state_[1]={};raw_={};output_={};queries_={};context_.Reset();device_.Reset();extent_={};
 initialized_=produced_=pending_=false;frame_=0;stateIndex_=0;timingCount_=0;warmup_=8;expensiveWindows_=0;++queryEpoch_;
 failures_=0;retryAt_=0;budgetPauseUntil_=0;diagnostics_={};timeline_=Timeline{};musicalSlew_={};lastSlew_=0;
}
} // namespace
