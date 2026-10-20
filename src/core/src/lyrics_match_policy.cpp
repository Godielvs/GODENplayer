#include "nekorox/lyrics_match_policy.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cwctype>

namespace nekorox {
namespace {

std::wstring utf8ToWideStrict(std::string_view value)
{
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        return {};
    }
    return result;
}

std::string wideToUtf8Strict(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count,
            nullptr, nullptr) != count) return {};
    return result;
}

std::wstring nfkcFold(std::wstring_view input)
{
    if (input.empty()) return {};
    const auto normalizedCount = NormalizeString(NormalizationKC,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    std::wstring normalized;
    if (normalizedCount > 0) {
        normalized.resize(static_cast<std::size_t>(normalizedCount));
        if (NormalizeString(NormalizationKC, input.data(),
                static_cast<int>(input.size()), normalized.data(),
                normalizedCount) <= 0) normalized.assign(input);
    } else {
        normalized.assign(input);
    }
    const auto foldedCount = LCMapStringEx(LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE, normalized.data(), static_cast<int>(normalized.size()),
        nullptr, 0, nullptr, nullptr, 0);
    if (foldedCount <= 0) return normalized;
    std::wstring folded(static_cast<std::size_t>(foldedCount), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            normalized.data(), static_cast<int>(normalized.size()),
            folded.data(), foldedCount, nullptr, nullptr, 0) <= 0) {
        return normalized;
    }
    return folded;
}

bool wordCharacter(wchar_t value) noexcept
{
    return iswalnum(value) != 0 || value >= 0x80;
}

std::string normalizedWords(std::string_view input)
{
    const auto wide = utf8ToWideStrict(input);
    if (wide.empty() && !input.empty()) return {};
    const auto folded = nfkcFold(wide);
    std::wstring compact;
    compact.reserve(folded.size());
    bool pendingSpace = false;
    for (const auto character : folded) {
        if (wordCharacter(character)) {
            if (pendingSpace && !compact.empty()) compact.push_back(L' ');
            compact.push_back(character);
            pendingSpace = false;
        } else {
            pendingSpace = !compact.empty();
        }
    }
    return wideToUtf8Strict(compact);
}

