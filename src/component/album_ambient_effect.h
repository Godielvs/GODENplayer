#pragma once

#include "album_ambient_controller.h"

#include "nekorox/art_palette.h"
#include "nekorox/flow_constants.h"

#include <array>
#include <cstdint>
#include <d2d1_1.h>
#include <wrl/client.h>

namespace nekorox::component {

// The HLSL layout is deliberately fixed and independently asserted. Direct2D
// receives one small immutable upload for each accepted ambient texture frame.
using AlbumAmbientConstants = FlowGpuConstants;

class AlbumAmbientEffect final {
public:
    static constexpr GUID effectId{
        0x8f0ccac1, 0x5512, 0x4056,
        {0xa4, 0xe7, 0x6a, 0x21, 0xc1, 0x65, 0x3a, 0x31}};

    // Registers once for the factory behind this device context, creates the
    // zero-input procedural effect. Failure is isolated to the caller's epoch.
    static HRESULT create(ID2D1DeviceContext* context,
        ID2D1Effect** effect) noexcept;

    static AlbumAmbientConstants constants(const D2D1_SIZE_F& viewportDip,
        const D2D1_SIZE_U& texturePixels,
        const D2D1_RECT_F& lyricsRectNormalized,
        const D2D1_RECT_F& tracksRectNormalized,
        const ArtPalette& palette,
        const AlbumAmbientPose& pose,
        bool reactiveMode,
        std::uint64_t artworkHash) noexcept;

    static HRESULT update(ID2D1Effect* effect,
        const AlbumAmbientConstants& constants) noexcept;
};

} // namespace nekorox::component
