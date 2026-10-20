#include "qa_hooks.h"
#include "discord_presence_service.h"

#include "runtime.h"
#include "text_util.h"
#include <filesystem>
#include <fstream>
#include "rollout_flags.h"
#include "sdk_compat.h"
#include "presence_deployment.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <string_view>
#include <tlhelp32.h>
#include <vector>
#include <map>
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace nekorox::component {
namespace {

using namespace std::chrono_literals;

// Public compatibility identity used by the MIT foo_discord_rich component.
// Goden Options accepts a user-owned application ID as an override.
constexpr char kCompatibilityApplicationId[] = "507982587416018945";
constexpr char kCompatibilityPlayingImage[] = "playing-dark";
constexpr char kCompatibilityPausedImage[] = "paused-dark";
constexpr std::uint32_t kHandshakeOpcode = 0;
constexpr std::uint32_t kFrameOpcode = 1;
constexpr std::uint32_t kCloseOpcode = 2;
constexpr std::uint32_t kPingOpcode = 3;
constexpr std::uint32_t kPongOpcode = 4;
constexpr std::size_t kMaximumFrameBytes = 1024U * 1024U;
constexpr auto kMinimumUpdateInterval = 4100ms;
constexpr auto kTrackMetadataSettle = 120ms;
// Give a cache hit or compact upload a short chance to join the semantic
// track update. One complete activity is both visually cleaner and consumes
// half as much of Discord's rolling update budget as a late cover patch.
constexpr auto kTrackArtworkSettle = 1500ms;
constexpr auto kRpcAckTimeout = 2500ms;
constexpr auto kDiscordRateWindow = 20s;
constexpr auto kTrackTransitionMinimumInterval = 650ms;
constexpr std::string_view kSeparator = "\xC2\xB7";

bool qaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

std::optional<bool> qaExternalPublisherConflictOverride() noexcept
{
    if (!qaHooksAllowed()) return std::nullopt;
    wchar_t value[8]{};
    const auto length = GetEnvironmentVariableW(
        L"NEKOROX_QA_EXTERNAL_PRESENCE_CONFLICT", value,
        static_cast<DWORD>(std::size(value)));
    // Deterministic QA must never inherit a real desktop helper process. An
    // explicit 1 opts into the conflict fixture; missing/0 means no conflict.
    return length == 1U && value[0] == L'1';
}

bool processIsRunning(std::wstring_view executableName) noexcept
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile,
                    std::wstring(executableName).c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::optional<std::string> readSmallTextFile(
    const std::wstring& path) noexcept
{
    const auto file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
            size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const auto succeeded = result.empty() ||
        ReadFile(file, result.data(), static_cast<DWORD>(result.size()),
            &read, nullptr);
    CloseHandle(file);
    if (!succeeded) return std::nullopt;
    result.resize(read);
    return result;
}

std::optional<std::string_view> jsonObjectForKey(
    std::string_view source, std::string_view key) noexcept
{
    std::string needle;
    needle.reserve(key.size() + 2U);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    auto keyPosition = source.find(needle);
    while (keyPosition != std::string_view::npos) {
        auto position = source.find(':', keyPosition + needle.size());
        if (position == std::string_view::npos) return std::nullopt;
        ++position;
        while (position < source.size() &&
                (source[position] == ' ' || source[position] == '\t' ||
                 source[position] == '\r' || source[position] == '\n')) {
            ++position;
        }
        if (position < source.size() && source[position] == '{') {
            const auto begin = position;
            unsigned depth = 0;
            bool quoted = false;
            bool escaped = false;
            for (; position < source.size(); ++position) {
                const auto value = source[position];
                if (quoted) {
                    if (escaped) escaped = false;
                    else if (value == '\\') escaped = true;
                    else if (value == '"') quoted = false;
                    continue;
                }
                if (value == '"') quoted = true;
                else if (value == '{') ++depth;
                else if (value == '}' && --depth == 0U) {
                    return source.substr(begin, position - begin + 1U);
                }
            }
            return std::nullopt;
        }
        keyPosition = source.find(needle, keyPosition + needle.size());
    }
    return std::nullopt;
}

std::optional<bool> jsonBooleanForKey(
    std::string_view source, std::string_view key) noexcept
{
    std::string needle;
    needle.reserve(key.size() + 2U);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    const auto keyPosition = source.find(needle);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    auto position = source.find(':', keyPosition + needle.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < source.size() &&
            (source[position] == ' ' || source[position] == '\t' ||
             source[position] == '\r' || source[position] == '\n')) {
        ++position;
    }
    if (source.substr(position, 4U) == "true") return true;
    if (source.substr(position, 5U) == "false") return false;
    return std::nullopt;
}

std::optional<std::string> jsonStringForKey(
    std::string_view source, std::string_view key)
{
    std::string needle;
    needle.reserve(key.size() + 2U);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    const auto keyPosition = source.find(needle);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    auto position = source.find(':', keyPosition + needle.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < source.size() &&
            (source[position] == ' ' || source[position] == '\t' ||
             source[position] == '\r' || source[position] == '\n')) {
        ++position;
    }
    if (position >= source.size() || source[position++] != '"') {
        return std::nullopt;
    }
    std::string value;
    bool escaped = false;
    for (; position < source.size(); ++position) {
        const auto character = source[position];
        if (escaped) {
            // Nonce/cmd values used here are ASCII. Preserve escaped bytes
            // only so malformed replies can never compare equal by accident.
            value.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value.push_back(character);
        }
    }
    return std::nullopt;
}

bool musicPresencePublishesFoobar() noexcept
{
    if (const auto override = qaExternalPublisherConflictOverride()) {
        return *override;
    }
    if (!processIsRunning(L"Music Presence.exe")) return false;
    std::array<wchar_t, 32768> appData{};
    const auto length = GetEnvironmentVariableW(L"APPDATA", appData.data(),
        static_cast<DWORD>(appData.size()));
    if (length == 0U || length >= appData.size()) return true;
    const auto path = std::wstring(appData.data(), length) +
        L"\\Music Presence\\settings.json";
    const auto settings = readSmallTextFile(path);
    if (!settings) return true;
    const auto players = jsonObjectForKey(*settings, "players");
    const auto wellKnown = players
        ? jsonObjectForKey(*players, "well_known") : std::nullopt;
    const auto foobar = wellKnown
        ? jsonObjectForKey(*wellKnown, "foobar2000") : std::nullopt;
    const auto enabled = foobar
        ? jsonBooleanForKey(*foobar, "enabled") : std::nullopt;
    // Unknown schemas are treated as potential conflicts while the publisher
    // process is live; false negatives would recreate the silent overwrite.
    return !enabled.has_value() || *enabled;
}

bool hasCompletePlaybackRange(const PlaybackSnapshotV2& playback) noexcept
{
    return std::isfinite(playback.positionSeconds) &&
        playback.positionSeconds >= 0.0 &&
        std::isfinite(playback.durationSeconds) &&
        playback.durationSeconds > playback.positionSeconds;
}

void qaPresenceLog(std::string_view message)
{
    if (!qaHooksAllowed()) return;
    std::array<wchar_t, 32768> path{};
    const auto length = GetEnvironmentVariableW(L"NEKOROX_QA_PRESENCE_LOG",
        path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return;
    const auto file = CreateFileW(path.data(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const auto line = std::to_string(GetTickCount64()) + " " +
        std::string(message) + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()),
        &written, nullptr);
    CloseHandle(file);
}

const GUID kPresenceEnabledGuid = {0xb7d97e82, 0x389a, 0x48a5,
    {0x92, 0x0c, 0xeb, 0x4e, 0xa5, 0xc0, 0x38, 0xc9}};
const GUID kPresencePausedGuid = {0x2be140ef, 0xa0c6, 0x4852,
    {0xb6, 0x66, 0x65, 0xdd, 0xa7, 0xd7, 0xe8, 0x3d}};
const GUID kPresenceApplicationGuid = {0xd21166e3, 0x0bf8, 0x49e5,
    {0x92, 0xe5, 0xa1, 0xb4, 0x53, 0x62, 0x16, 0xc8}};
const GUID kPresenceTemporaryCoversGuid = {0x07bc768a, 0x091c, 0x4986,
    {0x82, 0x06, 0xf5, 0x04, 0x2f, 0x30, 0x55, 0xcf}};

// Unset is deliberately distinct from a new public profile. The installer seeds
// the chosen provider after showing its disclosure; legacy private profiles never
// silently switch to persistent public uploads.
const GUID kCoverProviderGuid = {0x9448016b,0x56f5,0x4d7f,{0x83,0xb2,0x92,0x59,0x91,0xb6,0x88,0x01}};
const GUID kCoverDisclosureGuid = {0x9448016b,0x56f5,0x4d7f,{0x83,0xb2,0x92,0x59,0x91,0xb6,0x88,0x02}};
const GUID kRelayEndpointGuid = {0x9448016b,0x56f5,0x4d7f,{0x83,0xb2,0x92,0x59,0x91,0xb6,0x88,0x03}};
const GUID kRelayCredentialGuid = {0x9448016b,0x56f5,0x4d7f,{0x83,0xb2,0x92,0x59,0x91,0xb6,0x88,0x04}};
const GUID kCoverInstallChoiceConsumedGuid = {0x9448016b,0x56f5,0x4d7f,{0x83,0xb2,0x92,0x59,0x91,0xb6,0x88,0x05}};
cfg_uint g_coverProvider(kCoverProviderGuid, 0);
cfg_bool g_coverDisclosure(kCoverDisclosureGuid, false);
cfg_bool g_coverInstallChoiceConsumed(kCoverInstallChoiceConsumedGuid, false);
cfg_string g_relayEndpoint(kRelayEndpointGuid, "");
cfg_string g_relayCredential(kRelayCredentialGuid, "");

std::string protectRelayCredential(std::string_view value)
{
    DATA_BLOB input{static_cast<DWORD>(value.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))}, output{};
    if (!CryptProtectData(&input, L"GODENplayer private relay", nullptr, nullptr,
            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result(output.cbData * 2U, '0');
    for (DWORD i=0;i<output.cbData;++i) {
        result[2U*i]=digits[output.pbData[i]>>4U];
        result[2U*i+1U]=digits[output.pbData[i]&15U];
    }
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    return result;
}

std::string unprotectRelayCredential(std::string_view value)
{
    if (value.empty() || value.size()>8192U || value.size()%2U) return {};
    const auto nibble=[](char c)->int {
        if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10;
        return -1;
    };
    std::vector<BYTE> bytes(value.size()/2U);
    for(std::size_t i=0;i<bytes.size();++i) {
        const auto a=nibble(value[2*i]),b=nibble(value[2*i+1]);
        if(a<0||b<0)return {}; bytes[i]=static_cast<BYTE>(a*16+b);
    }
    DATA_BLOB input{static_cast<DWORD>(bytes.size()),bytes.data()},output{};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,&output)) return {};
    std::string result(reinterpret_cast<char*>(output.pbData),output.cbData);
    SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);
    return result;
}

void loadPresenceCoverConfiguration()
{
    PresenceCoverConfiguration value;
    const auto provider=static_cast<unsigned>(g_coverProvider);
    if(provider<=2U)value.provider=static_cast<PresenceCoverProvider>(provider);
    value.disclosureAccepted=static_cast<bool>(g_coverDisclosure);
    value.relayEndpoint=g_relayEndpoint.get().c_str();
    if(value.provider==PresenceCoverProvider::privateRelay)
        value.relayCredential=unprotectRelayCredential(g_relayCredential.get().c_str());
    publishPresenceCoverConfiguration(std::move(value));
}

cfg_bool g_presenceEnabled(kPresenceEnabledGuid, true);
cfg_bool g_presenceShowPaused(kPresencePausedGuid, false);
// New GODENplayer profiles publish Presence by default. Existing profiles keep
// their persisted choices because cfg_bool only supplies missing-key defaults.
// Cover delivery is also enabled for new profiles. Keep the same key so an
// existing user's saved disabled choice remains disabled on upgrade.
cfg_bool g_presenceTemporaryCovers(kPresenceTemporaryCoversGuid, true);
cfg_string g_presenceApplicationId(
    kPresenceApplicationGuid, kCompatibilityApplicationId);

std::string trimAscii(std::string value)
{
    const auto notSpace = [](unsigned char c) { return c > 0x20U; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());
    return value;
}

bool validApplicationId(const std::string& value) noexcept
{
    if (value.size() < 16U || value.size() > 24U) return false;
    return std::all_of(value.begin(), value.end(),
        [](unsigned char character) {
            return character >= '0' && character <= '9';
        });
}

std::string configuredApplicationId()
{
    const auto value = trimAscii(g_presenceApplicationId.get().c_str());
    if (value == kCompatibilityApplicationId) {
        const auto official = std::string(presence_deployment::applicationId);
        if (validApplicationId(official)) return official;
    }
    return validApplicationId(value) ? value : kCompatibilityApplicationId;
}

bool ephemeralRelayEnabled() noexcept
{
    return rolloutFlagEnabled(RolloutFlag::presenceV3) &&
        rolloutFlagEnabled(RolloutFlag::presenceEphemeralRelayV1) &&
        DiscordRelayClient::configured();
}

std::string bounded(std::string value, std::size_t maximum = 128U)
{
    if (value.size() > maximum) {
        // Discord limits these fields by UTF-8 byte length. Never leave a
        // truncated multi-byte code point at the end of the JSON string.
        auto cut = maximum;
        while (cut > 0U &&
            (static_cast<unsigned char>(value[cut]) & 0xc0U) == 0x80U) {
            --cut;
        }
        value.resize(cut);
    }
    return value;
}

std::string metadata(const metadb_handle_ptr& track,
    playback_control& playback, bool usePlaybackMetadata,
    const char* expression)
{
    if (!track.is_valid()) return {};
    // Captures are main-thread-owned; expressions are a fixed internal set.
    static std::map<std::string,titleformat_object::ptr> scripts;
    auto& script=scripts[expression];
    if(!script.is_valid())titleformat_compiler::get()->compile_safe(script, expression);
    pfc::string8_fast value;
    const auto formatted = usePlaybackMetadata
        ? playback.playback_format_title(
            nullptr, value, script, nullptr,
            playback_control::display_level_all)
        : track->format_title(nullptr, value, script, nullptr);
    if (!formatted) {
        return {};
    }
    auto result = bounded(trimAscii(value.c_str()));
    // display_level_all represents a missing title-format field as "?".
    // Treat it as absent so public Presence can apply explicit fallbacks.
    return result == "?" ? std::string{} : result;
}

bool missingPresenceText(const char* text) noexcept
{
    const auto value = trimAscii(text != nullptr ? text : "");
    return value.empty() || value == "?";
}

std::string jsonEscape(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 16U);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                result += "\\u00";
                result.push_back(hex[character >> 4U]);
                result.push_back(hex[character & 0x0fU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

std::string jsonQuoted(std::string_view value)
{
    return '"' + jsonEscape(value) + '"';
}

std::string joinNonEmpty(
    std::initializer_list<std::string_view> values,
    std::string_view separator)
{
    std::string result;
    for (const auto value : values) {
        if (value.empty()) continue;
        if (!result.empty()) result += separator;
        result += value;
    }
    return bounded(std::move(result));
}

template <std::size_t Size>
void copyPresenceText(
    std::array<char, Size>& target,
    std::string_view source) noexcept
{
    target.fill('\0');
    if (Size == 0U || source.empty()) return;
    const auto count = (std::min)(source.size(), Size - 1U);
    std::memcpy(target.data(), source.data(), count);
}

std::int64_t unixNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint64_t presenceMilliseconds() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now()
            .time_since_epoch()).count());
}

