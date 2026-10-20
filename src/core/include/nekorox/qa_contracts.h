#pragma once

#include <cstdint>
#include <cstddef>

namespace nekorox {

inline constexpr wchar_t kListenCaptureMessageV1[] =
    L"NekoRoX.ListenCapture.V1";
inline constexpr wchar_t kListenCaptureRelativePathV1[] =
    L"nekorox\\qa\\listen-latest.bmp";
inline constexpr wchar_t kListenCrtStatsMessageV1[] =
    L"NekoRoX.ListenCrtStats.V1";
inline constexpr wchar_t kListenCrtStatsRelativePathV1[] =
    L"nekorox\\qa\\listen-crt-stats.json";
inline constexpr wchar_t kListenAnalysisStateMessageV1[] =
    L"NekoRoX.ListenAnalysisState.V1";
inline constexpr wchar_t kListenArtworkStateMessageV1[] =
    L"NekoRoX.ListenArtworkState.V1";
inline constexpr wchar_t kCoverStateMessageV1[] =
    L"NekoRoX.CoverState.V1";
inline constexpr wchar_t kTransportStateMessageV1[] =
    L"NekoRoX.TransportState.V1";
inline constexpr wchar_t kLibraryCountMessageV1[] =
    L"NekoRoX.LibraryCount.V1";
inline constexpr wchar_t kFileOpenStateMessageV1[] =
    L"NekoRoX.FileOpenState.V1";
inline constexpr wchar_t kWorkspaceStateMessageV1[] =
    L"NekoRoX.WorkspaceState.V1";
inline constexpr wchar_t kWorkspaceRenderStateMessageV1[] =
    L"NekoRoX.WorkspaceRenderState.V1";
inline constexpr wchar_t kFrameClockStateMessageV1[] =
    L"NekoRoX.FrameClockState.V1";
inline constexpr wchar_t kWaveSeekStateMessageV1[] =
    L"NekoRoX.WaveSeekState.V1";
inline constexpr wchar_t kPerformanceEventMappingPrefixV2[] =
    L"Local\\Goden.PerformanceEventRingV2.";
inline constexpr std::uint32_t kPerformanceEventMagicV2 = 0x32524550U;
inline constexpr std::size_t kPerformanceEventCapacityV2 = 8192U;

enum class PerformanceEventKindV2 : std::uint16_t {
    heartbeat = 0,
    input,
    callback,
    post,
    wake,
    consume,
    paint,
    endDraw,
    present,
    seek,
    pressure,
    failure,
    duration,
    inputIncorporated,
    frameAttempt,
    frameResult,
    spanStart,
    spanEnd,
    cacheReason,
    collectionMaterialization,
    readinessNotReady,
    waitArmed,
    readySignaled,
    acceptedVisible,
    displayed,
};

struct alignas(64) PerformanceEventV2 {
    std::uint64_t sequence = 0;
    std::uint64_t qpc = 0;
    std::uint32_t threadId = 0;
    PerformanceEventKindV2 kind = PerformanceEventKindV2::heartbeat;
    std::uint16_t surface = 0;
    std::uint64_t argument0 = 0;
    std::uint64_t argument1 = 0;
    std::uint64_t argument2 = 0;
    std::uint64_t argument3 = 0;
    std::uint64_t durationQpc = 0;
};
static_assert(sizeof(PerformanceEventV2) == 64U);

struct alignas(64) PerformanceEventRingHeaderV2 {
    std::uint32_t magic = kPerformanceEventMagicV2;
    std::uint32_t version = 2;
    std::uint32_t recordBytes = sizeof(PerformanceEventV2);
    std::uint32_t capacity = kPerformanceEventCapacityV2;
    std::uint32_t ownerPid = 0;
    std::uint32_t reserved = 0;
    std::uint64_t qpcFrequency = 0;
    std::uint64_t latestSequence = 0;
    std::uint64_t heartbeatQpc = 0;
    std::uint64_t startupEpoch = 0;
    std::uint64_t reserved2 = 0;
};
static_assert(sizeof(PerformanceEventRingHeaderV2) == 64U);
inline constexpr std::uintptr_t kFileDropCopyDataTagV1 =
    static_cast<std::uintptr_t>(0x4e525844U); // 'NRXD'
inline constexpr std::uintptr_t kTagSetTitleCopyDataTagV1 =
    static_cast<std::uintptr_t>(0x4e525854U); // 'NRXT'
inline constexpr std::uintptr_t kTagSetDestinationCopyDataTagV1 =
    static_cast<std::uintptr_t>(0x4e525843U); // 'NRXC'
inline constexpr std::uintptr_t kTagSetFrontArtworkCopyDataTagV1 =
    static_cast<std::uintptr_t>(0x4e525841U); // 'NRXA'

inline constexpr std::uint32_t kListenAnalysisVisibleBitV1 = 1U << 0U;
inline constexpr std::uint32_t kListenAnalysisPendingBitV1 = 1U << 1U;
inline constexpr std::uint32_t kListenAnalysisStatusShiftV1 = 8U;
inline constexpr std::uint32_t kListenAnalysisStatusMaskV1 = 0xffU;

inline constexpr std::uint32_t kListenArtworkTrackValidBitV1 = 1U << 0U;
inline constexpr std::uint32_t kListenArtworkTrackInfoBitV1 = 1U << 1U;
inline constexpr std::uint32_t kListenArtworkPixelsReadyBitV1 = 1U << 2U;
inline constexpr std::uint32_t kListenArtworkRenderReadyBitV1 = 1U << 3U;
inline constexpr std::uint32_t kListenArtworkTransitionActiveBitV1 = 1U << 4U;
inline constexpr std::uint32_t kListenArtworkCacheLoadingBitV1 = 1U << 5U;
inline constexpr std::uint32_t kListenArtworkCacheMissingBitV1 = 1U << 6U;
inline constexpr std::uint32_t kListenArtworkCacheReadyBitV1 = 1U << 7U;
inline constexpr std::uint32_t kListenArtworkMotionModeShiftV1 = 16U;
inline constexpr std::uint32_t kListenArtworkMotionModeMaskV1 = 0x3U;

inline constexpr std::uint32_t kCoverTrackValidBitV1 = 1U << 0U;
inline constexpr std::uint32_t kCoverPixelsReadyBitV1 = 1U << 1U;
inline constexpr std::uint32_t kCoverBitmapReadyBitV1 = 1U << 2U;
inline constexpr std::uint32_t kCoverBitmapFailedBitV1 = 1U << 3U;
inline constexpr std::uint32_t kCoverCacheLoadingBitV1 = 1U << 4U;
inline constexpr std::uint32_t kCoverCacheMissingBitV1 = 1U << 5U;
inline constexpr std::uint32_t kCoverCacheReadyBitV1 = 1U << 6U;
inline constexpr std::uint32_t kCoverKeyValidBitV1 = 1U << 7U;
inline constexpr std::uint32_t kCoverPaintObservedBitV1 = 1U << 8U;
inline constexpr std::uint32_t kCoverRenderReadyBitV1 = 1U << 9U;
inline constexpr std::uint32_t kCoverTransitionActiveBitV1 = 1U << 10U;
inline constexpr std::uint32_t kCoverReconstructionUsedBitV1 = 1U << 11U;
inline constexpr std::uint32_t kCoverFadeFallbackUsedBitV1 = 1U << 12U;
inline constexpr std::uint32_t kCoverReconstructionPreparedBitV1 = 1U << 13U;

inline constexpr std::uint32_t kTransportPlayingBitV1 = 1U << 0U;
inline constexpr std::uint32_t kTransportMutedBitV1 = 1U << 1U;
inline constexpr std::uint32_t kTransportRepeatBitV1 = 1U << 2U;
inline constexpr std::uint32_t kTransportShuffleBitV1 = 1U << 3U;
inline constexpr std::uint32_t kTransportCostMapReadyBitV1 = 1U << 4U;
inline constexpr std::uint32_t kTransportVolumeShiftV1 = 8U;
inline constexpr std::uint32_t kTransportVolumeMaskV1 = 0x3ffU;
inline constexpr std::uint32_t kTransportOrderShiftV1 = 20U;
inline constexpr std::uint32_t kTransportOrderMaskV1 = 0xffU;

inline constexpr std::uint32_t kFileOpenDropObservedBitV1 = 1U << 0U;
inline constexpr std::uint32_t kFileOpenDropExtractedBitV1 = 1U << 1U;
inline constexpr std::uint32_t kFileOpenRequestStartedBitV1 = 1U << 2U;
inline constexpr std::uint32_t kFileOpenCallbackBitV1 = 1U << 3U;
inline constexpr std::uint32_t kFileOpenItemsResolvedBitV1 = 1U << 4U;
inline constexpr std::uint32_t kFileOpenPlaybackStartedBitV1 = 1U << 5U;
inline constexpr std::uint32_t kFileOpenTemporaryAlbumActiveBitV1 = 1U << 6U;
inline constexpr std::uint32_t kFileOpenTemporaryAlbumDismissedBitV1 = 1U << 7U;
inline constexpr std::uint32_t kFileOpenTemporaryAlbumCountShiftV1 = 16U;
inline constexpr std::uint32_t kFileOpenTemporaryAlbumCountMaskV1 = 0xffffU;

inline constexpr std::uint32_t kWorkspaceTabMaskV1 = 0x7U;
inline constexpr std::uint32_t kWorkspaceHistoryBitV1 = 1U << 3U;

inline constexpr std::uint32_t kWaveSeekGeometryReadyBitV1 = 1U << 0U;
inline constexpr std::uint32_t kWaveSeekCompleteBitV1 = 1U << 1U;
inline constexpr std::uint32_t kWaveSeekReconstructingBitV1 = 1U << 2U;

} // namespace nekorox
