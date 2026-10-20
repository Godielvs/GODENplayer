#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>

namespace nekorox::component {

// Per-target state. Query only on creation, monitor/settings changes, never
// per glyph or at audio cadence. Windows owns the monitor's RGB/BGR geometry.
class MonitorTextRendering final {
public:
    using Query = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT);
    explicit MonitorTextRendering(Query query = &SystemParametersInfoW) noexcept
        : query_(query) {}

    static void settingsChanged() noexcept
    { settingsRevision_.fetch_add(1, std::memory_order_release); }

    bool apply(ID2D1RenderTarget* target, IDWriteFactory* factory,
        HMONITOR monitor) noexcept
    {
        if (!target || !factory) return false;
        const auto revision = settingsRevision_.load(std::memory_order_acquire);
        if (target_.Get() == target && factory_ == factory &&
                monitor_ == monitor && revision_ == revision) return false;

        Microsoft::WRL::ComPtr<IDWriteRenderingParams> parameters;
        const bool monitorParameters = monitor && SUCCEEDED(
            factory->CreateMonitorRenderingParams(monitor,
                parameters.GetAddressOf()));
        BOOL smoothing = TRUE;
        UINT smoothingType = FE_FONTSMOOTHINGSTANDARD;
        const bool smoothingKnown = query_(SPI_GETFONTSMOOTHING, 0, &smoothing, 0) != FALSE;
        const bool typeKnown = query_(SPI_GETFONTSMOOTHINGTYPE, 0, &smoothingType, 0) != FALSE;
        auto mode = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
        if (smoothingKnown && !smoothing) mode = D2D1_TEXT_ANTIALIAS_MODE_ALIASED;
        else if (smoothingKnown && typeKnown && monitorParameters &&
                smoothingType == FE_FONTSMOOTHINGCLEARTYPE &&
                parameters->GetPixelGeometry() != DWRITE_PIXEL_GEOMETRY_FLAT)
            mode = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;

        // Null restores defaults after a failed monitor query; use grayscale
        // rather than assume that the primary monitor's subpixels apply here.
        target->SetTextRenderingParams(parameters.Get());
        target->SetTextAntialiasMode(mode);
        target_ = target;
        factory_ = factory;
        monitor_ = monitor;
        revision_ = revision;
        return true;
    }

    void reset() noexcept
    {
        target_.Reset();
        factory_ = nullptr;
        monitor_ = nullptr;
        revision_ = 0;
    }

private:
    Query query_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    IDWriteFactory* factory_ = nullptr;
    HMONITOR monitor_ = nullptr;
    std::uint64_t revision_ = 0;
    inline static std::atomic<std::uint64_t> settingsRevision_{1};
};

} // namespace nekorox::component
