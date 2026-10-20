#include "a4_effect.h"
#include <d2d1effectauthor.h>
#include <d2d1helper.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <mutex>
#include <string>
#include <vector>
#include "a4_field_ps.h"
#include "a4_glass_ps.h"
#include "a4_cover_ps.h"
#include "a4_film_ps.h"
#include "a4_encode_ps.h"
namespace nekorox::component::a4 {
namespace {
constexpr GUID ids[]{
 {0xf316b001,0xac61,0x432e,{0xa1,0x17,0x1b,0x25,0x47,0x40,0x32,0x01}},
 {0xf316b002,0xac61,0x432e,{0xa1,0x17,0x1b,0x25,0x47,0x40,0x32,0x02}},
 {0xf316b003,0xac61,0x432e,{0xa1,0x17,0x1b,0x25,0x47,0x40,0x32,0x03}},
 {0xf316b004,0xac61,0x432e,{0xa1,0x17,0x1b,0x25,0x47,0x40,0x32,0x04}},
 {0xf316b005,0xac61,0x432e,{0xa1,0x17,0x1b,0x25,0x47,0x40,0x32,0x05}}};
const BYTE* shader(unsigned i){switch(i){case 0:return g_a4Field;case 1:return g_a4Glass;case 2:return g_a4Cover;case 3:return g_a4Film;default:return g_a4Encode;}}
UINT32 shaderSize(unsigned i){switch(i){case 0:return sizeof(g_a4Field);case 1:return sizeof(g_a4Glass);case 2:return sizeof(g_a4Cover);case 3:return sizeof(g_a4Film);default:return sizeof(g_a4Encode);}}
UINT32 inputs(unsigned i){return i==0?0U:i==2?3U:1U;}
class Effect final:public ID2D1EffectImpl,public ID2D1DrawTransform {
 std::atomic<ULONG> refs_{1};unsigned pass_;Constants c_{};
 Microsoft::WRL::ComPtr<ID2D1DrawInfo> draw_;
 explicit Effect(unsigned p):pass_(p){}
 public:
 template<unsigned P>static HRESULT CALLBACK make(IUnknown** out) noexcept {
  if(!out)return E_POINTER;*out=static_cast<ID2D1EffectImpl*>(new(std::nothrow)Effect(P));return *out?S_OK:E_OUTOFMEMORY;
 }
 static HRESULT CALLBACK set(IUnknown* p,const BYTE* b,UINT32 n) noexcept {
  if(!p||!b||n!=sizeof(Constants))return E_INVALIDARG;
  Microsoft::WRL::ComPtr<ID2D1EffectImpl> i;auto hr=p->QueryInterface(IID_PPV_ARGS(i.GetAddressOf()));if(FAILED(hr))return hr;
  Constants c;std::memcpy(&c,b,n);
  std::array<float,sizeof(Constants)/sizeof(float)> values{}; std::memcpy(values.data(),&c,sizeof(c));
  for(float v:values)if(!std::isfinite(v))return E_INVALIDARG;
  if(c.output.x<1||c.output.y<1||c.output.x>16384||c.output.y>16384||
      c.viewport.x<=0||c.viewport.y<=0||c.viewport.z<=0)return E_INVALIDARG;
   auto* self=static_cast<Effect*>(i.Get());
   if(inputs(self->pass_)>0&&(c.input0.x<=0||c.input0.y<=0))return E_INVALIDARG;
   if(inputs(self->pass_)>1&&(c.input1.x<=0||c.input1.y<=0))return E_INVALIDARG;
   if(inputs(self->pass_)>2&&(c.lightControl.z<=0||c.lightControl.w<=0))return E_INVALIDARG;
   if((self->pass_==1||self->pass_==2)&&
      (c.rect.z<c.rect.x||c.rect.w<c.rect.y||std::abs(c.rect.x)>32768||
       std::abs(c.rect.y)>32768||std::abs(c.rect.z)>32768||std::abs(c.rect.w)>32768))return E_INVALIDARG;
  static_cast<Effect*>(i.Get())->c_=c;return S_OK;
 }
 static HRESULT CALLBACK get(const IUnknown* p,BYTE* b,UINT32 n,UINT32* actual) noexcept {
  if(actual)*actual=sizeof(Constants);if(!p)return E_POINTER;if(!b)return S_OK;if(n<sizeof(Constants))return E_NOT_SUFFICIENT_BUFFER;
  Microsoft::WRL::ComPtr<ID2D1EffectImpl> i;auto hr=const_cast<IUnknown*>(p)->QueryInterface(IID_PPV_ARGS(i.GetAddressOf()));
  if(SUCCEEDED(hr))std::memcpy(b,&static_cast<Effect*>(i.Get())->c_,sizeof(Constants));return hr;
 }
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override {
  if(!p)return E_POINTER;*p=nullptr;
  if(id==__uuidof(IUnknown)||id==__uuidof(ID2D1EffectImpl))*p=static_cast<ID2D1EffectImpl*>(this);
  else if(id==__uuidof(ID2D1TransformNode))*p=static_cast<ID2D1TransformNode*>(this);
  else if(id==__uuidof(ID2D1Transform))*p=static_cast<ID2D1Transform*>(this);
  else if(id==__uuidof(ID2D1DrawTransform))*p=static_cast<ID2D1DrawTransform*>(this);
  if(!*p)return E_NOINTERFACE;AddRef();return S_OK;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Initialize(ID2D1EffectContext* c,ID2D1TransformGraph* g)override{
  if(!c||!g)return E_POINTER;HRESULT hr=c->LoadPixelShader(ids[pass_],shader(pass_),shaderSize(pass_));
  return FAILED(hr)?hr:g->SetSingleTransformNode(this);
 }
 HRESULT STDMETHODCALLTYPE PrepareForRender(D2D1_CHANGE_TYPE)override{
  return draw_?draw_->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&c_),sizeof(c_)):E_UNEXPECTED;
 }
 HRESULT STDMETHODCALLTYPE SetGraph(ID2D1TransformGraph*)override{return E_NOTIMPL;}
 UINT32 STDMETHODCALLTYPE GetInputCount()const override{return inputs(pass_);}
 HRESULT STDMETHODCALLTYPE MapOutputRectToInputRects(const D2D1_RECT_L* out,D2D1_RECT_L* in,UINT32 n)const override{
  if(!out||n!=inputs(pass_)||(n&&!in))return E_INVALIDARG;
  // Conservative finite source bounds; complex sampling is declared in HLSL.
  // Backdrop is already cached and filtered, so this never recomputes blur per pane.
  for(UINT32 i=0;i<n;++i){auto s=i==2?Float4{c_.lightControl.z,c_.lightControl.w,0,0}:i?c_.input1:c_.input0;in[i]=D2D1::RectL(0,0,LONG(std::ceil(s.x)),LONG(std::ceil(s.y)));}
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE MapInputRectsToOutputRect(const D2D1_RECT_L*,const D2D1_RECT_L*,UINT32 n,D2D1_RECT_L* out,D2D1_RECT_L* opaque)override{
  if(!out||!opaque||n!=inputs(pass_))return E_INVALIDARG;
  *out=(pass_==1||pass_==2) ? D2D1::RectL(LONG(std::floor(c_.rect.x)),LONG(std::floor(c_.rect.y)),LONG(std::ceil(c_.rect.z)),LONG(std::ceil(c_.rect.w))) : D2D1::RectL(0,0,LONG(std::ceil(c_.output.x)),LONG(std::ceil(c_.output.y)));
  *opaque=(pass_==0||pass_==4)?*out:D2D1::RectL(0,0,0,0);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE MapInvalidRect(UINT32 i,D2D1_RECT_L,D2D1_RECT_L* out)const override{
  if(!out||i>=inputs(pass_))return E_INVALIDARG;
  *out=(pass_==1||pass_==2)?D2D1::RectL(LONG(std::floor(c_.rect.x)),LONG(std::floor(c_.rect.y)),LONG(std::ceil(c_.rect.z)),LONG(std::ceil(c_.rect.w))):D2D1::RectL(0,0,LONG(std::ceil(c_.output.x)),LONG(std::ceil(c_.output.y)));return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetDrawInfo(ID2D1DrawInfo* info)override{
  if(!info)return E_POINTER;draw_=info;auto hr=info->SetPixelShader(ids[pass_]);if(FAILED(hr))return hr;
  for(UINT32 i=0;i<inputs(pass_);++i){D2D1_INPUT_DESCRIPTION d{D2D1_FILTER_MIN_MAG_MIP_LINEAR,1U};if(FAILED(hr=info->SetInputDescription(i,d)))return hr;}
  return info->SetOutputBuffer(pass_==0?D2D1_BUFFER_PRECISION_16BPC_FLOAT:D2D1_BUFFER_PRECISION_8BPC_UNORM,D2D1_CHANNEL_DEPTH_4);
 }
};
HRESULT registerOne(ID2D1Factory1* f,unsigned p){
 if(!f||p>=5)return E_INVALIDARG;
 static std::mutex m;
 std::lock_guard lock(m);
 // The factory is the authority. A raw address can be reused after destruction.
 Microsoft::WRL::ComPtr<ID2D1Properties> properties;
 const auto known=f->GetEffectProperties(ids[p],properties.GetAddressOf());
 if(SUCCEEDED(known))return S_OK;
 if(known!=D2DERR_EFFECT_IS_NOT_REGISTERED &&
    known!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND))return known;
 std::wstring xml=LR"(<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='GODEN A4 Spectral Alloy'/><Property name='Author' type='string' value='GODENplayer'/><Property name='Category' type='string' value='Artistic'/><Property name='Description' type='string' value='Bounded Album optics'/><Inputs>)";
 for(unsigned i=0;i<inputs(p);++i)xml+=L"<Input name='Source"+std::to_wstring(i)+L"'/>";
 xml+=L"</Inputs><Property name='Constants' type='blob'><Property name='DisplayName' type='string' value='A4 constants'/></Property></Effect>";
 const D2D1_PROPERTY_BINDING binding{L"Constants",&Effect::set,&Effect::get};
 PD2D1_EFFECT_FACTORY factory=nullptr;
 switch(p){case 0:factory=&Effect::make<0>;break;case 1:factory=&Effect::make<1>;break;case 2:factory=&Effect::make<2>;break;case 3:factory=&Effect::make<3>;break;default:factory=&Effect::make<4>;break;}
 const auto hr=f->RegisterEffectFromString(ids[p],xml.c_str(),&binding,1,factory);
 return hr;
}
}
HRESULT createEffect(ID2D1DeviceContext* c,Pass pass,ID2D1Effect** result) noexcept{
 if(!c||!result)return E_POINTER;*result=nullptr;unsigned p=unsigned(pass);if(p>=5)return E_INVALIDARG;
 try{Microsoft::WRL::ComPtr<ID2D1Factory> b;c->GetFactory(b.GetAddressOf());Microsoft::WRL::ComPtr<ID2D1Factory1> f;
 auto hr=b.As(&f);if(FAILED(hr))return hr;hr=registerOne(f.Get(),p);return FAILED(hr)?hr:c->CreateEffect(ids[p],result);
 }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
HRESULT updateEffect(ID2D1Effect* e,const Constants& c) noexcept{
 return e?e->SetValueByName(L"Constants",D2D1_PROPERTY_TYPE_BLOB,reinterpret_cast<const BYTE*>(&c),sizeof(c)):E_POINTER;
}
}