std::string musicBrainzUrl(std::string_view kind, std::string_view id)
{
    if (id.empty()) return {};
    return "https://musicbrainz.org/" + std::string(kind) + '/' +
        std::string(id);
}

std::string activityJson(
    const DiscordTrackPresence& track,
    bool showPaused,
    std::uint64_t revision,
    std::string_view applicationId,
    std::optional<std::int64_t> timelineStart = std::nullopt,
    std::optional<std::int64_t> timelineEnd = std::nullopt)
{
    const auto compatibilityAssets =
        applicationId == kCompatibilityApplicationId;
    const auto staticAssets =
        presence_deployment::assetsForApplication(applicationId);
    const auto& playback = track.playback;
    std::ostringstream stream;
    stream << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"
           << GetCurrentProcessId() << ",\"activity\":";
    if (playback.state == PlaybackState::stopped ||
        (playback.state == PlaybackState::paused && !showPaused)) {
        stream << "null";
    } else {
        auto title = bounded(trimAscii(playback.title.data()));
        auto artist = bounded(trimAscii(playback.artist.data()));
        auto album = bounded(trimAscii(playback.album.data()));
        if (title.empty()) title = "Unknown track";
        const auto separator = std::string(" ") +
            std::string(kSeparator) + " ";
        const auto detail = track.dawMode
            ? bounded("DAW Mode" + separator +
                std::to_string(track.dawTrackCount) +
                (track.dawTrackCount == 1U ? " stem" : " stems"))
            : title;
        auto state = track.dawMode
            ? bounded(std::string(playback.state == PlaybackState::paused
                    ? "Paused" : "Previewing") +
                (artist.empty() ? " mix" : separator + artist))
            : track.localFileFallback
                ? bounded(std::string("Local file") +
                    (track.fileName.empty()
                        ? std::string{} : separator + track.fileName))
            : joinNonEmpty({artist, album}, separator);
        if (playback.state == PlaybackState::paused) {
            if (!track.dawMode) {
                state = state.empty() ? "Paused" :
                    "Paused" + separator + state;
            }
        }
        if (state.empty()) state = "GODENplayer";

        const auto musical = track.dawMode
            ? std::string("GODENplayer DAW Mode")
            : track.localFileFallback
                ? std::string("Local audio file")
            : std::string{};
        const auto technical = track.dawMode
            ? std::string("48 kHz") + separator + "live stem mix"
            : joinNonEmpty({track.codec, track.bitrate, track.sampleRate,
                 track.bitDepth, track.channels}, separator);
        const auto recordingUrl = musicBrainzUrl(
            "recording", track.musicBrainzRecordingId);
        const auto releaseUrl = musicBrainzUrl(
            "release", track.musicBrainzReleaseId);

        stream << "{\"type\":2,\"details\":" << jsonQuoted(detail)
               << ",\"state\":" << jsonQuoted(state)
               << ",\"status_display_type\":2,\"instance\":false";
        if (!recordingUrl.empty()) {
            stream << ",\"details_url\":" << jsonQuoted(recordingUrl);
        }
        if (!releaseUrl.empty()) {
            stream << ",\"state_url\":" << jsonQuoted(releaseUrl);
        }
        if (playback.state == PlaybackState::playing && timelineStart) {
            stream << ",\"timestamps\":{\"start\":" << *timelineStart;
            if (timelineEnd && *timelineEnd > *timelineStart) {
                stream << ",\"end\":" << *timelineEnd;
            }
            stream << '}';
        }

        stream << ",\"assets\":{";
        bool assetComma = false;
        const auto writeAsset = [&](std::string_view key,
                                    std::string_view value) {
            if (value.empty()) return;
            if (assetComma) stream << ',';
            stream << jsonQuoted(key) << ':' << jsonQuoted(value);
            assetComma = true;
        };
        if (!track.coverUrl.empty()) {
            writeAsset("large_image", track.coverUrl);
        } else if (staticAssets.declared) {
            writeAsset("large_image", track.dawMode
                ? staticAssets.daw
                : staticAssets.fallback);
        } else if (!track.musicBrainzReleaseId.empty()) {
            const auto cover = "https://coverartarchive.org/release/" +
                track.musicBrainzReleaseId + "/front-500";
            writeAsset("large_image", cover);
        }
        // The application name already appears as Discord's activity header.
        // Repeating GODENplayer as large_text creates a redundant third line
        // in the compact card, so normal playback intentionally omits it.
        writeAsset("large_text", musical);
        if (compatibilityAssets) {
            writeAsset("small_image",
                playback.state == PlaybackState::paused
                    ? kCompatibilityPausedImage : kCompatibilityPlayingImage);
        }
        writeAsset("small_text",
            technical.empty() ? "Local playback" : technical);
        if (!releaseUrl.empty()) {
            writeAsset("large_url", releaseUrl);
        }
        stream << '}';

        if (!recordingUrl.empty() || !releaseUrl.empty()) {
            stream << ",\"buttons\":[";
            bool comma = false;
            if (!recordingUrl.empty()) {
                stream << "{\"label\":\"Recording\",\"url\":"
                       << jsonQuoted(recordingUrl) << '}';
                comma = true;
            }
            if (!releaseUrl.empty()) {
                if (comma) stream << ',';
                stream << "{\"label\":\"Release\",\"url\":"
                       << jsonQuoted(releaseUrl) << '}';
            }
            stream << ']';
        }
        stream << '}';
    }
    stream << "},\"nonce\":\"goden-" << revision << "\"}";
    return stream.str();
}

