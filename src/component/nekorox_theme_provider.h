#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <ole2.h>

#include <foobar2000/SDK/foobar2000.h>

#include <cstdint>
#include <type_traits>

namespace nekorox::interop {

inline constexpr std::uint32_t kSharedThemeSnapshotVersionV2 = 2;
inline constexpr std::uint32_t kSharedThemeFlagArtworkDerived = 1U << 0U;

#pragma pack(push, 1)
struct SharedRgbaColorV1 {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct SharedThemeSnapshotV2 {
    std::uint32_t version = kSharedThemeSnapshotVersionV2;
    std::uint32_t sizeBytes = sizeof(SharedThemeSnapshotV2);
    std::uint64_t generation = 0;
    std::uint64_t artworkIdentityHash = 0;
    std::uint64_t artworkGeneration = 0;
    SharedRgbaColorV1 base{0.039f, 0.043f, 0.043f, 1.0f};
    SharedRgbaColorV1 panel{0.071f, 0.078f, 0.078f, 1.0f};
    SharedRgbaColorV1 text{0.824f, 0.847f, 0.847f, 1.0f};
    SharedRgbaColorV1 accent{0.0f, 0.847f, 0.816f, 1.0f};
    SharedRgbaColorV1 secondary{0.66f, 0.91f, 0.90f, 1.0f};
    SharedRgbaColorV1 highlight{0.84f, 0.94f, 0.95f, 1.0f};
    float minimumContrast = 4.5f;
    float transitionProgress = 1.0f;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

[[nodiscard]] inline constexpr bool validSharedThemeSnapshot(
    const SharedThemeSnapshotV2& snapshot) noexcept
{
    return snapshot.version == kSharedThemeSnapshotVersionV2 &&
        snapshot.sizeBytes == sizeof(SharedThemeSnapshotV2) &&
        snapshot.minimumContrast >= 1.0f &&
        snapshot.transitionProgress >= 0.0f &&
        snapshot.transitionProgress <= 1.0f;
}

class NOVTABLE ThemeProviderV1 : public service_base {
public:
    virtual bool getSnapshot(SharedThemeSnapshotV2& output) noexcept = 0;

    FB2K_MAKE_SERVICE_INTERFACE_ENTRYPOINT(ThemeProviderV1);
};

inline const GUID ThemeProviderV1::class_guid = {
    0x87dd4a74,
    0x4115,
    0x4ffa,
    {0xb6, 0x5a, 0xd3, 0xd0, 0x74, 0xe3, 0xbf, 0x9d},
};

static_assert(sizeof(SharedRgbaColorV1) == 16);
static_assert(sizeof(SharedThemeSnapshotV2) == 144);
static_assert(std::is_trivially_copyable_v<SharedThemeSnapshotV2>);

} // namespace nekorox::interop
