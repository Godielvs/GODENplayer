#pragma once
// Windows adapter. Reuses the CALLER'S Direct2D factory, target and brushes.
// No device, swapchain, window, timer, PNG/SVG decoder, or runtime file I/O.
// Prepare on attach/layout/DPI/theme-style changes, then drawPrepared in paint.
#include "goden_pixel_assets.generated.hpp"
#include "pixel_placement.hpp"
#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <utility>

namespace goden::pixel {
class PixelIconPainter final {
public:
    static constexpr std::size_t capacity = 512;
    struct Stats { std::uint64_t prepares=0, cacheHits=0, draws=0, misses=0; };
    struct Handle { std::size_t slot=capacity; std::uint64_t generation=0; };
    void bindFactory(ID2D1Factory* factory) noexcept {
        if (factory_.Get()==factory) return;
        clear(); factory_=factory;
    }
    void clear() noexcept {
        for (auto& e:entries_) e=Entry{};
        nextSlot_=0;
    }
    // Device-independent geometries survive a target/device reset while the
    // factory survives. Rebind only if that factory changes.
    HRESULT prepare(const GlyphView& glyph, std::int32_t widthPx,
                    std::int32_t heightPx, Handle& prepared) noexcept {
        prepared=Handle{};
        if (!factory_ || !glyph.rectangles || glyph.count==0 ||
            widthPx<glyph.width || heightPx<glyph.height ||
            widthPx>4096 || heightPx>4096) return E_INVALIDARG;
        if (const auto* existing=find(glyph,widthPx,heightPx)) {
            prepared={static_cast<std::size_t>(existing-entries_.data()),existing->generation};
            ++stats_.cacheHits; return S_OK;
        }
        Entry candidate; candidate.glyph=glyph; candidate.source=glyph.rectangles;
        candidate.generation=++generation_;
        candidate.width=widthPx; candidate.height=heightPx;
        for (unsigned role=1;role<=2;++role) {
            bool any=false;
            for (std::uint16_t i=0;i<glyph.count;++i)
                any=any || glyph.rectangles[i].role==role;
            if (!any) continue;
            auto& geometry=candidate.layers[role-1];
            auto hr=factory_->CreatePathGeometry(geometry.ReleaseAndGetAddressOf());
            if (FAILED(hr)) return hr;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            hr=geometry->Open(sink.ReleaseAndGetAddressOf());
            if (FAILED(hr)) return hr;
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);
            for (std::uint16_t i=0;i<glyph.count;++i) {
                const auto& r=glyph.rectangles[i]; if (r.role!=role) continue;
                const float x0=static_cast<float>(edge(r.x,glyph.width,widthPx));
                const float x1=static_cast<float>(edge(r.x+r.width,glyph.width,widthPx));
                const float y0=static_cast<float>(edge(r.y,glyph.height,heightPx));
                const float y1=static_cast<float>(edge(r.y+r.height,glyph.height,heightPx));
                if (x1<=x0 || y1<=y0) continue;
                sink->BeginFigure(D2D1::Point2F(x0,y0),D2D1_FIGURE_BEGIN_FILLED);
                const D2D1_POINT_2F points[]{D2D1::Point2F(x1,y0),
                    D2D1::Point2F(x1,y1),D2D1::Point2F(x0,y1)};
                sink->AddLines(points,3);sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
            hr=sink->Close();if (FAILED(hr)) return hr;
        }
        entries_[nextSlot_]=std::move(candidate);
        prepared={nextSlot_,entries_[nextSlot_].generation};
        nextSlot_=(nextSlot_+1)%capacity; ++stats_.prepares; return S_OK;
    }
    // A missing preparation is an integration error, not permission to run
    // SVG parsing, build geometry or a second legacy renderer in hot paint.
    HRESULT drawPrepared(ID2D1RenderTarget* target,Handle prepared,
        BoxDip localSlot, ID2D1Brush* primary,ID2D1Brush* secondary=nullptr) noexcept {
        if (!target || !primary) return E_INVALIDARG;
        if (prepared.slot>=capacity || entries_[prepared.slot].generation!=prepared.generation ||
            prepared.generation==0) {++stats_.misses;return E_PENDING;}
        const auto* found=&entries_[prepared.slot];
        const auto& glyph=found->glyph;
        D2D1_MATRIX_3X2_F oldTransform{};target->GetTransform(&oldTransform);
        FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
        const auto p=place(glyph.width,glyph.height,localSlot,dpiX,dpiY,
            {oldTransform._11,oldTransform._12,oldTransform._21,
             oldTransform._22,oldTransform._31,oldTransform._32});
        if (!p.valid) return E_INVALIDARG;
        if (found->width!=p.width || found->height!=p.height) {++stats_.misses;return E_PENDING;}
        const auto oldAa=target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        // The paths are in physical-pixel coordinates. This matrix converts
        // them to target DIP units exactly once. Existing clipping is retained.
        target->SetTransform(D2D1::Matrix3x2F(96.0f/dpiX,0,0,96.0f/dpiY,
            p.x*96.0f/dpiX,p.y*96.0f/dpiY));
        if (found->layers[0]) target->FillGeometry(found->layers[0].Get(),primary);
        if (found->layers[1]) target->FillGeometry(found->layers[1].Get(),
            secondary ? secondary : primary);
        target->SetTransform(oldTransform);
        target->SetAntialiasMode(oldAa);
        ++stats_.draws;return S_OK;
    }
    [[nodiscard]] const Stats& stats() const noexcept {return stats_;}
private:
    struct Entry {
        const PixelRect* source=nullptr;
        GlyphView glyph{};
        std::uint64_t generation=0;
        std::int32_t width=0,height=0;
        std::array<Microsoft::WRL::ComPtr<ID2D1PathGeometry>,2> layers{};
    };
    [[nodiscard]] const Entry* find(const GlyphView& g,std::int32_t w,
                                   std::int32_t h) const noexcept {
        for (const auto& e:entries_)
            if(e.source==g.rectangles && e.width==w && e.height==h) return &e;
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    std::array<Entry,capacity> entries_{};
    std::size_t nextSlot_=0;
    std::uint64_t generation_=0;
    Stats stats_{};
};
} // namespace goden::pixel
