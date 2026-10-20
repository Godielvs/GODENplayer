#include "qa_hooks.h"
#include "rollout_flags.h"

#include <array>
#include <cwchar>
#include <windows.h>

namespace nekorox::component {
namespace {

constexpr std::array<const wchar_t*,
    static_cast<std::size_t>(RolloutFlag::count)> kEnvironmentNames{
    L"GODEN_FLAG_PLAYBACK_POSITION_V1",
    L"GODEN_FLAG_UI_DIRTY_BUS_V1",
    L"GODEN_FLAG_LISTEN_RETAINED_V3",
    L"GODEN_FLAG_LISTEN_DEMAND_V2",
    L"GODEN_FLAG_DAW_ASYNC_LIFECYCLE_V2",
    L"GODEN_FLAG_PRESENCE_REVISION_V2",
    L"GODEN_FLAG_QA_TRACE_V2",
    L"GODEN_FLAG_MASTER_DSP_ENGINE_V1",
    L"GODEN_FLAG_MASTER_DSP_PLAYER_V1",
    L"GODEN_FLAG_MASTER_DSP_DAW_V1",
    L"GODEN_FLAG_MASTER_DSP_UI_V1",
    L"GODEN_FLAG_MASTER_DSP_FULL_CATALOG_V1",
    L"GODEN_FLAG_MASTER_DSP_ENGINE_V2",
    L"GODEN_FLAG_MASTER_DSP_STATE_V2",
    L"GODEN_FLAG_MASTER_DSP_CATALOG_QUALIFIED_V2",
    L"GODEN_FLAG_PRESENCE_V3",
    L"GODEN_FLAG_PRESENCE_EPHEMERAL_RELAY_V1",
    L"GODEN_FLAG_TEXT_RETENTION_V1",
    L"GODEN_FLAG_REGION_RETENTION_V1",
    L"GODEN_FLAG_SCROLL_CONTROLLER_V2",
    L"GODEN_FLAG_PRESENTATION_PACING_V1",
    L"GODEN_FLAG_PRESENTATION_OPPORTUNITY_R2",
    L"GODEN_FLAG_AUDITION_V1",
    L"GODEN_FLAG_TAG_EDITOR_V1",
    L"GODEN_FLAG_TAG_IMPORT_V1",
    L"GODEN_FLAG_ALBUM_AMBIENT_A1",
};

bool readEnabled(const wchar_t* name, bool defaultValue = true) noexcept
{
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(
        name, value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) return defaultValue;
    return _wcsicmp(value, L"0") != 0 &&
        _wcsicmp(value, L"off") != 0 &&
        _wcsicmp(value, L"false") != 0;
}

bool qaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

bool qaEnabled(const wchar_t* name) noexcept
{
    return qaHooksAllowed() && readEnabled(name, false);
}

} // namespace

bool rolloutFlagEnabled(RolloutFlag flag) noexcept
{
    static const auto enabled = [] {
        std::array<bool, static_cast<std::size_t>(RolloutFlag::count)> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto flag = static_cast<RolloutFlag>(index);
            // The editor is a first-class, review-before-apply surface and is
            // safe to expose by default. Only automatic import remains an
            // explicit write opt-in.
            const auto defaultOff = flag == RolloutFlag::tagImportV1;
            values[index] = readEnabled(kEnvironmentNames[index], !defaultOff);
        }
        return values;
    }();
    const auto index = static_cast<std::size_t>(flag);
    return index < enabled.size() && enabled[index];
}

bool qaAlbumAmbientWorkerOnly() noexcept
{
    static const auto enabled = qaEnabled(
        L"NEKOROX_QA_ALBUM_AMBIENT_WORKER_ONLY");
    return enabled;
}

bool qaAlbumAmbientForceDegraded() noexcept
{
    static const auto enabled = qaEnabled(
        L"NEKOROX_QA_ALBUM_AMBIENT_FORCE_320");
    return enabled;
}

bool qaAlbumAmbientEffectFailure() noexcept
{
    static const auto enabled = qaEnabled(
        L"NEKOROX_QA_ALBUM_AMBIENT_EFFECT_FAIL");
    return enabled;
}

bool qaArtworkBusyOnce() noexcept
{
    static const bool enabled = qaEnabled(L"NEKOROX_QA_ARTWORK_BUSY_ONCE");
    return enabled;
}

} // namespace nekorox::component