bool waitForOverlapped(HANDLE pipe, OVERLAPPED& request,
    std::stop_token stopToken, DWORD& transferred)
{
    for (;;) {
        const auto wait = WaitForSingleObject(request.hEvent, 10);
        if (wait == WAIT_OBJECT_0) {
            return GetOverlappedResult(pipe, &request, &transferred, FALSE) != FALSE;
        }
        if (wait != WAIT_TIMEOUT) {
            CancelIoEx(pipe, &request);
            GetOverlappedResult(pipe, &request, &transferred, TRUE);
            return false;
        }
        if (stopToken.stop_requested()) {
            // OVERLAPPED storage must remain alive through cancellation. The
            // blocking completion wait is safe here because CancelIoEx targets
            // this exact request and does not depend on the peer process.
            CancelIoEx(pipe, &request);
            GetOverlappedResult(pipe, &request, &transferred, TRUE);
            return false;
        }
    }
}

bool transferOverlapped(HANDLE pipe, bool writing, void* bytes, DWORD count,
    std::stop_token stopToken, DWORD& transferred)
{
    OVERLAPPED request{};
    request.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (request.hEvent == nullptr) return false;
    const auto started = writing
        ? WriteFile(pipe, bytes, count, nullptr, &request)
        : ReadFile(pipe, bytes, count, nullptr, &request);
    const auto error = started ? ERROR_SUCCESS : GetLastError();
    bool success = false;
    if (started || error == ERROR_IO_PENDING) {
        success = waitForOverlapped(pipe, request, stopToken, transferred);
    }
    CloseHandle(request.hEvent);
    return success;
}

bool writeAll(HANDLE pipe, const void* bytes, std::size_t count,
    std::stop_token stopToken)
{
    auto* cursor = const_cast<std::byte*>(
        static_cast<const std::byte*>(bytes));
    while (count != 0U) {
        DWORD written = 0;
        const auto portion = static_cast<DWORD>(std::min<std::size_t>(
            count, (std::numeric_limits<DWORD>::max)()));
        if (!transferOverlapped(pipe, true, cursor, portion,
                stopToken, written) || written == 0) {
            return false;
        }
        cursor += written;
        count -= written;
    }
    return true;
}

bool writeFrame(HANDLE pipe, std::uint32_t opcode, std::string_view payload,
    std::stop_token stopToken)
{
    if (payload.size() > kMaximumFrameBytes) return false;
    const std::array<std::uint32_t, 2> header{
        opcode, static_cast<std::uint32_t>(payload.size())};
    return writeAll(pipe, header.data(), sizeof(header), stopToken) &&
        writeAll(pipe, payload.data(), payload.size(), stopToken);
}

bool readExact(HANDLE pipe, void* bytes, std::size_t count,
    std::stop_token stopToken)
{
    auto* cursor = static_cast<std::byte*>(bytes);
    while (count != 0U) {
        DWORD read = 0;
        const auto portion = static_cast<DWORD>(std::min<std::size_t>(
            count, (std::numeric_limits<DWORD>::max)()));
        if (!transferOverlapped(pipe, false, cursor, portion,
                stopToken, read) || read == 0) {
            return false;
        }
        cursor += read;
        count -= read;
    }
    return true;
}

bool waitForBytes(HANDLE pipe, std::stop_token stopToken,
    DWORD minimumBytes, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stopToken.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        if (available >= minimumBytes) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool readFrame(HANDLE pipe, std::stop_token stopToken,
    std::uint32_t& opcode, std::string& payload)
{
    if (!waitForBytes(pipe, stopToken, 8, 2500ms)) return false;
    std::array<std::uint32_t, 2> header{};
    if (!readExact(pipe, header.data(), sizeof(header), stopToken) ||
        header[1] > kMaximumFrameBytes) {
        return false;
    }
    if (header[1] != 0U &&
        !waitForBytes(pipe, stopToken, header[1], 2500ms)) {
        return false;
    }
    payload.assign(header[1], '\0');
    if (header[1] != 0U &&
        !readExact(pipe, payload.data(), header[1], stopToken)) {
        return false;
    }
    opcode = header[0];
    return true;
}

HANDLE connectDiscordPipe()
{
    wchar_t qaPipe[512]{};
    const auto qaLength = qaHooksAllowed()
        ? GetEnvironmentVariableW(L"NEKOROX_QA_DISCORD_PIPE", qaPipe,
            static_cast<DWORD>(std::size(qaPipe)))
        : 0U;
    if (qaLength != 0U && qaLength < std::size(qaPipe)) {
        std::wstring path(qaPipe, qaLength);
        if (!path.starts_with(L"\\\\")) {
            path = L"\\\\.\\pipe\\" + path;
        }
        // A QA override is intentionally exclusive: never fall through to a
        // real Discord client when a controlled test endpoint was requested.
        return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    for (unsigned index = 0; index != 10U; ++index) {
        const auto path = L"\\\\?\\pipe\\discord-ipc-" +
            std::to_wstring(index);
        const auto handle = CreateFileW(path.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) return handle;
    }
    return INVALID_HANDLE_VALUE;
}

} // namespace

DiscordPresenceService::DiscordPresenceService()
{
    pending_.playback.sizeBytes = sizeof(PlaybackSnapshotV2);
    coverService_.setRelayStateChangedCallback([this] { changed_.notify_all(); });
}

DiscordPresenceService::~DiscordPresenceService()
{
    stop();
    if (drainWork_) {
        const auto work = static_cast<PTP_WORK>(drainWork_);
        WaitForThreadpoolWorkCallbacks(work, FALSE);
        CloseThreadpoolWork(work);
    }
}

DiscordPresenceService& DiscordPresenceService::instance()
{
    static DiscordPresenceService value;
    return value;
}

void DiscordPresenceService::start()
{
    try {
        const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
        const auto choicePath = std::filesystem::path(utf8ToWide(native.c_str())) /
            L"nekorox" / L"cover-provider.install-choice";
        const auto mayImport = !static_cast<bool>(g_coverInstallChoiceConsumed) &&
            static_cast<unsigned>(g_coverProvider) == 0U;
        // The marker is a one-time installer input, never a later source of
        // consent. Persist consumption even if removing a leftover file fails.
        g_coverInstallChoiceConsumed = true;
        if (mayImport &&
                std::filesystem::exists(choicePath) && std::filesystem::file_size(choicePath) <= 64U) {
            std::ifstream input(choicePath);
            std::string choice; std::getline(input, choice);
            if (!choice.empty() && choice.back() == '\r') choice.pop_back();
            if (choice == "catbox-accepted" || choice == "catbox-disabled") {
                g_coverProvider = 1U;
                g_coverDisclosure = choice == "catbox-accepted";
            }
        }
        std::error_code ignored; std::filesystem::remove(choicePath, ignored);
    } catch (...) { /* Invalid/missing installer choice never grants consent. */ }
    loadPresenceCoverConfiguration();
    {
        std::lock_guard lock(mutex_);
        if (started_) return;
        started_ = true;
        ++sessionGeneration_;
        ++coverEpoch_;
        pending_ = {};
        pending_.playback.sizeBytes = sizeof(PlaybackSnapshotV2);
    }
    refreshExternalPublisherConflict();
    if (static_cast<bool>(g_presenceEnabled) &&
            rolloutFlagEnabled(RolloutFlag::presenceRevisionV2)) {
        publishCurrent(true);
        launchWorker();
    } else if (!rolloutFlagEnabled(RolloutFlag::presenceRevisionV2)) {
        setStatus(DiscordPresenceStatus::disabled);
    }
}

void DiscordPresenceService::stop()
{
    if (static_cast<bool>(g_presenceEnabled)) enqueuePrivacyClear();
    {
        std::lock_guard lock(mutex_);
        if (!started_) return;
        started_ = false;
        ++sessionGeneration_;
        ++coverEpoch_;
    }
    coverService_.disableRelay();
    coverService_.cancel();
    stopWorker();
    setStatus(DiscordPresenceStatus::disabled);
}

void DiscordPresenceService::publishCurrent(bool force)
{
    if (!static_cast<bool>(g_presenceEnabled) ||
            !rolloutFlagEnabled(RolloutFlag::presenceRevisionV2)) return;
    auto captured = captureCurrent();
    const auto showPaused = static_cast<bool>(g_presenceShowPaused);
    const auto temporaryCovers =
        static_cast<bool>(g_presenceTemporaryCovers);
    {
        std::lock_guard lock(mutex_);
        if (!started_) return;
        const auto trackKey = captured.playback.trackIdentityHash;
        const auto allowCover = temporaryCovers &&
            presenceCoverConfiguration()->allowsCovers() && trackKey != 0 &&
            captured.trackHandle.is_valid() &&
            captured.playback.state != PlaybackState::stopped &&
            !(captured.playback.state == PlaybackState::paused &&
                !showPaused);
        // ThemeService deliberately moves through unresolved and resolved
        // artwork identities while reconstructing a cover. Those palette
        // lifecycle revisions are not new Discord cover requests. Tying the
        // epoch to them cancelled and restarted the same HTTP upload up to
        // three times on an ordinary track transition.
        if (trackKey != lastCoverTrackKey_ ||
                allowCover != lastCoverPolicy_) {
            ++coverEpoch_;
            lastCoverTrackKey_ = trackKey;
            lastCoverPolicy_ = allowCover;
            // Cancel the old request before the new identity becomes visible
            // to the worker. Publishing first left a narrow race where an
            // unqualified cancel could abort the new track instead.
            coverService_.cancel();
        } else if (allowCover && pending_.update.coverEpoch == coverEpoch_) {
            captured.coverUrl = pending_.coverUrl;
            captured.coverToken = pending_.coverToken;
            captured.relayExpiresAtMilliseconds=pending_.relayExpiresAtMilliseconds;
            captured.relayConnectionGeneration=pending_.relayConnectionGeneration;
        }

        ++pendingRevision_;
        captured.update.sessionGeneration = sessionGeneration_;
        captured.update.presenceRevision = pendingRevision_;
        captured.update.coverEpoch = coverEpoch_;
        captured.update.trackKey = trackKey;
        // The resolver validates the decoded artwork by its actual content
        // hash. A UI palette generation is intentionally not a request key.
        captured.update.artContentGeneration = 0;
        captured.update.seekGeneration =
            Runtime::instance().playbackPositionSnapshot().seekGeneration;
        captured.update.state = captured.playback.state;
        captured.update.enabled = true;
        captured.update.started = started_;
        captured.update.showPaused = showPaused;
        captured.update.temporaryCovers = temporaryCovers;
        captured.update.allowCover = allowCover;
        captured.update.urgentClear =
            captured.playback.state == PlaybackState::paused && !showPaused;
        if (force && captured.playback.state == PlaybackState::stopped) {
            captured.update.urgentClear = true;
        }
        pending_ = std::move(captured);
        pending_.update.presenceRevision = pendingRevision_;
        pending_.updateV3.base = pending_.update;
        pending_.updateV3.trackGeneration = coverEpoch_;
        pending_.updateV3.deliveryRevision = pendingRevision_;
        pending_.updateV3.coverToken = pending_.coverToken;
        if (pending_.update.trackKey != deliveryTrace_.trackGeneration) {
            deliveryTrace_ = {};
            deliveryTrace_.trackGeneration = pending_.update.trackKey;
            deliveryTrace_.activityCreatedMilliseconds = presenceMilliseconds();
        }
        qaPresenceLog("publish revision=" + std::to_string(pendingRevision_) +
            " session=" + std::to_string(sessionGeneration_) +
            " epoch=" + std::to_string(coverEpoch_) +
            " state=" + std::to_string(static_cast<unsigned>(
                pending_.playback.state)) +
            " cover=" + std::to_string(!pending_.coverUrl.empty()));
    }
    changed_.notify_all();
}

void DiscordPresenceService::publishDawPreview(
    DiscordDawPresence presence)
{
    presence.positionSeconds = std::max(0.0, presence.positionSeconds);
    presence.durationSeconds = std::max(
        presence.positionSeconds, presence.durationSeconds);
    presence.focus = bounded(trimAscii(std::move(presence.focus)));
    {
        std::lock_guard lock(mutex_);
        dawPresence_ = std::move(presence);
    }
    publishCurrent(true);
}

void DiscordPresenceService::clearDawPreview()
{
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = dawPresence_.active;
        dawPresence_ = {};
    }
    if (changed) publishCurrent(true);
}

