#include "nekorox/lyrics_match_policy.h"
#include "lrclib_provider.h"
#include "lyrics_local_provider.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace {

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

nekorox::LyricsMetadata wanted(std::string title = "Song")
{
    nekorox::LyricsMetadata value;
    value.recording = {"recording-a", 2U, "media-7"};
    value.metadata = {9U};
    value.title = std::move(title);
    value.artist = "Artist";
    value.album = "Album";
    value.durationSeconds = 180.0;
    return value;
}

nekorox::LyricsCandidateV2 candidate(std::string title = "Song")
{
    nekorox::LyricsCandidateV2 value;
    value.source = {"lrclib", "42", "1"};
    value.title = std::move(title);
    value.artist = "Artist";
    value.album = "Album";
    value.durationSeconds = 180.4;
    value.plainLyrics = "First\nSecond";
    value.syncedLyrics = "[00:10.00]First\n[00:20.00]Second";
    value.sourceDisplayName = "LRCLIB";
    return value;
}

void matchingRejectsWrongVersions()
{
    const auto exactEvaluation = nekorox::evaluateLyricsCandidate(
        wanted(), candidate());
    require(exactEvaluation.score == 100 && exactEvaluation.textEligible &&
        exactEvaluation.timingEligible,
        "LYR03 exact identity did not qualify deterministic timing");

    const auto live = nekorox::evaluateLyricsCandidate(
        wanted("Song (Live at Home)"), candidate("Song (Live elsewhere)"));
    require(!live.textEligible && !live.timingEligible,
        "LYR03 conflicting live recordings were accepted");

    const auto remix = nekorox::evaluateLyricsCandidate(
        wanted(), candidate("Song (Club Remix)"));
    require(!remix.textEligible,
        "LYR03 remix was collapsed into the base recording");

    const auto remaster = nekorox::evaluateLyricsCandidate(
        wanted("Song (2024 Remaster)"), candidate());
    require(remaster.textEligible && !remaster.timingEligible &&
        remaster.remasterUncertain,
        "LYR03 uncertain remaster timing was promoted");

    const auto songNamedLive = nekorox::evaluateLyricsCandidate(
        wanted("Live"), candidate("Live"));
    require(songNamedLive.textEligible,
        "LYR03 a song literally named Live was treated as a suffix");
}

void ambiguousCandidatesAreNotOrderDependent()
{
    auto first = candidate();
    auto second = candidate();
    first.plainLyrics = "one lyric";
    first.syncedLyrics.clear();
    second.plainLyrics = "different lyric";
    second.syncedLyrics.clear();
    const auto selected = nekorox::selectLyricsCandidate(
        wanted(), {first, second});
    require(selected.ambiguous && !selected.candidateIndex,
        "LYR04 close conflicting candidates used provider order");
}

void textualCandidateOutranksInstrumentalClaim()
{
    auto instrumental = candidate();
    instrumental.instrumental = true;
    instrumental.plainLyrics.clear();
    instrumental.syncedLyrics.clear();
    auto text = candidate();
    text.source.recordId = "43";
    const auto selected = nekorox::selectLyricsCandidate(
        wanted(), {instrumental, text});
    require(selected.candidateIndex && *selected.candidateIndex == 1U &&
        !selected.ambiguous,
        "R7-L04 an empty instrumental claim hid a textual candidate");
}

void partialLrcPreservesTextAndDoesNotFakeSync()
{
    auto value = candidate();
    value.plainLyrics = "First\nUntimed bridge\nSecond";
    value.syncedLyrics =
        "[00:10.00]First\nUntimed bridge\n[00:20.00]Second";
    const auto evaluation = nekorox::evaluateLyricsCandidate(wanted(), value);
    const auto parsed = nekorox::parseLyricsDocumentV2(
        wanted(), value, evaluation);
    require(parsed.document && parsed.validPayload && !parsed.completeTiming,
        "LYR05 partial LRC was presented as complete synchronization");
    require(parsed.document->timing == nekorox::TimingStatus::staticText &&
        parsed.document->plainTextUtf8.find("Untimed bridge") !=
            std::string::npos,
        "LYR05 partial LRC discarded untimed text");
}

