#include "collection_renderer.h"
#ifndef GODEN_SHADER_RENDER_PROCESS
#include "remote_frame_client.h"
#endif
#include "../visual_presentation.h"
#include "../device_failure.h"
#include <d2d1helper.h>
#include <dxgi.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

namespace nekorox::component::collection {
Renderer::Renderer()=default;
Renderer::~Renderer()=default;
bool Renderer::asynchronous()const noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 return remote_!=nullptr;
#else
 return false;
#endif
}
bool Renderer::responsePendingForProbe()const noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 return remote_&&remote_->responsePending();
#else
 return false;
#endif
}
bool Renderer::needsFrame()const noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_&&remote_->needsFrame()&&!failed())return true;
#endif
 return pending_||(timeline_.active()&&!failed());
}
bool Renderer::due(double now)const noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_&&remote_->needsFrame()&&!failed())return pending_||now+1e-9>=remotePollAt_;
#endif
 return pending_||(!failed()&&timeline_.due(now));
}
double Renderer::deadline()const noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_&&remote_->needsFrame()&&!failed())return pending_?now_:remotePollAt_;
#endif
 return pending_?now_:timeline_.deadline();
}
ID3D11Texture2D* Renderer::outputTexture()const noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_)return remote_->texture();
#endif
 return outputs_[7].tex[outputs_[7].current].resource.Get();
}
namespace {
using Microsoft::WRL::ComPtr;
struct StateScope {
 ID3D11DeviceContext* c;
 ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;ID3D11ComputeShader* cs=nullptr;
 ID3D11ClassInstance* vc[256]{},*pc[256]{},*cc[256]{};UINT vn=256,pn=256,cn=256;
 ID3D11GeometryShader* gs=nullptr;ID3D11HullShader* hs=nullptr;ID3D11DomainShader* ds=nullptr;
 ID3D11ClassInstance* gc[256]{},*hc[256]{},*dc[256]{};UINT gn=256,hn=256,dn=256;
 ID3D11Predicate* predicate=nullptr;BOOL predicateValue=FALSE;
 ID3D11InputLayout* layout=nullptr;D3D11_PRIMITIVE_TOPOLOGY topology{};
 ID3D11Buffer* pb=nullptr,*cb=nullptr;
 ID3D11ShaderResourceView* textures[128]{},*compute[11]{};
 ID3D11UnorderedAccessView* uav[3]{};ID3D11SamplerState* samplers[11]{},*cSamplers[11]{};
 ID3D11RenderTargetView* rtv[8]{};ID3D11DepthStencilView* dsv=nullptr;
 ID3D11BlendState* blend=nullptr;FLOAT factors[4]{};UINT sampleMask=0,stencil=0;
 ID3D11DepthStencilState* depth=nullptr;ID3D11RasterizerState* raster=nullptr;
 D3D11_VIEWPORT viewports[16]{};UINT viewportCount=16;
 explicit StateScope(ID3D11DeviceContext* context):c(context){
  c->VSGetShader(&vs,vc,&vn);c->PSGetShader(&ps,pc,&pn);c->CSGetShader(&cs,cc,&cn);
  c->GSGetShader(&gs,gc,&gn);c->HSGetShader(&hs,hc,&hn);c->DSGetShader(&ds,dc,&dn);
  c->GetPredication(&predicate,&predicateValue);c->SetPredication(nullptr,FALSE);
  c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);
  c->IAGetInputLayout(&layout);c->IAGetPrimitiveTopology(&topology);
  c->PSGetConstantBuffers(0,1,&pb);c->CSGetConstantBuffers(0,1,&cb);
  c->PSGetShaderResources(0,128,textures);c->CSGetShaderResources(0,11,compute);
  c->CSGetUnorderedAccessViews(0,3,uav);c->PSGetSamplers(0,11,samplers);c->CSGetSamplers(0,11,cSamplers);
  c->OMGetRenderTargets(8,rtv,&dsv);c->OMGetBlendState(&blend,factors,&sampleMask);c->OMGetDepthStencilState(&depth,&stencil);
  c->RSGetState(&raster);c->RSGetViewports(&viewportCount,viewports);clear();
 }
 void clear(){ID3D11ShaderResourceView* n[128]{};ID3D11UnorderedAccessView*u[3]{};
  c->OMSetRenderTargets(0,nullptr,nullptr);c->PSSetShaderResources(0,128,n);c->CSSetShaderResources(0,11,n);c->CSSetUnorderedAccessViews(0,3,u,nullptr);}
 template<class T,std::size_t N>static void release(T*(&a)[N]){for(auto*p:a)if(p)p->Release();}
 ~StateScope(){clear();c->VSSetShader(vs,vc,vn);c->PSSetShader(ps,pc,pn);c->CSSetShader(cs,cc,cn);
  c->GSSetShader(gs,gc,gn);c->HSSetShader(hs,hc,hn);c->DSSetShader(ds,dc,dn);c->SetPredication(predicate,predicateValue);
  c->IASetInputLayout(layout);c->IASetPrimitiveTopology(topology);c->PSSetConstantBuffers(0,1,&pb);c->CSSetConstantBuffers(0,1,&cb);
  c->OMSetRenderTargets(8,rtv,dsv);c->OMSetBlendState(blend,factors,sampleMask);c->OMSetDepthStencilState(depth,stencil);
  c->RSSetState(raster);c->RSSetViewports(viewportCount,viewports);c->PSSetShaderResources(0,128,textures);c->CSSetShaderResources(0,11,compute);
  c->CSSetUnorderedAccessViews(0,3,uav,nullptr);c->PSSetSamplers(0,11,samplers);c->CSSetSamplers(0,11,cSamplers);
  for(auto*p:{static_cast<IUnknown*>(vs),static_cast<IUnknown*>(ps),static_cast<IUnknown*>(cs),static_cast<IUnknown*>(layout),static_cast<IUnknown*>(pb),static_cast<IUnknown*>(cb),static_cast<IUnknown*>(dsv),static_cast<IUnknown*>(blend),static_cast<IUnknown*>(depth),static_cast<IUnknown*>(raster)})if(p)p->Release();
  if(gs)gs->Release();if(hs)hs->Release();if(ds)ds->Release();if(predicate)predicate->Release();
  for(UINT i=0;i<gn;++i)if(gc[i])gc[i]->Release();for(UINT i=0;i<hn;++i)if(hc[i])hc[i]->Release();for(UINT i=0;i<dn;++i)if(dc[i])dc[i]->Release();
  release(textures);release(compute);release(uav);release(samplers);release(cSamplers);release(rtv);
  for(UINT i=0;i<vn;++i)if(vc[i])vc[i]->Release();for(UINT i=0;i<pn;++i)if(pc[i])pc[i]->Release();for(UINT i=0;i<cn;++i)if(cc[i])cc[i]->Release();
 }
};
float average(const std::array<float,64>& a,unsigned begin,unsigned end){float s=0;for(unsigned i=begin;i<end;++i)s+=a[i];return s/float(end-begin);}
float peak(const std::array<float,64>& a,unsigned begin,unsigned end){float s=0;for(unsigned i=begin;i<end;++i)s=std::max(s,a[i]);return s;}
}
bool Renderer::makeTexture(Tex& out,Extent e,DXGI_FORMAT format,bool target,bool uav){
 if(!e.width||!e.height||e.width>2048||e.height>2048)return false;
 D3D11_TEXTURE2D_DESC d{};d.Width=e.width;d.Height=e.height;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
 d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(target?D3D11_BIND_RENDER_TARGET:0)|(uav?D3D11_BIND_UNORDERED_ACCESS:0);
 auto hr=device_->CreateTexture2D(&d,nullptr,out.resource.ReleaseAndGetAddressOf());
 if(SUCCEEDED(hr))hr=device_->CreateShaderResourceView(out.resource.Get(),nullptr,out.srv.ReleaseAndGetAddressOf());
 if(target&&SUCCEEDED(hr))hr=device_->CreateRenderTargetView(out.resource.Get(),nullptr,out.rtv.ReleaseAndGetAddressOf());
 if(uav&&SUCCEEDED(hr))hr=device_->CreateUnorderedAccessView(out.resource.Get(),nullptr,out.uav.ReleaseAndGetAddressOf());
 if(FAILED(hr)){fail(hr);return false;}out.size=e;
 const unsigned bpp=format==DXGI_FORMAT_R32G32B32A32_FLOAT?16:format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
 diag_.bytes+=std::uint64_t(e.width)*e.height*bpp;
 const float zero[4]{};if(out.rtv)context_->ClearRenderTargetView(out.rtv.Get(),zero);
 else {std::vector<std::byte> bytes(std::size_t(e.width)*e.height*bpp);context_->UpdateSubresource(out.resource.Get(),0,nullptr,bytes.data(),e.width*bpp,0);}
 return true;
}
bool Renderer::allocate(ID3D11Device* dev,ID3D11DeviceContext* ctx,const Selection& selection,Extent e,double now,Tex* retainedAudio){
 const auto resets=diag_.resetCount,resizes=diag_.resizeCount;
 reset();diag_.resetCount=resets+1;diag_.resizeCount=resizes;device_=dev;context_=ctx;selection_=selection;extent_=e;wanted_=e;
 const auto all=presets();if(selection.preset>=all.size()||dev->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0){fail(E_INVALIDARG);return false;}
 const auto&p=all[selection.preset];diag_.preset=selection.preset;
 if(p.count>programs_.size()){fail(E_INVALIDARG);return false;}
 const auto vb=fullscreenVertexShader();auto hr=dev->CreateVertexShader(vb.bytes,vb.count,nullptr,vertex_.ReleaseAndGetAddressOf());
 if(FAILED(hr)){fail(hr);return false;}
 const auto rb=resamplePixelShader();hr=dev->CreatePixelShader(rb.bytes,rb.count,nullptr,resample_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 for(unsigned i=0;i<p.count;++i){const auto&pass=p.passes[i];const auto blob=selection.music?pass.music:pass.original;
  if(!blob.bytes||!blob.count){fail(E_INVALIDARG);return false;}
  if(pass.stage==Stage::fragment)hr=dev->CreatePixelShader(blob.bytes,blob.count,nullptr,programs_[i].ps.ReleaseAndGetAddressOf());
  else hr=dev->CreateComputeShader(blob.bytes,blob.count,nullptr,programs_[i].cs.ReleaseAndGetAddressOf());
  if(FAILED(hr)){fail(hr);return false;}
  if(pass.output>=0){const Extent size{std::max(1U,e.width/pass.scale),std::max(1U,e.height/pass.scale)};
   const auto format=pass.final?DXGI_FORMAT_R8G8B8A8_UNORM:pass.fullFloat?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
   for(auto&t:outputs_[unsigned(pass.output)].tex)if(!makeTexture(t,size,format,true,pass.stage==Stage::compute))return false;
  }
 }
 D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Constants);b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 hr=dev->CreateBuffer(&b,nullptr,constants_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 bool noiseUsed=false,keyboardUsed=false;audioTextureUsed_=false;
 for(unsigned i=0;i<p.count;++i)for(const auto binding:p.passes[i].inputs){audioTextureUsed_=audioTextureUsed_||binding==8;noiseUsed=noiseUsed||binding==9;keyboardUsed=keyboardUsed||binding==10;}
 if(!makeTexture(zero_,{1,1},DXGI_FORMAT_R32G32B32A32_FLOAT,false,false))return false;
 const bool reusedAudio=audioTextureUsed_&&retainedAudio&&retainedAudio->resource;
 if(audioTextureUsed_){
  if(reusedAudio){audio_=std::move(*retainedAudio);diag_.bytes+=512ULL*512ULL*16ULL;}
  else {if(!makeTexture(audio_,{512,512},DXGI_FORMAT_R32G32B32A32_FLOAT,true,false))return false;
   if(++audioResourceEpoch_==0)audioResourceEpoch_=1;}
 }
 if(audioTextureUsed_&&!reusedAudio&&audio_.rtv){const float neutral[4]{0.f,.5f,0.f,0.f};context_->ClearRenderTargetView(audio_.rtv.Get(),neutral);}
 if(noiseUsed&&!makeTexture(noise_,{128,128},DXGI_FORMAT_R32G32B32A32_FLOAT,false,false))return false;
 if(keyboardUsed&&!makeTexture(keyboard_,{256,3},DXGI_FORMAT_R32G32B32A32_FLOAT,false,false))return false;
 // Deterministic replacement for the source's procedural noise annotation. Not
 // claimed to reproduce the site's unavailable procedural generator bitwise.
 if(noiseUsed){std::vector<V4> noise(128U*128U);std::uint32_t seed=0xC4A51EEDU;
 for(auto&v:noise){auto next=[&]{seed=1664525U*seed+1013904223U;return float(seed>>8)/16777216.f;};v={next(),next(),next(),next()};}
 context_->UpdateSubresource(noise_.resource.Get(),0,nullptr,noise.data(),128*sizeof(V4),0);}
 D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sampler.MaxLOD=0;sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;
 hr=dev->CreateSamplerState(&sampler,sampler_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;hr=dev->CreateSamplerState(&sampler,clampSampler_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
 hr=dev->CreateRasterizerState(&raster,rasterizer_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
 hr=dev->CreateBlendState(&blend,blend_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=FALSE;depth.StencilEnable=FALSE;
 hr=dev->CreateDepthStencilState(&depth,depth_.ReleaseAndGetAddressOf());if(FAILED(hr)){fail(hr);return false;}
 rayCount_=p.storage&&selection.music?32768U:8192U;
 if(p.storage){
  auto makeStorage=[&](unsigned bytes,Com<ID3D11Buffer>&buffer,Com<ID3D11UnorderedAccessView>&view){
   D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
   auto result=dev->CreateBuffer(&d,nullptr,buffer.ReleaseAndGetAddressOf());if(FAILED(result))return result;
   D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=bytes/4;u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
   result=dev->CreateUnorderedAccessView(buffer.Get(),&u,view.ReleaseAndGetAddressOf());if(SUCCEEDED(result)){const UINT z[4]{};ctx->ClearUnorderedAccessViewUint(view.Get(),z);diag_.bytes+=bytes;}return result;
  };
  hr=makeStorage((128U+rayCount_*3U)*16U,storage_,storageUav_);if(SUCCEEDED(hr))hr=makeStorage(e.width*e.height*3U*4U,photons_,photonsUav_);
  if(FAILED(hr)){fail(hr);return false;}
 }
 for(auto&q:queries_){D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};hr=dev->CreateQuery(&d,q.disjoint.GetAddressOf());d.Query=D3D11_QUERY_TIMESTAMP;
  if(SUCCEEDED(hr))hr=dev->CreateQuery(&d,q.begin.GetAddressOf());if(SUCCEEDED(hr))hr=dev->CreateQuery(&d,q.end.GetAddressOf());
  if(FAILED(hr)){queries_={};break;}}
 diag_.gpuTimingAvailable=queries_[0].disjoint!=nullptr;
 LARGE_INTEGER seedTime{};QueryPerformanceCounter(&seedTime);
 storm_.seed(stormSeedForProbe_?stormSeedForProbe_:std::uint32_t(seedTime.QuadPart)^GetCurrentProcessId());
 diag_.width=e.width;diag_.height=e.height;diag_.result=S_OK;timeline_.reset(now);initialized_=true;lastRestart_=selection.restart;return true;
}
bool Renderer::synchronizeAudioIdentity(AudioIdentity expected){
 if(!expected.valid()||
    (audioSerial_==expected.serial&&audioRecording_==expected.recording))return false;
 // Never expose the previous recording under the new playback identity. The
 // retained texture has an RTV so this is a GPU clear, not a 4 MiB CPU upload.
 if(audioTextureUsed_&&audioCount_&&audio_.rtv){const float neutral[4]{0.f,.5f,0.f,0.f};
  context_->ClearRenderTargetView(audio_.rtv.Get(),neutral);}
 lastAudio_=0;audioHead_=audioCount_=0;audioSerial_=expected.serial;audioRecording_=expected.recording;
 spectrumScroll_.reset();
 storm_.synchronizeIdentity(expected.serial,expected.recording);
 return true;
}
void Renderer::uploadAudio(const RetainedAudioRows::Batch* source,AudioIdentity expected,
 std::uint64_t readerGeneration,HistoryApplyReceipt&receipt){
 if(!audioTextureUsed_||!source)return;
 const auto&batch=*source;++diag_.audioReadCalls;diag_.audioRestoreFence=batch.restoreFence;
 diag_.audioRestorePending=batch.restoreFence!=0&&!batch.complete;
 if(batch.busy||batch.oldIdentity||batch.serial!=expected.serial||
    batch.recording!=expected.recording)return;
 const bool handled=batch.count!=0||(batch.complete&&batch.skippedObservations!=0);
 if(!handled)return;
 // Gap rows are explicit missing observations. They are not samples of silence.
 // A source discontinuity inserts a marker but preserves the preceding history.
 std::array<V4,512> empty{};for(auto&v:empty)v.y=.5f;
 auto marker=[&]{audioHead_=(audioHead_+1)%512;
  const D3D11_BOX box{0,audioHead_,0,512,audioHead_+1,1};
  context_->UpdateSubresource(audio_.resource.Get(),0,&box,empty.data(),512*sizeof(V4),0);
  audioCount_=std::min(512U,audioCount_+1);};
 diag_.audioRowsSkipped+=batch.skippedObservations;
 if(batch.skippedObservations>=512U){audioHead_=audioCount_=0;
  if(audio_.rtv){const float neutral[4]{0.f,.5f,0.f,0.f};context_->ClearRenderTargetView(audio_.rtv.Get(),neutral);}}
 else if(batch.skippedObservations){marker();}
 // Upload real rows in contiguous runs. Source gaps split runs; normal catch-up
 // uses at most two UpdateSubresource calls at the texture-ring seam.
 std::size_t index=0;
 while(index<batch.count){
  if(batch.rows[index].discontinuity)marker();
  const auto start=(audioHead_+1)%512;
  std::size_t n=std::min<std::size_t>(batch.count-index,512U-start);
  for(std::size_t j=1;j<n;++j)if(batch.rows[index+j].discontinuity){n=j;break;}
  const D3D11_BOX box{0,start,0,512,start+static_cast<UINT>(n),1};
  context_->UpdateSubresource(audio_.resource.Get(),0,&box,batch.rows[index].pixels.data(),
      static_cast<UINT>(sizeof(RetainedAudioRows::Row)),0);
  audioHead_=(start+static_cast<unsigned>(n)-1)%512;
  audioCount_=std::min(512U,audioCount_+static_cast<unsigned>(n));
  const auto& latest=batch.rows[index+n-1];
  spectrumScroll_.observe(latest.publication,latest.sourceSeconds,batch.rows[index].discontinuity);
  lastAudio_=batch.rows[index+n-1].publication;diag_.audioRows+=n;index+=n;
 }
 receipt.identity=expected;receipt.readerGeneration=readerGeneration;
 receipt.resourceEpoch=audioResourceEpoch_;receipt.throughPublication=batch.count?
  batch.rows[batch.count-1].publication:batch.restoreFence;
 receipt.skippedObservations=batch.skippedObservations;receipt.handled=true;
 receipt.restoreComplete=batch.complete;
}
void Renderer::step(const AudioFrame* audio){
 const auto&p=presets()[selection_.preset];diag_.audioSequenceApplied=audio&&audio->valid?audio->sequence:0;Constants c{};c.meta={float(diag_.frame),0,float(audioHead_),float(audioCount_)};
 if(audio&&audio->valid&&audio->rate>=8000&&audio->rate<=192000)
  spectrumNyquistHz_=.5f*float(audio->rate);
 c.options={std::clamp(selection_.intensity,0.f,2.f),spectrumNyquistHz_,float(rayCount_),selection_.albumColors?1.f:0.f};
 const float t=float(stepTime_);
 const double lo=audio&&audio->valid?average(audio->level,0,15):0;
 const double mid=audio&&audio->valid?average(audio->level,15,43):0;
 const double hi=audio&&audio->valid?average(audio->level,43,64):0;
 musicalPhase_[0]+=stepDelta_*(.35+.45*lo);
 musicalPhase_[1]+=stepDelta_*(.45+.40*mid);
 musicalPhase_[2]+=stepDelta_*(.65+.55*hi);
 musicalPhase_[3]+=stepDelta_*(.75+1.05*std::clamp((lo+mid+hi)/3.,0.,1.));
 c.motion={float(musicalPhase_[0]),float(musicalPhase_[1]),float(musicalPhase_[2]),float(musicalPhase_[3])};
 if(selection_.music&&std::strcmp(p.id,"rxhy4i5t")==0){
  c8Motion_.step(stepDelta_,float(lo),float(mid),float(hi));c.motion.w=float(c8Motion_.quietTravel);
 }
 c.palette=selection_.palette;
 if(selection_.music){
  if(std::strcmp(p.id,"cxbf4gfw")==0)storm_.step(audio,stepDelta_,c.motion.y*.42f,c.options.x,playing_,float(extent_.width)/float(extent_.height));
  if(std::strcmp(p.id,"u4uhc9b9")==0)radianceMotion_.step(audio,stepDelta_,c.options.x,playing_);
  if(std::strcmp(p.id,"avtqpukv")==0){spectrumScroll_.advance(stepDelta_);c.options.z=spectrumScroll_.rowsBehind();}
 }
 if(audio&&audio->valid){packBands(c.levels,audio->level);packBands(c.onsets,audio->hit);packBands(c.relatives,audio->relative);
  c.bands={average(audio->level,0,15),average(audio->level,15,43),average(audio->level,43,64),audio->gate};
  c.attacks={peak(audio->hit,0,15),peak(audio->hit,15,43),peak(audio->hit,43,64),0};}
 // No GI/pressure output becomes externally visible until the final Main pass.
 ID3D11Buffer*constant=constants_.Get();context_->PSSetConstantBuffers(0,1,&constant);context_->CSSetConstantBuffers(0,1,&constant);
 ID3D11SamplerState*samplers[11]{};for(auto&s:samplers)s=sampler_.Get();context_->PSSetSamplers(0,11,samplers);context_->CSSetSamplers(0,11,samplers);
 context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 context_->VSSetShader(vertex_.Get(),nullptr,0);context_->RSSetState(rasterizer_.Get());
 const FLOAT blendFactor[4]{};context_->OMSetBlendState(blend_.Get(),blendFactor,0xffffffff);context_->OMSetDepthStencilState(depth_.Get(),0);
 for(unsigned i=0;i<p.count;++i){const auto&pass=p.passes[i];if(pass.once&&onceDone_)continue;
  if(physicsOnly_&&std::strcmp(pass.key,"phys")!=0&&std::strcmp(pass.key,"physinit")!=0)continue;
  for(unsigned repeat=0;repeat<pass.repeat;++repeat){
   ID3D11SamplerState*states[11]{};for(unsigned j=0;j<11;++j)states[j]=(pass.wrapMask&(1U<<j))?sampler_.Get():clampSampler_.Get();context_->PSSetSamplers(0,11,states);context_->CSSetSamplers(0,11,states);
   ID3D11ShaderResourceView*none[11]{};ID3D11UnorderedAccessView*nullUav[3]{};
   context_->OMSetRenderTargets(0,nullptr,nullptr);context_->PSSetShaderResources(0,11,none);context_->CSSetShaderResources(0,11,none);context_->CSSetUnorderedAccessViews(0,3,nullUav,nullptr);
   ID3D11ShaderResourceView*inputs[11]{};
   for(unsigned j=0;j<11;++j){int source=pass.inputs[j];inputs[j]=zero_.srv.Get();if(source>=0&&source<8){auto&v=outputs_[unsigned(source)];if(v.tex[v.current].srv)inputs[j]=v.tex[v.current].srv.Get();}
    else if(source==8)inputs[j]=audio_.srv.Get();else if(source==9)inputs[j]=noise_.srv.Get();else if(source==10)inputs[j]=keyboard_.srv.Get();}
   Extent size=extent_;Tex*target=nullptr;unsigned next=0;
   if(pass.output>=0){auto&pair=outputs_[unsigned(pass.output)];next=1-pair.current;target=&pair.tex[next];size=target->size;}
   const bool lightPass=p.storage&&selection_.music&&std::strcmp(pass.key,"phys")!=0&&std::strcmp(pass.key,"physinit")!=0;
   c.size={float(size.width),float(size.height),t,float(lightPass?outputDelta_:stepDelta_)};c.meta.y=float(repeat);
   // Explicit per-pass union of slots unused by these two source programs.
   // Other passes, Original modes and all 64 spectral arrays retain their ABI.
   Constants submitted=c;
   if(selection_.music&&std::strcmp(p.id,"cxbf4gfw")==0){
    submitted.meta=storm_.events[0].descriptor();submitted.bands=storm_.events[1].descriptor();submitted.attacks=storm_.events[2].descriptor();
    // These slots have no other consumers in Cloud Bay. Retain the projection
    // at each channel's birth so resizing cannot relocate a live discharge.
    submitted.options.y=storm_.events[0].birthAspect;submitted.options.z=storm_.events[1].birthAspect;
    submitted.motion.x=storm_.events[2].birthAspect;
   }
   if(selection_.music&&std::strcmp(p.id,"u4uhc9b9")==0&&std::strcmp(pass.key,"pass1")==0){
    auto phases=[&](unsigned n){return V4{float(radianceMotion_.phase[n]),float(radianceMotion_.phase[n+1]),float(radianceMotion_.phase[n+2]),float(radianceMotion_.phase[n+3])};};
    submitted.bands=phases(0);submitted.attacks=phases(4);submitted.meta=phases(8);
   }
   context_->UpdateSubresource(constants_.Get(),0,nullptr,&submitted,0,0);
   if(pass.final)submittedConstants_=submitted;
   if(pass.stage==Stage::fragment){D3D11_VIEWPORT v{0,0,float(size.width),float(size.height),0,1};context_->RSSetViewports(1,&v);
    ID3D11RenderTargetView*r=target?target->rtv.Get():nullptr;context_->OMSetRenderTargets(1,&r,nullptr);
    context_->PSSetShader(programs_[i].ps.Get(),nullptr,0);context_->PSSetShaderResources(0,11,inputs);context_->Draw(3,0);++diag_.draws;
   }else{ID3D11UnorderedAccessView*views[3]{storageUav_.Get(),photonsUav_.Get(),target?target->uav.Get():nullptr};
    context_->CSSetShader(programs_[i].cs.Get(),nullptr,0);context_->CSSetShaderResources(0,11,inputs);context_->CSSetUnorderedAccessViews(0,3,views,nullptr);
    if(pass.dispatch==1)context_->Dispatch(1,1,1);else if(pass.dispatch==2)context_->Dispatch((rayCount_+31)/32,1,1);else context_->Dispatch((size.width+15)/16,(size.height+15)/16,1);
    ++diag_.dispatches;
   }
   if(pass.output>=0)outputs_[unsigned(pass.output)].current=next;
  }
 }
 onceDone_=true;++diag_.frame;
 if(!physicsOnly_){++diag_.produced;pending_=true;producedThisFrame_=true;}
}
bool Renderer::ensureBitmap(ID2D1DeviceContext* c,std::uint64_t epoch){
 if(bitmapContext_.Get()!=c||bitmapEpoch_!=epoch){invalidateBitmap();bitmapContext_=c;bitmapEpoch_=epoch;}
 const unsigned side=outputs_[7].current;
 if(!bitmapSides_[side]){auto*texture=outputTexture();if(!texture)return false;
  Com<IDXGISurface> surface;auto hr=texture->QueryInterface(IID_PPV_ARGS(surface.GetAddressOf()));
  if(SUCCEEDED(hr)){const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
   hr=c->CreateBitmapFromDxgiSurface(surface.Get(),&props,bitmapSides_[side].GetAddressOf());}
  if(FAILED(hr)){fail(hr);return false;}}
 bitmap_=bitmapSides_[side];return true;
}
bool Renderer::resizeTargets(Extent requested,ID2D1DeviceContext* d2d,std::uint64_t epoch) {
 // C8: same-device transaction. No program, sampler, constant buffer, analysis
 // texture, audio receipt generation or simulation clock is recreated here.
 const auto& preset=presets()[selection_.preset];
 ++diag_.resizeAttempts;
 const auto reject=[&](HRESULT hr)->bool {
  diag_.resizeLastHr=FAILED(hr)?hr:E_FAIL;++diag_.rejectedResizes;
  if(dxgiDeviceLost(hr))fail(hr);
  return false;
 };
 if(!d2d||!requested.width||!requested.height||requested.width>2048||requested.height>2048)
  return reject(E_INVALIDARG);
 const auto bytesPerPixel=[](DXGI_FORMAT f)->unsigned {
  return f==DXGI_FORMAT_R32G32B32A32_FLOAT?16U:f==DXGI_FORMAT_R16G16B16A16_FLOAT?8U:4U;
 };
 std::array<Pair,8> replacement{};
 std::array<bool,8> used{};
 std::uint64_t candidateBytes=0,retiredBytes=0;
 for(unsigned i=0;i<preset.count;++i){const auto& pass=preset.passes[i];
  if(pass.output<0)continue;
  const unsigned slot=unsigned(pass.output);
  if(slot>=replacement.size()||!pass.scale)return reject(E_INVALIDARG);
  if(used[slot])return reject(E_INVALIDARG); // catalogue must define one owner per output
  used[slot]=true;
  const auto format=pass.final?DXGI_FORMAT_R8G8B8A8_UNORM:
      pass.fullFloat?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
  const Extent size{std::max(1U,requested.width/pass.scale),std::max(1U,requested.height/pass.scale)};
  candidateBytes+=2ULL*size.width*size.height*bytesPerPixel(format);
 }
 if(preset.storage)candidateBytes+=std::uint64_t(requested.width)*requested.height*12ULL;
 // Explicit temporary-overlap bound, not the 12-ms timing budget. On refusal
 // retain the prior frame/graph and expose resizeLastHr, with bounded retry.
 constexpr std::uint64_t maximumOverlap=384ULL*1024ULL*1024ULL;
 if(diag_.bytes>maximumOverlap||candidateBytes>maximumOverlap-diag_.bytes)
  return reject(E_OUTOFMEMORY);
 StateScope state(context_.Get());
 unsigned createdForProbe=0;
 for(unsigned i=0;i<preset.count;++i){const auto& pass=preset.passes[i];if(pass.output<0)continue;
  const auto format=pass.final?DXGI_FORMAT_R8G8B8A8_UNORM:
      pass.fullFloat?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
  const Extent size{std::max(1U,requested.width/pass.scale),std::max(1U,requested.height/pass.scale)};
  for(auto& target:replacement[unsigned(pass.output)].tex){
   D3D11_TEXTURE2D_DESC desc{};desc.Width=size.width;desc.Height=size.height;
   desc.MipLevels=desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;
   desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
   if(pass.stage==Stage::compute)desc.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
   if(resizeFailureProbe_&&++createdForProbe==resizeFailureProbe_){
    resizeFailureProbe_=0;return reject(E_OUTOFMEMORY);
   }
   auto hr=device_->CreateTexture2D(&desc,nullptr,target.resource.GetAddressOf());
   if(SUCCEEDED(hr))hr=device_->CreateShaderResourceView(target.resource.Get(),nullptr,target.srv.GetAddressOf());
   if(SUCCEEDED(hr))hr=device_->CreateRenderTargetView(target.resource.Get(),nullptr,target.rtv.GetAddressOf());
   if(SUCCEEDED(hr)&&pass.stage==Stage::compute)
    hr=device_->CreateUnorderedAccessView(target.resource.Get(),nullptr,target.uav.GetAddressOf());
   if(FAILED(hr))return reject(hr);
   target.size=size;const float zero[4]{};context_->ClearRenderTargetView(target.rtv.Get(),zero);
  }
 }
 Com<ID3D11Buffer> newPhotons;Com<ID3D11UnorderedAccessView> newPhotonView;
 if(preset.storage){
  D3D11_BUFFER_DESC desc{};desc.ByteWidth=requested.width*requested.height*12U;
  desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  auto hr=device_->CreateBuffer(&desc,nullptr,newPhotons.GetAddressOf());
  D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=desc.ByteWidth/4U;
  view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
  if(SUCCEEDED(hr))hr=device_->CreateUnorderedAccessView(newPhotons.Get(),&view,newPhotonView.GetAddressOf());
  if(FAILED(hr))return reject(hr);
  const UINT zero[4]{};context_->ClearUnorderedAccessViewUint(newPhotonView.Get(),zero);
 }
 // Validate D2D consumers before replacing the existing graph. A resource
 // allocation success is not enough to declare the final surface drawable.
 std::array<Com<ID2D1Bitmap1>,2> newBitmaps{};
 for(unsigned side=0;side<2;++side){
  if(!replacement[7].tex[side].resource)return reject(E_INVALIDARG);
  Com<IDXGISurface> surface;
  auto hr=replacement[7].tex[side].resource->QueryInterface(IID_PPV_ARGS(surface.GetAddressOf()));
  if(SUCCEEDED(hr)){
   const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
       D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
   hr=d2d->CreateBitmapFromDxgiSurface(surface.Get(),&properties,newBitmaps[side].GetAddressOf());
  }
  if(FAILED(hr))return reject(hr);
 }
 context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(resample_.Get(),nullptr,0);
 context_->RSSetState(rasterizer_.Get());const FLOAT blendFactor[4]{};
 context_->OMSetBlendState(blend_.Get(),blendFactor,0xffffffff);context_->OMSetDepthStencilState(depth_.Get(),0);
 ID3D11SamplerState* sampler=sampler_.Get();context_->PSSetSamplers(0,1,&sampler);
 ID3D11Buffer* constants=constants_.Get();context_->PSSetConstantBuffers(0,1,&constants);
 for(unsigned slot=0;slot<8;++slot){
  const auto& source=outputs_[slot].tex[outputs_[slot].current];
  auto& targetPair=replacement[slot];
  for(const auto& texture:outputs_[slot].tex)if(texture.resource){
   D3D11_TEXTURE2D_DESC desc{};texture.resource->GetDesc(&desc);
   retiredBytes+=std::uint64_t(desc.Width)*desc.Height*bytesPerPixel(desc.Format);
  }
  if(!source.srv||!targetPair.tex[0].rtv)continue;
  Constants scale{};scale.size={1,1,1,1};
  if(std::strcmp(preset.id,"fspiu4nn")==0&&(slot==0||slot==6)){
   scale.size.x=float(targetPair.tex[0].size.width)/source.size.width;
   scale.size.y=float(targetPair.tex[0].size.height)/source.size.height;
  }
  context_->UpdateSubresource(constants_.Get(),0,nullptr,&scale,0,0);
  for(auto& target:targetPair.tex){
   ID3D11ShaderResourceView* nil=nullptr;context_->PSSetShaderResources(0,1,&nil);
   ID3D11RenderTargetView* out=target.rtv.Get();context_->OMSetRenderTargets(1,&out,nullptr);
   D3D11_VIEWPORT viewport{0,0,float(target.size.width),float(target.size.height),0,1};
   context_->RSSetViewports(1,&viewport);ID3D11ShaderResourceView* in=source.srv.Get();
   context_->PSSetShaderResources(0,1,&in);context_->Draw(3,0);
  }
  targetPair.current=0;
#ifdef GODEN_SHADER_RENDER_PROCESS
  if(slot==1&&source.size.width>=8&&targetPair.tex[0].size.width>=8){
   context_->OMSetRenderTargets(0,nullptr,nullptr);
   ID3D11ShaderResourceView* nil=nullptr;context_->PSSetShaderResources(0,1,&nil);
   const D3D11_BOX box{source.size.width-8,0,0,source.size.width,1,1};
   for(auto& target:targetPair.tex)
    context_->CopySubresourceRegion(target.resource.Get(),0,target.size.width-8,0,0,source.resource.Get(),0,&box);
  }
#endif
 }
 if(photons_){D3D11_BUFFER_DESC desc{};photons_->GetDesc(&desc);retiredBytes+=desc.ByteWidth;}
 const auto deviceStatus=device_->GetDeviceRemovedReason();
 if(FAILED(deviceStatus))return reject(deviceStatus);
 state.clear();
 // Commit: all dependent allocations and wraps are complete. The simulation
 // clock, immutable analysis, history receipt epoch, particles/rays and phases
 // stay alive. Photon *screen* accumulation alone restarts on a changed grid.
 outputs_=std::move(replacement);
 if(preset.storage){photons_=std::move(newPhotons);photonsUav_=std::move(newPhotonView);}
 bitmapSides_=std::move(newBitmaps);bitmap_=bitmapSides_[outputs_[7].current];bitmapContext_=d2d;bitmapEpoch_=epoch;
 extent_=requested;wanted_=requested;
 diag_.bytes=diag_.bytes>=retiredBytes?diag_.bytes-retiredBytes+candidateBytes:candidateBytes;
 diag_.width=requested.width;diag_.height=requested.height;++diag_.resizeCount;
 ++diag_.produced;pending_=true;producedThisFrame_=true;diag_.ready=true;
 diag_.resizeLastHr=S_OK;
 // Pending query objects are retained and drained; their old epoch cannot vote
 // on the resized graph's performance. No query object is recreated in flight.
 ++resourceEpoch_;sampleCount_=0;warmup_=6;
 return true;
}
int Renderer::queryBegin(){if(!queries_[0].disjoint)return -1;for(unsigned i=0;i<queries_.size();++i){auto&q=queries_[i];if(q.disjoint&&!q.pending){q.epoch=resourceEpoch_;context_->Begin(q.disjoint.Get());context_->End(q.begin.Get());return int(i);}}++diag_.queryDrops;return -1;}
void Renderer::queryEnd(int i){if(i<0)return;auto&q=queries_[unsigned(i)];context_->End(q.end.Get());context_->End(q.disjoint.Get());q.pending=true;}
void Renderer::collect(){
 for(auto&q:queries_){if(!q.pending)continue;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 a=0,b=0;
  auto h=context_->GetData(q.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH);if(h==S_FALSE)continue;
  if(FAILED(h)||d.Disjoint||!d.Frequency){q.pending=false;continue;}
  auto h1=context_->GetData(q.begin.Get(),&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH),h2=context_->GetData(q.end.Get(),&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH);
  if(h1==S_FALSE||h2==S_FALSE)continue;q.pending=false;if(FAILED(h1)||FAILED(h2)||b<a||q.epoch!=resourceEpoch_)continue;
  if(warmup_){--warmup_;continue;}samples_[sampleCount_++]=double(b-a)*1e6/double(d.Frequency);
  if(sampleCount_==samples_.size()){auto sorted=samples_;std::sort(sorted.begin(),sorted.end());sampleCount_=0;recordGpuWindow(sorted[28]);}
 }
}
void Renderer::recordGpuWindow(double p95Us) noexcept {
 if(!std::isfinite(p95Us)||p95Us<=0||!std::isfinite(now_))return;
 diag_.gpuP95Us=p95Us;diag_.gpuWindowEpoch=resourceEpoch_;
}
void Renderer::fail(HRESULT h)noexcept{diag_.result=FAILED(h)?h:E_FAIL;diag_.ready=false;diag_.active=false;timeline_.active(false,now_);pending_=false;}
bool Renderer::prepare(ID3D11Device*dev,ID3D11DeviceContext*ctx,ID2D1DeviceContext*d2d,std::uint64_t epoch,
 D2D1_SIZE_F viewport,const Selection& selection,const AudioFrame*audio,AudioIdentity expected,
 const RetainedAudioRows::Batch* history,std::uint64_t historyGeneration,
 HistoryApplyReceipt* historyReceipt,double now,bool allowWork)noexcept{
 const auto begin=std::chrono::steady_clock::now();struct Guard{double&v;std::chrono::steady_clock::time_point b;~Guard(){v=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-b).count();}}g{diag_.cpuUs,begin};
 HistoryApplyReceipt ignoredReceipt{};if(historyReceipt)*historyReceipt={};auto&receipt=historyReceipt?*historyReceipt:ignoredReceipt;
 producedThisFrame_=false;now_=now;
 if(!dev||!ctx||!d2d||!std::isfinite(now)||selection.preset>=presets().size())return false;
 const AudioFrame* acceptedAudio=audio&&audio->valid&&audio->serial==expected.serial&&
     audio->recording==expected.recording?audio:nullptr;
 try{const auto&p=presets()[selection.preset];
  const auto nominal=c8FitExtent(viewport.width,viewport.height,p,selection.music);
  if(!nominal.width||!nominal.height)return false;
#ifndef GODEN_SHADER_RENDER_PROCESS
  if(std::strcmp(p.id,"wXdfzj")==0){
   if(!allowWork&&!remote_)return false;
   if(!remote_||device_.Get()!=dev||context_.Get()!=ctx){
    reset();remote_=std::make_unique<RemoteFrameClient>();device_=dev;context_=ctx;
   }
   now_=now;selection_=selection;diag_.preset=selection.preset;
   timeline_.transport(playing_,wantedActive_,now);
   const bool submit=timeline_.due(now);
   if(submit)timeline_.advance(now,p.rate);
   const auto before=remote_->output().sequence;
   const bool ready=remote_->prepare(dev,ctx,d2d,epoch,viewport,selection,acceptedAudio,expected,now,
       playing_,wantedActive_,allowWork,submit&&allowWork);
   const auto& out=remote_->output();
   remotePollAt_=now+1.0/double(p.rate);
   diag_.result=remote_->result();
   if(FAILED(diag_.result)){fail(diag_.result);return false;}
   diag_.ready=ready;diag_.active=wantedActive_&&timeline_.active();
   diag_.deferred=!allowWork;diag_.frame=out.frames;diag_.audioSequenceApplied=out.audioSequence;
   diag_.width=out.width;diag_.height=out.height;
   diag_.bytes=out.bytes+goden::frames::SurfaceEdge*goden::frames::SurfaceEdge*4ULL+std::uint64_t(out.width)*out.height*4;
   diag_.gpuP95Us=out.gpuP95Us;diag_.gpuTimingAvailable=out.gpuP95Us>0;
   diag_.gpuWindowEpoch=diag_.gpuTimingAvailable?resourceEpoch_:0;
   if(out.sequence!=before){++diag_.produced;pending_=true;producedThisFrame_=true;}
   return ready;
  }
  if(remote_)reset();
#endif
  const bool requestChanged=device_.Get()!=dev||context_.Get()!=ctx||selection_.preset!=selection.preset||selection_.music!=selection.music||lastRestart_!=selection.restart;
  if(failed()&&!requestChanged)return false;
  const bool changed=!initialized_||requestChanged;
  diag_.deferred=!allowWork;
  if(!allowWork){
   timeline_.transport(playing_,wantedActive_,now);timeline_.defer(now);
   // Do not allocate, resize, dispatch or acknowledge history during occlusion.
   if(changed){diag_.ready=false;return false;}
   return diag_.ready;
  }
  if(!changed){
   collect();if(failed())return false;
  }
  const auto size=nominal;
  if(!(size==wanted_)){wanted_=size;resizeSince_=now;resizeRetryAfter_=0;resizeAttemptCount_=0;}
  const bool resize=initialized_&&!(size==extent_)&&now-resizeSince_>=.15;
  if(changed){
   StateScope state(ctx);const auto active=wantedActive_;
   if(!allocate(dev,ctx,selection,size,now))return false;
   wantedActive_=active;
  }else if(resize&&resizeAttemptCount_<3&&now>=resizeRetryAfter_){
   ++resizeAttemptCount_;
   if(resizeTargets(size,d2d,epoch)){resizeAttemptCount_=0;resizeRetryAfter_=0;}
   else {
    resizeRetryAfter_=now+(resizeAttemptCount_==1?.20:resizeAttemptCount_==2?.50:1.0);
   }
  }
  if(failed())return false;
  diag_.nominalWidth=nominal.width;diag_.nominalHeight=nominal.height;
  diag_.budget=false;diag_.pressureTier=0;diag_.pressureEvents=0;diag_.pressureResumeSeconds=0;
  timeline_.transport(playing_,wantedActive_,now);diag_.active=timeline_.active();diag_.deferred=!allowWork;
  if(!allowWork && diag_.frame){timeline_.defer(now);diag_.ready=ensureBitmap(d2d,epoch);return diag_.ready;}
  selection_.intensity=selection.intensity;selection_.albumColors=selection.albumColors;selection_.palette=selection.palette;
  bool identityChanged=false;
  {StateScope scope(ctx);identityChanged=synchronizeAudioIdentity(expected);}
  const bool wasDue=timeline_.due(now,p.rate);
  const auto steps=diag_.frame?timeline_.advance(now,p.rate):1U;
#ifdef GODEN_SHADER_RENDER_PROCESS
  const bool optical=true;
#else
  const bool optical=false;
#endif
  const bool renderLatest=(!p.stateful||optical)&&wasDue&&timeline_.delta()>0.;
  if(steps||renderLatest||identityChanged){
   StateScope scope(ctx);uploadAudio(history,expected,historyGeneration,receipt);
   const int query=queryBegin();
   outputDelta_=std::clamp(timeline_.delta(),1.0/240.0,.10);
   if(identityChanged&&!steps&&!renderLatest){
    // Refresh paused track ownership without advancing simulation time.
    physicsOnly_=false;stepDelta_=outputDelta_=0;stepTime_=timeline_.elapsed();step(acceptedAudio);
   }else if(p.stateful&&!optical){
    for(unsigned i=0;i<steps;++i){
     physicsOnly_=p.storage&&selection_.music&&i+1<steps;
     stepDelta_=1.0/60.0;
     stepTime_=std::max(0.0,timeline_.elapsed()-double(steps-i-1)/60.0);
     step(acceptedAudio);
    }
   }else{
    // Temporal image feedback is not a numerical solver. Raymarch/bloom once
    // per output, with a duration-aware TAA weight in the Music source.
    physicsOnly_=false;stepDelta_=outputDelta_;stepTime_=timeline_.elapsed();
    step(acceptedAudio);
   }
   physicsOnly_=false;
   queryEnd(query);
  }
  diag_.ready=ensureBitmap(d2d,epoch);return diag_.ready;
 }catch(...){fail(E_OUTOFMEMORY);return false;}
}
bool Renderer::draw(ID2D1RenderTarget*t,D2D1_RECT_F r,float opacity)noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_){const bool drawn=diag_.ready&&remote_->draw(t,r,opacity,presets()[selection_.preset].flipY);if(drawn)diag_.drawn=diag_.produced;return drawn;}
#endif
 if(!t||!bitmap_||!diag_.ready||r.right<=r.left||r.bottom<=r.top)return false;
 D2D1_MATRIX_3X2_F previous{};t->GetTransform(&previous);
 // Flip once at the boundary from source GL convention into native top-left.
 if(presets()[selection_.preset].flipY)t->SetTransform(D2D1::Matrix3x2F::Scale(1,-1,D2D1::Point2F(0,(r.top+r.bottom)*.5f))*previous);
 const auto source=visualSourceRectangle(bitmap_->GetSize(),r);
 t->DrawBitmap(bitmap_.Get(),r,std::clamp(opacity,0.f,1.f),D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,&source);t->SetTransform(previous);
 diag_.drawn=diag_.produced;return true;
}
void Renderer::active(bool value,double now)noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_&&!value&&wantedActive_)remote_->suspend(now);
#endif
 wantedActive_=value;timeline_.transport(playing_,value&&!failed(),now);diag_.active=timeline_.active();
}
void Renderer::transport(bool playing,bool eligible,double now)noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_&&!eligible&&wantedActive_)remote_->suspend(now);
#endif
 playing_=playing;wantedActive_=eligible;timeline_.transport(playing,eligible&&!failed(),now);diag_.active=timeline_.active();
}
void Renderer::presented(bool accepted)noexcept{if(accepted)diag_.accepted=std::max(diag_.accepted,std::min(diag_.drawn,diag_.produced));pending_=diag_.accepted<diag_.produced;}
void Renderer::retry()noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_){remote_.reset();initialized_=false;}
#endif
 ++diag_.explicitRetries;diag_.pressureTier=0;diag_.pressureEvents=0;diag_.pressureResumeSeconds=0;resizeAttemptCount_=0;resizeRetryAfter_=0;if(FAILED(diag_.result))initialized_=false;diag_.result=S_OK;diag_.budget=false;sampleCount_=0;warmup_=6;++resourceEpoch_;timeline_.transport(playing_,wantedActive_,now_);}
void Renderer::invalidateBitmap()noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 if(remote_)remote_->invalidateBitmap();
#endif
 bitmap_.Reset();bitmapSides_={};bitmapContext_.Reset();bitmapEpoch_=0;
}
void Renderer::reset()noexcept{
#ifndef GODEN_SHADER_RENDER_PROCESS
 remote_.reset();
#endif
 invalidateBitmap();programs_={};outputs_={};queries_={};audio_={};noise_={};keyboard_={};zero_={};
 constants_.Reset();vertex_.Reset();resample_.Reset();storage_.Reset();photons_.Reset();storageUav_.Reset();photonsUav_.Reset();sampler_.Reset();clampSampler_.Reset();rasterizer_.Reset();blend_.Reset();depth_.Reset();
 context_.Reset();device_.Reset();initialized_=pending_=onceDone_=false;lastAudio_=audioSerial_=audioRecording_=0;audioHead_=audioCount_=0;extent_={};wanted_={};
 sampleCount_=0;warmup_=6;timeline_={};musicalPhase_={};c8Motion_={};storm_={};radianceMotion_={};spectrumScroll_={};resizeAttemptCount_=0;resizeRetryAfter_=0;diag_={};submittedConstants_={};++resourceEpoch_;
}
} // namespace