DiscordPresenceSettings DiscordPresenceService::settings() const
{
    return {
        static_cast<bool>(g_presenceEnabled),
        static_cast<bool>(g_presenceShowPaused),
        static_cast<bool>(g_presenceTemporaryCovers),
        configuredApplicationId(),
        presenceCoverConfiguration()->provider,
        presenceCoverConfiguration()->relayEndpoint,
        !g_relayCredential.get().is_empty(),
        presenceCoverConfiguration()->disclosureAccepted,
    };
}

void DiscordPresenceService::setEnabled(bool enabled)
{
    if (enabled) {
        if (static_cast<bool>(g_presenceEnabled)) return;
        refreshExternalPublisherConflict();
        {
            std::lock_guard lock(mutex_);
            g_presenceEnabled = true;
            ++sessionGeneration_;
            ++coverEpoch_;
            lastCoverTrackKey_ = 0;
            lastCoverPolicy_ = false;
        }
        publishCurrent(true);
        launchWorker();
    } else {
        if (!static_cast<bool>(g_presenceEnabled)) return;
        {
            std::lock_guard lock(mutex_);
            g_presenceEnabled = false;
            coverService_.disableRelay();
        }
        setStatus(DiscordPresenceStatus::disabled);
        coverStatus_.store(DiscordCoverStatus::disabled,
            std::memory_order_release);
        enqueuePrivacyClear();
        std::uint64_t clearRevision = 0;
        {
            std::lock_guard lock(mutex_);
            clearRevision = pendingRevision_;
            ++sessionGeneration_;
            ++coverEpoch_;
            pending_.coverUrl.clear();
            pending_.coverToken.clear();
        }
        coverService_.cancel();
        stopWorkerAsync(clearRevision);
    }
}

void DiscordPresenceService::setShowPaused(bool showPaused)
{
    g_presenceShowPaused = showPaused;
    publishCurrent(true);
}

void DiscordPresenceService::setTemporaryCovers(bool enabled)
{
    {
        std::lock_guard lock(mutex_);
        g_presenceTemporaryCovers = enabled;
        if (!enabled) {
            coverService_.disableRelay();
            coverService_.cancel();
            ++coverEpoch_;
            pending_.coverUrl.clear();
            pending_.coverToken.clear();
            pending_.update.coverEpoch = coverEpoch_;
            pending_.update.allowCover = false;
        }
    }
    if (!enabled) {
        coverStatus_.store(DiscordCoverStatus::disabled,
            std::memory_order_release);
    } else if (static_cast<bool>(g_presenceEnabled)) {
        launchWorker();
    }
    publishCurrent(true);
    if (!enabled) {
        std::lock_guard lock(mutex_);
        // Policy clear must be its own revision. Otherwise the sender may
        // already hold the non-urgent copy created by publishCurrent() and
        // leave the old URL visible behind the normal 4.1 s throttle.
        ++pendingRevision_;
        pending_.update.presenceRevision = pendingRevision_;
        pending_.update.urgentClear = true;
    }
    changed_.notify_all();
}

bool DiscordPresenceService::setCoverConfiguration(PresenceCoverProvider provider,
    std::string endpoint, std::string credential, bool disclosureAccepted)
{
    if (provider != PresenceCoverProvider::unconfigured &&
            provider != PresenceCoverProvider::catbox &&
            provider != PresenceCoverProvider::privateRelay) return false;
    if (provider == PresenceCoverProvider::unconfigured) disclosureAccepted = false;
    endpoint = trimAscii(std::move(endpoint));
    while (!endpoint.empty() && endpoint.back()=='/') endpoint.pop_back();
    std::string protectedCredential = g_relayCredential.get().c_str();
    if (provider == PresenceCoverProvider::privateRelay) {
        if (!DiscordRelayClient::validEndpoint(endpoint)) return false;
        if (!credential.empty()) {
            if (credential.size()<32U || credential.size()>256U ||
                    !std::all_of(credential.begin(),credential.end(),[](unsigned char c) {
                        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                            (c>='0'&&c<='9')||c=='-'||c=='_';
                    })) return false;
            protectedCredential=protectRelayCredential(credential);
            SecureZeroMemory(credential.data(),credential.size());
        }
        if (protectedCredential.empty() ||
                unprotectRelayCredential(protectedCredential).empty()) return false;
    }
    {
        std::lock_guard lock(mutex_);
        coverService_.disableRelay();
        coverService_.cancel();
        ++coverEpoch_;
        pending_.coverUrl.clear(); pending_.coverToken.clear();
        pending_.update.coverEpoch=coverEpoch_;
        pending_.update.urgentClear=true;
        g_coverProvider=static_cast<unsigned>(provider);
        g_coverDisclosure=disclosureAccepted;
        g_coverInstallChoiceConsumed=true;
        g_relayEndpoint=endpoint.c_str();
        g_relayCredential=protectedCredential.c_str();
        loadPresenceCoverConfiguration();
        ++pendingRevision_;
    }
    publishCurrent(true);
    if (static_cast<bool>(g_presenceEnabled)) launchWorker();
    {
        std::lock_guard lock(mutex_);
        ++pendingRevision_;
        pending_.update.presenceRevision = pendingRevision_;
        pending_.update.urgentClear = true;
    }
    changed_.notify_all();
    return true;
}

bool DiscordPresenceService::setApplicationId(std::string applicationId)
{
    applicationId = trimAscii(std::move(applicationId));
    if (!validApplicationId(applicationId)) return false;
    g_presenceApplicationId = applicationId.c_str();
    if (static_cast<bool>(g_presenceEnabled)) {
        {
            std::lock_guard lock(mutex_);
            ++sessionGeneration_;
            ++coverEpoch_;
            pending_.coverUrl.clear();
            pending_.coverToken.clear();
        }
        coverService_.cancel();
        stopWorkerAsync(0);
        publishCurrent(true);
        launchWorker();
    }
    return true;
}

DiscordPresenceStatus DiscordPresenceService::status() const noexcept
{
    if (!static_cast<bool>(g_presenceEnabled))
        return DiscordPresenceStatus::disabled;
    return status_.load(std::memory_order_acquire);
}

DiscordCoverStatus DiscordPresenceService::coverStatus() const noexcept
{
    if (!static_cast<bool>(g_presenceEnabled))
        return DiscordCoverStatus::disabled;
    return coverStatus_.load(std::memory_order_acquire);
}

bool DiscordPresenceService::externalPublisherConflict() const noexcept
{
    return externalPublisherConflict_.load(std::memory_order_acquire);
}

std::wstring DiscordPresenceService::statusText() const
{
    if (static_cast<bool>(g_presenceEnabled) &&
            externalPublisherConflict()) {
        return L"Conflict \u00b7 disable foobar2000 inside Music Presence";
    }
    switch (status()) {
    case DiscordPresenceStatus::disabled:
        return L"Off \u00b7 no background work";
    case DiscordPresenceStatus::connecting:
        return L"Connecting to Discord\u2026";
    case DiscordPresenceStatus::connected:
        if (rolloutFlagEnabled(RolloutFlag::presenceV3) &&
                rolloutFlagEnabled(RolloutFlag::presenceEphemeralRelayV1) &&
                DiscordRelayClient::configured()) {
            const auto relay = coverService_.relayTrace();
            if (relay.delivery == DiscordRelayDelivery::exact) {
                return L"Cover delivered";
            }
            if (relay.delivery == DiscordRelayDelivery::fallback) {
                return L"Fallback";
            }
            if (relay.status == DiscordRelayStatus::degraded) {
                return L"Connected \u00b7 cover relay unavailable";
            }
        }
        switch (coverStatus()) {
        case DiscordCoverStatus::resolving:
            return L"Connected \u00b7 preparing cover\u2026";
        case DiscordCoverStatus::ready:
            return L"Connected";
        case DiscordCoverStatus::fallback:
            return L"Connected \u00b7 cover fallback active";
        case DiscordCoverStatus::disabled:
            return L"Connected \u00b7 cover uploads off";
        case DiscordCoverStatus::idle:
            break;
        }
        return L"Connected";
    case DiscordPresenceStatus::discordUnavailable: return L"Discord desktop is not running";
    case DiscordPresenceStatus::applicationRejected: return L"Discord rejected the application ID";
    }
    return L"Unavailable";
}

PresenceDeliveryTraceV3 DiscordPresenceService::deliveryTrace() const
{
    auto result = [&] {
        std::lock_guard lock(mutex_);
        return deliveryTrace_;
    }();
    const auto relay = coverService_.relayTrace();
    result.delivery = relay.delivery;
    result.coverToken = relay.assetId;
    result.coverDeliveredMilliseconds = relay.deliveredAtMilliseconds;
    if (relay.deliveredAtMilliseconds >= relay.deliveryMilliseconds) {
        result.mediaRequestedMilliseconds =
            relay.deliveredAtMilliseconds - relay.deliveryMilliseconds;
    }
    if (!relay.error.empty()) result.error = relay.error;
    return result;
}