void introAndSimultaneousGroupsAreExact()
{
    auto value = candidate();
    value.plainLyrics = "Lead\nHarmony\nAgain";
    value.syncedLyrics =
        "[00:10.00]Lead\n[00:10.00]Harmony\n"
        "[00:20.00][00:30.00]Again";
    const auto evaluation = nekorox::evaluateLyricsCandidate(wanted(), value);
    const auto parsed = nekorox::parseLyricsDocumentV2(
        wanted(), value, evaluation);
    require(parsed.document && parsed.completeTiming &&
        parsed.document->cues.size() == 3U &&
        parsed.document->cues.front().simultaneousLines.size() == 2U,
        "LYR06 simultaneous lines or repeated chorus occurrences were lost");
    require(!nekorox::activeLyricsCueIndex(*parsed.document, 9999) &&
        nekorox::activeLyricsCueIndex(*parsed.document, 10000) == 0U,
        "LYR06 intro incorrectly highlighted the first cue");
}

void invalidTimingFallsBackWithoutAudioClaims()
{
    auto value = candidate();
    value.plainLyrics = "Before zero\nPast duration";
    value.syncedLyrics =
        "[offset:-2000]\n[00:01.00]Before zero\n[04:00.00]Past duration";
    const auto evaluation = nekorox::evaluateLyricsCandidate(wanted(), value);
    const auto parsed = nekorox::parseLyricsDocumentV2(
        wanted(), value, evaluation);
    require(parsed.document && !parsed.completeTiming &&
        parsed.document->timing == nekorox::TimingStatus::staticText,
        "LYR07 invalid time bounds remained synchronized");
    require(parsed.document->plainTextUtf8 == value.plainLyrics,
        "LYR07 invalid timing also destroyed valid text");
    require(parsed.document->timing != nekorox::TimingStatus::audioVerified,
        "LYR12 metadata matching fabricated acoustic verification");
}

void payloadLimitsAreFailClosed()
{
    auto value = candidate();
    value.plainLyrics.assign(nekorox::kMaximumLyricsBytes + 1U, 'a');
    value.syncedLyrics.clear();
    const auto parsed = nekorox::parseLyricsDocumentV2(wanted(), value,
        nekorox::evaluateLyricsCandidate(wanted(), value));
    require(!parsed.validPayload && !parsed.document,
        "LYR07 oversized payload was retained");
}

void providerJsonLimitsAreFailClosed()
{
    const std::string valid = R"json({"id":42,"trackName":"Song","artistName":"Artist","albumName":"Album","duration":180.0,"instrumental":false,"plainLyrics":"First\\nSecond","syncedLyrics":"[00:10.00]First"})json";
    std::vector<nekorox::LyricsCandidateV2> candidates;
    require(nekorox::component::parseLrclibResponseForTests(
        valid, candidates) && candidates.size() == 1U &&
        candidates.front().source.recordId == "42",
        "LYR09 valid LRCLIB payload did not parse");

    candidates.clear();
    const std::string invalidCritical = R"json({"id":"42","trackName":"Song","artistName":"Artist","albumName":"Album","duration":180,"instrumental":false,"plainLyrics":"x","syncedLyrics":null})json";
    require(!nekorox::component::parseLrclibResponseForTests(
        invalidCritical, candidates) && candidates.empty(),
        "LYR09 invalid critical field type became a candidate");

    const std::string siblingObjects = "[" + valid + "," + valid + "]";
    candidates = {candidate("stale")};
    require(nekorox::component::parseLrclibResponseForTests(
        siblingObjects, candidates) && candidates.size() == 2U,
        "R7-L01 sibling objects with the same keys were rejected");

    const std::string emptyArray = "[]";
    candidates = {candidate("stale")};
    require(nekorox::component::parseLrclibResponseForTests(
        emptyArray, candidates) && candidates.empty(),
        "R7-L01 empty result was not classified as a valid not-found response");

    candidates.clear();
    const std::string duplicateCritical = R"json({"id":42,"id":43,"trackName":"Song","artistName":"Artist","albumName":"Album","duration":180,"instrumental":false,"plainLyrics":"x","syncedLyrics":null})json";
    require(!nekorox::component::parseLrclibResponseForTests(
        duplicateCritical, candidates) && candidates.empty(),
        "LYR09 duplicate JSON key was silently accepted");

    candidates = {candidate("stale")};
    const std::string escapedDuplicate = R"json({"id":42,"\u0069d":43,"trackName":"Song","artistName":"Artist","duration":180,"instrumental":false,"plainLyrics":"x"})json";
    require(!nekorox::component::parseLrclibResponseForTests(
        escapedDuplicate, candidates) && candidates.empty(),
        "R7-L02 equivalent escaped duplicate key was accepted transactionally");

    std::string tooDeep(33U, '[');
    tooDeep += "0";
    tooDeep.append(33U, ']');
    candidates.clear();
    require(!nekorox::component::parseLrclibResponseForTests(
        tooDeep, candidates),
        "LYR09 JSON depth limit was not enforced before DOM parsing");

    std::string tooMany = "[";
    for (unsigned index = 0; index < 51; ++index) {
        if (index != 0) tooMany.push_back(',');
        tooMany += valid;
    }
    tooMany.push_back(']');
    candidates.clear();
    require(!nekorox::component::parseLrclibResponseForTests(
        tooMany, candidates),
        "LYR09 candidate count limit was not enforced");
}

