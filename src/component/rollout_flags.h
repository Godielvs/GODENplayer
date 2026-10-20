#pragma once

#include <cstdint>

namespace nekorox::component {

enum class RolloutFlag : std::uint8_t {
    playbackPositionV1 = 0,
    uiDirtyBusV1,
    listenRetainedV3,
    listenDemandV2,
    dawAsyncLifecycleV2,
    presenceRevisionV2,
    qaTraceV2,
    masterDspEngineV1,
    masterDspPlayerV1,
    masterDspDawV1,
    masterDspUiV1,
    masterDspFullCatalogV1,
    masterDspEngineV2,
    masterDspStateV2,
    masterDspCatalogQualifiedV2,
    presenceV3,
    presenceEphemeralRelayV1,
    textRetentionV1,
    regionRetentionV1,
    scrollControllerV2,
    presentationPacingV1,
    presentationOpportunityR2,
    auditionV1,
    tagEditorV1,
    tagImportV1,
    albumAmbientA1,
    count,
};

// Live flags default on except automatic tag import. A QA run can set the corresponding
// GODEN_FLAG_* environment variable to 0/off/false before process startup.
// Values are read exactly once, keeping hot paths allocation and syscall free.
[[nodiscard]] bool rolloutFlagEnabled(RolloutFlag flag) noexcept;
[[nodiscard]] bool qaAlbumAmbientWorkerOnly() noexcept;
[[nodiscard]] bool qaAlbumAmbientForceDegraded() noexcept;
[[nodiscard]] bool qaAlbumAmbientEffectFailure() noexcept;
[[nodiscard]] bool qaArtworkBusyOnce() noexcept;

} // namespace nekorox::component