void DiscordPresenceService::enqueuePrivacyClear()
{
    std::uint64_t clearRevision = 0;
    bool waitForSend = false;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || !worker_.joinable()) return;
        ++coverEpoch_;
        ++pendingRevision_;
        pending_ = {};
        pending_.playback.sizeBytes = sizeof(PlaybackSnapshotV2);
        pending_.playback.state = PlaybackState::stopped;
        pending_.update.sessionGeneration = sessionGeneration_;
        pending_.update.presenceRevision = pendingRevision_;
        pending_.update.coverEpoch = coverEpoch_;
        pending_.update.state = PlaybackState::stopped;
        pending_.update.started = true;
        pending_.update.urgentClear = true;
        clearRevision = pendingRevision_;
        waitForSend = status() == DiscordPresenceStatus::connected;
    }
    coverService_.cancel();
    changed_.notify_all();
    if (!waitForSend) return;
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, 475ms, [&] {
        return sentRevision_ >= clearRevision ||
            status() != DiscordPresenceStatus::connected;
    });
}

DiscordTrackPresence DiscordPresenceService::captureCurrent() const
{
    DiscordTrackPresence result;
    DiscordDawPresence daw;
    {
        std::lock_guard lock(mutex_);
        daw = dawPresence_;
    }
    if (daw.active) {
        result.dawMode = true;
        result.dawTrackCount = daw.trackCount;
        result.playback.sizeBytes = sizeof(PlaybackSnapshotV2);
        result.playback.state = daw.state;
        result.playback.positionSeconds = daw.positionSeconds;
        result.playback.durationSeconds = daw.durationSeconds;
        copyPresenceText(result.playback.title, "DAW Mode");
        copyPresenceText(result.playback.artist,
            daw.focus.empty() ? std::string_view{"Mix preview"}
                              : std::string_view{daw.focus});
        copyPresenceText(result.playback.album, "GODENplayer DAW");
        result.codec = "48 kHz project";
        result.sampleRate = "48000 Hz";
        return result;
    }
    result.playback = Runtime::instance().playbackSnapshot();
    if (result.playback.state == PlaybackState::stopped) return result;
    result.trackHandle = Runtime::instance().currentTrackHandle();
    const auto control = playback_control::get();
    metadb_handle_ptr backend;
    const auto backendMatches = control->get_now_playing(backend) &&
        backend == result.trackHandle;
    // New-track callbacks may precede the backend handle transition. Keep
    // every field attached to Runtime's authoritative track; once it matches,
    // retain dynamic stream titles and technical info from the playback API.
    const auto usePlaybackMetadata = backendMatches && control->is_playing();
    const auto readMetadata = [&](const char* expression) {
        return metadata(result.trackHandle, *control.get_ptr(),
            usePlaybackMetadata, expression);
    };
    const auto taggedArtist = readMetadata("[%artist%]");
    const auto missingArtist = taggedArtist.empty();
    auto localPath = true;
    if (result.trackHandle.is_valid()) {
        const auto path = std::string_view(result.trackHandle->get_path());
        localPath = !(path.starts_with("http://") ||
            path.starts_with("https://") || path.starts_with("mms://"));
    }
    result.finiteLocalFile = result.trackHandle.is_valid() && localPath;

    // Some decoders publish the new-track callback before playback_get_length
    // has settled. Discord only renders its complete media card/progress bar
    // when both timestamp endpoints are present, so enrich the semantic
    // snapshot from both authoritative duration sources before serializing it.
    // A later metadata callback is also prevented from downgrading a duration
    // that was already known.
    if (result.finiteLocalFile && !hasCompletePlaybackRange(result.playback)) {
        const auto handleDuration = result.trackHandle->get_length();
        if (std::isfinite(handleDuration) && handleDuration > 0.0) {
            result.playback.durationSeconds = std::max(
                result.playback.durationSeconds, handleDuration);
        }
        if (backendMatches) {
            const auto backendDuration = control->playback_get_length();
            if (std::isfinite(backendDuration) && backendDuration > 0.0) {
                result.playback.durationSeconds = std::max(
                    result.playback.durationSeconds, backendDuration);
            }
        }
    }
    result.localFileFallback = result.trackHandle.is_valid() && localPath &&
        missingArtist;
    if (result.localFileFallback) {
        result.fileName = readMetadata(
            "$filename(%path%)$if($ext(%path%),.$ext(%path%))");
        if (result.fileName.empty()) {
            result.fileName = bounded(trimAscii(result.playback.title.data()));
        }
    }
    // Untagged files are still real playback. Keep their activity stable and
    // useful instead of depending on album-oriented metadata being present.
    if (missingPresenceText(result.playback.title.data())) {
        auto fallback = readMetadata("$if2(%title%,$filename(%path%))");
        if (fallback.empty()) fallback = "Unknown track";
        copyPresenceText(result.playback.title, fallback);
    }
    if (missingArtist && !result.localFileFallback) {
        copyPresenceText(result.playback.artist, "Unknown artist");
    }
    result.codec = readMetadata("[%codec%][$if(%codec_profile%, / %codec_profile%,)]");
    result.bitrate = readMetadata("[$if(%bitrate%,%bitrate% kbps,)]");
    result.sampleRate = readMetadata("[$if(%samplerate%,%samplerate% Hz,)]");
    result.bitDepth = readMetadata("[$if(%bitspersample%,%bitspersample%-bit,)]");
    result.channels = readMetadata("[%channels%]");
    result.albumArtist = readMetadata("[%album artist%]");
    result.musicBrainzRecordingId = readMetadata(
        "[$lower($if3($meta(MUSICBRAINZ_TRACKID),$meta(MUSICBRAINZ TRACK ID)))]");
    result.musicBrainzReleaseId = readMetadata(
        "[$lower($if3($meta(MUSICBRAINZ_ALBUMID),$meta(MUSICBRAINZ ALBUM ID)))]");
    return result;
}

void DiscordPresenceService::launchWorker()
{
    if (!rolloutFlagEnabled(RolloutFlag::presenceRevisionV2)) {
        setStatus(DiscordPresenceStatus::disabled);
        return;
    }
    std::lock_guard lock(mutex_);
    if (!started_ || drainPending_ || !static_cast<bool>(g_presenceEnabled)) return;
    if (!drainWork_) {
        drainWork_ = CreateThreadpoolWork(
            [](PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
                static_cast<DiscordPresenceService*>(context)->drainWorkers();
            }, this, nullptr);
        if (!drainWork_) {
            setStatus(DiscordPresenceStatus::discordUnavailable);
            return;
        }
    }
    launchWorkersLocked();
}

void DiscordPresenceService::launchWorkersLocked() noexcept
{
    try {
        if (!worker_.joinable()) {
            worker_ = std::jthread([this](std::stop_token token) { workerMain(token); });
        }
        if (static_cast<bool>(g_presenceTemporaryCovers)) {
            coverService_.startRelay();
            if (!coverWorker_.joinable()) {
                coverWorker_ = std::jthread([this](std::stop_token token) { coverWorkerMain(token); });
            }
        }
    } catch (...) {
        // Keep any successfully started RPC worker; a failed cover worker
        // must not leave a relay running without a consumer.
        if (!coverWorker_.joinable()) coverService_.disableRelay();
        coverStatus_.store(DiscordCoverStatus::fallback, std::memory_order_release);
        if (!worker_.joinable()) setStatus(DiscordPresenceStatus::discordUnavailable);
    }
}

void DiscordPresenceService::stopWorker()
{
    std::jthread worker;
    std::jthread coverWorker;
    PTP_WORK drainWork = nullptr;
    {
        std::unique_lock lock(mutex_);
        drainChanged_.wait(lock, [this] { return !drainPending_; });
        worker = std::move(worker_);
        coverWorker = std::move(coverWorker_);
        drainWork = static_cast<PTP_WORK>(drainWork_);
    }
    worker.request_stop();
    coverWorker.request_stop();
    // Invalidates and cancels the request immediately. The asynchronous
    // upload keeps its callback state alive through HANDLE_CLOSING.
    coverService_.cancel();
    coverService_.disableRelay();
    rpcReady_.store(false, std::memory_order_release);
    changed_.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    if (coverWorker.joinable()) {
        coverWorker.join();
    }
    // drainPending_ ends model ownership; the work callback can still be
    // returning. Final shutdown must also cover that last DLL/context use.
    if (drainWork) WaitForThreadpoolWorkCallbacks(drainWork, FALSE);
    coverService_.clear();
    coverStatus_.store(DiscordCoverStatus::disabled,
        std::memory_order_release);
}

void DiscordPresenceService::stopWorkerAsync(std::uint64_t clearRevision)
{
    std::stop_source cancelCover(std::nostopstate);
    PTP_WORK work = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (drainPending_) return;
        // Failure to reserve the work object prevented any worker admission.
        if (!drainWork_) return;
        drainPending_ = true;
        drainingRpc_ = std::move(worker_);
        drainingCover_ = std::move(coverWorker_);
        drainingClearRevision_ = rpcReady_.load(std::memory_order_acquire) ? clearRevision : 0;
        cancelCover = drainingCover_.get_stop_source();
        work = static_cast<PTP_WORK>(drainWork_);
    }
    cancelCover.request_stop();
    coverService_.disableRelay();
    coverService_.cancel();
    rpcReady_.store(false, std::memory_order_release);
    changed_.notify_all();
    // A preallocated Windows work object has no allocation/enqueue failure
    // path here, unlike the old bounded shared-reaper queue.
    SubmitThreadpoolWork(work);
}

void DiscordPresenceService::drainWorkers() noexcept
{
    std::jthread rpc;
    std::jthread cover;
    std::uint64_t clearRevision = 0;
    {
        std::lock_guard lock(mutex_);
        rpc = std::move(drainingRpc_);
        cover = std::move(drainingCover_);
        clearRevision = drainingClearRevision_;
    }
    if (rpc.joinable() && clearRevision != 0) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 475ms, [&] {
            return sentRevision_ >= clearRevision ||
                static_cast<bool>(g_presenceEnabled);
        });
    }
    rpc.request_stop();
    changed_.notify_all();
    if (rpc.joinable()) rpc.join();
    if (cover.joinable()) cover.join();
    coverService_.clear();
    {
        std::lock_guard lock(mutex_);
        if (started_ && static_cast<bool>(g_presenceEnabled) &&
                rolloutFlagEnabled(RolloutFlag::presenceRevisionV2)) {
            launchWorkersLocked();
        } else {
            status_.store(DiscordPresenceStatus::disabled, std::memory_order_release);
            coverStatus_.store(DiscordCoverStatus::disabled, std::memory_order_release);
        }
        drainPending_ = false;
        // A later opt-out may immediately reuse the slot. No member/slot
        // access after this notification and final unlock.
        drainChanged_.notify_all();
    }
}

