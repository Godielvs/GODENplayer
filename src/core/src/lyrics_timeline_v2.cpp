#include "nekorox/lyrics_match_policy.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <sstream>
#include <iomanip>

namespace nekorox {
namespace {

bool validUtf8(std::string_view value) noexcept
{
    if (value.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0) > 0;
}

std::string sha256(std::string_view value)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, digestBytes = 0, copied = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    const auto close = [&] {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
            &copied, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes),
            &copied, 0) < 0) {
        close();
        return {};
    }
    object.resize(objectBytes);
    digest.resize(digestBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
            nullptr, 0, 0) < 0 ||
        (!value.empty() && BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()), 0) < 0) ||
        BCryptFinishHash(hash, digest.data(), digestBytes, 0) < 0) {
        close();
        return {};
    }
    close();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2)
        << static_cast<unsigned>(byte);
    return output.str();
}

std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
        value.back() == '\r')) value.remove_suffix(1);
    return value;
}

bool startsWithInsensitive(std::string_view value,
    std::string_view prefix) noexcept
{
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character + ('a' - 'A')) : character;
        };
        if (lower(value[index]) != lower(prefix[index])) return false;
    }
    return true;
}

bool metadataTag(std::string_view value) noexcept
{
    constexpr std::string_view tags[]{"ar:", "al:", "ti:", "au:",
        "by:", "re:", "ve:", "length:", "offset:"};
    return std::any_of(std::begin(tags), std::end(tags),
        [&](auto tag) { return startsWithInsensitive(value, tag); });
}

std::optional<std::int64_t> timestampMs(std::string_view value) noexcept
{
    const auto colon = value.find(':');
    if (colon == std::string_view::npos ||
        value.find(':', colon + 1U) != std::string_view::npos) return std::nullopt;
    std::uint64_t minutes = 0, seconds = 0, fraction = 0;
    const auto minuteText = trim(value.substr(0, colon));
    auto secondText = trim(value.substr(colon + 1U));
    const auto decimal = secondText.find_first_of(".,");
    const auto whole = secondText.substr(0, decimal);
    auto parsed = std::from_chars(minuteText.data(),
        minuteText.data() + minuteText.size(), minutes);
    if (parsed.ec != std::errc{} || parsed.ptr != minuteText.data() + minuteText.size())
        return std::nullopt;
    parsed = std::from_chars(whole.data(), whole.data() + whole.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != whole.data() + whole.size() ||
        seconds >= 60U || minutes > 35791394U) return std::nullopt;
    std::uint64_t milliseconds = 0;
    if (decimal != std::string_view::npos) {
        const auto digits = secondText.substr(decimal + 1U);
        if (digits.empty() || digits.size() > 3U) return std::nullopt;
        parsed = std::from_chars(digits.data(), digits.data() + digits.size(), fraction);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
            return std::nullopt;
        milliseconds = digits.size() == 1U ? fraction * 100U
            : digits.size() == 2U ? fraction * 10U : fraction;
    }
    return static_cast<std::int64_t>((minutes * 60U + seconds) * 1000U +
        milliseconds);
}

std::optional<std::int64_t> offsetMs(std::string_view value) noexcept
{
    if (!startsWithInsensitive(value, "offset:")) return std::nullopt;
    const auto text = trim(value.substr(7U));
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    return result;
}

struct ParsedLine {
    std::string text;
    std::vector<std::int64_t> timestamps;
    bool metadataOnly = false;
};

std::vector<std::string> plainLines(std::string_view value)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size() && result.size() <= kMaximumLyricsLines) {
        const auto end = value.find('\n', start);
        auto line = trim(value.substr(start, end == std::string_view::npos
            ? value.size() - start : end - start));
        if (!line.empty()) result.emplace_back(line);
        if (end == std::string_view::npos) break;
        start = end + 1U;
    }
    return result;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string result;
    for (const auto& line : lines) {
        if (!result.empty()) result.push_back('\n');
        result += line;
    }
    return result;
}

std::string lexical(std::string_view value)
{
    std::string result;
    bool space = false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 0x80U || std::isalnum(byte)) {
            if (space && !result.empty()) result.push_back(' ');
            result.push_back(byte < 0x80U
                ? static_cast<char>(std::tolower(byte)) : character);
            space = false;
        } else {
            space = !result.empty();
        }
    }
    return result;
}