std::string trimAscii(std::string value)
{
    const auto space = [](unsigned char value) { return std::isspace(value) != 0; };
    while (!value.empty() && space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::optional<LyricsVersionKind> qualifierKind(std::string_view words)
{
    const auto contains = [&](std::string_view value) {
        return words == value || words.find(value) != std::string_view::npos;
    };
    if (contains("remix") || contains("mix")) return LyricsVersionKind::remix;
    if (contains("radio edit") || words == "edit") return LyricsVersionKind::edit;
    if (contains("live")) return LyricsVersionKind::live;
    if (contains("demo")) return LyricsVersionKind::demo;
    if (contains("acoustic")) return LyricsVersionKind::acoustic;
    if (contains("instrumental")) return LyricsVersionKind::instrumental;
    if (contains("sped up") || contains("speed up")) return LyricsVersionKind::spedUp;
    if (contains("slowed")) return LyricsVersionKind::slowed;
    if (contains("remaster")) return LyricsVersionKind::remaster;
    return std::nullopt;
}

std::pair<std::string, std::vector<LyricsVersionQualifier>>
titleAndVersions(std::string_view input)
{
    auto original = trimAscii(std::string(input));
    std::vector<LyricsVersionQualifier> versions;
    for (;;) {
        if (original.size() < 3U) break;
        const auto close = original.back();
        const auto open = close == ')' ? '(' : close == ']' ? '[' : '\0';
        if (open == '\0') break;
        const auto start = original.find_last_of(open);
        if (start == std::string::npos) break;
        const auto inside = original.substr(start + 1U,
            original.size() - start - 2U);
        const auto normalized = normalizedWords(inside);
        const auto kind = qualifierKind(normalized);
        if (!kind) break; // Unknown suffix remains part of the song title.
        versions.push_back({*kind, normalized});
        original = trimAscii(original.substr(0, start));
    }
    const auto delimiter = original.find_last_of('-');
    if (delimiter != std::string::npos && delimiter > 0U) {
        const auto suffix = normalizedWords(original.substr(delimiter + 1U));
        if (const auto kind = qualifierKind(suffix)) {
            versions.push_back({*kind, suffix});
            original = trimAscii(original.substr(0, delimiter));
        }
    }
    std::sort(versions.begin(), versions.end(), [](const auto& left,
        const auto& right) { return left.kind < right.kind; });
    return {normalizedWords(original), std::move(versions)};
}

const LyricsVersionQualifier* findVersion(
    const std::vector<LyricsVersionQualifier>& values,
    LyricsVersionKind kind) noexcept
{
    const auto found = std::find_if(values.begin(), values.end(),
        [kind](const auto& value) { return value.kind == kind; });
    return found == values.end() ? nullptr : &*found;
}

bool sameStrongVersion(const LyricsVersionQualifier& left,
    const LyricsVersionQualifier& right) noexcept
{
    if (left.kind != right.kind) return false;
    if (left.kind == LyricsVersionKind::live ||
        left.kind == LyricsVersionKind::remix ||
        left.kind == LyricsVersionKind::edit) return left.value == right.value;
    return true;
}

void addReason(std::vector<std::string>& values, std::string reason)
{
    if (std::find(values.begin(), values.end(), reason) == values.end())
        values.push_back(std::move(reason));
}

} // namespace

NormalizedLyricsIdentity normalizeLyricsIdentity(
    std::string_view title,
    std::string_view artist,
    std::string_view album)
{
    auto [normalizedTitle, versions] = titleAndVersions(title);
    return {std::move(normalizedTitle), normalizedWords(artist),
        normalizedWords(album), std::move(versions)};
}

LyricsCandidateEvaluation evaluateLyricsCandidate(
    const LyricsMetadata& wanted,
    const LyricsCandidateV2& candidate)
{
    LyricsCandidateEvaluation result;
    const auto expected = normalizeLyricsIdentity(
        wanted.title, wanted.artist, wanted.album);
    const auto actual = normalizeLyricsIdentity(
        candidate.title, candidate.artist, candidate.album);
    if (expected.title.empty() || expected.artist.empty() ||
        actual.title.empty() || actual.artist.empty() ||
        expected.title != actual.title || expected.artist != actual.artist) {
        addReason(result.reasonCodes, "identity_mismatch");
        return result;
    }

    constexpr std::array strongKinds{
        LyricsVersionKind::live, LyricsVersionKind::remix,
        LyricsVersionKind::edit, LyricsVersionKind::demo,
        LyricsVersionKind::acoustic, LyricsVersionKind::instrumental,
        LyricsVersionKind::spedUp, LyricsVersionKind::slowed};
    for (const auto kind : strongKinds) {
        const auto* left = findVersion(expected.versions, kind);
        const auto* right = findVersion(actual.versions, kind);
        if ((left == nullptr) != (right == nullptr) ||
            (left != nullptr && !sameStrongVersion(*left, *right))) {
            addReason(result.reasonCodes, "version_conflict");
            return result;
        }
    }

    const auto* expectedRemaster = findVersion(
        expected.versions, LyricsVersionKind::remaster);
    const auto* actualRemaster = findVersion(
        actual.versions, LyricsVersionKind::remaster);
    result.remasterUncertain =
        (expectedRemaster == nullptr) != (actualRemaster == nullptr) ||
        (expectedRemaster && actualRemaster &&
            expectedRemaster->value != actualRemaster->value);

    result.score = 80; // exact controlled title + primary artist
    if (!expected.album.empty() && expected.album == actual.album)
        result.score += 15;
    if (wanted.durationSeconds && candidate.durationSeconds &&
        std::isfinite(*wanted.durationSeconds) &&
        std::isfinite(*candidate.durationSeconds) &&
        std::abs(*wanted.durationSeconds - *candidate.durationSeconds) <= 1.5)
        result.score += 5;
    result.textEligible = result.score >= 80 &&
        (!candidate.plainLyrics.empty() || !candidate.syncedLyrics.empty() ||
            candidate.instrumental);
    result.timingEligible = result.score == 100 &&
        !result.remasterUncertain && !candidate.syncedLyrics.empty();
    if (result.remasterUncertain) addReason(result.reasonCodes,
        "remaster_timing_uncertain");
    if (!result.timingEligible && !candidate.syncedLyrics.empty())
        addReason(result.reasonCodes, "timing_identity_not_exact");
    return result;
}

LyricsSelection selectLyricsCandidate(
    const LyricsMetadata& wanted,
    const std::vector<LyricsCandidateV2>& candidates)
{
    LyricsSelection result;
    struct Ranked { std::size_t index; LyricsCandidateEvaluation evaluation; };
    std::vector<Ranked> ranked;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto evaluation = evaluateLyricsCandidate(wanted, candidates[index]);
        if (evaluation.textEligible)
            ranked.push_back({index, std::move(evaluation)});
    }
    if (ranked.empty()) {
        addReason(result.reasonCodes, "no_identity_match");
        return result;
    }
    const auto hasTextCandidate = std::any_of(ranked.begin(), ranked.end(),
        [&](const auto& item) {
            const auto& candidate = candidates[item.index];
            return !candidate.plainLyrics.empty() ||
                !candidate.syncedLyrics.empty();
        });
    if (hasTextCandidate) {
        ranked.erase(std::remove_if(ranked.begin(), ranked.end(),
            [&](const auto& item) {
                const auto& candidate = candidates[item.index];
                return candidate.plainLyrics.empty() &&
                    candidate.syncedLyrics.empty();
            }), ranked.end());
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left,
        const auto& right) { return left.evaluation.score > right.evaluation.score; });
    if (ranked.size() > 1U &&
        ranked[0].evaluation.score - ranked[1].evaluation.score <= 5) {
        const auto& first = candidates[ranked[0].index];
        const auto& second = candidates[ranked[1].index];
        const auto firstText = normalizedWords(!first.plainLyrics.empty()
            ? first.plainLyrics : first.syncedLyrics);
        const auto secondText = normalizedWords(!second.plainLyrics.empty()
            ? second.plainLyrics : second.syncedLyrics);
        if (firstText != secondText) {
            result.ambiguous = true;
            addReason(result.reasonCodes, "close_conflicting_candidates");
            return result;
        }
        if (first.syncedLyrics != second.syncedLyrics) {
            ranked[0].evaluation.timingEligible = false;
            addReason(ranked[0].evaluation.reasonCodes,
                "conflicting_timing_static_text");
        }
    }
    result.candidateIndex = ranked[0].index;
    result.evaluation = std::move(ranked[0].evaluation);
    return result;
}

} // namespace nekorox