void DiscordPresenceService::coverWorkerMain(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    struct CoverKey {
        std::uint64_t session = 0;
        std::uint64_t epoch = 0;
        std::uint64_t track = 0;
        bool operator==(const CoverKey&) const noexcept = default;
    };
    const auto keyFor = [](const DiscordTrackPresence& value) {
        return CoverKey{value.update.sessionGeneration,
            value.update.coverEpoch, value.update.trackKey};
    };

    std::uint64_t processedRevision = 0;
    std::optional<CoverKey> attemptedKey;
    unsigned attemptCount = 0;
    std::uint64_t observedRelayGeneration = 0;
    bool relayRecoveryPending = false;
    auto retryAt = std::chrono::steady_clock::time_point::min();
    while (!stopToken.stop_requested()) {
        DiscordTrackPresence track;
        std::uint64_t revision = 0;
        bool renewing = false;
        std::uint64_t coverAdmission = 0;
        {
            std::unique_lock lock(mutex_);
            const auto renewalDue=[&] {
                if(!pending_.update.allowCover || pending_.coverToken.empty() ||
                        !ephemeralRelayEnabled()) return false;
                const auto relay=coverService_.relayTrace();
                return (relay.status==DiscordRelayStatus::connected &&
                        relay.connectionGeneration!=pending_.relayConnectionGeneration) ||
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count())+15000U>=static_cast<std::uint64_t>(
                        (std::max)(std::int64_t{0},pending_.relayExpiresAtMilliseconds));
            };
            const auto workReady = [&] {
                const auto relay = coverService_.relayTrace();
                if (relay.status == DiscordRelayStatus::connected &&
                        relay.connectionGeneration != observedRelayGeneration) {
                    observedRelayGeneration = relay.connectionGeneration;
                    attemptCount = 0;
                    retryAt = std::chrono::steady_clock::time_point::min();
                    // A connected episode can recover a registration that
                    // failed before the relay was ready. No metadata revision
                    // or previous token is required to retry that same cover.
                    relayRecoveryPending = pending_.update.allowCover &&
                        ephemeralRelayEnabled() && pending_.coverToken.empty();
                }
                if (relayRecoveryPending && pending_.update.allowCover &&
                        ephemeralRelayEnabled()) return true;
                if (renewalDue() && attemptCount < 4U &&
                        (retryAt == std::chrono::steady_clock::time_point::min() ||
                         std::chrono::steady_clock::now() >= retryAt)) return true;
                if (!static_cast<bool>(g_presenceTemporaryCovers) ||
                        !static_cast<bool>(g_presenceEnabled)) return true;
                if (pendingRevision_ != processedRevision) {
                    // Consume clears/disabled states immediately, but never
                    // decode or upload artwork before Discord completed READY.
                    return !pending_.update.allowCover ||
                        !pending_.coverUrl.empty() ||
                        ephemeralRelayEnabled() ||
                        rpcReady_.load(std::memory_order_acquire);
                }
                return attemptedKey && attemptCount != 0 &&
                    retryAt != std::chrono::steady_clock::time_point::min() &&
                    pending_.update.allowCover &&
                    (ephemeralRelayEnabled() ||
                        rpcReady_.load(std::memory_order_acquire)) &&
                    keyFor(pending_) == *attemptedKey &&
                    std::chrono::steady_clock::now() >= retryAt;
            };
            while (!stopToken.stop_requested() && !workReady()) {
                const auto scheduled = attemptedKey && attemptCount != 0 &&
                    retryAt != std::chrono::steady_clock::time_point::min() &&
                    pending_.update.allowCover &&
                    (ephemeralRelayEnabled() || rpcReady_.load(std::memory_order_acquire)) &&
                    keyFor(pending_) == *attemptedKey;
                if (scheduled) {
                    changed_.wait_until(lock, stopToken, retryAt, workReady);
                } else {
                    if(attemptCount < 4U && !pending_.coverToken.empty() && ephemeralRelayEnabled()) {
                        const auto deadline=std::chrono::system_clock::time_point(
                            std::chrono::milliseconds(pending_.relayExpiresAtMilliseconds-15000));
                        changed_.wait_until(lock,stopToken,deadline,workReady);
                    } else {
                    changed_.wait(lock, stopToken, workReady);
                    }
                }
            }
            if (stopToken.stop_requested()) break;
            if (!static_cast<bool>(g_presenceTemporaryCovers) ||
                    !static_cast<bool>(g_presenceEnabled)) {
                coverStatus_.store(DiscordCoverStatus::disabled,
                    std::memory_order_release);
                attemptedKey.reset();
                attemptCount = 0;
                retryAt = std::chrono::steady_clock::time_point::min();
                changed_.wait(lock, stopToken, [&] {
                    return static_cast<bool>(g_presenceTemporaryCovers) &&
                        static_cast<bool>(g_presenceEnabled);
                });
                continue;
            }
            renewing = renewalDue() && attemptCount < 4U;
            revision = pendingRevision_;
            processedRevision = revision;
            relayRecoveryPending = false;
            track = pending_;
            coverAdmission = coverService_.admissionTicket();
            // Keep a still-valid published URL until a replacement is ready.
            if (renewing) { track.coverUrl.clear(); track.coverToken.clear(); }
        }

        if (!track.update.allowCover || !track.trackHandle.is_valid()) {
            coverStatus_.store(DiscordCoverStatus::idle,
                std::memory_order_release);
            attemptedKey.reset();
            attemptCount = 0;
            retryAt = std::chrono::steady_clock::time_point::min();
            continue;
        }
        const auto requestKey = keyFor(track);
        // A metadata revision or retained renewal URL does not grant another
        // network budget. Only a new request or relay connection does.
        if (attemptedKey && requestKey == *attemptedKey && attemptCount >= 4U) continue;
        if (!track.coverUrl.empty()) {
            coverStatus_.store(DiscordCoverStatus::ready,
                std::memory_order_release);
            attemptedKey.reset();
            attemptCount = 0;
            retryAt = std::chrono::steady_clock::time_point::min();
            continue;
        }

        if (!attemptedKey || requestKey != *attemptedKey) {
            attemptedKey = requestKey;
            attemptCount = 0;
            retryAt = std::chrono::steady_clock::time_point::min();
            // Collapse rapid Next/Previous bursts before touching album art or
            // the network. Metadata is published by the independent RPC lane.
            std::unique_lock lock(mutex_);
            const auto superseded = changed_.wait_for(
                lock, stopToken, 120ms, [&] {
                    return keyFor(pending_) != requestKey ||
                        !pending_.update.allowCover ||
                        (!renewing && !pending_.coverUrl.empty());
                });
            if (stopToken.stop_requested()) break;
            if (superseded) continue;
        } else if (attemptCount != 0 &&
                retryAt > std::chrono::steady_clock::now()) {
            // A seek or metadata refinement changed only the Presence
            // revision. Preserve this cover request's backoff instead of
            // restarting a slow HTTP operation from zero.
            continue;
        }

        ++attemptCount;
        coverStatus_.store(DiscordCoverStatus::resolving,
            std::memory_order_release);
        qaPresenceLog("cover resolve revision=" + std::to_string(revision) +
            " session=" + std::to_string(track.update.sessionGeneration) +
            " epoch=" + std::to_string(track.update.coverEpoch) +
            " attempt=" + std::to_string(attemptCount));
        auto result = coverService_.resolve(
            track.trackHandle, track.update.trackKey, stopToken, coverAdmission);
        if (stopToken.stop_requested()) break;
        if (!result.succeeded()) {
            qaPresenceLog("cover fallback revision=" +
                std::to_string(revision) + " attempt=" +
                std::to_string(attemptCount) + " error=" + result.error +
                " art_ms=" + std::to_string(result.artworkMilliseconds) +
                " encode_ms=" + std::to_string(result.encodeMilliseconds) +
                " upload_ms=" + std::to_string(result.uploadMilliseconds));
            bool stillCurrent = false;
            {
                std::lock_guard lock(mutex_);
                stillCurrent = pending_.update.sessionGeneration ==
                        track.update.sessionGeneration &&
                    pending_.update.coverEpoch == track.update.coverEpoch &&
                    pending_.update.trackKey == track.update.trackKey &&
                    pending_.update.allowCover &&
                    static_cast<bool>(g_presenceEnabled) &&
                    static_cast<bool>(g_presenceTemporaryCovers);
            }
            coverStatus_.store(stillCurrent
                    ? DiscordCoverStatus::fallback
                    : DiscordCoverStatus::idle,
                std::memory_order_release);
            if (!stillCurrent) staleCoverDiscards_.fetch_add(
                1, std::memory_order_relaxed);
            if (stillCurrent && attemptCount < 4U) {
                const auto delay = attemptCount == 1U ? 1s :
                    attemptCount == 2U ? 5s : 30s;
                retryAt = std::chrono::steady_clock::now() + delay;
            } else if (!stillCurrent) {
                attemptedKey.reset();
                attemptCount = 0;
                retryAt = std::chrono::steady_clock::time_point::min();
            } else {
                retryAt = std::chrono::steady_clock::time_point::min();
            }
            continue;
        }

        bool accepted = false;
        std::uint64_t acceptedRevision = 0;
        {
            std::lock_guard lock(mutex_);
            const auto sameRequest =
                pending_.update.sessionGeneration ==
                    track.update.sessionGeneration &&
                pending_.update.coverEpoch == track.update.coverEpoch &&
                pending_.update.trackKey == track.update.trackKey &&
                pending_.update.enabled && pending_.update.started &&
                pending_.update.allowCover &&
                (ephemeralRelayEnabled() ||
                    rpcReady_.load(std::memory_order_acquire)) &&
                static_cast<bool>(g_presenceEnabled) &&
                static_cast<bool>(g_presenceTemporaryCovers);
            if (sameRequest) {
                pending_.coverUrl = result.url;
                pending_.coverToken = result.token;
                pending_.relayExpiresAtMilliseconds=result.expiresAtMilliseconds;
                pending_.relayConnectionGeneration=result.connectionGeneration;
                ++pendingRevision_;
                pending_.update.presenceRevision = pendingRevision_;
                pending_.updateV3.base = pending_.update;
                pending_.updateV3.trackGeneration = pending_.update.coverEpoch;
                pending_.updateV3.deliveryRevision = pendingRevision_;
                pending_.updateV3.coverToken = result.token;
                processedRevision = pendingRevision_;
                acceptedRevision = pendingRevision_;
                accepted = true;
            }
        }
        if (!accepted) staleCoverDiscards_.fetch_add(
            1, std::memory_order_relaxed);
        coverStatus_.store(accepted
                ? DiscordCoverStatus::ready
                : DiscordCoverStatus::idle,
            std::memory_order_release);
        qaPresenceLog("cover result revision=" + std::to_string(revision) +
            " accepted=" + std::to_string(accepted) +
            " pending=" + std::to_string(acceptedRevision) +
            " source=" + std::to_string(static_cast<unsigned>(result.source)) +
            " art_ms=" + std::to_string(result.artworkMilliseconds) +
            " encode_ms=" + std::to_string(result.encodeMilliseconds) +
            " upload_ms=" + std::to_string(result.uploadMilliseconds) +
            " verify_ms=" +
                std::to_string(result.verificationMilliseconds));
        attemptedKey.reset();
        attemptCount = 0;
        retryAt = std::chrono::steady_clock::time_point::min();
        if (accepted) changed_.notify_all();
    }
}