void addReason(std::vector<std::string>& values, std::string reason)
{
    if (std::find(values.begin(), values.end(), reason) == values.end())
        values.push_back(std::move(reason));
}

} // namespace

LyricsParseResult parseLyricsDocumentV2(
    const LyricsMetadata& wanted,
    const LyricsCandidateV2& candidate,
    const LyricsCandidateEvaluation& evaluation)
{
    LyricsParseResult result;
    const auto payloadBytes = candidate.plainLyrics.size() +
        candidate.syncedLyrics.size();
    if (payloadBytes > kMaximumLyricsBytes ||
        !validUtf8(candidate.plainLyrics) || !validUtf8(candidate.syncedLyrics)) {
        addReason(result.reasonCodes, payloadBytes > kMaximumLyricsBytes
            ? "payload_too_large" : "invalid_utf8");
        return result;
    }
    result.validPayload = true;
    auto document = std::make_shared<LyricsDocumentV2>();
    document->documentId = candidate.source.provider + ":" +
        candidate.source.recordId;
    if (candidate.source.provider == "local") {
        // A tag name or sidecar path is not a globally unique document.
        // Include recording, source and exact payload with unambiguous fields.
        std::string material;
        const auto append = [&](std::string_view value) {
            material += std::to_string(value.size()) + ":";
            material.append(value);
        };
        append(wanted.recording.entityId);
        append(std::to_string(wanted.recording.subsong));
        append(std::to_string(wanted.metadata.value));
        append(candidate.source.recordId);
        append(candidate.plainLyrics);
        append(candidate.syncedLyrics);
        append(candidate.instrumental ? "1" : "0");
        const auto digest = sha256(material);
        if (digest.empty()) {
            result.validPayload = false;
            addReason(result.reasonCodes, "source_identity_failed");
            return result;
        }
        document->documentId = "local:v2:" + digest;
    }
    document->source = candidate.source;
    document->binding = wanted.recording;
    document->sourceDisplayName = candidate.sourceDisplayName;
    document->attribution = candidate.attribution;
    document->sourceUrl = candidate.sourceUrl;
    document->reasonCodes = evaluation.reasonCodes;

    if (candidate.instrumental) {
        document->timing = TimingStatus::staticText;
        addReason(document->reasonCodes, "instrumental_reported");
        result.document = std::move(document);
        return result;
    }

    std::vector<ParsedLine> parsed;
    std::vector<std::string> visible;
    std::size_t timestampCount = 0;
    std::int64_t sourceOffset = 0;
    bool invalidTimestamp = false;
    std::size_t start = 0;
    while (start <= candidate.syncedLyrics.size()) {
        if (parsed.size() > kMaximumLyricsLines) {
            addReason(result.reasonCodes, "too_many_lines");
            result.validPayload = false;
            return result;
        }
        const auto end = candidate.syncedLyrics.find('\n', start);
        auto line = trim(std::string_view(candidate.syncedLyrics).substr(start,
            end == std::string::npos ? candidate.syncedLyrics.size() - start
                : end - start));
        ParsedLine current;
        std::size_t cursor = 0;
        bool metadata = false;
        while (cursor < line.size() && line[cursor] == '[') {
            const auto close = line.find(']', cursor + 1U);
            if (close == std::string_view::npos) break;
            const auto tag = trim(line.substr(cursor + 1U, close - cursor - 1U));
            if (const auto offset = offsetMs(tag)) {
                sourceOffset = *offset;
                metadata = true;
            } else if (const auto time = timestampMs(tag)) {
                current.timestamps.push_back(*time);
                if (++timestampCount > kMaximumLyricsTimestampTokens) {
                    addReason(result.reasonCodes, "too_many_timestamps");
                    result.validPayload = false;
                    return result;
                }
            } else if (metadataTag(tag)) {
                metadata = true;
            } else {
                break;
            }
            cursor = close + 1U;
        }
        current.text.assign(trim(line.substr(cursor)));
        current.metadataOnly = metadata && current.timestamps.empty() &&
            current.text.empty();
        if (!current.text.empty()) visible.push_back(current.text);
        if (!current.text.empty() || !current.timestamps.empty())
            parsed.push_back(std::move(current));
        if (end == std::string::npos) break;
        start = end + 1U;
    }

    auto preferredPlain = plainLines(candidate.plainLyrics);
    if (preferredPlain.empty()) preferredPlain = visible;
    document->plainTextUtf8 = joinLines(preferredPlain);
    document->textHash = sha256(document->plainTextUtf8);
    const auto lexicalSynced = lexical(joinLines(visible));
    const auto lexicalPlain = lexical(document->plainTextUtf8);
    const auto allTextTimed = !visible.empty() &&
        std::all_of(parsed.begin(), parsed.end(), [](const auto& line) {
            return line.text.empty() || !line.timestamps.empty();
        });

    std::map<std::int64_t, std::vector<std::string>> groups;
    const auto durationLimit = wanted.durationSeconds &&
        std::isfinite(*wanted.durationSeconds) && *wanted.durationSeconds >= 0.0
        ? static_cast<std::int64_t>(std::llround(*wanted.durationSeconds * 1000.0))
            + 250LL
        : (std::numeric_limits<std::int64_t>::max)();
    for (const auto& line : parsed) {
        for (const auto raw : line.timestamps) {
            if ((sourceOffset > 0 && raw >
                    (std::numeric_limits<std::int64_t>::max)() - sourceOffset) ||
                (sourceOffset < 0 && raw <
                    (std::numeric_limits<std::int64_t>::min)() - sourceOffset)) {
                invalidTimestamp = true;
                continue;
            }
            const auto adjusted = raw + sourceOffset;
            if (adjusted < 0 || adjusted > durationLimit) {
                invalidTimestamp = true;
                continue;
            }
            groups[adjusted].push_back(line.text);
        }
    }
    if (groups.size() > kMaximumLyricsCueGroups) invalidTimestamp = true;
    if (!invalidTimestamp && groups.size() <= kMaximumLyricsCueGroups) {
        for (auto& [time, lines] : groups)
            document->cues.push_back({time, std::nullopt, std::move(lines)});
    }
    std::ostringstream timingMaterial;
    for (const auto& cue : document->cues) {
        timingMaterial << cue.startMs << '\n';
        for (const auto& line : cue.simultaneousLines)
            timingMaterial << line << '\n';
    }
    document->timingHash = sha256(timingMaterial.str());

    result.completeTiming = evaluation.timingEligible && !invalidTimestamp &&
        allTextTimed && !document->cues.empty() &&
        (candidate.plainLyrics.empty() || lexicalPlain == lexicalSynced);
    if (result.completeTiming) {
        document->timing = TimingStatus::catalogMatched;
    } else {
        document->timing = TimingStatus::staticText;
        if (invalidTimestamp) addReason(document->reasonCodes,
            "invalid_timeline_static_text");
        if (!allTextTimed) addReason(document->reasonCodes,
            "partial_timing_static_text");
        if (!candidate.plainLyrics.empty() && lexicalPlain != lexicalSynced)
            addReason(document->reasonCodes, "plain_synced_text_conflict");
        if (!evaluation.timingEligible && !candidate.syncedLyrics.empty())
            addReason(document->reasonCodes, "timing_identity_not_exact");
    }
    result.reasonCodes = document->reasonCodes;
    result.document = std::move(document);
    return result;
}

std::optional<std::size_t> activeLyricsCueIndex(
    const LyricsDocumentV2& document,
    std::int64_t playbackMs) noexcept
{
    if (document.timing != TimingStatus::catalogMatched &&
        document.timing != TimingStatus::audioVerified) return std::nullopt;
    if (document.cues.empty() || playbackMs < document.cues.front().startMs)
        return std::nullopt;
    const auto found = std::upper_bound(document.cues.begin(),
        document.cues.end(), playbackMs, [](std::int64_t time,
            const LyricsCue& cue) { return time < cue.startMs; });
    if (found == document.cues.begin()) return std::nullopt;
    return static_cast<std::size_t>(std::distance(document.cues.begin(), found) - 1);
}

} // namespace nekorox
