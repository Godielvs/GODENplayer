#include "a4/a4_effect.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>
using Microsoft::WRL::ComPtr;
using namespace nekorox::component::a4;
int main(){
 auto ci=CoInitializeEx(nullptr,COINIT_MULTITHREADED);int failures=0,checks=0;
 auto check=[&](bool value,const char* label){++checks;if(!value){++failures;std::cerr<<"FAIL "<<label<<'\n';}};
 ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> dc;D3D_FEATURE_LEVEL level{};
 auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
  nullptr,0,D3D11_SDK_VERSION,d.GetAddressOf(),&level,dc.GetAddressOf());
 if(FAILED(hr)){std::cerr<<"BLOCKED: hardware D3D11 device unavailable "<<std::hex<<hr<<'\n';return 3;}
 ComPtr<IDXGIDevice> dx;d.As(&dx);ComPtr<ID2D1Factory1> factory;D2D1_FACTORY_OPTIONS options{};
 hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,options,factory.GetAddressOf());if(FAILED(hr))return 4;
 ComPtr<ID2D1Device> device;ComPtr<ID2D1DeviceContext> context;
 if(FAILED(factory->CreateDevice(dx.Get(),device.GetAddressOf()))||FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.GetAddressOf())))return 5;
 constexpr UINT W=192,H=96;
 auto format=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
 auto prop=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,format,96,96);
 ComPtr<ID2D1Bitmap1> target,readback,input,detail;
 check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(W,H),nullptr,0,&prop,target.GetAddressOf())),"target");
 prop.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
 check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(W,H),nullptr,0,&prop,readback.GetAddressOf())),"readback");
 std::vector<unsigned char> pixels(W*H*4);
 for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){auto k=(y*W+x)*4;pixels[k]=static_cast<unsigned char>(x*255/W);pixels[k+1]=static_cast<unsigned char>(y*255/H);pixels[k+2]=90;pixels[k+3]=255;}
 prop.bitmapOptions=D2D1_BITMAP_OPTIONS_NONE;prop.dpiX=prop.dpiY=144;
 check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(W,H),pixels.data(),W*4,&prop,input.GetAddressOf())),"DPI 144 source");
 std::array<unsigned char,64*64*4> mask{};for(std::size_t i=0;i<mask.size()/4;++i)mask[i*4+3]=255;
 prop.dpiX=prop.dpiY=96;check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(64,64),mask.data(),256,&prop,detail.GetAddressOf())),"detail");
 Constants c{};c.viewport={1200,600,1,7};c.output={float(W),float(H),0,0};c.levels={.8f,.5f,.3f,1};
 c.hits={.4f,.2f,.1f,0};c.phase={.2f,.5f,.8f,1};
 c.color0={.35f,.08f,.015f,1};c.color1={.04f,.22f,.42f,1};c.color2={.6f,.5f,.3f,1};c.color3={.008f,.01f,.012f,1};
 c.material={.34f,.8f,.5f,.3f};c.rect={8,10,float(W-8),float(H-10)};c.input0={float(W)/1.5f,float(H)/1.5f,float(W),float(H)};
 c.input1={64,64,0,0};c.tuning={.07f,14,3,1};
 std::vector<unsigned char> first;
 auto run=[&](Pass pass,Constants p){
  ComPtr<ID2D1Effect> effect;auto status=createEffect(context.Get(),pass,effect.GetAddressOf());
  if(FAILED(status))std::cerr<<"create effect HRESULT=0x"<<std::hex<<static_cast<unsigned long>(status)<<std::dec<<'\n';
  check(SUCCEEDED(status),"create effect");if(FAILED(status))return std::vector<unsigned char>{};
  if(pass!=Pass::field)effect->SetInput(0,input.Get());if(pass==Pass::cover)effect->SetInput(1,detail.Get());
  check(SUCCEEDED(updateEffect(effect.Get(),p)),"constants");context->SetTarget(target.Get());context->SetDpi(96,96);context->BeginDraw();context->Clear(D2D1::ColorF(0.f,0.f,0.f,1.f));context->DrawImage(effect.Get());
  check(SUCCEEDED(context->EndDraw()),"EndDraw");context->SetTarget(nullptr);
  check(SUCCEEDED(readback->CopyFromBitmap(nullptr,target.Get(),nullptr)),"Copy");D2D1_MAPPED_RECT m{};
  if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&m)))return std::vector<unsigned char>{};
  std::vector<unsigned char> result(W*H*4);for(UINT y=0;y<H;++y)std::copy_n(m.bits+y*m.pitch,W*4,result.data()+y*W*4);readback->Unmap();
  return result;
 };
 first=run(Pass::field,c);auto p=c;p.phase.x+=1.1f;p.phase.y+=.6f;auto second=run(Pass::field,p);
 unsigned difference=0;if(first.size()==second.size())for(std::size_t i=0;i<first.size();++i)if(first[i]!=second[i])++difference;
 check(difference>W*H/20,"actual RGB changes, not opaque clear");
 for(auto pass:{Pass::glass,Pass::cover,Pass::film,Pass::encode})check(!run(pass,c).empty(),"consumer readback");
 ComPtr<ID2D1Effect> effect;createEffect(context.Get(),Pass::field,effect.GetAddressOf());p=c;p.output.x=0;
 check(FAILED(updateEffect(effect.Get(),p)),"reject invalid bounds");p=c;p.levels.x=std::nanf("");check(FAILED(updateEffect(effect.Get(),p)),"reject NaN");
 std::cout<<"{\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"provider\":\"hardware D3D11\"}\n";
 if(SUCCEEDED(ci))CoUninitialize();return failures?1:0;
}