void DiscordPresenceService::workerMain(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto retryDelay = 1s;
    auto lastSend = std::chrono::steady_clock::time_point::min();
    // Discord permits five activity updates per rolling 20 seconds. Ordinary
    // playback updates use at most four slots. Privacy/stop clears are never
    // held behind cosmetic work; the history intentionally survives reconnects.
    struct RateEvent {
        std::chrono::steady_clock::time_point sentAt{};
        bool privacyClear = false;
        bool cosmetic = false;
    };
    std::deque<RateEvent> sendHistory;
    auto nextExternalPublisherScan =
        std::chrono::steady_clock::time_point::min();
    const auto connectionAllowed = [&] {
        std::lock_guard lock(mutex_);
        return !stopToken.stop_requested() && started_ &&
            static_cast<bool>(g_presenceEnabled) && !drainPending_;
    };
    while (!stopToken.stop_requested()) {
        // The existing connection may finish its privacy clear during drain,
        // but a failed clear must not reopen RPC after the user opted out.
        if (!connectionAllowed()) break;
        const auto scanNow = std::chrono::steady_clock::now();
        if (scanNow >= nextExternalPublisherScan) {
            refreshExternalPublisherConflict();
            nextExternalPublisherScan = scanNow + 5s;
        }
        rpcReady_.store(false, std::memory_order_release);
        setStatus(DiscordPresenceStatus::connecting);
        const auto pipe = connectDiscordPipe();
        // Opening a pipe can race an opt-out or shutdown. Do not handshake a
        // connection that was admitted under a policy that no longer applies.
        if (!connectionAllowed()) {
            if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
            break;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            setStatus(DiscordPresenceStatus::discordUnavailable);
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, stopToken, retryDelay, [] { return false; });
            retryDelay = std::min(10s, retryDelay * 2);
            continue;
        }

        const auto appId = configuredApplicationId();
        std::string handshake = "{\"v\":1,\"client_id\":";
        handshake += jsonQuoted(appId);
        handshake.push_back('}');
        std::uint32_t opcode = 0;
        std::string reply;
        const bool ready = writeFrame(pipe, kHandshakeOpcode, handshake,
                stopToken) &&
            readFrame(pipe, stopToken, opcode, reply) &&
            opcode != kCloseOpcode &&
            reply.find("\"evt\":\"READY\"") != std::string::npos;
        if (!ready) {
            CloseHandle(pipe);
            if (!stopToken.stop_requested()) {
                const auto rejected =
                    reply.find("\"evt\":\"ERROR\"") != std::string::npos ||
                    reply.find("\"code\":4000") != std::string::npos;
                setStatus(rejected
                    ? DiscordPresenceStatus::applicationRejected
                    : DiscordPresenceStatus::discordUnavailable);
                std::unique_lock lock(mutex_);
                const auto delay = rejected ? 5s : retryDelay;
                changed_.wait_for(lock, stopToken, delay, [] { return false; });
                if (!rejected) retryDelay = std::min(10s, retryDelay * 2);
            }
            continue;
        }

        retryDelay = 1s;
        // Record the completed handshake before publishing the gate. The
        // cover worker wakes on rpcReady_; logging afterwards allowed its
        // trace to overtake this causal event even though no upload began
        // before READY.
        qaPresenceLog("rpc ready");
        rpcReady_.store(true, std::memory_order_release);
        setStatus(DiscordPresenceStatus::connected);
        changed_.notify_all();
        // Every named-pipe connection starts with an empty Discord activity
        // namespace. sentRevision_ describes our process, not this new pipe,
        // so the current track must be replayed once after every handshake.
        bool initialActivityPending = true;
        std::optional<PlaybackState> sentPlaybackState;
        std::optional<std::uint64_t> sentTrackKey;
        std::string sentCoverUrl;
        double sentDurationSeconds = 0.0;
        std::uint64_t durationProbeTrackKey = 0;
        unsigned durationProbeAttempts = 0;
        bool sentDawMode = false;
        bool sentActivityWasNull = false;
        std::uint64_t sentSeekGeneration = 0;
        std::optional<std::int64_t> sentTimelineStart;
        std::optional<std::int64_t> sentTimelineEnd;
        lastSend = std::chrono::steady_clock::time_point::min();
        const auto serviceIncoming = [&]() {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            while (available >= 8U) {
                if (!readFrame(pipe, stopToken, opcode, reply) ||
                    opcode == kCloseOpcode) {
                    return false;
                }
                if (opcode == kPingOpcode &&
                    !writeFrame(pipe, kPongOpcode, reply, stopToken)) {
                    return false;
                }
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr,
                        &available, nullptr)) {
                    return false;
                }
            }
            return true;
        };
        const auto awaitActivityAck = [&](std::string_view expectedNonce) {
            const auto deadline = std::chrono::steady_clock::now() +
                kRpcAckTimeout;
            while (!stopToken.stop_requested() &&
                    std::chrono::steady_clock::now() < deadline) {
                if (!readFrame(pipe, stopToken, opcode, reply)) return false;
                if (opcode == kCloseOpcode) return false;
                if (opcode == kPingOpcode) {
                    if (!writeFrame(pipe, kPongOpcode, reply, stopToken)) {
                        return false;
                    }
                    continue;
                }
                if (opcode != kFrameOpcode) continue;
                const auto nonce = jsonStringForKey(reply, "nonce");
                if (!nonce || *nonce != expectedNonce) continue;
                const auto event = jsonStringForKey(reply, "evt");
                if (event && *event == "ERROR") {
                    qaPresenceLog("rpc activity rejected nonce=" + *nonce);
                    return false;
                }
                const auto command = jsonStringForKey(reply, "cmd");
                return command && *command == "SET_ACTIVITY";
            }
            return false;
        };
        while (!stopToken.stop_requested()) {
            const auto connectedScanNow = std::chrono::steady_clock::now();
            if (connectedScanNow >= nextExternalPublisherScan) {
                refreshExternalPublisherConflict();
                nextExternalPublisherScan = connectedScanNow + 5s;
            }
            DiscordTrackPresence track;
            std::uint64_t revision = 0;
            bool hasUpdate = false;
            {
                std::unique_lock lock(mutex_);
                if (!initialActivityPending &&
                    sentRevision_ == pendingRevision_) {
                    // Also wake periodically to service RPC PING/CLOSE frames.
                    // One below-normal wake per second is negligible while the
                    // opt-in service is enabled and avoids 15-second stalls.
                    changed_.wait_for(lock, stopToken, 1s, [&] {
                        return sentRevision_ != pendingRevision_;
                    });
                }
                if (stopToken.stop_requested()) break;
                if (initialActivityPending ||
                    sentRevision_ != pendingRevision_) {
                    track = pending_;
                    revision = pendingRevision_;
                    hasUpdate = true;
                }
            }
            if (!hasUpdate) {
                if (!serviceIncoming()) {
                    qaPresenceLog("rpc incoming failed while idle");
                    break;
                }
                continue;
            }

            const auto trackTransition = sentTrackKey.has_value() &&
                track.update.trackKey != *sentTrackKey;
            const auto coverBecameReady = sentTrackKey.has_value() &&
                track.update.trackKey == *sentTrackKey &&
                !track.coverUrl.empty() && track.coverUrl != sentCoverUrl;
            const auto playbackStateChanged = sentPlaybackState.has_value() &&
                (track.playback.state != *sentPlaybackState ||
                    track.dawMode != sentDawMode);
            // A late cover is the only update that may be postponed for quota.
            // It must never make a later track/state transition wait behind
            // artwork belonging to an older desired activity.
            const auto cosmeticCoverUpdate = coverBecameReady &&
                !trackTransition && !playbackStateChanged &&
                track.update.seekGeneration == sentSeekGeneration;
            // Never carry artwork across track identities merely because the
            // album tags match. Several releases deliberately embed a unique
            // image per track; retaining the previous URL made Discord show
            // stale art until a later update happened to survive throttling.
            if (!trackTransition && sentTrackKey.has_value() &&
                    track.update.trackKey == *sentTrackKey &&
                    sentDurationSeconds > track.playback.durationSeconds) {
                // Never let an incomplete late metadata snapshot replace a
                // complete progress range already accepted for this track.
                track.playback.durationSeconds = sentDurationSeconds;
            }
            if (track.update.trackKey != durationProbeTrackKey) {
                durationProbeTrackKey = track.update.trackKey;
                durationProbeAttempts = 0;
            }

            if (track.playback.state == PlaybackState::playing &&
                    track.finiteLocalFile &&
                    !hasCompletePlaybackRange(track.playback)) {
                // Never publish the elapsed-only compact card for a normal
                // local track. Some input components expose the duration a
                // little after on_playback_new_track; retain the preceding
                // activity and request a fresh main-thread capture instead.
                // Streams are intentionally exempt because they may not have
                // a finite end timestamp at all.
                const auto probeDelay = durationProbeAttempts < 4U
                    ? 125ms : 500ms;
                std::unique_lock lock(mutex_);
                const auto replaced = changed_.wait_for(
                    lock, stopToken, probeDelay, [&] {
                        return pendingRevision_ != revision;
                    });
                if (stopToken.stop_requested()) break;
                if (replaced) continue;
                ++durationProbeAttempts;
                qaPresenceLog("duration probe track=" +
                    std::to_string(track.update.trackKey) +
                    " attempt=" + std::to_string(durationProbeAttempts));
                lock.unlock();
                fb2k::inMainThread([] {
                    if (!core_api::is_shutting_down()) {
                        DiscordPresenceService::instance().publishCurrent(true);
                    }
                });
                lock.lock();
                changed_.wait_for(lock, stopToken, 750ms, [&] {
                    return pendingRevision_ != revision;
                });
                continue;
            }
            durationProbeAttempts = 0;
            if (!track.update.urgentClear &&
                    (trackTransition || initialActivityPending)) {
                // Coalesce metadata and the asynchronous local-cover result
                // into one activity. Replacing an otherwise identical
                // Listening activity a moment later can make current Discord
                // clients rebuild it as the compact media card.
                const auto settleFor = track.update.allowCover &&
                        track.coverUrl.empty()
                    ? kTrackArtworkSettle
                    : kTrackMetadataSettle;
                std::unique_lock lock(mutex_);
                const auto replaced = changed_.wait_for(
                    lock, stopToken, settleFor, [&] {
                        return pendingRevision_ != revision;
                    });
                if (stopToken.stop_requested()) break;
                if (replaced) continue;
            }

            // State and track transitions are semantic, not ordinary metadata
            // churn. A hidden pause must clear even before this pipe has sent
            // its first non-paused revision.
            // Sending pause immediately removes Discord's running timestamp;
            // resume likewise must not sit behind the 4.1 s track throttle.
            bool urgent = track.update.urgentClear ||
                (track.playback.state == PlaybackState::paused &&
                    !track.update.showPaused) ||
                trackTransition ||
                coverBecameReady ||
                playbackStateChanged;
            if (track.playback.state == PlaybackState::stopped && !urgent) {
                std::unique_lock lock(mutex_);
                const auto replaced = changed_.wait_for(
                    lock, stopToken, 250ms, [&] {
                        return pendingRevision_ != revision;
                    });
                if (stopToken.stop_requested()) break;
                if (replaced) continue;
                urgent = true;
            }

            auto now = std::chrono::steady_clock::now();
            if (!track.update.urgentClear &&
                    (trackTransition || coverBecameReady) &&
                    lastSend != std::chrono::steady_clock::time_point::min() &&
                    now - lastSend < kTrackTransitionMinimumInterval) {
                std::unique_lock lock(mutex_);
                const auto replaced = changed_.wait_until(
                    lock, stopToken,
                    lastSend + kTrackTransitionMinimumInterval, [&] {
                        return pendingRevision_ != revision;
                    });
                if (stopToken.stop_requested()) break;
                if (replaced) continue;
                now = std::chrono::steady_clock::now();
            }
            if (!urgent &&
                lastSend != std::chrono::steady_clock::time_point::min() &&
                now - lastSend < kMinimumUpdateInterval) {
                std::unique_lock lock(mutex_);
                const auto replaced = changed_.wait_until(lock, stopToken,
                    lastSend + kMinimumUpdateInterval, [&] {
                        return pendingRevision_ != revision;
                    });
                if (stopToken.stop_requested()) break;
                if (replaced) continue;
                track = pending_;
                revision = pendingRevision_;
            }

            {
                std::lock_guard lock(mutex_);
                if (revision != pendingRevision_ ||
                        track.update.sessionGeneration !=
                            pending_.update.sessionGeneration ||
                        track.update.presenceRevision !=
                            pending_.update.presenceRevision) {
                    continue;
                }
            }

            const auto activityIsNull =
                track.playback.state == PlaybackState::stopped ||
                (track.playback.state == PlaybackState::paused &&
                    !track.update.showPaused);
            if (activityIsNull &&
                    (sentActivityWasNull || initialActivityPending)) {
                // Several lifecycle callbacks can describe the same hidden
                // state. A new RPC connection has no activity yet, so its
                // initial stopped snapshot also needs no SET_ACTIVITY.
                {
                    std::lock_guard lock(mutex_);
                    sentRevision_ = revision;
                }
                sentPlaybackState = track.playback.state;
                sentTrackKey = track.update.trackKey;
                sentCoverUrl = track.coverUrl;
                sentDawMode = track.dawMode;
                initialActivityPending = false;
                changed_.notify_all();
                if (!serviceIncoming()) {
                    qaPresenceLog("rpc incoming failed after null dedupe");
                    break;
                }
                continue;
            }

            // Apply Discord's documented rolling limit to all SET_ACTIVITY
            // traffic. Semantic activity may use the fifth documented slot;
            // cover-only refinement stops at four, reserving one slot for the
            // next track/state transition. Privacy clear remains fail-closed
            // and bypasses the queue. While waiting, latest revision wins.
            auto rateNow = std::chrono::steady_clock::now();
            while (!sendHistory.empty() &&
                    rateNow - sendHistory.front().sentAt >=
                        kDiscordRateWindow) {
                sendHistory.pop_front();
            }
            const auto ordinaryCount = sendHistory.size();
            const auto ordinaryLimit = cosmeticCoverUpdate ? 4U : 5U;
            if (!activityIsNull && !track.update.urgentClear &&
                    ordinaryCount >= ordinaryLimit) {
                const auto allowedAt = sendHistory.front().sentAt +
                    kDiscordRateWindow;
                std::unique_lock lock(mutex_);
                const auto replaced = changed_.wait_until(
                    lock, stopToken, allowedAt, [&] {
                        return pendingRevision_ != revision;
                    });
                if (stopToken.stop_requested()) break;
                if (replaced) continue;
                rateNow = std::chrono::steady_clock::now();
                while (!sendHistory.empty() &&
                        rateNow - sendHistory.front().sentAt >=
                            kDiscordRateWindow) {
                    sendHistory.pop_front();
                }
            }

            std::optional<std::int64_t> timelineStart;
            std::optional<std::int64_t> timelineEnd;
            if (track.playback.state == PlaybackState::playing) {
                const auto stableTimeline = sentTrackKey &&
                    *sentTrackKey == track.update.trackKey &&
                    sentPlaybackState == PlaybackState::playing &&
                    sentSeekGeneration == track.update.seekGeneration &&
                    sentTimelineStart.has_value();
                if (stableTimeline) {
                    timelineStart = sentTimelineStart;
                    timelineEnd = sentTimelineEnd;
                } else {
                    if (!track.dawMode) {
                        const auto current = Runtime::instance().playbackSnapshot();
                        if (current.trackIdentityHash ==
                                track.update.trackKey) {
                            track.playback.positionSeconds =
                                Runtime::instance().presentationPlaybackPosition();
                            if (current.durationSeconds > 0.0) {
                                track.playback.durationSeconds =
                                    current.durationSeconds;
                            }
                        }
                    }
                    if (std::isfinite(track.playback.positionSeconds) &&
                            track.playback.positionSeconds >= 0.0) {
                        timelineStart = unixNow() -
                            static_cast<std::int64_t>(std::llround(
                                track.playback.positionSeconds));
                        if (std::isfinite(track.playback.durationSeconds) &&
                                track.playback.durationSeconds >
                                    track.playback.positionSeconds) {
                            timelineEnd = *timelineStart +
                                static_cast<std::int64_t>(std::llround(
                                    track.playback.durationSeconds));
                        }
                    }
                }
            }

            qaPresenceLog("rpc send revision=" + std::to_string(revision) +
                " state=" + std::to_string(static_cast<unsigned>(
                    track.playback.state)) +
                " cover=" + std::to_string(!track.coverUrl.empty()) +
                " position=" + std::to_string(
                    track.playback.positionSeconds) +
                " duration=" + std::to_string(
                    track.playback.durationSeconds) +
                " complete=" + std::to_string(
                    hasCompletePlaybackRange(track.playback)));

            const auto payload = activityJson(track,
                track.update.showPaused, revision,
                appId,
                timelineStart, timelineEnd);
            if (!writeFrame(pipe, kFrameOpcode, payload, stopToken)) {
                qaPresenceLog("rpc write failed");
                break;
            }
            {
                std::lock_guard lock(mutex_);
                if (deliveryTrace_.trackGeneration == track.update.trackKey) {
                    deliveryTrace_.activitySentMilliseconds =
                        presenceMilliseconds();
                }
            }
            lastSend = std::chrono::steady_clock::now();
            sendHistory.push_back(
                {lastSend, activityIsNull, cosmeticCoverUpdate});
            const auto expectedNonce = "goden-" +
                std::to_string(revision);
            if (!awaitActivityAck(expectedNonce)) {
                qaPresenceLog("rpc activity ack failed nonce=" +
                    expectedNonce);
                break;
            }
            {
                std::lock_guard lock(mutex_);
                sentRevision_ = revision;
                if (deliveryTrace_.trackGeneration == track.update.trackKey) {
                    deliveryTrace_.activityAcknowledgedMilliseconds =
                        presenceMilliseconds();
                    deliveryTrace_.error.clear();
                }
            }
            sentPlaybackState = track.playback.state;
            sentTrackKey = track.update.trackKey;
            sentCoverUrl = track.coverUrl;
            sentSeekGeneration = track.update.seekGeneration;
            sentTimelineStart = timelineStart;
            sentTimelineEnd = timelineEnd;
            if (hasCompletePlaybackRange(track.playback)) {
                sentDurationSeconds = track.playback.durationSeconds;
            } else if (trackTransition) {
                sentDurationSeconds = 0.0;
            }
            sentDawMode = track.dawMode;
            sentActivityWasNull = activityIsNull;
            changed_.notify_all();
            initialActivityPending = false;

            // RPC ACK confirms only that Discord accepted SET_ACTIVITY. It
            // does not confirm that the media proxy fetched or decoded the
            // external asset. Relay V3 reports that second stage explicitly.
            // If no request arrives, replace the URL once with an application
            // asset, so the card can never remain on Discord's question mark.
            if (ephemeralRelayEnabled() && !track.coverToken.empty()) {
                const auto relay=coverService_.relayTrace();
                if(relay.assetId==track.coverToken &&
                        relay.delivery==DiscordRelayDelivery::fallback)
                    coverStatus_.store(DiscordCoverStatus::fallback,std::memory_order_release);
            }
            if (!serviceIncoming()) {
                qaPresenceLog("rpc incoming failed after send");
                break;
            }
        }
        rpcReady_.store(false, std::memory_order_release);
        coverService_.cancel();
        changed_.notify_all();
        CloseHandle(pipe);
    }
    rpcReady_.store(false, std::memory_order_release);
    coverService_.cancel();
    changed_.notify_all();
}

void DiscordPresenceService::refreshExternalPublisherConflict() noexcept
{
    externalPublisherConflict_.store(
        musicPresencePublishesFoobar(), std::memory_order_release);
}

void DiscordPresenceService::setStatus(
    DiscordPresenceStatus next) noexcept
{
    status_.store(next, std::memory_order_release);
}

} // namespace nekorox::component
