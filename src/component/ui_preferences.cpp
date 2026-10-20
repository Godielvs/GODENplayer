#include "ui_preferences.h"

#include "listen_window.h"
#include "sdk_compat.h"
#include "text_util.h"
#include "theme_service.h"
#include "workspace_notifications.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace nekorox::component {
namespace {

const GUID kMotionModeGuid = {0x4900c30b, 0xd3dc, 0x4148,
    {0xa8, 0xca, 0xd1, 0x9a, 0x78, 0x02, 0x72, 0x61}};
const GUID kHeaderWordmarkGuid = {0xc21c1ae7, 0x2fe4, 0x47d6,
    {0x93, 0x95, 0x9a, 0x42, 0x63, 0x0f, 0x6b, 0x13}};
const GUID kHeaderWordmarkPlacementV2MigratedGuid = {
    0x2a975d1e, 0x7f8f, 0x4d68,
    {0x88, 0x28, 0x54, 0xb1, 0x27, 0xf4, 0x3a, 0x91}};
const GUID kHeaderWordmarkChoiceV3ConfiguredGuid = {
    0x06d1648a, 0xd354, 0x4d3c,
    {0xb5, 0x16, 0x73, 0x8a, 0x47, 0x9a, 0xd9, 0x28}};
const GUID kFallbackAccentRgbGuid = {0x5fbc44bc, 0xc4d5, 0x4f2a,
    {0x85, 0x2a, 0x11, 0xfd, 0xe2, 0x97, 0x77, 0x0a}};
const GUID kFallbackAccentConfiguredGuid = {0xd77638b1, 0xe7e1, 0x4f86,
    {0x96, 0x10, 0xce, 0xaf, 0x14, 0xba, 0xd8, 0xa1}};
const GUID kListenEffectsModeGuid = {0xa798b0c5, 0x87bf, 0x4b3c,
    {0x89, 0x2c, 0x0b, 0xb1, 0x9e, 0x86, 0x64, 0xd1}};
const GUID kAlbumAmbientModeGuid = {0xf29107e1, 0x3de9, 0x42e7,
    {0x91, 0x79, 0x56, 0x4e, 0xc0, 0x48, 0xe8, 0x21}};
const GUID kAlbumAmbientReactiveDefaultMigratedGuid = {
    0xa5f3afca, 0x4d7c, 0x45fc,
    {0xb5, 0x95, 0xc3, 0x9d, 0x5f, 0x0f, 0x54, 0x67}};
const GUID kAlbumShowTracksGuid = {0x51cba66e, 0x4210, 0x4ab8,
    {0x88, 0xc3, 0xbf, 0xa5, 0x97, 0x94, 0x54, 0x01}};
const GUID kAlbumShowLyricsGuid = {0x51cba66e, 0x4210, 0x4ab8,
    {0x88, 0xc3, 0xbf, 0xa5, 0x97, 0x94, 0x54, 0x02}};
const GUID kAlbumShowArtworkGuid = {0x51cba66e, 0x4210, 0x4ab8,
    {0x88, 0xc3, 0xbf, 0xa5, 0x97, 0x94, 0x54, 0x03}};
const GUID kAlbumArtworkAlignmentGuid = {0x51cba66e, 0x4210, 0x4ab8,
    {0x88, 0xc3, 0xbf, 0xa5, 0x97, 0x94, 0x54, 0x04}};
const GUID kAlbumTheaterModeGuid = {0x51cba66e, 0x4210, 0x4ab8,
    {0x88, 0xc3, 0xbf, 0xa5, 0x97, 0x94, 0x54, 0x05}};
constexpr std::uint32_t kDefaultFallbackAccentRgb = 0x00D8D0U;
cfg_uint g_motionMode(kMotionModeGuid, static_cast<unsigned>(MotionPreference::followWindows));
cfg_bool g_headerWordmark(kHeaderWordmarkGuid, false);
cfg_bool g_headerWordmarkPlacementV2Migrated(
    kHeaderWordmarkPlacementV2MigratedGuid, false);
cfg_bool g_headerWordmarkChoiceV3Configured(
    kHeaderWordmarkChoiceV3ConfiguredGuid, false);
cfg_uint g_fallbackAccentRgb(
    kFallbackAccentRgbGuid, kDefaultFallbackAccentRgb);
cfg_bool g_fallbackAccentConfigured(
    kFallbackAccentConfiguredGuid, false);
cfg_uint g_listenEffectsMode(
    kListenEffectsModeGuid,
    static_cast<unsigned>(ListenEffectsMode::softLight));
cfg_uint g_albumAmbientMode(kAlbumAmbientModeGuid,
    static_cast<unsigned>(AlbumAmbientMode::reactive));
cfg_bool g_albumAmbientReactiveDefaultMigrated(
    kAlbumAmbientReactiveDefaultMigratedGuid, false);
cfg_uint g_albumShowTracks(kAlbumShowTracksGuid, 1U);
cfg_uint g_albumShowLyrics(kAlbumShowLyricsGuid, 1U);
cfg_uint g_albumShowArtwork(kAlbumShowArtworkGuid, 1U);
cfg_uint g_albumArtworkAlignment(kAlbumArtworkAlignmentGuid, 0U);
cfg_bool g_albumTheaterMode(kAlbumTheaterModeGuid, false);
std::atomic<MotionMode> g_runtimeMotionMode{MotionMode::normal};
std::atomic<MotionPreference> g_runtimeMotionPreference{MotionPreference::followWindows};
std::atomic<ListenEffectsMode> g_runtimeListenEffectsMode{
    ListenEffectsMode::softLight};
std::atomic<AlbumAmbientMode> g_runtimeAlbumAmbientMode{
    AlbumAmbientMode::reactive};
std::atomic<bool> g_runtimeAlbumShowTracks{true};
std::atomic<bool> g_runtimeAlbumShowLyrics{true};
std::atomic<bool> g_runtimeAlbumShowArtwork{true};
std::atomic<bool> g_runtimeAlbumTheaterMode{false};
std::atomic<goden::c11::ArtworkAlignment> g_runtimeAlbumArtworkAlignment{
    goden::c11::ArtworkAlignment::left};
std::atomic<AlbumAmbientFallback> g_albumAmbientFallback{
    AlbumAmbientFallback::rolloutDisabled};
std::atomic<std::uint64_t> g_albumAmbientTextureUpdates{0};
std::atomic<std::uint64_t> g_albumAmbientFeaturePublications{0};
std::atomic<std::uint64_t> g_albumAmbientResourceBytes{0};
std::atomic<bool> g_albumAmbientGpuMeasured{false};
std::atomic<bool> g_albumAmbientDegraded{false};
std::atomic<double> g_albumAmbientGpuMicroseconds{0.0};
std::atomic<double> g_albumAmbientGpuP95Microseconds{0.0};
std::atomic<ListenEffectsFallback> g_listenEffectsFallback{
    ListenEffectsFallback::none};
std::atomic<std::uint64_t> g_listenEffectsSubmissions{0};
std::atomic<std::uint64_t> g_listenEffectsSuccesses{0};
std::atomic<std::uint64_t> g_listenEffectsResourceBytes{0};
std::atomic<std::uint64_t> g_listenEffectsGpuNanoseconds{0};
std::atomic<bool> g_listenEffectsGpuMeasured{false};
std::atomic<bool> g_runtimeHeaderWordmark{false};
std::atomic<std::uint32_t> g_runtimeFallbackAccentRgb{
    kDefaultFallbackAccentRgb};

std::uint8_t colorByte(float value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::uint32_t packColor(RgbColor color) noexcept
{
    return (static_cast<std::uint32_t>(colorByte(color.red)) << 16U) |
        (static_cast<std::uint32_t>(colorByte(color.green)) << 8U) |
        static_cast<std::uint32_t>(colorByte(color.blue));
}

RgbColor unpackColor(std::uint32_t packed) noexcept
{
    return {
        static_cast<float>((packed >> 16U) & 0xffU) / 255.0f,
        static_cast<float>((packed >> 8U) & 0xffU) / 255.0f,
        static_cast<float>(packed & 0xffU) / 255.0f,
    };
}

MotionMode effectiveMotion(MotionPreference preference) noexcept
{
    BOOL animations = TRUE;
    const bool queried = preference == MotionPreference::followWindows &&
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return resolveMotionPreference(preference, queried, animations != FALSE);
}

ListenEffectsMode sanitizedListenEffects(unsigned value) noexcept
{
    return value <= static_cast<unsigned>(ListenEffectsMode::noEffects)
        ? static_cast<ListenEffectsMode>(value)
        : ListenEffectsMode::softLight;
}

AlbumAmbientMode sanitizedAlbumAmbient(unsigned value) noexcept
{
    return value <= static_cast<unsigned>(AlbumAmbientMode::reactive)
        ? static_cast<AlbumAmbientMode>(value)
        : AlbumAmbientMode::reactive;
}

struct Initializer {
    Initializer()
    {
        g_runtimeListenEffectsMode.store(
            sanitizedListenEffects(static_cast<unsigned>(g_listenEffectsMode)),
            std::memory_order_relaxed);
        g_runtimeAlbumAmbientMode.store(
            sanitizedAlbumAmbient(static_cast<unsigned>(g_albumAmbientMode)),
            std::memory_order_relaxed);
        g_runtimeHeaderWordmark.store(
            static_cast<bool>(g_headerWordmark),
            std::memory_order_relaxed);
        g_runtimeFallbackAccentRgb.store(
            static_cast<unsigned>(g_fallbackAccentRgb) & 0x00ffffffU,
            std::memory_order_relaxed);
    }
} g_initializer;

std::filesystem::path installerWordmarkChoicePath()
{
    const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
    return std::filesystem::path(utf8ToWide(native.c_str())) /
        L"nekorox" / L"header-wordmark.install-choice";
}

std::filesystem::path installerFallbackAccentChoicePath()
{
    const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
    return std::filesystem::path(utf8ToWide(native.c_str())) /
        L"nekorox" / L"fallback-accent.install-choice";
}

std::optional<RgbColor> parseFallbackAccent(std::string value)
{
    while (!value.empty() && std::isspace(
            static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    if (!value.empty() && value.front() == '#') {
        value.erase(value.begin());
        if (value.size() != 6U) return std::nullopt;
        char* end = nullptr;
        const auto packed = std::strtoul(value.c_str(), &end, 16);
        if (end == nullptr || *end != '\0') return std::nullopt;
        return unpackColor(static_cast<std::uint32_t>(packed));
    }
    for (auto& character : value) {
        if (character == ',') character = ' ';
    }
    std::istringstream stream(value);
    int red = -1;
    int green = -1;
    int blue = -1;
    std::string remainder;
    if (!(stream >> red >> green >> blue) ||
        (stream >> remainder) ||
        red < 0 || red > 255 || green < 0 || green > 255 ||
        blue < 0 || blue > 255) {
        return std::nullopt;
    }
    return RgbColor{
        static_cast<float>(red) / 255.0f,
        static_cast<float>(green) / 255.0f,
        static_cast<float>(blue) / 255.0f,
    };
}

std::optional<RgbColor> consumeInstallerFallbackAccentChoice()
{
    const auto path = installerFallbackAccentChoicePath();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string value;
    std::getline(stream, value);
    const auto choice = parseFallbackAccent(std::move(value));
    if (choice) {
        stream.close();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    return choice;
}

std::optional<bool> consumeInstallerWordmarkChoice()
{
    const auto path = installerWordmarkChoicePath();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string value;
    std::getline(stream, value);
    while (!value.empty() && std::isspace(
            static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(
            static_cast<unsigned char>(character)));
    }
    std::optional<bool> choice;
    if (value == "show" || value == "enabled" || value == "1") {
        choice = true;
    } else if (value == "hide" || value == "disabled" || value == "0") {
        choice = false;
    }
    if (choice.has_value()) {
        stream.close();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    return choice;
}

bool promptForHeaderWordmark()
{
    constexpr int kShowIdentity = 1001;
    constexpr int kKeepMinimal = 1002;
    const TASKDIALOG_BUTTON buttons[]{
        {kShowIdentity, L"Show GODENplayer in the header"},
        {kKeepMinimal, L"Keep the header minimal"},
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = static_cast<HWND>(core_api::get_main_window());
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW |
        TDF_SIZE_TO_CONTENT | TDF_USE_COMMAND_LINKS;
    config.pszWindowTitle = L"GODENplayer";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"How should GODENplayer appear?";
    config.pszContent =
        L"The hand-drawn pixel wordmark follows the artwork palette.";
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = kShowIdentity;
    config.pszFooter =
        L"You can change this later in Menu > GODENplayer > Show header identity.";
    int selected = 0;
    if (SUCCEEDED(TaskDialogIndirect(
            &config, &selected, nullptr, nullptr))) {
        return selected != kKeepMinimal;
    }
    return MessageBoxW(
        config.hwndParent,
        L"Show GODENplayer in the header?\n\n"
        L"You can change this later in the GODENplayer menu.",
        L"GODENplayer",
        MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON1) == IDYES;
}

class HeaderWordmarkFirstRun : public main_thread_callback {
public:
    void callback_run() override
    {
        if (static_cast<bool>(g_headerWordmarkChoiceV3Configured)) return;
        setHeaderWordmarkEnabled(promptForHeaderWordmark());
    }
};

class FallbackAccentFirstRun : public main_thread_callback {
public:
    void callback_run() override
    {
        if (static_cast<bool>(g_fallbackAccentConfigured)) return;
        if (!chooseFallbackAccentColor(
                static_cast<HWND>(core_api::get_main_window()))) {
            // Cancel means "keep cyan", not "ask on every launch".
            setFallbackAccentColor(fallbackAccentColor());
        }
    }
};

} // namespace

void initializeMotionPreferences() noexcept
{
    // Read cfg only after the host loaded it. Do not invoke setters, persist
    // defaults, or open any window during preference rehydration.
    const auto preference=sanitizeMotionPreference(static_cast<unsigned>(g_motionMode));
    const auto motion=effectiveMotion(preference);
    const auto effects=sanitizedListenEffects(static_cast<unsigned>(g_listenEffectsMode));
    g_runtimeMotionMode.store(motion,std::memory_order_release);
    g_runtimeMotionPreference.store(preference,std::memory_order_release);
    g_runtimeListenEffectsMode.store(effects,std::memory_order_release);
    g_listenEffectsFallback.store(effects==ListenEffectsMode::noEffects?
        ListenEffectsFallback::disabled:ListenEffectsFallback::none,std::memory_order_release);
}

MotionMode motionMode() noexcept
{
    return g_runtimeMotionMode.load(std::memory_order_relaxed);
}

MotionPreference motionPreference() noexcept
{
    return g_runtimeMotionPreference.load(std::memory_order_relaxed);
}

void setMotionPreference(MotionPreference preference)
{
    preference = sanitizeMotionPreference(static_cast<unsigned>(preference));
    g_motionMode = static_cast<unsigned>(preference);
    g_runtimeMotionPreference.store(preference, std::memory_order_release);
    g_runtimeMotionMode.store(effectiveMotion(preference), std::memory_order_release);
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
    ListenWindow::instance().requestRefresh();
}

void setMotionMode(MotionMode mode)
{
    setMotionPreference(static_cast<MotionPreference>(mode));
}

void refreshSystemMotionPreference() noexcept
{
    // Environment notifications run on the host UI thread. Never persist the
    // effective mode: an OS change must not overwrite an explicit selection.
    const auto preference = motionPreference();
    const auto effective = effectiveMotion(preference);
    g_runtimeMotionMode.store(effective, std::memory_order_release);
    // Also redraw retained text after font-smoothing settings change.
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
    ListenWindow::instance().requestRefresh();
}

double scrollResponseScale() noexcept
{
    switch (motionMode()) {
    case MotionMode::normal: return 1.0;
    case MotionMode::reduced: return 0.45;
    case MotionMode::off: return 0.0;
    }
    return 1.0;
}

ListenEffectsMode listenEffectsMode() noexcept
{
    return g_runtimeListenEffectsMode.load(std::memory_order_acquire);
}

void setListenEffectsMode(ListenEffectsMode mode)
{
    mode = sanitizedListenEffects(static_cast<unsigned>(mode));
    g_listenEffectsMode = static_cast<unsigned>(mode);
    g_runtimeListenEffectsMode.store(mode, std::memory_order_release);
    if (mode == ListenEffectsMode::noEffects) {
        g_listenEffectsFallback.store(
            ListenEffectsFallback::disabled, std::memory_order_release);
    }
    ListenWindow::instance().requestRefresh();
}

ListenEffectsDiagnostics listenEffectsDiagnostics() noexcept
{
    ListenEffectsDiagnostics out;
    out.mode = listenEffectsMode();
    out.fallback = g_listenEffectsFallback.load(std::memory_order_acquire);
    out.submissions = g_listenEffectsSubmissions.load(
        std::memory_order_acquire);
    out.successes = g_listenEffectsSuccesses.load(
        std::memory_order_acquire);
    out.resourceBytes = g_listenEffectsResourceBytes.load(
        std::memory_order_acquire);
    out.gpuMeasured = g_listenEffectsGpuMeasured.load(
        std::memory_order_acquire);
    out.gpuMicroseconds = static_cast<double>(
        g_listenEffectsGpuNanoseconds.load(std::memory_order_acquire)) /
        1000.0;
    return out;
}

void publishListenEffectsDiagnostics(
    const ListenEffectsDiagnostics& diagnostics) noexcept
{
    g_listenEffectsFallback.store(
        diagnostics.fallback, std::memory_order_release);
    g_listenEffectsSubmissions.store(
        diagnostics.submissions, std::memory_order_release);
    g_listenEffectsSuccesses.store(
        diagnostics.successes, std::memory_order_release);
    g_listenEffectsResourceBytes.store(
        diagnostics.resourceBytes, std::memory_order_release);
    g_listenEffectsGpuNanoseconds.store(
        static_cast<std::uint64_t>(std::max(
            0.0, diagnostics.gpuMicroseconds) * 1000.0),
        std::memory_order_release);
    g_listenEffectsGpuMeasured.store(
        diagnostics.gpuMeasured, std::memory_order_release);
}

AlbumAmbientMode albumAmbientMode() noexcept
{
    return g_runtimeAlbumAmbientMode.load(std::memory_order_acquire);
}

void setAlbumAmbientMode(AlbumAmbientMode mode)
{
    mode = sanitizedAlbumAmbient(static_cast<unsigned>(mode));
    g_albumAmbientMode = static_cast<unsigned>(mode);
    g_runtimeAlbumAmbientMode.store(mode, std::memory_order_release);
    notifyWorkspacePlaybackChanged();
}

void initializeAlbumAmbientPreference()
{
    // cfg_var values are restored by foobar2000 after static constructors.
    // Refresh the runtime mirror at after_config_read so the persisted
    // Off/Subtle/Reactive choice wins over the compile-time default.
    if (!static_cast<bool>(g_albumAmbientReactiveDefaultMigrated)) {
        // No historical marker distinguishes an explicit Subtle choice from
        // the old default. Fresh profiles already default to Reactive; never
        // rewrite an existing user's valid selection to migrate that default.
        g_albumAmbientReactiveDefaultMigrated = true;
    }
    g_runtimeAlbumAmbientMode.store(
        sanitizedAlbumAmbient(static_cast<unsigned>(g_albumAmbientMode)),
        std::memory_order_release);
}

void initializeAlbumPanelPreferences() noexcept
{
    g_runtimeAlbumTheaterMode.store(g_albumTheaterMode, std::memory_order_release);
    g_runtimeAlbumShowTracks.store(
        static_cast<unsigned>(g_albumShowTracks) != 0U,
        std::memory_order_release);
    g_runtimeAlbumShowLyrics.store(
        static_cast<unsigned>(g_albumShowLyrics) != 0U,
        std::memory_order_release);
    g_runtimeAlbumShowArtwork.store(
        static_cast<unsigned>(g_albumShowArtwork) != 0U,
        std::memory_order_release);
    g_runtimeAlbumArtworkAlignment.store(
        static_cast<goden::c11::ArtworkAlignment>(
            std::min(2U,static_cast<unsigned>(g_albumArtworkAlignment))),
        std::memory_order_release);
}

bool albumShowTracks() noexcept
{
    return g_runtimeAlbumShowTracks.load(std::memory_order_acquire);
}

bool albumShowLyrics() noexcept
{
    return g_runtimeAlbumShowLyrics.load(std::memory_order_acquire);
}

bool setAlbumShowTracks(bool visible)
{
    if (g_runtimeAlbumShowTracks.exchange(
            visible, std::memory_order_acq_rel) == visible) return false;
    g_albumShowTracks = visible ? 1U : 0U;
    return true;
}

bool setAlbumShowLyrics(bool visible)
{
    if (g_runtimeAlbumShowLyrics.exchange(
            visible, std::memory_order_acq_rel) == visible) return false;
    g_albumShowLyrics = visible ? 1U : 0U;
    return true;
}

bool albumShowArtwork() noexcept
{
    return g_runtimeAlbumShowArtwork.load(std::memory_order_acquire);
}

bool albumTheaterMode() noexcept
{
    return g_runtimeAlbumTheaterMode.load(std::memory_order_acquire);
}

bool setAlbumTheaterMode(bool enabled)
{
    if (g_runtimeAlbumTheaterMode.exchange(enabled, std::memory_order_acq_rel) == enabled)
        return false;
    g_albumTheaterMode = enabled;
    return true;
}

goden::c11::ArtworkAlignment albumArtworkAlignment() noexcept
{
    return g_runtimeAlbumArtworkAlignment.load(std::memory_order_acquire);
}

bool setAlbumShowArtwork(bool visible)
{
    if(g_runtimeAlbumShowArtwork.exchange(visible,std::memory_order_acq_rel)==visible)return false;
    g_albumShowArtwork=visible?1U:0U;
    return true;
}

bool setAlbumArtworkAlignment(goden::c11::ArtworkAlignment alignment)
{
    alignment=static_cast<goden::c11::ArtworkAlignment>(std::min(2U,static_cast<unsigned>(alignment)));
    if(g_runtimeAlbumArtworkAlignment.exchange(alignment,std::memory_order_acq_rel)==alignment)return false;
    g_albumArtworkAlignment=static_cast<unsigned>(alignment);
    return true;
}

AlbumAmbientDiagnostics albumAmbientDiagnostics() noexcept
{
    AlbumAmbientDiagnostics value;
    value.mode = albumAmbientMode();
    value.fallback = g_albumAmbientFallback.load(std::memory_order_acquire);
    value.textureUpdates = g_albumAmbientTextureUpdates.load(
        std::memory_order_acquire);
    value.featurePublications = g_albumAmbientFeaturePublications.load(
        std::memory_order_acquire);
    value.resourceBytes = g_albumAmbientResourceBytes.load(
        std::memory_order_acquire);
    value.gpuMeasured = g_albumAmbientGpuMeasured.load(
        std::memory_order_acquire);
    value.degraded = g_albumAmbientDegraded.load(std::memory_order_acquire);
    value.gpuMicroseconds = g_albumAmbientGpuMicroseconds.load(
        std::memory_order_acquire);
    value.gpuP95Microseconds = g_albumAmbientGpuP95Microseconds.load(
        std::memory_order_acquire);
    return value;
}

void publishAlbumAmbientDiagnostics(
    const AlbumAmbientDiagnostics& diagnostics) noexcept
{
    g_albumAmbientFallback.store(
        diagnostics.fallback, std::memory_order_release);
    g_albumAmbientTextureUpdates.store(
        diagnostics.textureUpdates, std::memory_order_release);
    g_albumAmbientFeaturePublications.store(
        diagnostics.featurePublications, std::memory_order_release);
}

void publishAlbumAmbientRenderDiagnostics(std::uint64_t resourceBytes,
    bool degraded, bool gpuMeasured, double gpuMicroseconds,
    double gpuP95Microseconds) noexcept
{
    g_albumAmbientResourceBytes.store(resourceBytes, std::memory_order_release);
    g_albumAmbientDegraded.store(degraded, std::memory_order_release);
    g_albumAmbientGpuMeasured.store(gpuMeasured, std::memory_order_release);
    g_albumAmbientGpuMicroseconds.store(
        gpuMicroseconds, std::memory_order_release);
    g_albumAmbientGpuP95Microseconds.store(
        gpuP95Microseconds, std::memory_order_release);
}

bool headerWordmarkEnabled() noexcept
{
    return g_runtimeHeaderWordmark.load(std::memory_order_relaxed);
}

void setHeaderWordmarkEnabled(bool enabled)
{
    g_headerWordmark = enabled;
    g_headerWordmarkPlacementV2Migrated = true;
    g_headerWordmarkChoiceV3Configured = true;
    g_runtimeHeaderWordmark.store(enabled, std::memory_order_release);
    notifyNativePanelsPlaybackChanged();
}

void initializeHeaderWordmarkPreference()
{
    // cfg_var values are fully restored by on_init, after static constructors
    // have already run. Refresh the runtime mirror here so a saved wordmark
    // choice cannot intermittently fall back to the compile-time default.
    g_runtimeHeaderWordmark.store(
        static_cast<bool>(g_headerWordmark),
        std::memory_order_release);
    if (const auto installerChoice = consumeInstallerWordmarkChoice()) {
        setHeaderWordmarkEnabled(*installerChoice);
        return;
    }
    if (static_cast<bool>(g_headerWordmarkChoiceV3Configured)) return;
    // Profiles that already saw the previous placement migration have an
    // intentional saved state. Preserve it without showing a new prompt.
    if (static_cast<bool>(g_headerWordmarkPlacementV2Migrated)) {
        g_headerWordmarkChoiceV3Configured = true;
        return;
    }
    if (core_api::is_quiet_mode_enabled()) {
        setHeaderWordmarkEnabled(true);
        return;
    }
    main_thread_callback_add(
        new service_impl_t<HeaderWordmarkFirstRun>());
}

RgbColor fallbackAccentColor() noexcept
{
    return unpackColor(g_runtimeFallbackAccentRgb.load(
        std::memory_order_acquire));
}

void setFallbackAccentColor(RgbColor color)
{
    const auto packed = packColor(color);
    g_fallbackAccentRgb = packed;
    g_fallbackAccentConfigured = true;
    g_runtimeFallbackAccentRgb.store(packed, std::memory_order_release);
    ThemeService::instance().refreshFallbackPalette();
    notifyNativePanelsPlaybackChanged(true);
    ListenWindow::instance().requestRefresh();
}

bool chooseFallbackAccentColor(HWND owner)
{
    const auto current = packColor(fallbackAccentColor());
    COLORREF customColors[16]{
        RGB(0, 216, 208),       // Goden cyan
        RGB(233, 116, 81),      // Burnt sienna
        RGB(204, 92, 92),
        RGB(214, 93, 177),
        RGB(141, 110, 230),
        RGB(80, 142, 230),
        RGB(76, 175, 123),
        RGB(222, 177, 79),
    };
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = owner;
    choice.rgbResult = RGB(
        (current >> 16U) & 0xffU,
        (current >> 8U) & 0xffU,
        current & 0xffU);
    choice.lpCustColors = customColors;
    choice.Flags = CC_FULLOPEN | CC_RGBINIT | CC_SOLIDCOLOR;
    if (!ChooseColorW(&choice)) return false;
    setFallbackAccentColor({
        static_cast<float>(GetRValue(choice.rgbResult)) / 255.0f,
        static_cast<float>(GetGValue(choice.rgbResult)) / 255.0f,
        static_cast<float>(GetBValue(choice.rgbResult)) / 255.0f,
    });
    return true;
}

void initializeFallbackAccentPreference()
{
    g_runtimeFallbackAccentRgb.store(
        static_cast<unsigned>(g_fallbackAccentRgb) & 0x00ffffffU,
        std::memory_order_release);
    if (const auto installerChoice = consumeInstallerFallbackAccentChoice()) {
        setFallbackAccentColor(*installerChoice);
        return;
    }
    if (static_cast<bool>(g_fallbackAccentConfigured)) return;
    // This preference did not exist in older Goden profiles. Preserve their
    // established cyan appearance; the menu exposes the picker explicitly.
    if (static_cast<bool>(g_headerWordmarkChoiceV3Configured) ||
        static_cast<bool>(g_headerWordmarkPlacementV2Migrated)) {
        setFallbackAccentColor(fallbackAccentColor());
        return;
    }
    if (core_api::is_quiet_mode_enabled()) {
        setFallbackAccentColor(fallbackAccentColor());
        return;
    }
    main_thread_callback_add(
        new service_impl_t<FallbackAccentFirstRun>());
}

} // namespace nekorox::component
