#pragma once

#include <d2d1_3.h>
#include <atlbase.h>
#include <array>

// Rectangle submission only. The analyzer, color calculation, compositing
// order, physical gaps and presentation clock stay with their existing owners.
class neko_bar_batch_t final
{
public:
    bool begin(ID2D1DeviceContext* owner, bool enabled) noexcept
    {
        active_ = false;
        count_ = 0;
        if (!enabled) return false;
        if (owner_ != owner) {
            reset();
            owner_ = owner;
        }
        if (!owner_ || unsupported_) return false;
        if (retryAfter_ && GetTickCount64() < retryAfter_) return false;
        if (!context_) {
            const auto hr = owner_->QueryInterface(IID_PPV_ARGS(&context_));
            if (hr == E_NOINTERFACE) { unsupported_ = true; return false; }
            if (FAILED(hr)) return retryLater();
        }
        if (!batch_ && FAILED(context_->CreateSpriteBatch(&batch_)))
            return retryLater();
        if (!white_) {
            const UINT32 white = 0xffffffff;
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(context_->CreateBitmap(D2D1::SizeU(1, 1), &white,
                    sizeof(white), &props, &white_))) return retryLater();
        }
        if (!fallback_ && FAILED(context_->CreateSolidColorBrush(
                D2D1::ColorF(0), &fallback_))) return retryLater();
        retryAfter_ = 0;
        active_ = context_ && batch_ && white_ && fallback_;
        return active_;
    }

    bool add(const D2D1_RECT_F& rect, ID2D1SolidColorBrush* brush) noexcept
    {
        if (!active_ || !brush) return false;
        if (count_ == Capacity) flush();
        rectangles_[count_] = rect;
        // Sprite tint accepts straight color, just like a solid brush. D2D
        // applies alpha during compositing; do not premultiply it twice.
        auto color = brush->GetColor();
        color.a *= brush->GetOpacity();
        colors_[count_++] = color;
        return true;
    }

    void flush() noexcept
    {
        if (!count_) return;
        HRESULT hr = S_OK;
        const auto stored = batch_->GetSpriteCount();
        if (count_ > stored) {
            hr = batch_->AddSprites(count_ - stored,
                rectangles_.data() + stored, nullptr,
                colors_.data() + stored, nullptr);
        }
        if (SUCCEEDED(hr)) hr = batch_->SetSprites(0, count_,
            rectangles_.data(), nullptr, colors_.data(), nullptr);
        if (SUCCEEDED(hr)) {
            context_->DrawSpriteBatch(batch_, 0, count_, white_,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                D2D1_SPRITE_OPTIONS_CLAMP_TO_SOURCE_RECTANGLE);
        } else {
            // Preserve the frame if an optional batch cannot be uploaded.
            for (UINT32 i = 0; i < count_; ++i) {
                fallback_->SetColor(colors_[i]);
                context_->FillRectangle(rectangles_[i], fallback_);
            }
        }
        count_ = 0;
    }

    void end() noexcept { flush(); active_ = false; }
    void reset() noexcept
    {
        active_ = false;
        count_ = 0;
        fallback_.Release(); white_.Release(); batch_.Release();
        context_.Release(); owner_.Release();
        unsupported_ = false;
        retryAfter_ = 0;
    }

private:
    bool retryLater() noexcept
    {
        // Transient allocation failure uses the ordinary renderer immediately;
        // retry is bounded, and the normal path never reads a retry clock.
        retryAfter_ = GetTickCount64() + 5000;
        return false;
    }
    static constexpr UINT32 Capacity = 2048;
    std::array<D2D1_RECT_F, Capacity> rectangles_{};
    std::array<D2D1_COLOR_F, Capacity> colors_{};
    CComPtr<ID2D1DeviceContext> owner_;
    CComPtr<ID2D1DeviceContext3> context_;
    CComPtr<ID2D1SpriteBatch> batch_;
    CComPtr<ID2D1Bitmap1> white_;
    CComPtr<ID2D1SolidColorBrush> fallback_;
    UINT32 count_ = 0;
    bool active_ = false;
    bool unsupported_ = false;
    ULONGLONG retryAfter_ = 0;
};
