#include "lrclib_provider.h"

#include "nlohmann-json/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace nekorox::component {
namespace {

std::wstring percentEncode(std::string_view utf8)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::wstring result;
    result.reserve(utf8.size() * 3U);
    for (const auto byte : utf8) {
        const auto value = static_cast<unsigned char>(byte);
        if ((value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' ||
            value == '_' || value == '.' || value == '~') {
            result.push_back(static_cast<wchar_t>(value));
        } else {
            result.push_back(L'%');
            result.push_back(hex[value >> 4]);
            result.push_back(hex[value & 0x0f]);
        }
    }
    return result;
}

std::wstring query(const LyricsMetadata& metadata, bool includeAlbum,
    bool exact)
{
    std::wstring path = exact ? L"/api/get?" : L"/api/search?";
    path += L"artist_name=" + percentEncode(metadata.artist);
    path += L"&track_name=" + percentEncode(metadata.title);
    if (includeAlbum && !metadata.album.empty())
        path += L"&album_name=" + percentEncode(metadata.album);
    if (exact && metadata.durationSeconds &&
        std::isfinite(*metadata.durationSeconds) &&
        *metadata.durationSeconds > 0.0) {
        std::wostringstream value;
        value.imbue(std::locale::classic());
        value << std::fixed << std::setprecision(3) << *metadata.durationSeconds;
        path += L"&duration=" + value.str();
    }
    return path;
}

bool stringField(const nlohmann::json& object,
    const char* name, std::string& output, bool optional = false)
{
    const auto found = object.find(name);
    if (found == object.end() || found->is_null()) return optional;
    if (!found->is_string()) return false;
    output = found->get<std::string>();
    if (output.size() > 512U * 1024U) return false;
    return true;
}

std::optional<LyricsCandidateV2> parseCandidate(
    const nlohmann::json& object)
{
    if (!object.is_object()) return std::nullopt;
    const auto id = object.find("id");
    const auto duration = object.find("duration");
    const auto instrumental = object.find("instrumental");
    if (id == object.end() || !id->is_number_integer() ||
        duration == object.end() ||
        (!duration->is_number() && !duration->is_null()) ||
        (duration->is_number() && !std::isfinite(duration->get<double>())) ||
        instrumental == object.end() || !instrumental->is_boolean()) {
        return std::nullopt;
    }
    LyricsCandidateV2 candidate;
    candidate.source.provider = "lrclib";
    candidate.source.recordId = std::to_string(id->get<std::int64_t>());
    candidate.source.revision = "api-v1";
    if (!stringField(object, "trackName", candidate.title) ||
        !stringField(object, "artistName", candidate.artist) ||
        !stringField(object, "albumName", candidate.album, true) ||
        !stringField(object, "plainLyrics", candidate.plainLyrics, true) ||
        !stringField(object, "syncedLyrics", candidate.syncedLyrics, true)) {
        return std::nullopt;
    }
    if (duration->is_number() && duration->get<double>() > 0.0)
        candidate.durationSeconds = duration->get<double>();
    candidate.instrumental = instrumental->get<bool>();
    candidate.sourceDisplayName = "LRCLIB";
    candidate.attribution = "Lyrics provided by LRCLIB";
    candidate.sourceUrl = "https://lrclib.net";
    if (candidate.title.empty() || candidate.artist.empty() ||
        (!candidate.instrumental && candidate.plainLyrics.empty() &&
            candidate.syncedLyrics.empty())) return std::nullopt;
    return candidate;
}

bool depthWithinLimit(std::string_view body, unsigned maximum) noexcept
{
    unsigned depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const auto character : body) {
        if (inString) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') inString = false;
            continue;
        }
        if (character == '"') inString = true;
        else if (character == '{' || character == '[') {
            if (++depth > maximum) return false;
        } else if ((character == '}' || character == ']') && depth != 0) {
            --depth;
        }
    }
    return !inString && depth == 0;
}

