// SPDX-License-Identifier: GPL-3.0-only
// Independent native image program; no foobar2000 SDK, host or plugin imports.
// Covered NPGS-derived portions and remaining contributions: see COPYING.md.
#include "frame_protocol.h"
#include "../component/collection/collection_renderer.h"
#include "../component/collection/remote_frame_client.h"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <limits>

namespace {
using Microsoft::WRL::ComPtr;
using namespace nekorox::component::collection;
using goden::frames::Input;using goden::frames::Output;
struct Environment {
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 ComPtr<ID2D1Factory1> factory;ComPtr<ID2D1Device> d2dDevice;ComPtr<ID2D1DeviceContext> d2d;
 bool create(const LUID* wanted=nullptr,bool warp=false){
  ComPtr<IDXGIFactory1> dxgiFactory;ComPtr<IDXGIAdapter1> adapter;
  if(wanted){
   if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))))return false;
   for(UINT n=0;;++n){ComPtr<IDXGIAdapter1> candidate;const auto hr=dxgiFactory->EnumAdapters1(n,&candidate);if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return false;
    DXGI_ADAPTER_DESC1 desc{};if(FAILED(candidate->GetDesc1(&desc)))return false;
    if(desc.AdapterLuid.LowPart==wanted->LowPart&&desc.AdapterLuid.HighPart==wanted->HighPart){adapter=candidate;break;}}
   if(!adapter)return false;
  }
  D3D_FEATURE_LEVEL feature{};const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
  auto hr=D3D11CreateDevice(adapter.Get(),adapter?D3D_DRIVER_TYPE_UNKNOWN:warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,
      nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,1,D3D11_SDK_VERSION,&device,&feature,&context);
  if(FAILED(hr))return false;
  hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,IID_PPV_ARGS(&factory));
  ComPtr<IDXGIDevice> dxgi;if(SUCCEEDED(hr))hr=device.As(&dxgi);
  if(SUCCEEDED(hr))hr=factory->CreateDevice(dxgi.Get(),&d2dDevice);
  if(SUCCEEDED(hr))hr=d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&d2d);
  return SUCCEEDED(hr);
 }
};
bool validate(const Input& input){
 if(!std::isfinite(input.seconds)||input.seconds<0||!std::isfinite(input.width)||!std::isfinite(input.height)||
    input.width<1||input.height<1||input.width>65536||input.height>65536||!std::isfinite(input.intensity)||input.intensity<0||input.intensity>2||
    input.flags&~63U||input.sampleRate<8000||input.sampleRate>192000||!std::isfinite(input.gate)||input.gate<0||input.gate>1)return false;
 for(float x:input.energy)if(!std::isfinite(x)||x<0||x>1)return false;
 for(float x:input.onsets)if(!std::isfinite(x)||x<0||x>8)return false;
 for(float x:input.activity)if(!std::isfinite(x)||x< -1||x>1)return false;
 for(float x:input.colors)if(!std::isfinite(x)||x<0||x>1)return false;
 return true;
}
bool render(Environment& env,Renderer& renderer,const Input& input,Output& out){
 out={};out.sequence=input.sequence;out.generation=input.generation;
 if(!validate(input)){out.result=E_INVALIDARG;return false;}
 const bool playing=(input.flags&goden::frames::Playing)!=0,visible=(input.flags&goden::frames::Visible)!=0;
 renderer.transport(playing,visible,input.seconds);
 Selection selection{};
 const auto all=presets();selection.preset=static_cast<unsigned>(all.size());
 for(unsigned n=0;n<all.size();++n)if(std::strcmp(all[n].id,"wXdfzj")==0){selection.preset=n;break;}
 selection.music=(input.flags&goden::frames::Music)!=0;
 selection.albumColors=(input.flags&goden::frames::CoverColors)!=0;selection.restart=input.restart;selection.intensity=input.intensity;
 for(unsigned i=0;i<4;++i)selection.palette[i]={input.colors[i*4],input.colors[i*4+1],input.colors[i*4+2],input.colors[i*4+3]};
 AudioFrame audio{};audio.serial=input.track;audio.recording=input.stream;audio.sequence=input.audioSequence;audio.rate=input.sampleRate;
 audio.valid=(input.flags&goden::frames::AudioValid)!=0;audio.gate=input.gate;
 audio.level=input.energy;audio.hit=input.onsets;audio.relative=input.activity;
 const bool ok=renderer.prepare(env.device.Get(),env.context.Get(),env.d2d.Get(),1,{input.width,input.height},selection,
     audio.valid?&audio:nullptr,{input.track,input.stream},nullptr,0,nullptr,input.seconds,(input.flags&goden::frames::Work)!=0);
 if(!(input.flags&goden::frames::Work)){out.result=S_FALSE;return false;}
 const auto& d=renderer.diagnostics();out.result=ok?S_OK:FAILED(d.result)?d.result:E_FAIL;
 out.frames=d.frame;out.width=d.width;out.height=d.height;out.bytes=d.bytes;
 out.audioSequence=d.audioSequenceApplied;out.cpuUs=d.cpuUs;out.gpuP95Us=d.gpuP95Us;
 return ok;
}
bool drawAndAcknowledge(Environment& env,Renderer& renderer){
 auto* image=renderer.outputTexture();if(!image)return false;
 D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
 desc.MiscFlags=0;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D> canvas;ComPtr<IDXGISurface> surface;ComPtr<ID2D1Bitmap1> bitmap;
 auto hr=env.device->CreateTexture2D(&desc,nullptr,&canvas);if(SUCCEEDED(hr))hr=canvas.As(&surface);
 if(SUCCEEDED(hr)){const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
     D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
  hr=env.d2d->CreateBitmapFromDxgiSurface(surface.Get(),&props,&bitmap);}
 if(FAILED(hr))return false;
 env.d2d->SetTarget(bitmap.Get());env.d2d->BeginDraw();
 const bool drawn=renderer.draw(env.d2d.Get(),D2D1::RectF(0,0,float(desc.Width),float(desc.Height)));
 hr=env.d2d->EndDraw();env.d2d->SetTarget(nullptr);renderer.presented(drawn&&SUCCEEDED(hr));
 return drawn&&SUCCEEDED(hr);
}
bool saveBitmap(Environment& env,ID3D11Texture2D* image,const std::filesystem::path& path){
 if(!image)return false;D3D11_TEXTURE2D_DESC td{};image->GetDesc(&td);
 td.Usage=D3D11_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> copy;if(FAILED(env.device->CreateTexture2D(&td,nullptr,&copy)))return false;
 env.context->CopyResource(copy.Get(),image);D3D11_MAPPED_SUBRESOURCE map{};
 if(FAILED(env.context->Map(copy.Get(),0,D3D11_MAP_READ,0,&map)))return false;
 BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(td.Width);info.biHeight=-LONG(td.Height);
 info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=td.Width*td.Height*4;
 file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+info.biSizeImage;
 std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(&file),sizeof(file));stream.write(reinterpret_cast<const char*>(&info),sizeof(info));
 std::vector<unsigned char> row(td.Width*4);
 for(UINT y=0;y<td.Height;++y){const auto* source=static_cast<const unsigned char*>(map.pData)+y*map.RowPitch;
  for(UINT x=0;x<td.Width;++x){row[x*4]=source[x*4+2];row[x*4+1]=source[x*4+1];row[x*4+2]=source[x*4];row[x*4+3]=source[x*4+3];}
  stream.write(reinterpret_cast<const char*>(row.data()),row.size());}
 env.context->Unmap(copy.Get(),0);return stream.good();
}
int fixture(const std::filesystem::path& inputPath,const std::filesystem::path& outputPath,bool warp,bool viaClient){
 // Fixed Windows x64 fixture: four LE uint32 (magic, version, sizeof(Input), count),
 // followed by bounded value records. No personal paths or media are required.
 std::ifstream stream(inputPath,std::ios::binary);std::uint32_t header[4]{};stream.read(reinterpret_cast<char*>(header),sizeof(header));
 if(!stream||header[0]!=goden::frames::Magic||header[1]!=goden::frames::Version||header[2]!=sizeof(Input)||!header[3]||header[3]>4096)return 2;
 std::vector<Input> inputs(header[3]);stream.read(reinterpret_cast<char*>(inputs.data()),inputs.size()*sizeof(Input));
 if(!stream||stream.peek()!=std::char_traits<char>::eof())return 2;
 for(const auto& input:inputs)if(!validate(input))return 2;
 Environment env;if(!env.create(nullptr,warp))return 3;Renderer renderer;RemoteFrameClient client;
 std::filesystem::create_directories(outputPath);std::ofstream report(outputPath/L"frames.jsonl");
 for(std::size_t i=0;i<inputs.size();++i){Output result{};bool ok=false;
#ifdef GODEN_KERR_CLIENT_PROBE
  if(i==8){
   auto* retained=renderer.outputTexture();Selection s{};
   if(renderer.prepare(env.device.Get(),env.context.Get(),env.d2d.Get(),1,{0,0},s,nullptr,
       {inputs[i].track,inputs[i].stream},nullptr,0,nullptr,inputs[i].seconds)||renderer.failed()||renderer.outputTexture()!=retained)return 12;
  }
  if(i==20)renderer.invalidateBitmap();
#endif
  if(!viaClient){
   ok=render(env,renderer,inputs[i],result);
#ifdef GODEN_KERR_CLIENT_PROBE
   if(i==0&&renderer.responsePendingForProbe()&&(!renderer.needsFrame()||renderer.due(inputs[i].seconds+.001)||
       renderer.deadline()<inputs[i].seconds+.03))return 13;
   const auto deadline=GetTickCount64()+15000;
   while(renderer.responsePendingForProbe()&&GetTickCount64()<deadline){Sleep(1);ok=render(env,renderer,inputs[i],result);}
   if(renderer.responsePendingForProbe())return 10;
#endif
  }
  else {
   const auto& input=inputs[i];Selection s{};s.music=(input.flags&goden::frames::Music)!=0;s.albumColors=(input.flags&goden::frames::CoverColors)!=0;s.restart=input.restart;s.intensity=input.intensity;
   for(unsigned n=0;n<4;++n)s.palette[n]={input.colors[n*4],input.colors[n*4+1],input.colors[n*4+2],input.colors[n*4+3]};
   AudioFrame a{};a.valid=(input.flags&goden::frames::AudioValid)!=0;a.serial=input.track;a.recording=input.stream;a.sequence=input.audioSequence;
   a.level=input.energy;a.hit=input.onsets;a.relative=input.activity;a.rate=input.sampleRate;a.gate=input.gate;
   const auto before=client.completedSequence();const auto deadline=GetTickCount64()+15000;bool first=true;
   do {
    ok=client.prepare(env.device.Get(),env.context.Get(),env.d2d.Get(),1,{input.width,input.height},s,&a,{input.track,input.stream},input.seconds,
        (input.flags&goden::frames::Playing)!=0,(input.flags&goden::frames::Visible)!=0,(input.flags&goden::frames::Work)!=0,first);
    first=false;if(FAILED(client.result()))return 9;
    if(client.completedSequence()>before)break;Sleep(1);
   }while(GetTickCount64()<deadline);
   if(client.completedSequence()<=before)return 10;
   result=client.output();if(!(input.flags&goden::frames::Work)){ok=false;result.result=S_FALSE;}
  }
  if(!ok&&result.result!=S_FALSE)return 4;
  if(ok&&!saveBitmap(env,viaClient?client.texture():renderer.outputTexture(),outputPath/(L"frame-"+std::to_wstring(i)+L".bmp")))return 5;
  if(ok&&!viaClient&&!drawAndAcknowledge(env,renderer))return 11;
  report<<"{\"sequence\":"<<result.sequence<<",\"frames\":"<<result.frames<<",\"width\":"<<result.width<<",\"height\":"<<result.height<<",\"cpuUs\":"<<result.cpuUs<<",\"gpuP95Us\":"<<result.gpuP95Us<<"}\n";
 }
 return report.good()?0:5;
}
HANDLE handle(const wchar_t* text){
 wchar_t* end=nullptr;const auto value=std::wcstoull(text,&end,10);
 if(!text[0]||!end||*end||!value||value==std::numeric_limits<std::uintptr_t>::max())return nullptr;
 return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
}
int ipc(wchar_t** args){
 const auto mapping=handle(args[0]),mutex=handle(args[1]),request=handle(args[2]),notification=handle(args[3]),surface=handle(args[4]),parent=handle(args[5]);
 if(!mapping||!mutex||!request||!notification||!surface||!parent)return 2;
 auto* box=static_cast<goden::frames::Mailbox*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(goden::frames::Mailbox)));
 if(!box)return 2;
 struct View {void* p;~View(){UnmapViewOfFile(p);}} view{box};
 if(!goden::frames::valid(*box))return 2;
 const LUID adapter{box->adapterLow,box->adapterHigh};Environment env;if(!env.create(&adapter))return 3;
 ComPtr<ID3D11Device1> device;ComPtr<ID3D11Texture2D> target;ComPtr<IDXGIKeyedMutex> imageMutex;
 if(FAILED(env.device.As(&device))||FAILED(device->OpenSharedResource1(surface,IID_PPV_ARGS(&target)))||FAILED(target.As(&imageMutex)))return 3;
 D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);
 if(desc.Width!=goden::frames::SurfaceEdge||desc.Height!=goden::frames::SurfaceEdge||desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)return 3;
 Renderer renderer;const HANDLE waits[]{parent,request};Input input{};std::uint64_t completed=0,suspension=0;bool retry=false;
 for(;;){
  const auto wait=WaitForMultipleObjects(2,waits,FALSE,retry?16:INFINITE);
  if(wait==WAIT_OBJECT_0)return 0;if(wait!=WAIT_TIMEOUT&&wait!=WAIT_OBJECT_0+1)return 6;
  if(WaitForSingleObject(mutex,100)!=WAIT_OBJECT_0)return 6;
  input=box->input;const bool good=goden::frames::valid(*box);const auto suspended=box->suspension;const double hiddenAt=box->suspendedAt;
  ReleaseMutex(mutex);if(!good)return 2;
  if(suspended!=suspension){
   if(!std::isfinite(hiddenAt)||hiddenAt<0||hiddenAt>input.seconds)return 2;
   renderer.transport(false,false,hiddenAt);suspension=suspended;
  }
  if(input.sequence<=completed){retry=false;continue;}
  Output result{};result.sequence=input.sequence;result.generation=input.generation;
  bool locked=false;
  if(input.flags&goden::frames::Work){
   const auto acquired=imageMutex->AcquireSync(0,0);
   if(acquired==WAIT_TIMEOUT){retry=true;continue;}
   if(acquired!=S_OK)return 7;locked=true;
  }
  const bool ok=render(env,renderer,input,result);
  if(!(input.flags&goden::frames::Work)){
   // Preserve an unread image/response rather than overwriting its metadata.
   // No polling while hidden. A later visible request resumes the program.
   if(WaitForSingleObject(mutex,100)!=WAIT_OBJECT_0)return 6;
   box->acknowledged=input.sequence;ReleaseMutex(mutex);
   completed=input.sequence;retry=false;continue;
  }
  if(ok){
   const D3D11_BOX region{0,0,0,result.width,result.height,1};
   env.context->CopySubresourceRegion(target.Get(),0,0,0,0,renderer.outputTexture(),0,&region);
   // Flush/release publishes only the completed final image. No intermediate
   // texture or shader/camera state is visible to the client.
   env.context->Flush();if(FAILED(imageMutex->ReleaseSync(1)))return 7;
  }else if(locked&&FAILED(imageMutex->ReleaseSync(0)))return 7;
  if(WaitForSingleObject(mutex,100)!=WAIT_OBJECT_0)return 6;
  box->output=result;box->acknowledged=input.sequence;ReleaseMutex(mutex);SetEvent(notification);completed=input.sequence;retry=false;
 }
}
}
int wmain(int argc,wchar_t** argv){
 try{
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc==8&&std::wstring_view(argv[1])==L"--ipc")return ipc(argv+2);
  if(argc>=5&&argc<=7&&std::wstring_view(argv[1])==L"--fixture"&&std::wstring_view(argv[3])==L"--out"){
   bool warp=false,client=false;for(int i=5;i<argc;++i){const std::wstring_view option=argv[i];if(option==L"--warp")warp=true;else if(option==L"--via-client")client=true;else return 2;}
   return fixture(argv[2],argv[4],warp,client);
  }
  std::cout<<"Independent Kerr-Newman renderer\nUsage: goden-shader-render --fixture values.bin --out frames [--warp] [--via-client]\n"
      "Covered program: GPLv3; see installation licenses/KERR-RENDERER-COPYING.md\n"
      "or src/shader-render/COPYING.md and LICENSES/NPGS-NOTICE.txt in corresponding sources.\n";return argc==1?0:2;
 }catch(...){return 8;}
}
