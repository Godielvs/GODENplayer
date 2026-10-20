#pragma once
#include "collection_graph.h"
#include "collection_audio_history.h"
#include "../../shader-render/frame_protocol.h"
#include <windows.h>
#include <d3d11_1.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <memory>

namespace nekorox::component::collection {
class RemoteFrameClient final {
    template<class T>using Com=Microsoft::WRL::ComPtr<T>;
    HANDLE mapping_=nullptr,mutex_=nullptr,request_=nullptr,notification_=nullptr;
    HANDLE process_=nullptr,job_=nullptr;
    goden::frames::Mailbox* mailbox_=nullptr;
    Com<ID3D11Device> device_;Com<ID3D11DeviceContext> context_;
    Com<ID3D11Texture2D> shared_,image_;
    Com<IDXGIKeyedMutex> imageMutex_;
    Com<ID2D1Bitmap1> bitmap_;Com<ID2D1DeviceContext> bitmapContext_;
    std::uint64_t bitmapEpoch_=0,submitted_=0,received_=0,published_=0,acknowledged_=0,generation_=0;
    double submittedAt_=0;unsigned retries_=0;HRESULT result_=S_OK;
    bool suspensionPending_=false;double suspendedAt_=0;
    bool resizing_=false;
    goden::frames::Input pending_{};goden::frames::Output output_{};
    bool start(ID3D11Device*,ID3D11DeviceContext*);
public:
    ~RemoteFrameClient();
    bool prepare(ID3D11Device*,ID3D11DeviceContext*,ID2D1DeviceContext*,std::uint64_t,
        D2D1_SIZE_F,const Selection&,const AudioFrame*,AudioIdentity,double,
        bool playing,bool visible,bool work,bool submit)noexcept;
    bool draw(ID2D1RenderTarget*,D2D1_RECT_F,float,bool flip)noexcept;
    bool responsePending()const noexcept{return submitted_>acknowledged_||published_>received_;}
    bool inFlight()const noexcept{return responsePending()||resizing_;}
    bool needsFrame()const noexcept{return (pending_.flags&goden::frames::Work)&&inFlight();}
    std::uint64_t completedSequence()const noexcept{return acknowledged_;}
    HRESULT result()const noexcept{return result_;}
    const goden::frames::Output& output()const noexcept{return output_;}
    ID3D11Texture2D* texture()const noexcept{return image_.Get();}
    void invalidateBitmap()noexcept {bitmap_.Reset();bitmapContext_.Reset();bitmapEpoch_=0;}
    void suspend(double now)noexcept;
};
} // namespace