bool parseCandidates(std::string_view body,
    std::vector<LyricsCandidateV2>& output)
{
    if (body.empty() || body.size() > 2U * 1024U * 1024U ||
        !depthWithinLimit(body, 32)) {
        output.clear();
        return false;
    }
    try {
        std::vector<std::unordered_set<std::string>> objectKeys;
        const nlohmann::json::parser_callback_t callback =
            [&](int, nlohmann::json::parse_event_t event,
                    nlohmann::json& parsed) {
                if (event == nlohmann::json::parse_event_t::object_start) {
                    objectKeys.emplace_back();
                } else if (event == nlohmann::json::parse_event_t::key &&
                        parsed.is_string()) {
                    if (objectKeys.empty())
                        throw std::runtime_error("LRCLIB key outside object");
                    if (!objectKeys.back().insert(parsed.get<std::string>()).second)
                        throw std::runtime_error("duplicate LRCLIB object key");
                } else if (event == nlohmann::json::parse_event_t::object_end) {
                    if (objectKeys.empty())
                        throw std::runtime_error("LRCLIB object stack underflow");
                    objectKeys.pop_back();
                }
                return true;
            };
        const auto json = nlohmann::json::parse(
            body.begin(), body.end(), callback, true, false);
        if (!objectKeys.empty() || json.is_discarded()) {
            output.clear();
            return false;
        }
        std::vector<LyricsCandidateV2> next;
        std::size_t sourceRecords = 0;
        const auto append = [&](const auto& value) {
            ++sourceRecords;
            if (const auto candidate = parseCandidate(value))
                next.push_back(*candidate);
        };
        if (json.is_array()) {
            if (json.size() > 50U) {
                output.clear();
                return false;
            }
            for (const auto& value : json) append(value);
        } else if (json.is_object()) {
            append(json);
        } else {
            output.clear();
            return false;
        }
        if (sourceRecords != 0 && next.empty()) {
            output.clear();
            return false;
        }
        output = std::move(next);
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

} // namespace

bool parseLrclibResponseForTests(std::string_view body,
    std::vector<LyricsCandidateV2>& output)
{
    return parseCandidates(body, output);
}

LyricsHttpResponse LrclibProvider::request(
    std::wstring path, std::stop_token stop)
{
    for (;;) {
        if (stop.stop_requested()) {
            LyricsHttpResponse response;
            response.cancelled = true;
            return response;
        }
        std::chrono::steady_clock::time_point allowed;
        {
            std::lock_guard lock(rateMutex_);
            allowed = std::max(nextRequest_, serverBackoff_);
        }
        const auto now = std::chrono::steady_clock::now();
        if (allowed <= now) break;
        const auto pause = std::min(allowed - now,
            std::chrono::steady_clock::duration(std::chrono::milliseconds(50)));
        std::this_thread::sleep_for(pause);
    }
    {
        std::lock_guard lock(rateMutex_);
        nextRequest_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
    }
    auto response = http_.getLrclib(path, stop);
    {
        std::lock_guard lock(rateMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (response.status == 429) {
            serverBackoff_ = std::chrono::steady_clock::now() +
                (response.retryAfter.count() > 0 ? response.retryAfter
                                                 : std::chrono::seconds(60));
        } else if (response.status >= 500 && response.status < 600) {
            constexpr std::array delays{
                std::chrono::seconds(5),
                std::chrono::seconds(30),
                std::chrono::seconds(120)};
            const auto index = std::min<std::size_t>(
                consecutiveServerFailures_, delays.size() - 1U);
            serverBackoff_ = now + delays[index];
            consecutiveServerFailures_ = std::min(
                consecutiveServerFailures_ + 1U,
                static_cast<unsigned>(delays.size()));
        } else if (response.status != 0) {
            consecutiveServerFailures_ = 0;
            serverBackoff_ = now;
        }
    }
    return response;
}

LyricsProviderResult LrclibProvider::lookup(
    const LyricsRequestContext& context,
    std::stop_token stop)
{
    LyricsProviderResult result;
    if (context.metadata.title.empty() || context.metadata.artist.empty()) {
        result.reason = "metadata-incomplete";
        return result;
    }
    const std::array requests{
        query(context.metadata, true, true),
        query(context.metadata, true, false),
        query(context.metadata, false, false)};
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (stop.stop_requested()) {
            result.outcome = LyricsProviderOutcome::cancelled;
            return result;
        }
        auto response = request(requests[index], stop);
        ++result.logicalRequests;
        if (response.cancelled) {
            result.outcome = LyricsProviderOutcome::cancelled;
            return result;
        }
        if (!response.error.empty()) {
            result.outcome = LyricsProviderOutcome::networkError;
            result.reason = std::move(response.error);
            return result;
        }
        if (response.status == 429) {
            result.outcome = LyricsProviderOutcome::rateLimited;
            result.retryAfter = response.retryAfter;
            result.reason = "lrclib-rate-limited";
            return result;
        }
        if (response.status >= 500) {
            result.outcome = LyricsProviderOutcome::networkError;
            result.reason = "lrclib-server-error";
            return result;
        }
        if (response.status != 200 && response.status != 404) {
            result.outcome = LyricsProviderOutcome::networkError;
            result.reason = "lrclib-http-" + std::to_string(response.status);
            return result;
        }
        if (response.status == 404) continue;
        std::vector<LyricsCandidateV2> parsed;
        if (!parseCandidates(response.body, parsed)) {
            result.outcome = LyricsProviderOutcome::invalidResponse;
            result.reason = "lrclib-invalid-json";
            return result;
        }
        result.candidates.insert(result.candidates.end(),
            std::make_move_iterator(parsed.begin()),
            std::make_move_iterator(parsed.end()));
        const auto selected = selectLyricsCandidate(
            context.metadata, result.candidates);
        if (selected.candidateIndex) {
            const auto& candidate = result.candidates[*selected.candidateIndex];
            if (!candidate.plainLyrics.empty() ||
                !candidate.syncedLyrics.empty()) break;
        }
    }
    result.outcome = result.candidates.empty()
        ? LyricsProviderOutcome::notFound
        : LyricsProviderOutcome::candidates;
    return result;
}

} // namespace nekorox::component
