#include "master_dsp_history.h"
#include "dsp_meter_paint.h"
#include "device_failure.h"
#include "worker_failure.h"
#include "nekorox/lyrics_match_policy.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void latencyHistoryKeepsEntireCallback()
{
    // Exercise the production capacity, including a callback larger than the
    // remaining room after a 384 kHz / 100 ms rack delay.
    nekorox::goden_audio::FixedDspHistory<65536, 32768, 8> history;
    for (const auto channels : {1U, 2U, 8U}) {
        history.reset();
        std::vector<float> oracle;
        std::uint32_t random = 0x735129;
        for (unsigned block = 0; block < 24; ++block) {
            random = random * 1664525U + 1013904223U;
            const std::size_t frames = block % 3 == 0 ? 32768 :
                1 + (random % 12000);
            const auto start = oracle.size() / channels;
            std::vector<float> input(frames * channels), output(input.size());
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<float>((start * channels + i) % 100003 + 1);
            oracle.insert(oracle.end(), input.begin(), input.end());
            history.push(input.data(), frames, channels);
            for (const auto delay : {0U, 1U, 4096U, 38400U, 65536U}) {
                history.read(delay, output.data(), frames, channels);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    for (std::uint32_t channel = 0; channel < channels; ++channel) {
                        const auto expected = start + frame < delay ? 0.0f :
                            oracle[(start + frame - delay) * channels + channel];
                        require(output[frame * channels + channel] == expected,
                            "REVIEW PDC lost history after whole callback push");
                    }
                }
            }
        }
        history.reset();
        std::array<float, 64> silence{};
        history.read(65536, silence.data(), 8, channels);
        require(std::all_of(silence.begin(), silence.end(),
            [](float value) { return value == 0; }), "REVIEW PDC reset leaked old channels");
    }
}

void localLyricsHaveIndependentDocuments()
{
    nekorox::LyricsMetadata metadata;
    metadata.recording = {"recording-a", 0, "media-1"};
    metadata.metadata = {1};
    metadata.title = "Same title";
    metadata.artist = "Same artist";
    metadata.album = "Same album";
    nekorox::LyricsCandidateV2 candidate;
    candidate.source = {"local", "embedded-plain", "file-revision-v1"};
    candidate.title = metadata.title;
    candidate.artist = metadata.artist;
    candidate.album = metadata.album;
    candidate.plainLyrics = "First recording";
    const auto parse = [&] {
        const auto result = nekorox::parseLyricsDocumentV2(metadata, candidate,
            nekorox::evaluateLyricsCandidate(metadata, candidate));
        require(result.document != nullptr, "REVIEW valid local lyrics rejected");
        return result.document->documentId;
    };
    const auto original = parse();
    require(original.rfind("local:v2:", 0) == 0 && parse() == original,
        "REVIEW local document identity not stable/versioned");
    metadata.recording.entityId = "recording-b";
    require(parse() != original, "REVIEW embedded lyrics collide between tracks");
    metadata.recording.entityId = "recording-a";
    candidate.plainLyrics = "Other recording";
    require(parse() != original, "REVIEW edited local payload retained old ID");
    candidate.source.recordId = "C:/music/shared.lrc";
    candidate.syncedLyrics = "[00:01]Line one\n[00:02]Line two";
    const auto sidecar = parse();
    metadata.recording.subsong = 1;
    require(parse() != sidecar, "REVIEW sidecar subsongs share document identity");
    candidate.source = {"lrclib", "42", "1"};
    require(parse() == "lrclib:42", "REVIEW remote provider IDs changed");
}

void disjointMeterDamageDoesNotHideSemanticDamage()
{
    const RECT client{0, 0, 1000, 720};
    const RECT toolbar{0, 0, 1000, 64}, footer{360, 270, 960, 294};
    const auto update = CreateRectRgnIndirect(&toolbar);
    const auto extra = CreateRectRgnIndirect(&footer);
    require(update && extra, "REVIEW native region allocation failed");
    struct Cleanup { HRGN a, b; ~Cleanup() { DeleteObject(a); DeleteObject(b); } } cleanup{update, extra};
    CombineRgn(update, update, extra, RGN_OR);
    require(nekorox::component::isDspMeterRegionOnly(update, client, footer),
        "REVIEW disjoint meter updates still rebuild controls");
    const RECT parameter{380, 350, 430, 390};
    SetRectRgn(extra, parameter.left, parameter.top, parameter.right, parameter.bottom);
    CombineRgn(update, update, extra, RGN_OR);
    require(!nekorox::component::isDspMeterRegionOnly(update, client, footer),
        "REVIEW semantic damage hidden by pending meter tick");
    SetRectRgn(update, 0, 0, 1000, 720);
    require(!nekorox::component::isDspMeterRegionOnly(update, client, footer),
        "REVIEW full refresh mistaken for meter tick");
}

void deviceFailureClassification()
{
    using nekorox::component::confirmedDeviceLost;
    for (const auto hr : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
            DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR})
        require(confirmedDeviceLost(nullptr, hr), "REVIEW device loss missed");
    for (const auto hr : {S_OK, E_FAIL, E_OUTOFMEMORY, DXGI_STATUS_OCCLUDED,
            DXGI_ERROR_WAS_STILL_DRAWING, D2DERR_RECREATE_TARGET})
        require(!confirmedDeviceLost(nullptr, hr), "REVIEW local error resets shared device");
}
} // namespace

void runFullReviewRegressionTests()
{
    using nekorox::component::workerExitDiagnostic;
    const auto missing = workerExitDiagnostic(L"Analysis", 0xC0000135U);
    require(missing.find(L"0xC0000135") != std::wstring::npos &&
        missing.find(L"Windows N") != std::wstring::npos &&
        missing.find(L"Media Feature Pack") != std::wstring::npos,
        "COMPAT loader failure lost prerequisite or exact status");
    require(workerExitDiagnostic(L"Packet-cost", 0xC000007BU).find(L"x64") != std::wstring::npos,
        "COMPAT wrong architecture error lacks repair guidance");
    require(workerExitDiagnostic(L"Analysis", 0xC000001DU).find(L"CPU") != std::wstring::npos,
        "COMPAT instruction failure lacks CPU guidance");
    require(workerExitDiagnostic(L"Analysis", 123U).find(L"0x0000007B") != std::wstring::npos,
        "COMPAT unknown exit code lost");
    latencyHistoryKeepsEntireCallback();
    localLyricsHaveIndependentDocuments();
    disjointMeterDamageDoesNotHideSemanticDamage();
    deviceFailureClassification();
}
