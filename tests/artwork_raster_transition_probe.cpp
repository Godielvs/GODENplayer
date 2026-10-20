#include "artwork_transition_renderer.h"
#include "interaction_latency_guard.h"
#include "visual_presentation.h"
#include <wincodec.h>
#include <iostream>
#include <cmath>
#include <stdexcept>

using namespace nekorox::component;
using Microsoft::WRL::ComPtr;
static void hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D2D/WIC setup failed"); }
static std::shared_ptr<ArtPixels> art(unsigned edge, std::uint64_t hash) {
    auto p = std::make_shared<ArtPixels>(); p->width=p->height=edge; p->contentHash=hash;
    p->bgra.resize(std::size_t(edge)*edge*4);
    for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x){
        const auto i=(std::size_t(y)*edge+x)*4;
        p->bgra[i]=std::uint8_t(30+200*x/edge);p->bgra[i+1]=std::uint8_t(30+180*y/edge);
        p->bgra[i+2]=std::uint8_t(hash*30);p->bgra[i+3]=255;
    }return p;
}
int main() {
    try {
        hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        ComPtr<IWICImagingFactory> wic; hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,
            CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)));
        ComPtr<IWICBitmap> bitmap;hr(wic->CreateBitmap(384,384,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap));
        ComPtr<ID2D1Factory> factory;hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));
        ComPtr<ID2D1RenderTarget> target;hr(factory->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(),&target));
        unsigned failed=0;
        auto check=[&](bool ok,const char* text){std::cout<<(ok?"PASS ":"FAIL ")<<text<<"\n";failed+=!ok;};
        for (const auto dest : {D2D1::RectF(0,0,1919,620),D2D1::RectF(0,0,1600,400),D2D1::RectF(0,0,800,800)}) {
            const auto src=visualSourceRectangle(D2D1::SizeF(768,192),dest);
            const auto sx=(dest.right-dest.left)/(src.right-src.left);
            const auto sy=(dest.bottom-dest.top)/(src.bottom-src.top);
            check(std::abs(sx-sy)<.00001f,"aligned shader pixels retain their aspect in wide/fullscreen/square viewports");
        }
        // Exercise the actual Direct2D blit too. A circle in the aligned
        // 768x192 texture must remain circular on a wide physical canvas.
        std::vector<unsigned char> circle(768*192*4);
        for(unsigned y=0;y<192;++y)for(unsigned x=0;x<768;++x){
            const auto dx=int(x)-384,dy=int(y)-96;
            const auto i=(std::size_t(y)*768+x)*4;
            circle[i]=circle[i+1]=circle[i+2]=(dx*dx+dy*dy<70*70)?255:0;
            circle[i+3]=255;
        }
        ComPtr<ID2D1Bitmap> aligned;hr(target->CreateBitmap(D2D1::SizeU(768,192),circle.data(),768*4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&aligned));
        const auto viewport=D2D1::RectF(0,0,384,124);
        const auto source=visualSourceRectangle(aligned->GetSize(),viewport);
        target->BeginDraw();target->Clear(D2D1::ColorF(0,0,0));
        target->DrawBitmap(aligned.Get(),viewport,1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,&source);
        hr(target->EndDraw());
        ComPtr<IWICBitmapLock> lock;hr(bitmap->Lock(nullptr,WICBitmapLockRead,&lock));
        UINT stride=0,bytes=0;BYTE* data=nullptr;hr(lock->GetStride(&stride));hr(lock->GetDataPointer(&bytes,&data));
        unsigned minX=384,minY=384,maxX=0,maxY=0;
        for(unsigned y=0;y<124;++y)for(unsigned x=0;x<384;++x)if(data[y*stride+x*4]>128){
            minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);
        }
        check(std::abs(int(maxX-minX)-int(maxY-minY))<=2,"native Direct2D presentation keeps a circular shader feature circular");
        lock.Reset();
        auto draw=[&](ArtworkTransitionRenderer& renderer,MotionMode mode,bool accepted=true){
            target->BeginDraw();target->Clear(D2D1::ColorF(0,0,0));
            check(renderer.draw(target.Get(),D2D1::RectF(0,0,384,384),mode),"production artwork draw");
            const auto ticket=renderer.pendingPresentation();
            hr(target->EndDraw());
            renderer.presented(ticket,accepted);
        };
        ArtworkTransitionRenderer delayed;
        delayed.update(target.Get(),96,L"delayed",art(512,1));
        Sleep(700);draw(delayed,MotionMode::normal,false);
        check(delayed.active(MotionMode::normal)&&delayed.diagnostics().reconstructionFrames>0,
            "first visible frame starts reconstruction after a delayed presentation");
        Sleep(700);draw(delayed,MotionMode::normal);
        check(delayed.active(MotionMode::normal),"rejected first presentation did not consume animation time");
        Sleep(700);draw(delayed,MotionMode::normal,false);
        check(delayed.active(MotionMode::normal),"rejected final raster keeps reconstruction pending");
        draw(delayed,MotionMode::normal);
        check(!delayed.active(MotionMode::normal),"accepted final raster settles reconstruction");
        ArtworkTransitionRenderer fitted;
        fitted.update(target.Get(),96,L"old",art(128,2));draw(fitted,MotionMode::off);
        fitted.update(target.Get(),96,L"next",art(512,3));draw(fitted,MotionMode::normal);
        Sleep(150);draw(fitted,MotionMode::normal);
        const auto before=fitted.diagnostics();
        fitted.update(target.Get(),96,L"next",art(160,3));
        check(fitted.active(MotionMode::normal),"fitted raster does not settle an active reconstruction");
        Sleep(100);draw(fitted,MotionMode::normal);
        check(fitted.diagnostics().generation==before.generation&&
            fitted.diagnostics().reconstructionFrames>before.reconstructionFrames,
            "raster replacement continues the same reconstruction generation");
        Sleep(450);draw(fitted,MotionMode::normal);
        check(!fitted.active(MotionMode::normal),"transition resolves once without a restart");
        const auto settled=fitted.diagnostics().generation;
        fitted.update(target.Get(),96,L"same album different track",art(160,3));
        check(!fitted.active(MotionMode::normal)&&fitted.diagnostics().generation==settled,
            "same cover ownership change remains settled");
        fitted.update(target.Get(),96,L"same album different track",art(192,3));
        check(!fitted.active(MotionMode::normal),"resize of a settled cover does not manufacture a transition");
        ArtworkTransitionRenderer reduced;reduced.update(target.Get(),96,L"reduced",art(128,4));
        draw(reduced,MotionMode::reduced);Sleep(140);draw(reduced,MotionMode::reduced);
        check(!reduced.active(MotionMode::reduced),"reduced motion retains its 120 ms duration");
        ArtworkTransitionRenderer input;
        input.update(target.Get(),96,L"input",art(512,5));
        InteractionLatencyGuard::instance().noteScroll();
        draw(input,MotionMode::normal);Sleep(50);draw(input,MotionMode::normal);
        check(input.diagnostics().fadeFallbackFrames==0 && input.diagnostics().reconstructionFrames==2,
            "recent real input does not replace reconstruction with a permanent fade");
        std::cout<<"failed="<<failed<<"\n";return failed?1:0;
    }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}
}
