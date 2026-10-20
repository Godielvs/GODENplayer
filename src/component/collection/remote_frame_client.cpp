#include "remote_frame_client.h"
#include "../visual_presentation.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace nekorox::component::collection {
namespace {
void close(HANDLE& handle)noexcept {if(handle){CloseHandle(handle);handle=nullptr;}}
struct HandleScope {HANDLE value=nullptr;~HandleScope(){close(value);}};
struct AttributeScope {LPPROC_THREAD_ATTRIBUTE_LIST value=nullptr;~AttributeScope(){if(value)DeleteProcThreadAttributeList(value);}};
const char moduleMarker=0;
std::wstring rendererPath(){
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleMarker),&module))return {};
    std::wstring path(32768,L'\0');const auto n=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if(!n||n>=path.size())return {};path.resize(n);
    const auto slash=path.find_last_of(L"\\/");if(slash==std::wstring::npos)return {};
    return path.substr(0,slash+1)+L"goden-shader-render.exe";
}
float unit(float value)noexcept{return std::isfinite(value)?std::clamp(value,0.f,1.f):0.f;}
bool lock(HANDLE mutex,HRESULT& result)noexcept{
 const auto wait=WaitForSingleObject(mutex,0);
 if(wait==WAIT_ABANDONED){ReleaseMutex(mutex);result=E_UNEXPECTED;return false;}
 if(wait==WAIT_FAILED)result=HRESULT_FROM_WIN32(GetLastError());
 return wait==WAIT_OBJECT_0;
}
}
RemoteFrameClient::~RemoteFrameClient(){
    // Job close terminates only our private child; never wait on the UI thread.
    close(job_);close(process_);
    if(mailbox_)UnmapViewOfFile(mailbox_);
    close(mapping_);close(mutex_);close(request_);close(notification_);
}
bool RemoteFrameClient::start(ID3D11Device* dev,ID3D11DeviceContext* context){
    device_=dev;context_=context;SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(goden::frames::Mailbox),nullptr);
    mutex_=CreateMutexW(&sa,FALSE,nullptr);request_=CreateEventW(&sa,FALSE,FALSE,nullptr);
    notification_=CreateEventW(&sa,FALSE,FALSE,nullptr);
    if(!mapping_||!mutex_||!request_||!notification_)return false;
    mailbox_=static_cast<goden::frames::Mailbox*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(*mailbox_)));
    if(!mailbox_)return false;*mailbox_=goden::frames::Mailbox{};
    Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC ad{};
    if(FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&ad)))return false;
    mailbox_->adapterLow=ad.AdapterLuid.LowPart;mailbox_->adapterHigh=ad.AdapterLuid.HighPart;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=goden::frames::SurfaceEdge;
    td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    Com<IDXGIResource1> resource;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&shared_))||FAILED(shared_.As(&imageMutex_))||FAILED(shared_.As(&resource)))return false;
    HandleScope surface,parent;HRESULT hr=resource->CreateSharedHandle(&sa,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&surface.value);
    if(FAILED(hr))return false;
    parent.value=OpenProcess(SYNCHRONIZE,FALSE,GetCurrentProcessId());
    if(!parent.value||!SetHandleInformation(parent.value,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT))return false;
    const auto path=rendererPath();
    if(path.empty())return false;
    const HANDLE inherited[]{mapping_,mutex_,request_,notification_,surface.value,parent.value};
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<std::byte> storage(size);auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool attributeReady=InitializeProcThreadAttributeList(attributes,1,0,&size)!=FALSE;
    if(!attributeReady)return false;AttributeScope attributeScope{attributes};
    const bool updated=UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(inherited),sizeof(inherited),nullptr,nullptr)!=FALSE;
    std::wstring command=L"\""+path+L"\" --ipc";
    for(auto handle:inherited)command+=L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;
    PROCESS_INFORMATION process{};
    job_=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    const bool jobReady=job_&&SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    const bool created=updated&&jobReady&&CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process);
    if(!created)return false;
    process_=process.hProcess;
    const bool assigned=AssignProcessToJobObject(job_,process_)!=FALSE;
    if(!assigned||ResumeThread(process.hThread)==DWORD(-1)){
        TerminateProcess(process_,1);CloseHandle(process.hThread);return false;
    }
    CloseHandle(process.hThread);return true;
}
bool RemoteFrameClient::prepare(ID3D11Device* dev,ID3D11DeviceContext* context,ID2D1DeviceContext* d2d,std::uint64_t epoch,
    D2D1_SIZE_F viewport,const Selection& selection,const AudioFrame* audio,AudioIdentity identity,double now,
    bool playing,bool visible,bool work,bool submit)noexcept{
    try{
        if(FAILED(result_))return false;
        if(!process_&&!start(dev,context)){result_=HRESULT_FROM_WIN32(GetLastError());if(SUCCEEDED(result_))result_=E_FAIL;return false;}
        if(device_.Get()!=dev||context_.Get()!=context||WaitForSingleObject(process_,0)!=WAIT_TIMEOUT){result_=E_FAIL;return false;}
        const auto flags=(playing?goden::frames::Playing:0U)|(visible?goden::frames::Visible:0U)|(work?goden::frames::Work:0U)|
            (selection.music?goden::frames::Music:0U)|(selection.albumColors?goden::frames::CoverColors:0U);
        bool colorsChanged=false;for(unsigned i=0;i<4;++i){const auto& p=selection.palette[i];colorsChanged|=pending_.colors[i*4]!=unit(p.x)||pending_.colors[i*4+1]!=unit(p.y)||pending_.colors[i*4+2]!=unit(p.z)||pending_.colors[i*4+3]!=unit(p.w);}
        const bool changed=pending_.restart!=selection.restart||pending_.track!=identity.serial||pending_.stream!=identity.recording||
            pending_.width!=viewport.width||pending_.height!=viewport.height||pending_.intensity!=selection.intensity||colorsChanged||
            ((pending_.flags^flags)&(goden::frames::Music|goden::frames::CoverColors|goden::frames::Playing|goden::frames::Visible|goden::frames::Work));
        if(changed)++generation_;
        // Copy only complete responses while holding the inherited mutex. GPU
        // ownership is separately acquired without waiting; a busy GPU retries
        // on the next presentation tick rather than stalling input or audio.
        if(lock(mutex_,result_)){
            const auto response=mailbox_->output;published_=response.sequence;acknowledged_=mailbox_->acknowledged;ReleaseMutex(mutex_);
            if(response.sequence>received_){
                if(FAILED(response.result)){result_=response.result;received_=response.sequence;return false;}
                if(response.result==S_FALSE){received_=response.sequence;retries_=0;submittedAt_=now;}
                const auto acquired=response.result==S_FALSE?HRESULT(WAIT_TIMEOUT):imageMutex_->AcquireSync(1,0);
                if(acquired==S_OK){
                    HRESULT hr=S_OK;
                    if(response.width==0||response.height==0||response.width>goden::frames::SurfaceEdge||response.height>goden::frames::SurfaceEdge)hr=E_INVALIDARG;
                    if(SUCCEEDED(hr)&&response.generation==generation_){
                        D3D11_TEXTURE2D_DESC current{};if(image_)image_->GetDesc(&current);
                        if(current.Width!=response.width||current.Height!=response.height){
                            invalidateBitmap();image_.Reset();D3D11_TEXTURE2D_DESC td{};
                            td.Width=response.width;td.Height=response.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
                            td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                            hr=dev->CreateTexture2D(&td,nullptr,&image_);
                        }
                        if(SUCCEEDED(hr)){
                            const D3D11_BOX box{0,0,0,response.width,response.height,1};
                            context->CopySubresourceRegion(image_.Get(),0,0,0,0,shared_.Get(),0,&box);output_=response;
                        }
                    }
                    const auto released=imageMutex_->ReleaseSync(0);received_=response.sequence;retries_=0;submittedAt_=now;
                    if(FAILED(hr)||FAILED(released)){result_=FAILED(hr)?hr:released;return false;}
                }else if(acquired!=WAIT_TIMEOUT){result_=FAILED(acquired)?acquired:E_FAIL;return false;}
            }
        }
        const bool timedOut=inFlight()&&now-submittedAt_>(output_.sequence?1.0:5.0);
        if(timedOut){submittedAt_=now;if(++retries_>3){result_=HRESULT_FROM_WIN32(ERROR_TIMEOUT);return false;}}
        const auto desired=fitExtent(viewport.width,viewport.height,480);
        resizing_=work&&(output_.width!=desired.width||output_.height!=desired.height);
        const bool resizePending=resizing_&&now-pending_.seconds>=1.0/30.0;
        if(submit||changed||resizePending||timedOut||submitted_==0){
            goden::frames::Input input{};input.sequence=submitted_+1;input.generation=generation_;input.seconds=now;
            input.restart=selection.restart;input.track=identity.serial;input.stream=identity.recording;
            input.width=viewport.width;input.height=viewport.height;input.intensity=std::clamp(selection.intensity,0.f,2.f);input.flags=flags;
            for(unsigned i=0;i<4;++i){const auto& p=selection.palette[i];input.colors[i*4]=unit(p.x);input.colors[i*4+1]=unit(p.y);input.colors[i*4+2]=unit(p.z);input.colors[i*4+3]=unit(p.w);}
            if(audio&&audio->valid&&audio->serial==identity.serial&&audio->recording==identity.recording){
                input.flags|=goden::frames::AudioValid;input.audioSequence=audio->sequence;input.sampleRate=audio->rate;input.gate=unit(audio->gate);
                for(unsigned i=0;i<64;++i){input.energy[i]=unit(audio->level[i]);input.onsets[i]=std::isfinite(audio->hit[i])?std::clamp(audio->hit[i],0.f,8.f):0.f;input.activity[i]=std::isfinite(audio->relative[i])?std::clamp(audio->relative[i],-1.f,1.f):0.f;}
            }
            if(lock(mutex_,result_)){
                if(suspensionPending_||((pending_.flags&goden::frames::Visible)&&!(input.flags&goden::frames::Visible))){
                    ++mailbox_->suspension;mailbox_->suspendedAt=suspensionPending_?suspendedAt_:now;suspensionPending_=false;
                }
                mailbox_->input=input;ReleaseMutex(mutex_);pending_=input;if(submitted_==acknowledged_)submittedAt_=now;submitted_=input.sequence;SetEvent(request_);
            }
        }
        if(!image_)return false;
        if(bitmapContext_.Get()!=d2d||bitmapEpoch_!=epoch)invalidateBitmap();
        if(!bitmap_){
            Com<IDXGISurface> surface;auto hr=image_.As(&surface);
            if(SUCCEEDED(hr)){const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
                hr=d2d->CreateBitmapFromDxgiSurface(surface.Get(),&properties,&bitmap_);}
            if(FAILED(hr)){result_=hr;return false;}bitmapContext_=d2d;bitmapEpoch_=epoch;
        }
        return true;
    }catch(...){result_=E_OUTOFMEMORY;return false;}
}
void RemoteFrameClient::suspend(double now)noexcept{
    if(!mailbox_||!std::isfinite(now))return;
    // Preserve the edge even if the child briefly owns the mailbox. The next
    // prepare publishes it before a coalesced resume; no hidden polling needed.
    if(!suspensionPending_){suspensionPending_=true;suspendedAt_=now;}
    if(!lock(mutex_,result_))return;
    auto input=pending_;input.seconds=now;input.flags&=~(goden::frames::Visible|goden::frames::Work);input.sequence=submitted_+1;
    ++mailbox_->suspension;mailbox_->suspendedAt=suspendedAt_;suspensionPending_=false;
    mailbox_->input=input;ReleaseMutex(mutex_);pending_=input;submitted_=input.sequence;submittedAt_=now;SetEvent(request_);
}
bool RemoteFrameClient::draw(ID2D1RenderTarget* target,D2D1_RECT_F rect,float opacity,bool flip)noexcept{
    if(!bitmap_||!target||rect.right<=rect.left||rect.bottom<=rect.top)return false;
    D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
    if(flip)target->SetTransform(D2D1::Matrix3x2F::Scale(1,-1,D2D1::Point2F(0,(rect.top+rect.bottom)*.5f))*transform);
    const auto source=visualSourceRectangle(bitmap_->GetSize(),rect);
    target->DrawBitmap(bitmap_.Get(),rect,std::clamp(opacity,0.f,1.f),D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,&source);target->SetTransform(transform);return true;
}
} // namespace