void localProviderUsesExactRevisionAwareFiles()
{
    wchar_t temporary[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temporary) != 0,
        "LYR10 temporary directory is unavailable");
    const auto root = std::filesystem::path(temporary) /
        (L"goden-r3-lyrics-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root / L"profile" / L"lyrics", ignored);
    require(!ignored, "LYR10 could not create local provider fixture");
    const auto media = root / L"media.flac";
    {
        std::ofstream titleOnly(root / L"profile" / L"lyrics" / L"Song.txt");
        titleOnly << "wrong global fallback";
    }
    nekorox::component::LyricsRequestContext request;
    request.nativePath = media;
    request.profilePath = root / L"profile";
    request.metadata = wanted();
    request.metadata.recording.subsong = 2;
    request.request = {request.metadata.recording, 1};
    nekorox::component::LyricsLocalProvider provider;
    auto result = provider.lookup(request);
    require(result.candidates.empty(),
        "LYR10 global title-only fallback was retained");
    const auto scans=provider.indexBuilds();
    for(unsigned i=0;i<20;++i)provider.lookup(request);
    require(provider.indexBuilds()==scans,"LYR10 unchanged cache was scanned per track");
    std::stop_source stopped;stopped.request_stop();
    require(provider.lookup(request,stopped.get_token()).outcome==nekorox::component::LyricsProviderOutcome::cancelled,
        "LYR10 cancelled lookup still scanned files");
    {
        std::ofstream exact(root / L"profile" / L"lyrics" /
            L"Artist - Song.lrc");
        exact << "[00:01.00]Exact line";
    }
    result = provider.lookup(request);
    require(result.candidates.size() == 1U &&
        !result.candidates.front().syncedLyrics.empty(),
        "LYR10 exact artist-title local file was not refreshed");
    {
        std::ofstream generic(root / L"media.lrc");
        generic << "[00:02.00]Generic subsong line";
    }
    result = provider.lookup(request);
    require(result.candidates.size() == 1U &&
        result.candidates.front().syncedLyrics.empty() &&
        result.candidates.front().plainLyrics == "Generic subsong line",
        "LYR10 generic multi-subsong sidecar inherited trusted timing");
    std::filesystem::remove_all(root, ignored);
}

} // namespace

void runR3LyricsTests()
{
    matchingRejectsWrongVersions();
    ambiguousCandidatesAreNotOrderDependent();
    textualCandidateOutranksInstrumentalClaim();
    partialLrcPreservesTextAndDoesNotFakeSync();
    introAndSimultaneousGroupsAreExact();
    invalidTimingFallsBackWithoutAudioClaims();
    payloadLimitsAreFailClosed();
    providerJsonLimitsAreFailClosed();
    localProviderUsesExactRevisionAwareFiles();
}
