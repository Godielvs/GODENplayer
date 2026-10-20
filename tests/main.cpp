#include "nekorox/art_palette.h"
#include "nekorox/ambient_features.h"
#include "nekorox/artwork_resampler.h"
#include "nekorox/compression.h"
#include "nekorox/contracts.h"
#include "nekorox/continuity.h"
#include "nekorox/crt_xy_model.h"
#include "nekorox/database.h"
#include "nekorox/glyph_flow.h"
#include "nekorox/lyrics_timeline.h"
#include "nekorox/packet_cost.h"
#include "nekorox/pause_intent.h"
#include "nekorox/playback_presentation_gate.h"
#include "nekorox/presentation_timing.h"
#include "nekorox/public_branding.h"
#include "nekorox/qa_contracts.h"
#include "nekorox/realtime_visual_model.h"
#include "nekorox/realtime_ring.h"
#include "nekorox/scroll_controller.h"
#include "nekorox/scrub_crossfade.h"
#include "nekorox/track_identity.h"
#include "nekorox/visual_sample_guard.h"
#include "nekorox/waveform_pyramid.h"
#include "../src/component/daw_output_backend.h"
#include "../src/component/listen_lifecycle.h"
#include "../src/component/interaction_latency_guard.h"
#include "../src/component/ambient_texture_scheduler.h"
#include "../src/component/a4/a4_budget.h"
#include "../src/component/curated/curated_core.h"
#include "../src/component/collection/collection_identity.h"
#include "../src/component/performance_telemetry.h"
#include "../src/component/rollout_flags.h"
#include "../src/component/discord_relay_client.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <winhttp.h>

void runWorkpackLayoutTests();
void runWorkpackRetentionTests();
void runWorkpackScrollTests();
void runAlbumScrollTests();
void runWorkpackPresentationTests();
void runWorkpackPcmTests();
void runWorkpackLifetimeTests();
void runWorkpackDspWakeTests();
void runWorkpackIdentityTests();
void runDatabaseInitializationTests();
int runDatabaseInitializationChild(const char* token);
void runWorkpackTagTests();
void runWorkpackImportTests();
void runWorkpackShellTests();
void runWorkpackAlbumSessionTests();
void runWorkpackAlbumViewTests();
void runWorkpackBookletTests();
void runWorkpackScrubTests();
void runWorkpackAuditionTests();
void runWorkpackScopeTests();
void runWorkpackSpectrumTests();
void runWorkpackArtworkTests();
void runR3ProbeTests();
void runR3TagIntegrityTests();
void runR3LifetimeTests();
void runR3AuditionTests();
void runR3LyricsTests();
void runC6HistoryTests();
void runVisualRefinementTests();
void runCorrectiveR2PacingTests();
void runCorrectiveR2AlbumSessionTests();
void runCorrectiveR2ShellIntegrationTests();
void runCorrectiveR2TagSelectionTests();
void runCorrectiveR2TagMediaTests();
void runCorrectiveR2ListenTests();
void runCorrectiveR2AuditionTests();
void runPixelUiDataTests();
void runR6AmbientFeaturesTests();
void runR7DynamicsTests();
void runC11CoreTests();
void runC13ResponseTests();
void runSpectrumRenderCacheTests();

namespace {

void require(bool condition, std::string message,
    const std::source_location location = std::source_location::current())
{
    if (condition) return;
    if (message.empty()) {
        message = std::string(location.file_name()) + ":" +
            std::to_string(location.line()) + " assertion failed";
    }
    throw std::runtime_error(std::move(message));
}

void testPerformanceInputPresentationMarkers()
{
    using nekorox::component::PerformanceTelemetry;
    using nekorox::component::TelemetrySurface;
    auto& telemetry = PerformanceTelemetry::instance();
    const auto before = telemetry.snapshot(TelemetrySurface::workspace);
    telemetry.input(TelemetrySurface::workspace);
    telemetry.inputIncorporated(TelemetrySurface::workspace);
    telemetry.present(TelemetrySurface::workspace, S_OK, true);
    const auto refused = telemetry.snapshot(TelemetrySurface::workspace);
    require(refused.inputToPresentSamples ==
            before.inputToPresentSamples,
        "refused frame consumed pending input");
    telemetry.present(TelemetrySurface::workspace, S_OK, false);
    const auto accepted = telemetry.snapshot(TelemetrySurface::workspace);
    require(accepted.inputToPresentSamples == before.inputToPresentSamples,
        "accepted Present was incorrectly treated as externally displayed");
    telemetry.displayed(TelemetrySurface::workspace, 1U);
    const auto displayed = telemetry.snapshot(TelemetrySurface::workspace);
    require(displayed.inputToPresentSamples ==
            before.inputToPresentSamples + 1U,
        "displayed frame did not consume pending incorporated input once");
    telemetry.displayed(TelemetrySurface::workspace, 1U);
    const auto repeated = telemetry.snapshot(TelemetrySurface::workspace);
    require(repeated.inputToPresentSamples ==
            displayed.inputToPresentSamples,
        "pending input was associated with more than one displayed frame");
}

std::uint8_t crc8(const std::vector<std::uint8_t>& bytes)
{
    std::uint8_t crc = 0;
    for (const auto value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0
                ? static_cast<std::uint8_t>((crc << 1U) ^ 0x07U)
                : static_cast<std::uint8_t>(crc << 1U);
        }
    }
    return crc;
}

std::uint16_t crc16(const std::vector<std::uint8_t>& bytes)
{
    std::uint16_t crc = 0;
    for (const auto value : bytes) {
        crc ^= static_cast<std::uint16_t>(value) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x8005U)
                : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

void appendFlacFrame(std::vector<std::uint8_t>& out, std::uint8_t number)
{
    std::vector<std::uint8_t> frame{0xFF, 0xF8, 0x19, 0x18, number};
    frame.push_back(crc8(frame));
    for (std::uint8_t i = 0; i < 20; ++i) frame.push_back(i + number);
    const auto checksum = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(checksum >> 8U));
    frame.push_back(static_cast<std::uint8_t>(checksum & 0xFFU));
    out.insert(out.end(), frame.begin(), frame.end());
}

std::vector<std::uint8_t> makeFlac()
{
    std::vector<std::uint8_t> bytes{'f', 'L', 'a', 'C'};
    bytes.insert(bytes.end(), {0x80, 0x00, 0x00, 0x22});
    std::array<std::uint8_t, 34> info{};
    info[0] = 0x00; info[1] = 0xC0;
    info[2] = 0x00; info[3] = 0xC0;
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(44100) << 44U) |
        (static_cast<std::uint64_t>(1) << 41U) |
        (static_cast<std::uint64_t>(15) << 36U) |
        384U;
    for (std::size_t i = 0; i < 8; ++i) {
        info[10 + i] = static_cast<std::uint8_t>(packed >> ((7 - i) * 8U));
    }
    bytes.insert(bytes.end(), info.begin(), info.end());
    appendFlacFrame(bytes, 0);
    appendFlacFrame(bytes, 1);
    return bytes;
}

std::vector<std::uint8_t> makeMp3()
{
    constexpr std::size_t frameBytes = 417;
    std::vector<std::uint8_t> bytes(frameBytes * 3, 0);
    for (std::size_t frame = 0; frame < 3; ++frame) {
        const auto at = frame * frameBytes;
        bytes[at] = 0xFF;
        bytes[at + 1] = 0xFB;
        bytes[at + 2] = 0x90;
        bytes[at + 3] = 0x00;
    }
    return bytes;
}

void appendBig32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendLittle16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendLittle32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
}

std::vector<std::uint8_t> makeAdts()
{
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t frameBytes : {40U, 60U, 45U}) {
        bytes.insert(bytes.end(), {0xFF, 0xF1, 0x4C,
            static_cast<std::uint8_t>(0x80U | (frameBytes >> 11U)),
            static_cast<std::uint8_t>(frameBytes >> 3U),
            static_cast<std::uint8_t>((frameBytes << 5U) | 0x1FU), 0xFC});
        bytes.resize(bytes.size() + frameBytes - 7U, 0x55);
    }
    return bytes;
}

std::vector<std::uint8_t> makeWave()
{
    constexpr std::uint32_t dataBytes = 9600;
    std::vector<std::uint8_t> bytes{'R','I','F','F'};
    appendLittle32(bytes, 36U + dataBytes);
    bytes.insert(bytes.end(), {'W','A','V','E','f','m','t',' '});
    appendLittle32(bytes, 16U);
    appendLittle16(bytes, 1U);
    appendLittle16(bytes, 1U);
    appendLittle32(bytes, 48000U);
    appendLittle32(bytes, 96000U);
    appendLittle16(bytes, 2U);
    appendLittle16(bytes, 16U);
    bytes.insert(bytes.end(), {'d','a','t','a'});
    appendLittle32(bytes, dataBytes);
    bytes.resize(bytes.size() + dataBytes, 0x11);
    return bytes;
}

std::vector<std::uint8_t> isoBox(std::string_view type,
    const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> box;
    appendBig32(box, static_cast<std::uint32_t>(8U + payload.size()));
    box.insert(box.end(), type.begin(), type.end());
    box.insert(box.end(), payload.begin(), payload.end());
    return box;
}

void appendBytes(std::vector<std::uint8_t>& out,
    const std::vector<std::uint8_t>& value)
{
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> makeIsoBmff()
{
    std::vector<std::uint8_t> ftypPayload{'M','4','A',' ',0,0,0,0,'i','s','o','m'};
    std::vector<std::uint8_t> mdhd(12U, 0U);
    appendBig32(mdhd, 48000U);
    appendBig32(mdhd, 3072U);
    std::vector<std::uint8_t> hdlr(8U, 0U);
    hdlr.insert(hdlr.end(), {'s','o','u','n'});
    std::vector<std::uint8_t> stsz(8U, 0U);
    appendBig32(stsz, 3U);
    appendBig32(stsz, 120U);
    appendBig32(stsz, 180U);
    appendBig32(stsz, 140U);
    std::vector<std::uint8_t> stts(4U, 0U);
    appendBig32(stts, 1U);
    appendBig32(stts, 3U);
    appendBig32(stts, 1024U);
    std::vector<std::uint8_t> stbl;
    appendBytes(stbl, isoBox("stsz", stsz));
    appendBytes(stbl, isoBox("stts", stts));
    std::vector<std::uint8_t> minf;
    appendBytes(minf, isoBox("stbl", stbl));
    std::vector<std::uint8_t> mdia;
    appendBytes(mdia, isoBox("mdhd", mdhd));
    appendBytes(mdia, isoBox("hdlr", hdlr));
    appendBytes(mdia, isoBox("minf", minf));
    std::vector<std::uint8_t> trak;
    appendBytes(trak, isoBox("mdia", mdia));
    std::vector<std::uint8_t> moov;
    appendBytes(moov, isoBox("trak", trak));
    std::vector<std::uint8_t> file;
    appendBytes(file, isoBox("ftyp", ftypPayload));
    appendBytes(file, isoBox("moov", moov));
    return file;
}

void testRealtimeRing()
{
    auto ring = std::make_unique<nekorox::RealtimeRing<8>>();
    std::array<float, 8> samples{};
    for (std::uint64_t sequence = 1; sequence <= 40; ++sequence) {
        samples.fill(static_cast<float>(sequence));
        require(ring->publish(samples.data(), samples.size(), 48000, 2, 4,
                    sequence * 100, sequence / 10.0, 1),
            "Ring publish rejected valid PCM");
    }
    auto output = std::make_unique<nekorox::RealtimeFrameV1>();
    std::uint64_t last = 0;
    std::uint64_t skipped = 0;
    require(ring->consumeLatest(*output, last, &skipped),
        "Ring did not expose a complete frame");
    require(output->sequence == 40 && output->samples[0] == 40.0F,
        "Ring did not return newest frame");
    require(skipped == 39 && ring->overwrittenCount() > 0,
        "Ring did not account for superseded frames");

    auto ordered = std::make_unique<nekorox::RealtimeRing<8>>();
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        samples.fill(static_cast<float>(sequence));
        require(ordered->publish(samples.data(), samples.size(), 48000, 2, 4,
                    sequence * 100, sequence / 10.0, 1),
            "Ordered ring rejected valid PCM");
    }
    last = 0;
    for (std::uint64_t expected = 1; expected <= 3; ++expected) {
        require(ordered->consumeNext(*output, last, &skipped),
            "Ordered ring did not expose its next frame");
        require(output->sequence == expected && skipped == 0,
            "Ordered ring skipped a queued frame");
    }
}

void testRealtimeFrameBus()
{
    auto bus = std::make_unique<nekorox::RealtimeFrameBus<8>>();
    std::array<float, 8> samples{};
    for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
        samples.fill(static_cast<float>(sequence));
        require(bus->publish(samples.data(), samples.size(), 48000, 2, 4,
                    sequence * 100, sequence / 10.0, 1,
                    9, sequence * 4, 77, 0xabc),
            "Frame bus rejected valid PCM");
    }

    auto first = std::make_unique<nekorox::RealtimeFrameV1>();
    auto second = std::make_unique<nekorox::RealtimeFrameV1>();
    std::uint64_t firstCursor = 0;
    std::uint64_t secondCursor = 0;
    std::uint64_t skipped = 0;
    for (std::uint64_t expected = 1; expected <= 6; ++expected) {
        require(bus->consumeNext(*first, firstCursor, &skipped),
            "First frame-bus subscriber lost a queued frame");
        require(first->sequence == expected && skipped == 0,
            "First frame-bus subscriber observed the wrong sequence");
        require(first->playbackSerial == 77 &&
                first->recordingIdentityHash == 0xabc,
            "First frame-bus subscriber lost playback admission identity");
    }
    for (std::uint64_t expected = 1; expected <= 6; ++expected) {
        require(bus->consumeNext(*second, secondCursor, &skipped),
            "Second frame-bus subscriber was starved by the first");
        require(second->sequence == expected && skipped == 0,
            "Second frame-bus subscriber observed the wrong sequence");
        require(second->playbackSerial == 77 &&
                second->recordingIdentityHash == 0xabc,
            "Second frame-bus subscriber lost playback admission identity");
    }
    require(bus->publishedSequence() == 6,
        "Frame bus did not expose its latest cursor");
}

void testDiscontinuityCommitLatch()
{
    nekorox::DiscontinuityLatch latch;
    latch.mark(nekorox::DiscontinuityReason::seek, 41, 900);
    const auto firstAttempt = latch.pending();
    require(firstAttempt.reasonMask ==
            nekorox::reasonMask(nekorox::DiscontinuityReason::seek) &&
            !firstAttempt.metadataCoherent &&
            firstAttempt.firstSequence == 0 && firstAttempt.firstQpc == 0,
        "C10 latch did not expose the pending authoritative boundary");
    latch.rejected();
    latch.rejected();
    const auto retry = latch.pending();
    require(retry.reasonMask == firstAttempt.reasonMask &&
            retry.rejectedAttempts == 2,
        "C10 rejected publication consumed discontinuity semantics");
    latch.mark(nekorox::DiscontinuityReason::backlogLoss, 42, 950);
    const auto combined = latch.pending();
    require((combined.reasonMask &
            nekorox::reasonMask(nekorox::DiscontinuityReason::seek)) != 0 &&
            (combined.reasonMask &
            nekorox::reasonMask(nekorox::DiscontinuityReason::backlogLoss)) != 0,
        "C10 concurrent discontinuity reasons were not combined");
    latch.commit(combined);
    require(!latch.pending(),
        "C10 accepted publication repeated a committed discontinuity");

    latch.mark(nekorox::DiscontinuityReason::seek);
    const auto olderSeek = latch.pending();
    latch.mark(nekorox::DiscontinuityReason::seek);
    latch.commit(olderSeek);
    const auto newerSeek = latch.pending();
    require(newerSeek.reasonMask ==
            nekorox::reasonMask(nekorox::DiscontinuityReason::seek),
        "C11 older commit consumed a newer occurrence of the same reason");
    latch.commit(newerSeek);
    require(!latch.pending(),
        "C11 newer same-reason occurrence did not commit independently");
}

std::unique_ptr<nekorox::RealtimeFrameV1> makeStereoSignal(
    std::uint64_t sequence,
    double playbackSeconds,
    float rightPolarity = 1.0f)
{
    auto frame = std::make_unique<nekorox::RealtimeFrameV1>();
    frame->sequence = sequence;
    frame->qpcTicks = sequence * 100;
    frame->playbackSeconds = playbackSeconds;
    frame->sampleRate = 48000;
    frame->channels = 2;
    frame->frameCount = 2048;
    frame->sampleCount = frame->frameCount * frame->channels;
    for (std::uint32_t index = 0; index < frame->frameCount; ++index) {
        const auto phase = 2.0 * std::numbers::pi * 440.0 * index /
            static_cast<double>(frame->sampleRate);
        const auto value = static_cast<float>(std::sin(phase) * 0.65);
        frame->samples[index * 2U] = value;
        frame->samples[index * 2U + 1U] = value * rightPolarity;
    }
    return frame;
}

std::unique_ptr<nekorox::RealtimeFrameV1> makeStereoChunk(
    std::uint64_t sequence,
    std::uint64_t firstSample,
    double frequency,
    std::uint32_t frameCount = 400,
    float rightPolarity = 1.0f,
    float amplitude = 0.65f)
{
    auto frame = std::make_unique<nekorox::RealtimeFrameV1>();
    frame->sequence = sequence;
    frame->qpcTicks = sequence * 100;
    frame->playbackSeconds = static_cast<double>(firstSample) / 48000.0;
    frame->sampleRate = 48000;
    frame->channels = 2;
    frame->frameCount = frameCount;
    frame->sampleCount = frame->frameCount * frame->channels;
    for (std::uint32_t index = 0; index < frame->frameCount; ++index) {
        const auto phase = 2.0 * std::numbers::pi * frequency *
            static_cast<double>(firstSample + index) /
            static_cast<double>(frame->sampleRate);
        const auto value = static_cast<float>(std::sin(phase) * amplitude);
        frame->samples[index * 2U] = value;
        frame->samples[index * 2U + 1U] = value * rightPolarity;
    }
    return frame;
}

std::unique_ptr<nekorox::RealtimeFrameV1> makeCrtFrame(
    std::uint64_t sequence,
    std::uint32_t sampleRate,
    std::span<const float> left,
    std::span<const float> right)
{
    require(left.size() == right.size() &&
            !left.empty() &&
            left.size() <= nekorox::kRealtimeMaxFrames,
        "Invalid CRT test frame");
    auto frame = std::make_unique<nekorox::RealtimeFrameV1>();
    frame->sequence = sequence;
    frame->sampleRate = sampleRate;
    frame->channels = 2;
    frame->frameCount = static_cast<std::uint32_t>(left.size());
    frame->sampleCount = frame->frameCount * frame->channels;
    for (std::size_t index = 0; index < left.size(); ++index) {
        frame->samples[index * 2U] = left[index];
        frame->samples[index * 2U + 1U] = right[index];
    }
    return frame;
}

bool sameBeamSegment(
    const nekorox::CrtBeamSegment& first,
    const nekorox::CrtBeamSegment& second)
{
    return first.x0 == second.x0 &&
        first.y0 == second.y0 &&
        first.x1 == second.x1 &&
        first.y1 == second.y1 &&
        first.charge == second.charge;
}

void testCrtXyModel()
{
    require(sizeof(nekorox::CrtBeamSegment) == 24,
        "CRT beam segment is not the renderer's fixed 24-byte layout");

    constexpr std::array rawLeft{-1.25f, 0.25f, 1.50f};
    constexpr std::array rawRight{2.00f, -0.50f, -2.25f};
    nekorox::CrtXyModel rawModel;
    auto rawFrame = makeCrtFrame(1, 48000, rawLeft, rawRight);
    const auto raw = rawModel.ingest(*rawFrame, false);
    require(raw.size() == 2,
        "CRT X-Y did not emit one segment per sample interval");
    require(raw[0].x0 == rawLeft[0] && raw[0].y0 == rawRight[0] &&
            raw[0].x1 == rawLeft[1] && raw[0].y1 == rawRight[1] &&
            raw[1].x1 == rawLeft[2] && raw[1].y1 == rawRight[2],
        "CRT X-Y changed the raw X=L/Y=R mapping");
    require(raw[0].x0 < -1.0f && raw[0].y0 > 1.0f &&
            raw[1].x1 > 1.0f && raw[1].y1 < -1.0f,
        "CRT X-Y clamped or normalized raw channel values");
    const auto expectedCharge =
        static_cast<float>(1.0 / static_cast<double>(rawFrame->sampleRate));
    require(raw[0].charge == expectedCharge &&
            raw[1].charge == expectedCharge,
        "CRT X-Y segment charge is not one sample period");
    nekorox::CrtXyModel agedModel;
    const auto aged = agedModel.ingest(*rawFrame, false, 0.025);
    require(aged.size() == 2 &&
            aged[0].ageSeconds > aged[1].ageSeconds &&
            aged[1].ageSeconds > 0.025f,
        "CRT X-Y did not preserve sample-relative beam age");
    require(rawModel.stats().samples == rawLeft.size() &&
            rawModel.stats().segments == rawLeft.size() - 1U &&
            rawModel.stats().brokenPaths == 0,
        "CRT X-Y raw-ingest statistics are incorrect");

    constexpr std::array orderedLeft{
        -0.8f, -0.4f, 0.1f, 0.7f, 0.3f, -0.2f, -0.9f, 0.6f};
    constexpr std::array orderedRight{
        0.5f, -0.7f, -0.1f, 0.9f, -0.3f, 0.2f, 0.8f, -0.6f};
    nekorox::CrtXyModel wholeModel;
    auto wholeFrame = makeCrtFrame(
        2, 44100, orderedLeft, orderedRight);
    const auto wholeView = wholeModel.ingest(*wholeFrame, false);
    const std::vector<nekorox::CrtBeamSegment> wholeSegments(
        wholeView.begin(), wholeView.end());

    nekorox::CrtXyModel chunkedModel;
    std::vector<nekorox::CrtBeamSegment> chunkedSegments;
    chunkedSegments.reserve(orderedLeft.size() - 1U);
    constexpr std::array<std::size_t, 4> chunkSizes{1U, 3U, 2U, 2U};
    std::size_t offset = 0;
    std::uint64_t sequence = 10;
    for (const auto chunkSize : chunkSizes) {
        const auto left = std::span<const float>(orderedLeft).subspan(
            offset, chunkSize);
        const auto right = std::span<const float>(orderedRight).subspan(
            offset, chunkSize);
        auto frame = makeCrtFrame(sequence++, 44100, left, right);
        const auto emitted = chunkedModel.ingest(*frame, false);
        chunkedSegments.insert(
            chunkedSegments.end(), emitted.begin(), emitted.end());
        offset += chunkSize;
    }
    require(wholeSegments.size() == orderedLeft.size() - 1U &&
            chunkedSegments.size() == wholeSegments.size(),
        "CRT X-Y output depends on chunk boundaries");
    for (std::size_t index = 0; index < wholeSegments.size(); ++index) {
        require(sameBeamSegment(
                wholeSegments[index], chunkedSegments[index]),
            "CRT X-Y changed segment geometry or order across chunks");
    }
    require(chunkedModel.stats().samples == orderedLeft.size() &&
            chunkedModel.stats().segments == orderedLeft.size() - 1U &&
            chunkedModel.stats().brokenPaths == 0,
        "CRT X-Y chunked-ingest statistics are incorrect");

    const auto primitive = [](
        const std::array<float, 2>& left,
        const std::array<float, 2>& right) {
        nekorox::CrtXyModel model;
        auto frame = makeCrtFrame(20, 48000, left, right);
        const auto segments = model.ingest(*frame, false);
        require(segments.size() == 1,
            "CRT X-Y stereo primitive did not produce one segment");
        return segments.front();
    };
    const auto mono = primitive(
        std::array{-0.75f, 0.75f},
        std::array{-0.75f, 0.75f});
    require(mono.x0 == mono.y0 && mono.x1 == mono.y1,
        "CRT X-Y mono primitive is not the X=Y diagonal");
    const auto antiphase = primitive(
        std::array{-0.75f, 0.75f},
        std::array{0.75f, -0.75f});
    require(antiphase.x0 == -antiphase.y0 &&
            antiphase.x1 == -antiphase.y1,
        "CRT X-Y antiphase primitive is not the X=-Y diagonal");
    const auto leftOnly = primitive(
        std::array{-0.75f, 0.75f},
        std::array{0.0f, 0.0f});
    require(leftOnly.y0 == 0.0f && leftOnly.y1 == 0.0f,
        "CRT X-Y left-only primitive is not horizontal");
    const auto rightOnly = primitive(
        std::array{0.0f, 0.0f},
        std::array{-0.75f, 0.75f});
    require(rightOnly.x0 == 0.0f && rightOnly.x1 == 0.0f,
        "CRT X-Y right-only primitive is not vertical");

    nekorox::CrtXyModel discontinuousModel;
    constexpr std::array beforeLeft{0.10f, 0.20f};
    constexpr std::array beforeRight{0.30f, 0.40f};
    constexpr std::array afterLeft{-0.90f, -0.80f};
    constexpr std::array afterRight{0.90f, 0.80f};
    auto beforeFrame = makeCrtFrame(
        30, 48000, beforeLeft, beforeRight);
    require(discontinuousModel.ingest(*beforeFrame, false).size() == 1,
        "CRT X-Y rejected the path before a discontinuity");
    auto afterFrame = makeCrtFrame(
        31, 48000, afterLeft, afterRight);
    const auto afterBreak =
        discontinuousModel.ingest(*afterFrame, true);
    require(afterBreak.size() == 1 &&
            afterBreak[0].x0 == afterLeft[0] &&
            afterBreak[0].y0 == afterRight[0] &&
            afterBreak[0].x1 == afterLeft[1] &&
            afterBreak[0].y1 == afterRight[1],
        "CRT X-Y emitted a retrace across an explicit discontinuity");
    require(discontinuousModel.stats().samples == 4 &&
            discontinuousModel.stats().segments == 2 &&
            discontinuousModel.stats().brokenPaths == 1,
        "CRT X-Y discontinuity statistics are incorrect");

    nekorox::CrtXyModel nonFiniteModel;
    constexpr std::array nonFiniteLeft{
        0.0f, 0.25f, std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.75f};
    constexpr std::array nonFiniteRight{0.0f, 0.25f, 0.5f, 0.5f, 0.75f};
    auto nonFiniteFrame = makeCrtFrame(
        32, 48000, nonFiniteLeft, nonFiniteRight);
    const auto finiteSegments =
        nonFiniteModel.ingest(*nonFiniteFrame, false);
    require(finiteSegments.size() == 2 &&
            finiteSegments[0].x0 == 0.0f &&
            finiteSegments[0].x1 == 0.25f &&
            finiteSegments[1].x0 == 0.5f &&
            finiteSegments[1].x1 == 0.75f &&
            nonFiniteModel.stats().brokenPaths == 1,
        "CRT X-Y did not isolate a non-finite PCM discontinuity");

    const nekorox::CrtXyRasterSpec rasterSpec{
        5U, 5U, -1.0, 1.0, -1.0, 1.0};
    const std::array<nekorox::CrtBeamSegment, 2> cross{{
        {-0.8f, 0.0f, 0.8f, 0.0f, 0.5f},
        {0.0f, -0.8f, 0.0f, 0.8f, 0.5f},
    }};
    const auto crossRaster = nekorox::rasterizeCrtXyReference(
        std::span<const nekorox::CrtBeamSegment>(cross), rasterSpec);
    require(crossRaster.valid() &&
            std::abs(crossRaster.totalEnergy() - 1.0) < 1e-12 &&
            std::abs(crossRaster.visibleCharge - 1.0) < 1e-12,
        "CRT reference rasterizer does not conserve visible energy");
    for (std::size_t y = 0; y < rasterSpec.height; ++y) {
        for (std::size_t x = 0; x < rasterSpec.width; ++x) {
            const auto onCross = x == 2U || y == 2U;
            require(onCross
                    ? crossRaster.pixel(x, y) > 0.0
                    : crossRaster.pixel(x, y) == 0.0,
                "CRT reference rasterizer changed line geometry");
        }
    }
    require(crossRaster.pixel(2U, 2U) >
            crossRaster.pixel(0U, 2U),
        "CRT reference rasterizer did not accumulate crossing dwell");

    const std::array<nekorox::CrtBeamSegment, 1> dwellSegment{{
        {0.0f, 0.0f, 0.0f, 0.0f, 0.25f},
    }};
    const auto dwellRaster = nekorox::rasterizeCrtXyReference(
        std::span<const nekorox::CrtBeamSegment>(dwellSegment),
        rasterSpec);
    require(std::abs(dwellRaster.totalEnergy() - 0.25) < 1e-12 &&
            std::abs(dwellRaster.pixel(2U, 2U) - 0.25) < 1e-12,
        "CRT reference rasterizer lost stationary-beam dwell");

    const std::array<nekorox::CrtBeamSegment, 1> forwardSegment{{
        {-0.8f, -0.4f, 0.8f, -0.4f, 0.25f},
    }};
    const std::array<nekorox::CrtBeamSegment, 1> reverseSegment{{
        {0.8f, -0.4f, -0.8f, -0.4f, 0.25f},
    }};
    const auto forwardRaster = nekorox::rasterizeCrtXyReference(
        std::span<const nekorox::CrtBeamSegment>(forwardSegment),
        rasterSpec);
    const auto reverseRaster = nekorox::rasterizeCrtXyReference(
        std::span<const nekorox::CrtBeamSegment>(reverseSegment),
        rasterSpec);
    require(std::abs(forwardRaster.totalEnergy() - 0.25) < 1e-12 &&
            std::abs(reverseRaster.totalEnergy() - 0.25) < 1e-12,
        "CRT raster energy incorrectly scales with trace length");
    for (std::size_t index = 0;
         index < forwardRaster.energy.size();
         ++index) {
        require(std::abs(
                forwardRaster.energy[index] -
                reverseRaster.energy[index]) < 1e-12,
            "CRT retrace direction changed deposited energy");
    }
    const std::array<nekorox::CrtBeamSegment, 2> retrace{{
        forwardSegment[0],
        reverseSegment[0],
    }};
    const auto retraceRaster = nekorox::rasterizeCrtXyReference(
        std::span<const nekorox::CrtBeamSegment>(retrace), rasterSpec);
    require(std::abs(retraceRaster.totalEnergy() - 0.5) < 1e-12,
        "CRT retrace energy is not the sum of sample-period charge");

    nekorox::CrtXyModel gapModel;
    constexpr std::array gapLeftA{-0.9f};
    constexpr std::array gapRightA{-0.9f};
    constexpr std::array gapLeftB{0.9f};
    constexpr std::array gapRightB{0.9f};
    auto gapFrameA = makeCrtFrame(40, 48000, gapLeftA, gapRightA);
    require(gapModel.ingest(*gapFrameA, false).empty(),
        "CRT X-Y emitted a segment for a single initial sample");
    auto gapFrameB = makeCrtFrame(41, 48000, gapLeftB, gapRightB);
    const auto gapSegments = gapModel.ingest(*gapFrameB, true);
    require(gapSegments.empty() &&
            gapModel.stats().brokenPaths == 1 &&
            gapModel.stats().segments == 0,
        "CRT X-Y charged a discontinuity retrace");
    const auto gapRaster = nekorox::rasterizeCrtXyReference(
        gapSegments, rasterSpec);
    require(gapRaster.totalEnergy() == 0.0,
        "CRT discontinuity created raster energy");

    double decayFactor = -1.0;
    require(nekorox::crtXyDecayFactor(0.25, 0.5, decayFactor) &&
            std::abs(decayFactor - std::exp(-0.5)) < 1e-15,
        "CRT decay factor is not exp(-dt/tau)");
    std::array<double, 3> decayEnergy{1.0, 0.5, 0.25};
    require(nekorox::decayCrtXyRaster(
            decayEnergy, 0.25, 0.5),
        "CRT decay rejected valid parameters");
    require(std::abs(decayEnergy[0] - decayFactor) < 1e-15 &&
            std::abs(decayEnergy[1] - 0.5 * decayFactor) < 1e-15 &&
            std::abs(decayEnergy[2] - 0.25 * decayFactor) < 1e-15,
        "CRT raster decay applied the wrong factor");
    const auto unchangedEnergy = decayEnergy;
    require(!nekorox::decayCrtXyRaster(
                decayEnergy, -0.01, 0.5) &&
            decayEnergy == unchangedEnergy,
        "CRT decay mutated energy after invalid parameters");
    double invalidFactor = 7.0;
    require(!nekorox::crtXyDecayFactor(
                0.25, 0.0, invalidFactor) &&
            invalidFactor == 7.0 &&
            !nekorox::validCrtXyDecay(
                std::numeric_limits<double>::quiet_NaN(), 0.5) &&
            !nekorox::validCrtXyDecay(
                0.25, std::numeric_limits<double>::infinity()),
        "CRT decay accepted a non-finite or non-positive parameter");

    struct ChargeRun {
        float segmentCharge = 0.0f;
        nekorox::CrtXyStats stats{};
        double totalEnergy = 0.0;
        std::vector<double> energy{};
    };
    const auto runAtSampleRate = [&](std::uint32_t sampleRate) {
        nekorox::CrtXyModel model;
        std::vector<nekorox::CrtBeamSegment> collected;
        collected.reserve(sampleRate);
        const auto totalSamples =
            static_cast<std::uint64_t>(sampleRate) + 1U;
        std::uint64_t firstSample = 0;
        std::uint64_t frameSequence = 100;
        while (firstSample < totalSamples) {
            const auto frameCount = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    nekorox::kRealtimeMaxFrames,
                    totalSamples - firstSample));
            auto frame = std::make_unique<nekorox::RealtimeFrameV1>();
            frame->sequence = frameSequence++;
            frame->sampleRate = sampleRate;
            frame->channels = 2;
            frame->frameCount = frameCount;
            frame->sampleCount = frameCount * frame->channels;
            for (std::uint32_t index = 0; index < frameCount; ++index) {
                const auto absolute = firstSample + index;
                const auto amount = static_cast<double>(absolute) /
                    static_cast<double>(sampleRate);
                frame->samples[index * 2U] =
                    static_cast<float>(-0.8 + 1.6 * amount);
                frame->samples[index * 2U + 1U] = 0.25f;
            }
            const auto emitted = model.ingest(*frame, false);
            collected.insert(
                collected.end(), emitted.begin(), emitted.end());
            firstSample += frameCount;
        }
        require(collected.size() == sampleRate,
            "CRT X-Y sample-rate fixture lost sample intervals");
        const nekorox::CrtXyRasterSpec chargeSpec{
            17U, 5U, -1.0, 1.0, -1.0, 1.0};
        const auto raster = nekorox::rasterizeCrtXyReference(
            std::span<const nekorox::CrtBeamSegment>(collected),
            chargeSpec);
        require(raster.valid(),
            "CRT sample-rate fixture did not rasterize");
        ChargeRun run;
        run.segmentCharge = collected.front().charge;
        run.stats = model.stats();
        run.totalEnergy = raster.totalEnergy();
        run.energy = raster.energy;
        return run;
    };
    const auto charge48k = runAtSampleRate(48000);
    const auto charge96k = runAtSampleRate(96000);
    require(std::abs(
            static_cast<double>(charge48k.segmentCharge) * 48000.0 -
            1.0) < 5e-8 &&
            std::abs(
                static_cast<double>(charge96k.segmentCharge) * 96000.0 -
                1.0) < 5e-8,
        "CRT segment charge is not proportional to 1/sampleRate");
    require(charge48k.stats.samples == 48001 &&
            charge48k.stats.segments == 48000 &&
            charge96k.stats.samples == 96001 &&
            charge96k.stats.segments == 96000,
        "CRT sample-rate statistics lost chunk continuity");
    require(std::abs(charge48k.totalEnergy - 1.0) < 5e-8 &&
            std::abs(charge96k.totalEnergy - 1.0) < 5e-8 &&
            std::abs(
                charge48k.totalEnergy -
                charge96k.totalEnergy) < 5e-8,
        "CRT total charge changes with sample rate");
    require(charge48k.energy.size() == charge96k.energy.size(),
        "CRT sample-rate rasters have different geometry");
    for (std::size_t index = 0; index < charge48k.energy.size(); ++index) {
        require(std::abs(
                charge48k.energy[index] -
                charge96k.energy[index]) < 2e-6,
            "CRT dwell distribution changes with sample rate");
    }
}

void testRealtimeVisualModel()
{
    require(nekorox::boundedVisualSample(0.75f) == 0.75f &&
            nekorox::boundedVisualSample(-0.75f) == -0.75f,
        "Visual safety transfer changed normal PCM");
    const auto guardedPositive = nekorox::boundedVisualSample(2.0f);
    const auto guardedNegative = nekorox::boundedVisualSample(-2.0f);
    require(guardedPositive > 0.92f && guardedPositive < 1.0f &&
            guardedNegative == -guardedPositive &&
            nekorox::boundedVisualSample(
                std::numeric_limits<float>::infinity()) == 0.0f,
        "Visual safety transfer is not finite, symmetric, and bounded");

    nekorox::CompactScopeModel compactScope;
    auto compactFrame = makeStereoSignal(100, 12.0);
    require(compactScope.ingest(*compactFrame),
        "Compact scope rejected valid PCM");
    require(*std::max_element(
            compactScope.samples().begin(), compactScope.samples().end()) > 0.35f,
        "Compact scope lost a centered signal");
    require(!compactScope.ingest(*compactFrame),
        "Compact scope accepted a duplicate sequence");
    compactScope.reset();
    auto cancelledFrame = makeStereoSignal(101, 13.0, -1.0f);
    require(compactScope.ingest(*cancelledFrame),
        "Compact scope rejected antiphase PCM");
    require(std::all_of(compactScope.samples().begin(), compactScope.samples().end(),
            [](float sample) { return std::abs(sample) < 0.001f; }),
        "Compact scope is not derived from (L+R)/2");

    nekorox::CompactScopeModel irregularScope;
    for (std::uint64_t chunk = 0; chunk < 24; ++chunk) {
        auto irregular = makeStereoChunk(
            150 + chunk, chunk * 200U, 137.0, 200);
        for (std::uint32_t index = 0;
             index < irregular->frameCount;
             ++index) {
            auto bits = static_cast<std::uint32_t>(
                chunk * 200U + index) * 747796405U + 2891336453U;
            bits = ((bits >> ((bits >> 28U) + 4U)) ^ bits) * 277803737U;
            bits = (bits >> 22U) ^ bits;
            const auto value =
                (static_cast<float>(bits & 0xffffU) / 32767.5f - 1.0f) *
                0.55f;
            irregular->samples[index * 2U] = value;
            irregular->samples[index * 2U + 1U] = value;
        }
        const auto revision = irregularScope.renderRevision();
        require(irregularScope.ingest(*irregular),
            "Compact scope rejected irregular PCM");
        if (chunk >= 4) {
            require(irregularScope.renderRevision() > revision,
                "Compact scope held a stale weak-correlation window");
        }
    }
    require(!irregularScope.locked(),
        "Irregular PCM unexpectedly acquired a phase lock");
    nekorox::CompactScopePresenter irregularPresenter;
    require(irregularPresenter.syncTarget(irregularScope),
        "Compact scope presenter rejected an irregular baseline");
    const auto irregularBefore = irregularPresenter.samples();
    auto irregularNext = makeStereoChunk(174, 24U * 200U, 137.0, 200);
    for (std::uint32_t index = 0;
         index < irregularNext->frameCount;
         ++index) {
        auto bits = static_cast<std::uint32_t>(
            24U * 200U + index) * 747796405U + 2891336453U;
        bits = ((bits >> ((bits >> 28U) + 4U)) ^ bits) * 277803737U;
        bits = (bits >> 22U) ^ bits;
        const auto value =
            (static_cast<float>(bits & 0xffffU) / 32767.5f - 1.0f) *
            0.55f;
        irregularNext->samples[index * 2U] = value;
        irregularNext->samples[index * 2U + 1U] = value;
    }
    require(irregularScope.ingest(*irregularNext),
        "Compact scope rejected the next irregular PCM block");
    irregularPresenter.syncTarget(irregularScope);
    const auto irregularTarget = irregularScope.samples();
    require(irregularPresenter.advance(1.0 / 165.0),
        "Compact scope presenter did not smooth an incoherent target");
    float irregularFullDelta = 0.0f;
    float irregularFirstStep = 0.0f;
    for (std::size_t index = 0; index < nekorox::kScopeSamples; ++index) {
        irregularFullDelta = std::max(irregularFullDelta,
            std::abs(irregularTarget[index] - irregularBefore[index]));
        irregularFirstStep = std::max(irregularFirstStep,
            std::abs(irregularPresenter.samples()[index] -
                irregularBefore[index]));
    }
    require(irregularFullDelta > 0.10f &&
            irregularFirstStep < irregularFullDelta * 0.55f,
        "Incoherent scope motion is not temporally anti-aliased");
    for (int displayFrame = 0; displayFrame < 30; ++displayFrame) {
        irregularPresenter.advance(1.0 / 165.0);
    }
    float irregularResidual = 0.0f;
    for (std::size_t index = 0; index < nekorox::kScopeSamples; ++index) {
        irregularResidual = std::max(irregularResidual,
            std::abs(irregularPresenter.samples()[index] -
                irregularTarget[index]));
    }
    require(irregularResidual < 0.002f,
        "Incoherent scope smoothing has a visible long tail");

    compactScope.reset();
    constexpr double bassFrequency = 73.0;
    constexpr double flShortScopeReferenceHz = 65.40639132514966;
    for (std::uint64_t chunk = 0; chunk < 18; ++chunk) {
        auto bass = makeStereoChunk(
            200 + chunk, chunk * 400U, bassFrequency);
        require(compactScope.ingest(*bass),
            "Compact scope rejected continuous bass PCM");
    }
    const auto expectedWindow = 48000.0 / flShortScopeReferenceHz;
    require(std::abs(
            static_cast<double>(compactScope.displayWindowSamples()) -
            expectedWindow) < 2.0,
        "Compact scope does not use FL Studio's fixed C2 timebase");
    require(compactScope.locked() && compactScope.lockConfidence() > 0.99f,
        "Compact scope did not settle after one fixed reference window");
    require(std::abs(compactScope.periodHz() - flShortScopeReferenceHz) < 0.01f,
        "Compact scope reports the wrong fixed reference frequency");
    const auto renderedPeak = *std::max_element(
        compactScope.samples().begin(), compactScope.samples().end());
    require(renderedPeak > 0.60f && renderedPeak < 0.70f,
        "Compact scope unexpectedly normalizes the signal amplitude");
    require(*std::max_element(compactScope.maximumSamples().begin(),
                compactScope.maximumSamples().end()) >= renderedPeak,
        "Compact scope min/max envelope lost a rendered peak");

    nekorox::CompactScopeModel boostedSubBassScope;
    for (std::uint64_t chunk = 0; chunk < 18; ++chunk) {
        auto bass = makeStereoChunk(
            250 + chunk, chunk * 400U, 45.0, 400, 1.0f, 2.4f);
        require(boostedSubBassScope.ingest(*bass),
            "Compact scope rejected over-range post-DSP sub-bass");
    }
    const auto boostedMinimum = *std::min_element(
        boostedSubBassScope.minimumSamples().begin(),
        boostedSubBassScope.minimumSamples().end());
    const auto boostedMaximum = *std::max_element(
        boostedSubBassScope.maximumSamples().begin(),
        boostedSubBassScope.maximumSamples().end());
    require(boostedMinimum > -1.0f && boostedMaximum < 1.0f &&
            std::abs(boostedMinimum + boostedMaximum) < 0.01f,
        "Compact scope did not center and bound boosted sub-bass visually");
    require(*std::max_element(boostedSubBassScope.rawSamples().begin(),
                boostedSubBassScope.rawSamples().end()) > 1.0f,
        "Compact scope altered the raw post-DSP phase reference");

    nekorox::CompactScopeModel highScope;
    for (std::uint64_t chunk = 0; chunk < 18; ++chunk) {
        auto high = makeStereoChunk(300 + chunk, chunk * 400U, 220.0);
        require(highScope.ingest(*high),
            "Compact scope rejected fixed-timebase comparison PCM");
    }
    require(highScope.displayWindowSamples() ==
            compactScope.displayWindowSamples(),
        "Compact scope stretches its viewport to the detected frequency");

    nekorox::CompactScopeModel referenceScope;
    for (std::uint64_t chunk = 0; chunk < 18; ++chunk) {
        auto reference = makeStereoChunk(
            400 + chunk, chunk * 400U, flShortScopeReferenceHz);
        require(referenceScope.ingest(*reference),
            "Compact scope rejected C2 reference PCM");
    }
    const auto stationary = referenceScope.samples();
    auto nextReference = makeStereoChunk(
        418, 18U * 400U, flShortScopeReferenceHz);
    require(referenceScope.ingest(*nextReference),
        "Compact scope rejected the next C2 reference window");
    float stationaryDifference = 0.0f;
    for (std::size_t index = 0; index < nekorox::kScopeSamples; ++index) {
        stationaryDifference = std::max(stationaryDifference,
            std::abs(stationary[index] - referenceScope.samples()[index]));
    }
    require(stationaryDifference < 0.02f,
        "Compact scope does not advance on FL Studio's reference-period lattice");

    nekorox::CompactScopeModel aliasScope;
    constexpr double worstCaseFrequency = flShortScopeReferenceHz * 1.5;
    for (std::uint64_t chunk = 0; chunk < 18; ++chunk) {
        auto signal = makeStereoChunk(
            500 + chunk, chunk * 400U, worstCaseFrequency);
        require(aliasScope.ingest(*signal),
            "Compact scope rejected temporal-alias test PCM");
    }
    auto coherentFrame = aliasScope.samples();
    float worstTemporalDifference = 0.0f;
    for (std::uint64_t chunk = 18; chunk < 42; ++chunk) {
        auto signal = makeStereoChunk(
            500 + chunk, chunk * 400U, worstCaseFrequency);
        require(aliasScope.ingest(*signal),
            "Compact scope rejected phase-coherence continuation PCM");
        float difference = 0.0f;
        for (std::size_t index = 0; index < nekorox::kScopeSamples; ++index) {
            difference = std::max(difference,
                std::abs(coherentFrame[index] - aliasScope.samples()[index]));
        }
        worstTemporalDifference = std::max(worstTemporalDifference, difference);
        coherentFrame = aliasScope.samples();
    }
    require(worstTemporalDifference < 0.12f,
        "Compact scope exposes half-cycle temporal aliasing");

    for (std::uint64_t chunk = 42; chunk < 52; ++chunk) {
        auto quiet = makeStereoChunk(
            500 + chunk, chunk * 400U, worstCaseFrequency, 400, 1.0f, 0.16f);
        require(aliasScope.ingest(*quiet),
            "Compact scope rejected amplitude-change PCM");
    }
    const auto quietPeak = *std::max_element(
        aliasScope.maximumSamples().begin(), aliasScope.maximumSamples().end());
    require(quietPeak < 0.22f,
        "Compact scope phase latch froze stale signal amplitude");

    nekorox::CompactScopeModel presentationScope;
    for (std::uint64_t chunk = 0; chunk < 18; ++chunk) {
        auto signal = makeStereoChunk(
            600 + chunk, chunk * 400U, worstCaseFrequency);
        require(presentationScope.ingest(*signal),
            "Compact scope rejected presentation baseline PCM");
    }
    nekorox::CompactScopePresenter presenter;
    require(presenter.syncTarget(presentationScope) && presenter.ready(),
        "Compact scope presenter did not acquire its first target");
    const auto presentationBefore = presenter.samples();
    for (std::uint64_t chunk = 18; chunk < 22; ++chunk) {
        auto quieter = makeStereoChunk(
            600 + chunk, chunk * 400U, worstCaseFrequency,
            400, 1.0f, 0.30f);
        require(presentationScope.ingest(*quieter),
            "Compact scope rejected presentation transition PCM");
    }
    presenter.syncTarget(presentationScope);
    const auto presentationTarget = presentationScope.samples();
    require(presenter.active(),
        "Compact scope presenter ignored a changed phase-aligned target");
    require(presenter.advance(1.0 / 165.0),
        "Compact scope presenter did not advance at display cadence");
    for (std::size_t index = 0; index < nekorox::kScopeSamples; ++index) {
        const auto low = std::min(
            presentationBefore[index], presentationTarget[index]) - 0.0001f;
        const auto high = std::max(
            presentationBefore[index], presentationTarget[index]) + 0.0001f;
        require(presenter.samples()[index] >= low &&
                presenter.samples()[index] <= high,
            "Compact scope presentation created a non-convex ghost frame");
    }
    for (int displayFrame = 0; displayFrame < 8; ++displayFrame) {
        presenter.advance(1.0 / 165.0);
    }
    float presentationResidual = 0.0f;
    for (std::size_t index = 0; index < nekorox::kScopeSamples; ++index) {
        presentationResidual = std::max(
            presentationResidual,
            std::abs(presenter.samples()[index] - presentationTarget[index]));
    }
    require(presentationResidual < 0.002f,
        "Compact scope presentation adds visible long-tail latency");
    const auto revisionBeforeGap = presentationScope.renderRevision();
    auto afterGap = makeStereoChunk(
        700, 22U * 400U, worstCaseFrequency);
    require(presentationScope.ingest(*afterGap),
        "Compact scope rejected PCM after a ring gap");
    require(presentationScope.renderRevision() > revisionBeforeGap,
        "Compact scope reused a presentation revision after resynchronizing");

    auto monoStorage = std::make_unique<nekorox::RealtimeVisualModel>();
    auto& mono = *monoStorage;
    mono.reset(240.0);
    auto monoFrame = makeStereoSignal(1, 60.0);
    require(mono.ingest(*monoFrame), "Realtime model rejected valid mono-compatible PCM");
    require(mono.snapshot().correlation > 0.99f && mono.snapshot().width < 0.01f,
        "Realtime model misclassified a centered signal");
    require(mono.snapshot().rms > 0.35f && mono.snapshot().peak > 0.60f,
        "Realtime model lost signal level");
    require(!mono.ingest(*monoFrame),
        "Realtime model accepted a duplicate sequence");

    auto spectralToneStorage = std::make_unique<nekorox::RealtimeVisualModel>();
    auto& spectralTone = *spectralToneStorage;
    spectralTone.reset(30.0);
    auto toneFirst = makeStereoChunk(20, 0, 440.0, 2048);
    auto toneSecond = makeStereoChunk(21, 2048, 440.0, 2048);
    auto toneThird = makeStereoChunk(22, 4096, 440.0, 2048);
    auto toneFourth = makeStereoChunk(23, 6144, 440.0, 2048);
    require(spectralTone.ingest(*toneFirst) &&
            spectralTone.ingest(*toneSecond) &&
            spectralTone.ingest(*toneThird) &&
            spectralTone.ingest(*toneFourth),
        "High-resolution spectrum rejected continuous PCM");
    const auto& toneSpectrum = spectralTone.snapshot();
    const auto peakBin = static_cast<std::size_t>(std::distance(
        toneSpectrum.spectrumFft.begin(),
        std::max_element(
            toneSpectrum.spectrumFft.begin(),
            toneSpectrum.spectrumFft.end())));
    const auto peakFrequency = static_cast<double>(peakBin) * 48000.0 /
        static_cast<double>(nekorox::kAnalysisFftSize);
    require(std::abs(peakFrequency - 440.0) < 18.0,
        "High-resolution FFT lost a bin-centred tone");
    require(toneSpectrum.spectralConfidence > 0.5f &&
            std::abs(toneSpectrum.spectralCentroidHz - 440.0f) < 80.0f &&
            std::abs(toneSpectrum.spectralRolloff85Hz - 440.0f) < 80.0f,
        "Spectrum markers do not describe a stable tone: centroid=" +
            std::to_string(toneSpectrum.spectralCentroidHz) +
            " rolloff=" +
            std::to_string(toneSpectrum.spectralRolloff85Hz) +
            " confidence=" +
            std::to_string(toneSpectrum.spectralConfidence));

    auto antiPhaseStorage = std::make_unique<nekorox::RealtimeVisualModel>();
    auto& antiPhase = *antiPhaseStorage;
    antiPhase.reset(240.0);
    auto antiPhaseFrame = makeStereoSignal(2, 120.0, -1.0f);
    require(antiPhase.ingest(*antiPhaseFrame),
        "Realtime model rejected antiphase PCM");
    require(antiPhase.snapshot().correlation < -0.99f &&
            antiPhase.snapshot().width > 0.99f,
        "Vectorscope width does not expose antiphase stereo");
    require(std::abs(antiPhase.snapshot().scope[nekorox::kScopeSamples / 2U]) < 0.001f,
        "Mono oscilloscope is not derived from (L+R)/2");

    auto wideStorage = std::make_unique<nekorox::RealtimeVisualModel>();
    auto& wide = *wideStorage;
    wide.reset(240.0);
    auto wideFrame = makeStereoSignal(3, 180.0);
    for (std::uint32_t index = 0; index < wideFrame->frameCount; ++index) {
        const auto phase = 2.0 * std::numbers::pi * 673.0 * index /
            static_cast<double>(wideFrame->sampleRate);
        wideFrame->samples[index * 2U + 1U] =
            static_cast<float>(std::sin(phase) * 0.65);
    }
    require(wide.ingest(*wideFrame), "Realtime model rejected wide stereo PCM");
    require(std::abs(wide.snapshot().correlation) < 0.12f &&
            wide.snapshot().width > 0.44f,
        "Realtime model collapsed an uncorrelated stereo signal");

    auto transient = makeStereoSignal(4, 61.0);
    transient->samples.fill(0.0f);
    transient->samples[1024U * 2U] = 1.0f;
    transient->samples[1024U * 2U + 1U] = 1.0f;
    require(mono.ingest(*transient), "Realtime model rejected an impulse");
    require(mono.snapshot().peak >= 1.0f,
        "Realtime model clipped or missed the input peak");

    auto sweep = makeStereoSignal(5, 62.0);
    for (std::uint32_t index = 0; index < sweep->frameCount; ++index) {
        const auto t = index / static_cast<double>(sweep->sampleRate);
        const auto phase = 2.0 * std::numbers::pi *
            (80.0 * t + 0.5 * 9000.0 * t * t);
        const auto value = static_cast<float>(std::sin(phase) * 0.7);
        sweep->samples[index * 2U] = value;
        sweep->samples[index * 2U + 1U] = value;
    }
    require(mono.ingest(*sweep), "Realtime model rejected a sweep");
    require(mono.snapshot().brightness >= 0.0f &&
            mono.snapshot().brightness <= 1.0f,
        "Spectrum brightness escaped normalized bounds");

    for (std::uint64_t sequence = 6;
         sequence < 6 + nekorox::kSpectrogramColumns + 7U;
         ++sequence) {
        auto frame = makeStereoSignal(sequence, 60.0 + sequence * 0.01);
        require(mono.ingest(*frame), "Realtime model rejected a ring test frame");
    }
    constexpr auto ingestedFrames = 3U + nekorox::kSpectrogramColumns + 7U;
    require(mono.spectrogramHead() ==
            ingestedFrames % nekorox::kSpectrogramColumns,
        "Spectrogram ring did not wrap deterministically");
    require(mono.historyHead() ==
            ingestedFrames % nekorox::kEvolutionSamples,
        "Evolution history ring did not wrap deterministically");
    std::vector<nekorox::AnalysisWaveformBinV1> waveform(640U);
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        const auto loud = index >= 240U && index < 480U;
        waveform[index].rms = loud ? 0.42f : 0.025f;
        waveform[index].peak = loud ? 0.82f : 0.055f;
    }
    const auto profile = nekorox::buildMusicMapProfile(waveform);
    const auto repeatedProfile = nekorox::buildMusicMapProfile(waveform);
    require(std::memcmp(
            profile.data(),
            repeatedProfile.data(),
            sizeof(profile)) == 0,
        "Full-track Music Map is not deterministic");
    for (const auto& bin : profile) {
        require(bin.observations > 0 &&
                std::isfinite(bin.energy) &&
                std::isfinite(bin.width) &&
                std::isfinite(bin.brightness) &&
                std::isfinite(bin.pulse) &&
                bin.energy >= 0.0f && bin.energy <= 1.0f &&
                bin.width >= 0.0f && bin.width <= 1.0f &&
                bin.brightness >= 0.0f && bin.brightness <= 1.0f &&
                bin.pulse >= 0.0f && bin.pulse <= 1.0f,
            "Full-track Music Map escaped its fixed normalized contract");
    }
    require(profile[200U].energy > profile[40U].energy + 0.35f,
        "Full-track Music Map did not preserve fixed-scale loudness");
    require(profile[119U].pulse > profile[40U].pulse + 0.15f,
        "Full-track Music Map did not expose the section transition");
}

void testScroll()
{
    nekorox::ScrollController scroll;
    scroll.setBounds(0.0, 5000.0);
    scroll.setPosition(0.0);
    double scrollTime = 0.0;
    for (int i = 0; i < 5; ++i) {
        scrollTime += 0.008;
        scroll.addWheelImpulseAt(120.0, scrollTime);
        scroll.update(1.0 / 165.0);
    }
    require(scroll.overscanRows(20.0) > 5,
        "Scroll overscan did not react to velocity");
    for (int i = 0; i < 2000 && scroll.active(); ++i) {
        scroll.update(1.0 / 165.0);
        require(scroll.position() >= 0.0 && scroll.position() <= 5000.0,
            "Scroll escaped its bounds");
    }
    require(!scroll.active(), "Scroll did not settle");
    require(std::abs(scroll.position() - 600.0) < 0.01,
        "Scroll settled away from its logical target");

    scroll.setPosition(600.0);
    scroll.setResponseScale(0.45);
    scroll.addWheelImpulseAt(120.0, 100.0);
    require(scroll.active(), "Reduced-motion scroll did not retain short settling");
    for (int i = 0; i < 1000 && scroll.active(); ++i) {
        scroll.update(1.0 / 165.0);
    }
    require(!scroll.active() && std::abs(scroll.position() - 720.0) < 0.01,
        "Reduced-motion scroll did not settle deterministically");

    scroll.setResponseScale(0.0);
    scroll.addWheelImpulseAt(120.0, 101.0);
    require(!scroll.active() && std::abs(scroll.position() - 840.0) < 0.01,
        "Motion-off scroll did not snap to its target");

    nekorox::ScrollController freshAfterIdle;
    nekorox::ScrollController freshAtNominalCadence;
    for (auto* controller : {&freshAfterIdle, &freshAtNominalCadence}) {
        controller->setBounds(0.0, 5000.0);
        controller->setPosition(500.0);
    }
    freshAfterIdle.addWheelImpulseAt(120.0, 2.0);
    freshAtNominalCadence.addWheelImpulseAt(120.0, 1.0 / 60.0);
    require(std::abs(freshAfterIdle.position() - 500.0) < 0.001,
        "First wheel input changed position before a frame");
    require(std::abs(
            freshAfterIdle.position() -
            freshAtNominalCadence.position()) < 0.001,
        "First wheel response still depends on the preceding idle duration");
    freshAfterIdle.update(1.0 / 165.0);
    require(freshAfterIdle.position() > 500.0,
        "First useful frame did not begin spring motion");

    nekorox::ScrollController burstA;
    nekorox::ScrollController burstB;
    for (auto* controller : {&burstA, &burstB}) {
        controller->setBounds(0.0, 5000.0);
        controller->setPosition(500.0);
    }
    for (int index = 0; index < 8; ++index) {
        burstA.addWheelImpulseAt(30.0, 0.0);
        burstB.addWheelImpulseAt(30.0, 0.0);
    }
    require(std::abs(burstA.target() - 740.0) < 0.001 &&
            std::abs(burstB.target() - 740.0) < 0.001,
        "Wheel burst cadence changed the logical destination");
    require(std::abs(burstA.position() - burstB.position()) < 0.001,
        "Wheel burst cadence changed the immediate visual response");

    const auto beforeReversal = burstA.position();
    burstA.addWheelImpulseAt(-120.0, 0.008);
    require(burstA.target() < beforeReversal &&
            burstA.position() >= beforeReversal,
        "Wheel direction reversal teleported backward inside input handling");
    const auto atReversal = burstA.position();
    burstA.update(1.0 / 165.0);
    require(burstA.position() < atReversal,
        "Wheel direction reversal did not respond on the next frame");
}

void testArtPalette()
{
    constexpr std::uint32_t width = 32;
    constexpr std::uint32_t height = 32;
    std::vector<std::uint8_t> pixels(width * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = static_cast<std::size_t>(y * width + x) * 4U;
            const bool green = x < width * 3U / 4U;
            pixels[at + 0] = green ? 45 : 210;
            pixels[at + 1] = green ? 190 : 45;
            pixels[at + 2] = green ? 55 : 205;
            pixels[at + 3] = 255;
        }
    }
    const auto first = nekorox::extractArtPalette(
        pixels, width, height, width * 4U);
    const auto second = nekorox::extractArtPalette(
        pixels, width, height, width * 4U);
    require(first.valid, "Art palette rejected a colorful deterministic image");
    require(std::abs(first.primary.red - second.primary.red) < 0.00001f &&
            std::abs(first.primary.green - second.primary.green) < 0.00001f &&
            std::abs(first.primary.blue - second.primary.blue) < 0.00001f,
        "Art palette extraction is not deterministic");
    require(nekorox::contrastRatio(
            first.primary, nekorox::RgbColor{0.035f, 0.039f, 0.039f}) >= 4.5,
        "Art-derived accent does not meet contrast gate");
    const nekorox::RgbColor cyan{0.0f, 0.847f, 0.816f};
    const auto start = nekorox::interpolateOklch(
        cyan, first.primary, 0.0f);
    const auto middle = nekorox::interpolateOklch(
        cyan, first.primary, 0.5f);
    const auto finish = nekorox::interpolateOklch(
        cyan, first.primary, 1.0f);
    require(start.red == cyan.red &&
            start.green == cyan.green &&
            start.blue == cyan.blue &&
            finish.red == first.primary.red &&
            finish.green == first.primary.green &&
            finish.blue == first.primary.blue,
        "OKLCH palette interpolation changed an endpoint");
    require(std::isfinite(middle.red) &&
            std::isfinite(middle.green) &&
            std::isfinite(middle.blue) &&
            nekorox::contrastRatio(
                middle,
                nekorox::RgbColor{0.035f, 0.039f, 0.039f}) >= 4.5,
        "OKLCH palette interpolation lost contrast");
    require(!nekorox::extractArtPalette({}, 0, 0, 0).valid,
        "Art palette accepted an invalid image");

    std::vector<std::uint8_t> triad(width * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = static_cast<std::size_t>(y * width + x) * 4U;
            const auto section = (x * 3U) / width;
            triad[at + 0] = section == 2U ? 230 : 24;
            triad[at + 1] = section == 1U ? 230 : 24;
            triad[at + 2] = section == 0U ? 230 : 24;
            triad[at + 3] = 255;
        }
    }
    const auto clustered = nekorox::extractArtPalette(
        triad, width, height, width * 4U);
    const auto distance = [](const nekorox::RgbColor& a,
                             const nekorox::RgbColor& b) {
        return std::abs(a.red - b.red) + std::abs(a.green - b.green) +
            std::abs(a.blue - b.blue);
    };
    require(clustered.flowValid &&
            distance(clustered.flowLow, clustered.flowMid) > 0.1f &&
            distance(clustered.flowLow, clustered.flowHigh) > 0.1f &&
            distance(clustered.flowMid, clustered.flowHigh) > 0.1f,
        "R9-SI05 three-color artwork lost distinct Flow representatives");

    const auto channelSpread = [](const nekorox::RgbColor& color) {
        return (std::max)({color.red, color.green, color.blue}) -
            (std::min)({color.red, color.green, color.blue});
    };

    // A dark cover with small neon linework must not collapse its material
    // palette to the large neutral field. This mirrors album artwork where the
    // accessible chrome accent is vivid while most pixels are nearly black.
    constexpr std::uint32_t neonWidth = 100;
    constexpr std::uint32_t neonHeight = 100;
    std::vector<std::uint8_t> neonOnDark(neonWidth * neonHeight * 4U);
    for (std::uint32_t y = 0; y < neonHeight; ++y) {
        for (std::uint32_t x = 0; x < neonWidth; ++x) {
            const auto at = (static_cast<std::size_t>(y) * neonWidth + x) * 4U;
            std::array<std::uint8_t, 3> bgr{18U, 18U, 18U};
            if (x == 12U) bgr = {235U, 35U, 255U};
            if (x == 49U) bgr = {255U, 220U, 25U};
            if (x == 81U) bgr = {35U, 245U, 80U};
            neonOnDark[at] = bgr[0];
            neonOnDark[at + 1U] = bgr[1];
            neonOnDark[at + 2U] = bgr[2];
            neonOnDark[at + 3U] = 255U;
        }
    }
    const auto neonPalette = nekorox::extractArtPalette(
        neonOnDark, neonWidth, neonHeight, neonWidth * 4U);
    const auto chromaticFlowCount =
        (channelSpread(neonPalette.flowLow) > 0.20f ? 1 : 0) +
        (channelSpread(neonPalette.flowMid) > 0.20f ? 1 : 0) +
        (channelSpread(neonPalette.flowHigh) > 0.20f ? 1 : 0);
    require(neonPalette.flowValid && !neonPalette.flowMonochrome &&
            chromaticFlowCount >= 2,
        "Dark artwork erased its small neon Flow pigments");

    std::vector<std::uint8_t> gray(width * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = static_cast<std::size_t>(y * width + x) * 4U;
            const auto value = static_cast<std::uint8_t>(
                24U + ((x + y) * 190U) / (width + height - 2U));
            gray[at + 0] = value;
            gray[at + 1] = value;
            gray[at + 2] = value;
            gray[at + 3] = 255;
        }
    }
    const auto monochrome = nekorox::extractArtPalette(
        gray, width, height, width * 4U);
    require(monochrome.flowValid && monochrome.flowMonochrome &&
            channelSpread(monochrome.flowLow) < 0.001f &&
            channelSpread(monochrome.flowMid) < 0.001f &&
            channelSpread(monochrome.flowHigh) < 0.001f,
        "R9-SI06 monochrome artwork gained an artificial Flow hue");

    constexpr std::uint32_t tallWidth = 100;
    constexpr std::uint32_t tallHeight = 100;
    std::vector<std::uint8_t> lowerThird(
        static_cast<std::size_t>(tallWidth) * tallHeight * 4U);
    for (std::uint32_t y = 0; y < tallHeight; ++y) {
        for (std::uint32_t x = 0; x < tallWidth; ++x) {
            const auto at = static_cast<std::size_t>(y * tallWidth + x) * 4U;
            const auto lower = y >= tallHeight * 2U / 3U;
            lowerThird[at + 0] = lower ? 25 : 190;
            lowerThird[at + 1] = lower ? 220 : 35;
            lowerThird[at + 2] = lower ? 35 : 25;
            lowerThird[at + 3] = 255;
        }
    }
    const auto fullImage = nekorox::extractArtPalette(
        lowerThird, tallWidth, tallHeight, tallWidth * 4U);
    const auto hasLowerGreen = [](const nekorox::RgbColor& color) {
        return color.green > color.red * 1.5f &&
            color.green > color.blue * 1.5f;
    };
    require(fullImage.flowValid && (hasLowerGreen(fullImage.flowLow) ||
            hasLowerGreen(fullImage.flowMid) ||
            hasLowerGreen(fullImage.flowHigh)),
        "R10-PA01 lower-third colour was omitted from the Flow palette");

    constexpr std::uint32_t stripHeight = 1000;
    std::vector<std::uint8_t> strip(stripHeight * 4U);
    for (std::uint32_t y = 0; y < stripHeight; ++y) {
        strip[y * 4U + 0U] = static_cast<std::uint8_t>(30U + y % 180U);
        strip[y * 4U + 1U] = static_cast<std::uint8_t>(220U - y % 160U);
        strip[y * 4U + 2U] = static_cast<std::uint8_t>(50U + y % 150U);
        strip[y * 4U + 3U] = 255U;
    }
    require(nekorox::extractArtPalette(strip, 1U, stripHeight, 4U).flowValid,
        "R10-PA03 1x1000 palette fixture produced no representative");
    require(first.materialValid && monochrome.materialValid && monochrome.materialMonochrome,
        "Collection material palette validity/monochrome contract");
    for(const auto& c:monochrome.material)require(std::abs(c.red-c.green)<1e-5f&&
        std::abs(c.green-c.blue)<1e-5f,"Monochrome cover acquired a foreign pigment");
    for(unsigned i=0;i<4;++i)require(first.material[i].red==second.material[i].red&&
        first.material[i].green==second.material[i].green&&first.material[i].blue==second.material[i].blue,
        "Material extraction must be deterministic");
    std::vector<std::uint8_t> dominantRed(100*4,255);
    for(unsigned i=0;i<100;++i){dominantRed[4*i]=i<90?20:200;
        dominantRed[4*i+1]=20;dominantRed[4*i+2]=i<90?200:20;}
    const auto red=nekorox::extractArtPalette(dominantRed,100,1,400);
    require(red.material[0].red>red.material[0].blue*4,
        "A minority accent displaced the dominant cover material");
    require(red.material[1].red>red.material[1].blue*4&&red.material[2].blue>red.material[2].red*4,
        "A minor contrasting mark became the broad secondary material instead of the accent");
    require(!nekorox::extractArtPalette({},0,0,0).materialValid,
        "Missing artwork must retain the neutral material fallback");
}

void testVisualCollectionMigration()
{
    using namespace nekorox::component::collection;
    require(legacyPresetIds.size()==24,"Migration lost the old ordinal table");
    require(migratePresetIndex(9)==fallbackPresetId&&migratePresetIndex(22)==fallbackPresetId&&
        migratePresetIndex(23)==fallbackPresetId&&migratePresetIndex(8)==fallbackPresetId,
        "Nebula family migration selected the wrong visual");
    for(unsigned i:{7U,10U,12U})require(migratePresetIndex(i)=="sculpted_ribbons","Fusion migration");
    for(unsigned i:{5U,15U,18U,21U,500U})require(migratePresetIndex(i)==fallbackPresetId,"Retired selection fallback");
    for(unsigned i:{0U,1U,2U,3U,4U,6U,11U,13U,14U,16U,17U,19U,20U})
        require(migratePresetIndex(i)==legacyPresetIds[i],"Surviving preference changed identity");
    const auto migrated=migratePresetFavorites("|nj47a5wg||nj47a5wgclassic||nj47a5wgexpressive||lnbg0175||paisgvhv||rq3v1xn2||3107||sxbkbfpc||wwi3etmp||gt8966nk||wavefield||caustics||alloy_v2||alloy|");
    require(migrated=="|nj47a5wgexpressive||sculpted_ribbons||wavefield||caustics||alloy|",
        "Favorites were not aliased, deduplicated or preserved correctly");
    require(migratePresetFavorites(migrated)==migrated,"Favorites migration is not idempotent");
    using namespace nekorox::component;
    curated::Input input;input.levels[63]=.81f;input.onsets[17]=.63f;input.relatives[0]=-.35f;
    auto packed=curated::makeConstants(input,{512,288},10);
    require(sizeof(packed)==896&&packed.levels[15][3]==.81f&&packed.onsets[4][1]==.63f&&
        packed.relatives[0][0]==-.35f,"Curated spectral buffer layout mismatch");
    input.preset=curated::Preset::lagrangian;packed=curated::makeConstants(input,{512,288},10);
    require(packed.levels[15][3]==0&&(packed.dimensions[3]&8U)==0,"Wave Field inherited Caustics controls");
    input.preset=curated::Preset::caustics;input.mode=curated::Mode::original;
    packed=curated::makeConstants(input,{512,288},10);
    require(packed.onsets[4][1]==0&&(packed.dimensions[3]&8U)==0,"Original inherited spectral deformation");
}

void testArtworkResampler()
{
    require(nekorox::quantizeArtworkPhysicalEdgeV1(32) == 32 &&
            nekorox::quantizeArtworkPhysicalEdgeV1(33) == 36 &&
            nekorox::quantizeArtworkPhysicalEdgeV1(97) == 104 &&
            nekorox::quantizeArtworkPhysicalEdgeV1(257) == 272,
        "Artwork physical-size buckets changed unexpectedly");

    constexpr std::uint32_t sourceEdge = 64;
    std::vector<std::uint8_t> constant(
        static_cast<std::size_t>(sourceEdge) * sourceEdge * 4U);
    for (std::size_t at = 0; at < constant.size(); at += 4U) {
        constant[at] = 32;
        constant[at + 1U] = 96;
        constant[at + 2U] = 200;
        constant[at + 3U] = 255;
    }
    const auto reduced = nekorox::resampleArtworkPbgraV1(
        constant, sourceEdge, sourceEdge, 8, 8);
    require(reduced.width == 8 && reduced.height == 8 &&
            reduced.bgra.size() == 8U * 8U * 4U,
        "Artwork resampler returned the wrong dimensions");
    for (std::size_t at = 0; at < reduced.bgra.size(); at += 4U) {
        require(std::abs(static_cast<int>(reduced.bgra[at]) - 32) <= 1 &&
                std::abs(static_cast<int>(reduced.bgra[at + 1U]) - 96) <= 1 &&
                std::abs(static_cast<int>(reduced.bgra[at + 2U]) - 200) <= 1 &&
                reduced.bgra[at + 3U] == 255,
            "Artwork resampler did not preserve a constant colour");
    }

    std::vector<std::uint8_t> checker(
        static_cast<std::size_t>(sourceEdge) * sourceEdge * 4U);
    for (std::uint32_t y = 0; y < sourceEdge; ++y) {
        for (std::uint32_t x = 0; x < sourceEdge; ++x) {
            const auto value = ((x + y) & 1U) != 0 ? 255U : 0U;
            const auto at = (static_cast<std::size_t>(y) * sourceEdge + x) * 4U;
            checker[at] = static_cast<std::uint8_t>(value);
            checker[at + 1U] = static_cast<std::uint8_t>(value);
            checker[at + 2U] = static_cast<std::uint8_t>(value);
            checker[at + 3U] = 255;
        }
    }
    const auto checkerPixel = nekorox::resampleArtworkPbgraV1(
        checker, sourceEdge, sourceEdge, 1, 1);
    require(checkerPixel.bgra.size() == 4 &&
            checkerPixel.bgra[0] >= 186 && checkerPixel.bgra[0] <= 189 &&
            checkerPixel.bgra[1] == checkerPixel.bgra[0] &&
            checkerPixel.bgra[2] == checkerPixel.bgra[0] &&
            checkerPixel.bgra[3] == 255,
        "Artwork resampler averaged checkerboard energy outside linear light");

    const std::array<std::uint8_t, 8> alphaEdge{
        0, 0, 0, 0,
        255, 255, 255, 255,
    };
    const auto alphaPixel = nekorox::resampleArtworkPbgraV1(
        alphaEdge, 2, 1, 1, 1);
    require(alphaPixel.bgra.size() == 4 &&
            alphaPixel.bgra[3] >= 127 && alphaPixel.bgra[3] <= 128 &&
            alphaPixel.bgra[0] <= alphaPixel.bgra[3] &&
            alphaPixel.bgra[1] <= alphaPixel.bgra[3] &&
            alphaPixel.bgra[2] <= alphaPixel.bgra[3] &&
            alphaPixel.bgra[0] >= 126,
        "Artwork resampler introduced a premultiplied-alpha fringe");

    const auto repeated = nekorox::resampleArtworkPbgraV1(
        checker, sourceEdge, sourceEdge, 13, 13);
    const auto repeatedAgain = nekorox::resampleArtworkPbgraV1(
        checker, sourceEdge, sourceEdge, 13, 13);
    require(repeated.bgra == repeatedAgain.bgra,
        "Artwork resampler is not deterministic");
    for (std::size_t at = 0; at < repeated.bgra.size(); at += 4U) {
        require(repeated.bgra[at] <= repeated.bgra[at + 3U] &&
                repeated.bgra[at + 1U] <= repeated.bgra[at + 3U] &&
                repeated.bgra[at + 2U] <= repeated.bgra[at + 3U],
            "Artwork resampler violated premultiplied-alpha bounds");
    }
}

void testPacketParsers()
{
    const auto flac = nekorox::parseFlacPackets(makeFlac());
    require(flac.error.empty() && flac.packets.size() == 2,
        "Synthetic FLAC was not parsed with CRC validation");
    require(flac.sampleRate == 44100 && flac.durationSeconds > 0.0,
        "FLAC timing was not decoded");
    const auto flacCost = nekorox::makePacketCost(flac);
    require(flacCost.supported && flacCost.levels.size() == 3,
        "FLAC cost pyramid was not generated");
    std::vector<std::byte> encodedCost;
    std::string costError;
    require(nekorox::serializePacketCost(flacCost, encodedCost, costError),
        costError.c_str());
    nekorox::PacketCostResult restoredCost;
    require(nekorox::deserializePacketCost(
            encodedCost, restoredCost, costError),
        costError.c_str());
    require(restoredCost.supported &&
            restoredCost.codec == nekorox::PacketCodec::flac &&
            restoredCost.levels.size() == flacCost.levels.size() &&
            restoredCost.levels.front().bins.size() ==
                flacCost.levels.front().bins.size() &&
            std::abs(restoredCost.stats.p95Kbps -
                flacCost.stats.p95Kbps) < 0.000001,
        "Packet-cost binary round-trip changed the result");
    encodedCost.back() ^= std::byte{0x40};
    require(!nekorox::deserializePacketCost(
            encodedCost, restoredCost, costError),
        "Packet-cost binary accepted a corrupted reserved field");

    const auto mp3 = nekorox::parseMp3Packets(makeMp3());
    require(mp3.error.empty() && mp3.packets.size() == 3,
        "Synthetic MP3 was not parsed");
    require(mp3.sampleRate == 44100,
        "MP3 sample rate was not decoded");

    const auto adts = nekorox::parseAacAdtsPackets(makeAdts());
    require(adts.error.empty() && adts.packets.size() == 3U &&
            adts.sampleRate == 48000U && adts.packets[1].bytes == 60U,
        "AAC ADTS packet sizes or timing were not decoded");
    const auto adtsCost = nekorox::makePacketCost(adts, 10U);
    require(adtsCost.supported &&
            adtsCost.codec == nekorox::PacketCodec::aacAdts &&
            adtsCost.stats.peakKbps > adtsCost.stats.minimumKbps,
        "AAC ADTS variability was lost in the cost map");
    encodedCost.clear();
    require(nekorox::serializePacketCost(adtsCost, encodedCost, costError) &&
            nekorox::deserializePacketCost(encodedCost, restoredCost, costError) &&
            restoredCost.codec == nekorox::PacketCodec::aacAdts,
        "Extended packet codec values do not survive cache serialization");

    const auto mp4 = nekorox::parseIsoBmffPackets(makeIsoBmff());
    require(mp4.error.empty() && mp4.packets.size() == 3U &&
            mp4.sampleRate == 48000U && mp4.packets[1].bytes == 180U &&
            std::abs(mp4.durationSeconds - 0.064) < 0.000001,
        "ISO BMFF audio sample tables were not decoded");

    const auto wave = nekorox::parseWavePackets(makeWave());
    require(wave.error.empty() && wave.packets.size() == 1U &&
            wave.sampleRate == 48000U &&
            std::abs(wave.durationSeconds - 0.1) < 0.000001,
        "WAVE byte rate and duration were not decoded");

    // Page lacing, not granule presence, distinguishes artwork comments from
    // audio spanning multiple pages. Serial zero is a legal logical stream.
    const auto oggFixture = [](bool largeComment, bool id3,
            bool vorbis=false, bool mixedSetup=false, std::uint32_t serial=0) {
        std::vector<std::uint8_t> bytes;
        if (id3) bytes = {'I','D','3',4,0,0,0,0,0,4,0,0,0,0};
        std::uint32_t sequence=0;
        const auto page = [&](std::uint64_t granule, unsigned flags,
                std::vector<std::uint8_t> body, bool continuedPacket,
                unsigned headerPrefix=0) {
            const auto at=bytes.size();
            bytes.insert(bytes.end(), {'O','g','g','S',0,static_cast<std::uint8_t>(flags)});
            for (unsigned i=0;i<8;++i) bytes.push_back(static_cast<std::uint8_t>(granule>>(i*8)));
            for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(serial>>(i*8)));
            for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(sequence>>(i*8)));
            ++sequence;
            bytes.insert(bytes.end(),4,0); // checksum computed below
            bytes.push_back(headerPrefix?2:1);
            if(headerPrefix) bytes.push_back(static_cast<std::uint8_t>(headerPrefix));
            const auto dataSize=body.size()-headerPrefix;
            require(dataSize<255 || (continuedPacket && dataSize==255), "Fixture lacing");
            bytes.push_back(static_cast<std::uint8_t>(dataSize));
            bytes.insert(bytes.end(),body.begin(),body.end());
            std::uint32_t crc=0;
            for (auto p=at;p<bytes.size();++p) {
                crc^=static_cast<std::uint32_t>(bytes[p])<<24;
                for(unsigned bit=0;bit<8;++bit) crc=(crc<<1)^((crc&0x80000000U)?0x04c11db7U:0U);
            }
            for(unsigned i=0;i<4;++i) bytes[at+22+i]=static_cast<std::uint8_t>(crc>>(i*8));
        };
        std::vector<std::uint8_t> head(vorbis?30:19,0);
        if(vorbis){head[0]=1;std::copy_n("vorbis",6,head.begin()+1);head[11]=2;head[12]=0x80;head[13]=0xbb;}
        else {std::copy_n("OpusHead",8,head.begin());head[8]=1;head[9]=2;
            head[10]=56;head[11]=1;} // Opus pre-skip 312
        page(0,2,head,false);
        std::vector<std::uint8_t> tags(largeComment?255:16,0);
        if(vorbis){tags[0]=3;std::copy_n("vorbis",6,tags.begin()+1);}
        else std::copy_n("OpusTags",8,tags.begin());
        page(largeComment?UINT64_MAX:0,0,tags,largeComment);
        if(largeComment) page(0,1,std::vector<std::uint8_t>(32,0),false);
        std::vector<std::uint8_t> setup(7,0);setup[0]=5;std::copy_n("vorbis",6,setup.begin()+1);
        if(vorbis && !mixedSetup) page(0,0,setup,false);
        auto continued=std::vector<std::uint8_t>(255,0x80);
        if(vorbis && mixedSetup){continued.insert(continued.begin(),setup.begin(),setup.end());}
        page(UINT64_MAX,0,continued,true,(vorbis && mixedSetup)?7:0);
        page(vorbis?48000:48312,5,std::vector<std::uint8_t>(128,0x80),false);
        return bytes;
    };
    for(bool largeComment:{false,true}) for(bool id3:{false,true}) {
        auto bytes=oggFixture(largeComment,id3);
        const auto ogg=nekorox::parseOggPackets(bytes);
        require(ogg.error.empty() && ogg.codec==nekorox::PacketCodec::oggOpus &&
                ogg.packets.size()==1 && ogg.packets[0].bytes==439 &&
                std::abs(ogg.durationSeconds-1)<1e-9,
            "Ogg lost initial continued audio or included comment/artwork bytes");
        require(std::abs(nekorox::makePacketCost(ogg).stats.averageKbps-3.512)<1e-9,
            "Ogg low bitrate was clamped or used wrong time units");
        bytes.resize(bytes.size()-1);
        require(!nekorox::makePacketCost(nekorox::parseOggPackets(bytes)).supported,
            "Incomplete initial Ogg audio was accepted");
    }
    for(bool mixed:{false,true}) for(bool id3:{false,true}) {
        const auto ogg=nekorox::parseOggPackets(oggFixture(true,id3,true,mixed,41));
        require(ogg.error.empty() && ogg.codec==nekorox::PacketCodec::oggVorbis &&
                ogg.packets.size()==1 && ogg.packets[0].bytes==439 && ogg.durationSeconds==1,
            "Vorbis continued setup/mixed audio page changed cost or timing");
    }
    nekorox::PacketParseResult partialBin;
    partialBin.codec=nekorox::PacketCodec::wave;partialBin.durationSeconds=.15;
    partialBin.packets={{0,1000,0,.1},{1000,10,.1,.05}};
    require(std::abs(nekorox::makePacketCost(partialBin).stats.averageKbps-
                1010*8/.15/1000)<1e-8,
        "Average bitrate overweights the final partial bin");
    require(nekorox::packetCostDurationMatches(100.1,100) &&
            !nekorox::packetCostDurationMatches(4800,100) &&
            !nekorox::packetCostDurationMatches(100,0) &&
            !nekorox::packetCostDurationMatches(std::numeric_limits<double>::infinity(),100),
        "Transport accepted absent/mismatched packet timing");
}

void testTrackIdentity()
{
    const auto file = std::filesystem::temp_directory_path() /
        L"nekorox-track-identity-test.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << "AAAA";
    }
    nekorox::TrackIdentityInput input{file, 0, 4, 123};
    const auto first = nekorox::makeTrackId(input);
    const auto repeated = nekorox::makeTrackId(input);
    require(first.value == repeated.value,
        "Track identity is not deterministic");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "BBBB";
    }
    const auto changed = nekorox::makeTrackId(input);
    std::filesystem::remove(file);
    require(first.value != changed.value,
        "Track identity ignored changed file content");
}

void testDatabaseMigrationsAndHistory()
{
    const auto folder = std::filesystem::temp_directory_path() /
        L"nekorox-database-test";
    const auto file = folder / L"nekorox.db";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);

    nekorox::NekoDatabase database;
    std::string error;
    if (!database.open(file, error)) {
        throw std::runtime_error("Database open failed: " + error);
    }
    require(database.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "Database migration did not reach the current schema");
    require(database.beginSession("test-session", 1000, "tests", error),
        error.c_str());

    nekorox::PersistedTrack track;
    track.trackId = "v1:test-track";
    track.path = "e:\\music\\test.flac";
    track.partialFingerprint = "fingerprint";
    track.artist = "Artist";
    track.album = "Album";
    track.title = "Title";
    track.fileSize = 1234;
    track.modifiedTicks = 5678;
    track.subsong = 2;
    track.durationMs = 180000;
    track.lastSeenUtcMs = 1000;

    const std::array types{
        nekorox::PlayEventType::start,
        nekorox::PlayEventType::seek,
        nekorox::PlayEventType::finish,
    };
    for (std::size_t index = 0; index < types.size(); ++index) {
        nekorox::PersistedPlayEvent event;
        event.sessionId = "test-session";
        event.trackId = track.trackId;
        event.type = types[index];
        event.utcMs = 1000 + static_cast<std::int64_t>(index);
        event.positionMs = static_cast<std::int64_t>(index * 5000);
        event.durationMs = track.durationMs;
        event.eventUid = "test-event-" + std::to_string(index);
        bool inserted = false;
        require(database.recordPlayEvent(track, event, error, &inserted) &&
            inserted, error.c_str());
        require(database.recordPlayEvent(track, event, error, &inserted) &&
            !inserted, error.c_str());
    }
    require(database.endSession("test-session", 2000, error), error.c_str());

    auto events = database.readSessionEvents("test-session", 100, error);
    require(error.empty() && events.size() == types.size(),
        "Database did not preserve an idempotent session event stream");
    require(events[0].artist == "Artist" &&
            events[0].path == track.path && events[0].subsong == track.subsong &&
            events[0].trackId == track.trackId &&
            events[1].type == nekorox::PlayEventType::seek &&
            events[2].type == nekorox::PlayEventType::finish,
        "Database history join or ordering is incorrect");

    nekorox::PersistedTrackUserState userState;
    require(database.readTrackUserState(track.trackId, userState, error),
        error.c_str());
    require(userState.found && !userState.favorite && userState.note.empty(),
        "New track did not receive an empty user state");
    require(database.setFavorite(track, true, 2100, error), error.c_str());
    require(database.setNote(track, "remember this transition", 2200, error),
        error.c_str());
    require(database.readTrackUserState(track.trackId, userState, error),
        error.c_str());
    require(userState.favorite && userState.note == "remember this transition" &&
            userState.favoriteUpdatedUtcMs == 2100 &&
            userState.noteUpdatedUtcMs == 2200,
        "Favorite or note did not round-trip transactionally");
    require(!database.setNote(track, std::string(64U * 1024U + 1U, 'x'),
        2300, error) && error == "note exceeds 64 KiB",
        "Oversized note was not rejected");

    nekorox::PersistedPersona persona;
    persona.personaId = "persona:halley-labs";
    persona.displayName = "HALLEY LABS";
    persona.profile = "Independent music collective and connected aliases.";
    persona.updatedUtcMs = 2250;
    persona.aliases = {
        {"HALLEY LABS", "halley labs"},
        {"Halley Labs R&D", "halley labs r&d"},
    };
    persona.relationships = {{
        "persona:rotteen", "Rotteen", "rotteen", "alias", "related persona"}};
    if (!database.upsertPersona(persona, error)) {
        throw std::runtime_error("Persona upsert failed: " + error);
    }
    nekorox::PersistedPersona loadedPersona;
    if (!database.readPersonaByAliasKey("halley labs", loadedPersona, error)) {
        throw std::runtime_error("Persona alias read failed: " + error);
    }
    require(loadedPersona.found &&
            loadedPersona.personaId == persona.personaId &&
            loadedPersona.profile == persona.profile &&
            loadedPersona.aliases.size() == 2 &&
            loadedPersona.relationships.size() == 1 &&
            loadedPersona.relationships.front().targetDisplayName == "Rotteen",
        "Persona graph did not round-trip transactionally");
    if (!database.readPersonaByAliasKey("rotteen", loadedPersona, error)) {
        throw std::runtime_error("Persona target read failed: " + error);
    }
    require(loadedPersona.found && loadedPersona.displayName == "Rotteen",
        "Persona relationship target was not resolvable by alias");

    nekorox::PersistedPersona conflicting;
    conflicting.personaId = "persona:conflict";
    conflicting.displayName = "Conflict";
    conflicting.aliases = {{"HALLEY LABS", "halley labs"}};
    require(!database.upsertPersona(conflicting, error),
        "Duplicate normalized persona alias was accepted");
    if (!database.readPersonaById(conflicting.personaId, loadedPersona, error)) {
        throw std::runtime_error("Persona rollback read failed: " + error);
    }
    require(!loadedPersona.found,
        "Failed persona transaction left a partial persona row");

    nekorox::PersistedArtAsset art;
    art.assetId = "sha256:test-art";
    art.personaId = persona.personaId;
    art.role = "artist";
    art.path = "assets/legacy-yttm/art_img/h/halley.jpg";
    art.contentHash = "test-art";
    art.updatedUtcMs = 2260;
    require(database.upsertArtAsset(art, error), error.c_str());
    std::vector<nekorox::PersistedArtAsset> artAssets;
    require(database.readArtAssetsForPersona(
        persona.personaId, artAssets, error), error.c_str());
    require(artAssets.size() == 1 && artAssets.front().path == art.path &&
            artAssets.front().contentHash == art.contentHash,
        "Persona art index did not round-trip");

    nekorox::PersistedArtAsset selectedArt = art;
    selectedArt.assetId = "manual:test-art";
    selectedArt.path = "assets/legacy-yttm/art_img/h/manual-halley.jpg";
    selectedArt.selectedByUser = true;
    require(database.upsertArtAsset(selectedArt, error), error.c_str());
    require(database.deleteUnselectedArtAssetsWithPathPrefix(
        "assets/legacy-yttm/", error), error.c_str());
    require(database.readArtAssetsForPersona(
        persona.personaId, artAssets, error), error.c_str());
    require(artAssets.size() == 1 && artAssets.front().assetId == selectedArt.assetId &&
            artAssets.front().selectedByUser,
        "Reproducible art cleanup did not preserve user-selected art");

    require(database.setSetting(
        "legacy_yttm_import", "complete:1", 2270, error), error.c_str());
    std::string settingValue;
    bool settingFound = false;
    require(database.readSetting(
        "legacy_yttm_import", settingValue, settingFound, error), error.c_str());
    require(settingFound && settingValue == "complete:1",
        "Database setting did not round-trip");
    require(database.readSetting(
        "missing_setting", settingValue, settingFound, error), error.c_str());
    require(!settingFound && settingValue.empty(),
        "Missing database setting was not reported cleanly");

    const auto asBytes = [](std::string_view value) {
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        return std::vector<std::byte>(begin, begin + value.size());
    };
    nekorox::PersistedAnalysis firstAnalysis;
    firstAnalysis.trackId = track.trackId;
    firstAnalysis.analyzerVersion = "tests-v1";
    firstAnalysis.manifestJson = "{\"fixture\":1}";
    firstAnalysis.resultJson = asBytes("{\"bpm\":173.5,\"key\":\"D#\"}");
    firstAnalysis.waveform100ms.assign(4096, std::byte{0x2a});
    firstAnalysis.updatedUtcMs = 100;
    firstAnalysis.lastAccessUtcMs = 100;
    require(database.storeAnalysis(track, firstAnalysis,
        nekorox::kAnalysisCacheLimitBytes, error), error.c_str());
    const auto firstCacheBytes = database.analysisCacheBytes(error);
    require(error.empty() && firstCacheBytes > 0,
        "Analysis cache did not account for compressed layers");

    nekorox::PersistedAnalysis loaded;
    require(database.readAnalysis(track.trackId, "tests-v1", loaded, error),
        error.c_str());
    require(loaded.found && loaded.resultJson == firstAnalysis.resultJson &&
            loaded.waveform100ms == firstAnalysis.waveform100ms,
        "Analysis cache did not round-trip both layers");

    auto secondTrack = track;
    secondTrack.trackId = "v1:test-track-2";
    secondTrack.path = "e:\\music\\test-2.flac";
    secondTrack.partialFingerprint = "fingerprint-2";
    auto secondAnalysis = firstAnalysis;
    secondAnalysis.trackId = secondTrack.trackId;
    secondAnalysis.updatedUtcMs = 200;
    secondAnalysis.lastAccessUtcMs = 200;
    require(database.storeAnalysis(secondTrack, secondAnalysis,
        nekorox::kAnalysisCacheLimitBytes, error), error.c_str());
    require(database.pruneAnalysisCache(firstCacheBytes,
        secondTrack.trackId, secondAnalysis.analyzerVersion, error), error.c_str());
    require(database.readAnalysis(track.trackId, "tests-v1", loaded, error),
        error.c_str());
    require(!loaded.found, "Analysis LRU retained the oldest cache group");
    require(database.readAnalysis(secondTrack.trackId, "tests-v1", loaded, error),
        error.c_str());
    require(loaded.found, "Analysis LRU evicted its protected current group");

    nekorox::PersistedAnalysisBundle costBundle;
    costBundle.trackId = secondTrack.trackId;
    costBundle.analyzerVersion = "packet-cost-tests-v1";
    costBundle.manifestJson = "{\"kind\":\"packet-cost\"}";
    costBundle.layers.push_back(
        {"packet-cost", 100, asBytes("structured-packet-cost-fixture")});
    require(database.storeAnalysisBundle(secondTrack, costBundle,
        nekorox::kAnalysisCacheLimitBytes, error), error.c_str());
    nekorox::PersistedAnalysisBundle loadedBundle;
    require(database.readAnalysisBundle(secondTrack.trackId,
        costBundle.analyzerVersion, loadedBundle, error), error.c_str());
    require(loadedBundle.found && loadedBundle.layers.size() == 1 &&
            loadedBundle.layers.front().name == "packet-cost" &&
            loadedBundle.layers.front().data == costBundle.layers.front().data,
        "Generic analysis bundle did not round-trip its named layer");
    nekorox::PersistedLyricsRejectionV2 lyricRejection;
    lyricRejection.entityId = "track:lyrics-rejection";
    lyricRejection.subsong = 3;
    lyricRejection.mediaRevision = "media-revision-a";
    lyricRejection.documentId = "lrclib:42";
    lyricRejection.textHash = "text-hash-a";
    lyricRejection.timingHash = "timing-hash-a";
    lyricRejection.reason = "wrong-lyrics";
    lyricRejection.rejectedUtcMs = 2700;
    require(database.storeLyricsRejection(lyricRejection, error),
        error.c_str());
    nekorox::PersistedLyricsRejectionV2 loadedRejection;
    require(database.readLyricsRejection(lyricRejection.entityId,
            lyricRejection.subsong, lyricRejection.mediaRevision,
            lyricRejection.documentId, loadedRejection, error),
        error.c_str());
    require(loadedRejection.found &&
            loadedRejection.reason == lyricRejection.reason &&
            loadedRejection.textHash == lyricRejection.textHash &&
            loadedRejection.timingHash == lyricRejection.timingHash,
        "Lyrics rejection did not round-trip with full binding identity");
    nekorox::PersistedLyricsRejectionV2 unrelatedRejection;
    require(database.readLyricsRejection(lyricRejection.entityId,
            lyricRejection.subsong, "media-revision-b",
            lyricRejection.documentId, unrelatedRejection, error),
        error.c_str());
    require(!unrelatedRejection.found,
        "Lyrics rejection leaked across a media revision");
    database.close();

    if (!database.open(file, error)) {
        throw std::runtime_error("Database reopen failed: " + error);
    }
    require(database.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "Database migration was not idempotent after reopen");
    events = database.readSessionEvents("test-session", 100, error);
    require(error.empty() && events.size() == types.size(),
        "Database history did not survive reopen");
    require(database.readTrackUserState(track.trackId, userState, error),
        error.c_str());
    require(userState.found && userState.favorite &&
            userState.note == "remember this transition",
        "User state did not survive database reopen");
    if (!database.readPersonaById(persona.personaId, loadedPersona, error)) {
        throw std::runtime_error("Persona reopen read failed: " + error);
    }
    require(loadedPersona.found && loadedPersona.profile == persona.profile &&
            loadedPersona.aliases.size() == 2 &&
            loadedPersona.relationships.size() == 1,
        "Persona graph did not survive database reopen");
    require(database.readLyricsRejection(lyricRejection.entityId,
            lyricRejection.subsong, lyricRejection.mediaRevision,
            lyricRejection.documentId, loadedRejection, error),
        error.c_str());
    require(loadedRejection.found &&
            loadedRejection.rejectedUtcMs == lyricRejection.rejectedUtcMs,
        "Lyrics rejection did not survive database reopen");
    require(database.setFavorite(track, false, 2400, error), error.c_str());
    require(database.setNote(track, {}, 2500, error), error.c_str());
    require(database.readTrackUserState(track.trackId, userState, error),
        error.c_str());
    require(userState.found && !userState.favorite && userState.note.empty(),
        "Favorite or note could not be cleared");
    database.close();
    std::filesystem::remove_all(folder, ignored);
}

void testAnalysisProtocol()
{
    nekorox::AnalysisFrameHeaderV1 header;
    header.kind = nekorox::AnalysisMessageKind::progress;
    header.payloadBytes = sizeof(nekorox::AnalysisProgressV1);
    header.requestId = 42;
    require(nekorox::validAnalysisFrameHeader(header),
        "Valid analysis protocol frame was rejected");
    header.version += 1;
    require(!nekorox::validAnalysisFrameHeader(header),
        "Unknown analysis protocol version was accepted");
    header.version = nekorox::kAnalysisProtocolVersion;
    header.payloadBytes = nekorox::kAnalysisMaxPayloadBytes + 1U;
    require(!nekorox::validAnalysisFrameHeader(header),
        "Oversized analysis protocol payload was accepted");
}

void testUiContracts()
{
    nekorox::PlaybackSnapshotV1 playback;
    playback.sizeBytes = sizeof(playback);
    playback.state = nekorox::PlaybackState::playing;
    require(nekorox::validPlaybackSnapshot(playback),
        "Valid playback snapshot was rejected");
    playback.sizeBytes -= 1U;
    require(!nekorox::validPlaybackSnapshot(playback),
        "Playback snapshot with an unknown layout was accepted");

    nekorox::PlaybackSnapshotV2 playbackV2;
    playbackV2.sizeBytes = sizeof(playbackV2);
    playbackV2.state = nekorox::PlaybackState::paused;
    playbackV2.playbackOrder = 3;
    playbackV2.volumeDb = -12.0f;
    playbackV2.flags =
        nekorox::kPlaybackFlagMuted |
        nekorox::kPlaybackFlagFavoriteAvailable;
    playbackV2.queueRevision = 9;
    playbackV2.artworkIdentityHash = 42;
    require(nekorox::validPlaybackSnapshot(playbackV2),
        "Valid V2 playback snapshot was rejected");
    playbackV2.flags |= 1U << 31;
    require(!nekorox::validPlaybackSnapshot(playbackV2),
        "V2 playback snapshot accepted unknown flags");

    nekorox::PlaybackPositionSnapshotV1 position;
    position.positionSeconds = 42.5;
    position.qpcTicks = 1000;
    position.positionRevision = 8;
    position.seekGeneration = 3;
    require(nekorox::validPlaybackPositionSnapshot(position),
        "Valid playback position snapshot was rejected");
    position.positionSeconds = -0.001;
    require(!nekorox::validPlaybackPositionSnapshot(position),
        "Playback position snapshot accepted a negative position");

    require(sizeof(nekorox::PerformanceEventV2) == 64U &&
            sizeof(nekorox::PerformanceEventRingHeaderV2) == 64U &&
            nekorox::kPerformanceEventCapacityV2 == 8192U,
        "PerformanceEventRingV2 no longer matches its fixed ABI");

    nekorox::ListenCommandV1 command;
    command.kind = nekorox::ListenCommandKind::navigate;
    command.index = static_cast<std::uint32_t>(nekorox::ListenSpace::live);
    require(nekorox::validListenCommand(command),
        "Valid Listen command was rejected");
    command.index = 9;
    require(!nekorox::validListenCommand(command),
        "Listen navigation accepted an unknown space");
    command.index = 0;
    command.version += 1U;
    require(!nekorox::validListenCommand(command),
        "Unknown Listen command version was accepted");

    nekorox::ThemeSnapshotV1 theme;
    theme.flags = nekorox::kThemeFlagArtworkDerived;
    require(nekorox::validThemeSnapshot(theme),
        "Valid theme snapshot was rejected");
    theme.minimumContrast = 0.5f;
    require(!nekorox::validThemeSnapshot(theme),
        "Theme snapshot accepted invalid contrast");

    nekorox::ThemeSnapshotV2 themeV2;
    themeV2.artworkGeneration = 12;
    themeV2.transitionProgress = 0.5f;
    require(nekorox::validThemeSnapshot(themeV2),
        "Valid V2 theme snapshot was rejected");
    themeV2.transitionProgress = 1.1f;
    require(!nekorox::validThemeSnapshot(themeV2),
        "V2 theme snapshot accepted invalid transition progress");
}

void testLyricsTimeline()
{
    const auto synchronized = nekorox::LyricsTimelineV1::parseUtf8(
        "\xEF\xBB\xBF[ar:Local Artist]\r\n"
        "[offset:-250]\r\n"
        "[00:01.00][00:03.500]First line\r\n"
        "[00:02.25]Second line\r\n");
    require(synchronized.synchronized(),
        "Timed LRC was not recognized as synchronized");
    require(synchronized.offsetMilliseconds() == -250,
        "LRC offset was not retained");
    require(synchronized.lines().size() == 3,
        "Multiple LRC timestamps were not expanded");
    require(std::abs(
            synchronized.lines()[0].timestampSeconds - 0.75) < 0.0001,
        "Negative LRC offset was not applied");
    require(
        synchronized.activeLineIndex(1.9).value_or(99) == 0 &&
        synchronized.activeLineIndex(2.0).value_or(99) == 1,
        "Active lyric line lookup crossed a timestamp incorrectly");
    require(
        synchronized.seekTargetIndex(2.3).value_or(99) == 2 &&
        std::abs(synchronized.seekTargetSeconds(2).value_or(-1.0) -
            3.25) < 0.0001,
        "Lyric seek target lookup was not deterministic");

    const auto untimed = nekorox::LyricsTimelineV1::parseUtf8(
        "First untimed line\n"
        "\xF0\x28\x8C\x28\n"
        "Third untimed line");
    require(!untimed.synchronized() && untimed.lines().size() == 3,
        "Untimed lyrics fallback lost visible lines");
    require(untimed.lines()[1].text.find("\xEF\xBF\xBD") !=
            std::string::npos,
        "Invalid UTF-8 was not replaced safely");
    require(!untimed.activeLineIndex(10.0).has_value(),
        "Untimed lyrics exposed a false active timestamp");
}

void testWaveformPyramid()
{
    constexpr std::size_t frameCount = 960;
    std::vector<float> samples(frameCount * 2U);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        samples[frame * 2U] = frame < frameCount / 2U ? 0.5f : 0.25f;
        samples[frame * 2U + 1U] =
            frame < frameCount / 2U ? -0.25f : -0.75f;
    }

    nekorox::WaveformPyramidBuilder builder(42, frameCount);
    std::string error;
    require(builder.appendInterleaved(
            samples,
            frameCount,
            2,
            48000,
            error),
        error);
    const auto pyramid = builder.finalize();
    require(pyramid.complete() && pyramid.generation == 42,
        "Waveform pyramid did not preserve completion metadata");
    require(pyramid.version == nekorox::kWaveformPyramidVersion,
        "Waveform builder did not emit the detailed V2 format");
    require(pyramid.levels[0].bins.size() == 2,
        "Waveform pyramid did not produce 10 ms base bins");
    require(std::abs(pyramid.levels[0].bins[0].peak - 0.5f) < 0.0001f,
        "Waveform peak did not use max(abs(L), abs(R))");
    require(std::abs(
            pyramid.levels[0].bins[0].rms -
            static_cast<float>(std::sqrt(0.3125))) < 0.0001f,
        "Waveform RMS did not combine stereo energy");
    const auto& detail = pyramid.levels[0].bins[0].detail;
    require(
        detail.peakLeft > detail.peakRight &&
        detail.rmsLeft > detail.rmsRight,
        "Waveform V2 did not preserve stereo amplitude detail");
    require(
        detail.denseAmplitude >= detail.medianAmplitude &&
        (detail.lowEnergy != 0 || detail.midEnergy != 0 ||
            detail.highEnergy != 0),
        "Waveform V2 did not preserve density and spectral detail");
    require(pyramid.levels[1].bins.size() == 1,
        "Waveform pyramid did not derive its 40 ms level");

    std::vector<std::byte> encoded;
    require(nekorox::encodeWaveformPyramid(pyramid, encoded, error), error);
    nekorox::WaveformPyramidV1 decoded;
    require(nekorox::decodeWaveformPyramid(encoded, decoded, error), error);
    require(
        decoded.levels[0].bins.size() == pyramid.levels[0].bins.size() &&
        decoded.scannedFrames == frameCount &&
        decoded.levels[0].bins[0].detail.peakLeft == detail.peakLeft,
        "Waveform pyramid wire format did not round-trip");

    auto legacy = pyramid;
    legacy.version = nekorox::kWaveformPyramidLegacyVersion;
    for (auto& level : legacy.levels) {
        for (auto& bin : level.bins) bin.detail = {};
    }
    std::vector<std::byte> legacyEncoded;
    require(nekorox::encodeWaveformPyramid(
            legacy, legacyEncoded, error), error);
    require(nekorox::decodeWaveformPyramid(
            legacyEncoded, decoded, error) &&
        decoded.version == nekorox::kWaveformPyramidLegacyVersion,
        "Waveform V2 reader rejected a valid V1 cache");
    encoded.back() ^= std::byte{0x01};
    require(!nekorox::decodeWaveformPyramid(encoded, decoded, error),
        "Corrupted waveform pyramid passed CRC validation");
}

void testGlyphFlow()
{
    const std::array<float, 6> luminance{
        0.0f, 0.15f, 0.4f, 0.62f, 0.85f, 1.0f};
    const nekorox::GlyphFlowInputV1 input{
        nekorox::kGlyphFlowVersionV1,
        3,
        2,
        luminance,
        73,
        0x123456789abcdef0ULL,
        12,
    };

    nekorox::GlyphFlowV1 first;
    const auto configured = first.configure(input);
    require(configured.ok() && first.cellCount() == luminance.size(),
        "GlyphFlow rejected a valid luminance field");

    std::array<nekorox::GlyphFlowCellV1, luminance.size()> normal{};
    const auto normalSample = first.sample(
        nekorox::GlyphFlowMotionModeV1::Normal,
        180.0,
        normal);
    require(normalSample.ok() && !normalSample.complete,
        "GlyphFlow Normal mode skipped its transition");
    for (const auto& cell : normal) {
        require(
            std::hypot(
                cell.offsetXNormalized,
                cell.offsetYNormalized) <= 1.00001f,
            "GlyphFlow exceeded the one-DIP normalized motion bound");
    }

    std::array<nekorox::GlyphFlowCellV1, luminance.size()> reduced{};
    const auto reducedSample = first.sample(
        nekorox::GlyphFlowMotionModeV1::Reduced,
        180.0,
        reduced);
    require(reducedSample.ok(),
        "GlyphFlow Reduced mode failed");
    for (const auto& cell : reduced) {
        require(
            cell.offsetXNormalized == 0.0f &&
            cell.offsetYNormalized == 0.0f,
            "GlyphFlow Reduced mode introduced spatial motion");
    }

    std::array<nekorox::GlyphFlowCellV1, luminance.size()> off{};
    const auto offSample = first.sample(
        nekorox::GlyphFlowMotionModeV1::Off,
        0.0,
        off);
    require(offSample.ok() && offSample.complete,
        "GlyphFlow Off mode was not instantaneous");
    require(std::all_of(off.begin(), off.end(), [](const auto& cell) {
            return cell.opacity == 1.0f &&
                cell.offsetXNormalized == 0.0f &&
                cell.offsetYNormalized == 0.0f;
        }),
        "GlyphFlow Off mode did not emit its final state");

    nekorox::GlyphFlowV1 second;
    require(second.configure(input).ok(),
        "A second deterministic GlyphFlow configuration failed");
    std::array<nekorox::GlyphFlowCellV1, luminance.size()> duplicate{};
    require(second.sample(
            nekorox::GlyphFlowMotionModeV1::Normal,
            180.0,
            duplicate).ok(),
        "A second deterministic GlyphFlow sample failed");
    for (std::size_t index = 0; index < normal.size(); ++index) {
        require(
            normal[index].glyphIndex == duplicate[index].glyphIndex &&
            normal[index].opacity == duplicate[index].opacity &&
            normal[index].offsetXNormalized ==
                duplicate[index].offsetXNormalized &&
            normal[index].offsetYNormalized ==
                duplicate[index].offsetYNormalized,
            "GlyphFlow changed output for an identical seed and artwork");
    }

    const auto previousGeneration = first.artworkGeneration();
    auto invalid = input;
    invalid.width = 0;
    require(!first.configure(invalid).ok() &&
            first.artworkGeneration() == previousGeneration,
        "Invalid GlyphFlow input mutated the active plan");
}

void testCompressedBlobs()
{
    std::vector<std::byte> source(256U * 1024U);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::byte>(
            (index * 17U + index / 97U) & 0xffU);
    }
    std::vector<std::byte> compressed;
    std::vector<std::byte> restored;
    std::string error;
    require(nekorox::compressZstdBlob(source, compressed, error), error.c_str());
    require(compressed.size() < source.size(),
        "Deterministic cache fixture did not compress");
    require(nekorox::decompressZstdBlob(compressed, restored, error), error.c_str());
    require(restored == source, "Compressed cache blob did not round-trip");

    auto corrupted = compressed;
    corrupted.back() ^= std::byte{0x5a};
    require(!nekorox::decompressZstdBlob(corrupted, restored, error),
        "Corrupted cache blob passed integrity validation");
    require(!nekorox::decompressZstdBlob(compressed, restored, error, 1024),
        "Compressed cache blob ignored its output limit");
}

void testListenLifecycle()
{
    using nekorox::component::ListenLifecycleState;
    constexpr std::uint64_t frequency = 1000;
    ListenLifecycleState lifecycle;

    lifecycle.onTrackChanged(true, 100, frequency);
    require(!lifecycle.analysisPending(),
        "Hidden Listen armed deep analysis");

    require(lifecycle.setVisible(true, 100, frequency),
        "Listen visibility transition was ignored");
    require(!lifecycle.analysisPending(),
        "Visible but paused Listen armed deep analysis");
    require(lifecycle.setPlaying(true, 100, frequency),
        "Listen playback transition was ignored");
    require(lifecycle.analysisPending(),
        "Visible and playing Listen did not arm deep analysis");
    require(lifecycle.remainingMilliseconds(100, frequency) == 1500,
        "Listen deep-analysis dwell was not 1.5 seconds");
    require(!lifecycle.consumeAnalysisDue(1599),
        "Listen deep analysis started before 1.5 seconds");
    require(lifecycle.consumeAnalysisDue(1600),
        "Listen deep analysis did not start after 1.5 seconds");
    require(!lifecycle.analysisPending(),
        "Consumed Listen analysis remained pending");

    lifecycle.requestAnalysis(1700, frequency);
    require(lifecycle.consumeAnalysisDue(1700),
        "Manual analysis ignored an already-satisfied dwell");

    lifecycle.onTrackChanged(true, 2000, frequency);
    require(!lifecycle.consumeAnalysisDue(3499),
        "Track change inherited the previous analysis dwell");
    lifecycle.onTrackChanged(true, 3000, frequency);
    require(!lifecycle.consumeAnalysisDue(4499),
        "Second track change did not rearm analysis");
    require(lifecycle.consumeAnalysisDue(4500),
        "Rearmed track analysis missed its deadline");

    lifecycle.onTrackChanged(true, 5000, frequency);
    require(lifecycle.setPlaying(false, 5100, frequency),
        "Listen pause transition was ignored");
    require(!lifecycle.analysisPending() &&
            !lifecycle.consumeAnalysisDue(7000),
        "Paused Listen retained pending deep analysis");
    require(lifecycle.setPlaying(true, 6000, frequency),
        "Listen resume transition was ignored");
    require(lifecycle.analysisPending(),
        "Listen resume did not rearm deep analysis");
    require(lifecycle.setVisible(false, 6100, frequency),
        "Listen minimize/hide transition was ignored");
    require(!lifecycle.analysisPending() &&
            !lifecycle.consumeAnalysisDue(7000),
        "Hidden Listen retained pending deep analysis");

    require(lifecycle.setVisible(true, 7000, frequency),
        "Listen restore transition was ignored");
    require(!lifecycle.setVisible(true, 8000, frequency),
        "Duplicate visible event was treated as a lifecycle transition");
    require(!lifecycle.consumeAnalysisDue(8499) &&
            lifecycle.consumeAnalysisDue(8500),
        "Listen restore dwell was reset by a duplicate visibility event");

    lifecycle.onTrackChanged(false, 9000, frequency);
    require(!lifecycle.analysisPending(),
        "Non-local track armed deep analysis");
}

void testDawOutputFaultInjection()
{
    SetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", L"1");
    SetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", L"1");
    SetEnvironmentVariableW(L"NEKOROX_QA_DAW_FAULT", L"open");

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    HWAVEOUT output = nullptr;
    const auto result =
        nekorox::component::DawOutputBackend::instance().open(
            &output, &format, nullptr);

    SetEnvironmentVariableW(L"NEKOROX_QA_DAW_FAULT", nullptr);
    SetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", nullptr);
    SetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", nullptr);
    require(result != MMSYSERR_NOERROR && output == nullptr,
        "Guarded DAW open fault was not injected before WinMM");
}

void testRolloutFlags()
{
    SetEnvironmentVariableW(L"GODEN_FLAG_UI_DIRTY_BUS_V1", L"off");
    SetEnvironmentVariableW(L"GODEN_FLAG_LISTEN_DEMAND_V2", L"1");
    const auto dirtyBus = nekorox::component::rolloutFlagEnabled(
        nekorox::component::RolloutFlag::uiDirtyBusV1);
    const auto listenDemand = nekorox::component::rolloutFlagEnabled(
        nekorox::component::RolloutFlag::listenDemandV2);
    const auto playbackDefault = nekorox::component::rolloutFlagEnabled(
        nekorox::component::RolloutFlag::playbackPositionV1);
    SetEnvironmentVariableW(L"GODEN_FLAG_UI_DIRTY_BUS_V1", nullptr);
    SetEnvironmentVariableW(L"GODEN_FLAG_LISTEN_DEMAND_V2", nullptr);
    require(!dirtyBus && listenDemand && playbackDefault,
        "Rollout flags did not honor startup overrides and defaults");
}

void testInteractionLatencyGuard()
{
    auto& guard = nekorox::component::InteractionLatencyGuard::instance();
    require(!guard.preferLatency(),
        "Fresh interaction latency guard started active");
    guard.noteScroll();
    require(guard.preferLatency(),
        "Scroll did not activate the interaction latency guard");
}

void testAmbientTextureScheduler()
{
    nekorox::component::AmbientTextureScheduler scheduler;
    scheduler.setPeriod(30);
    scheduler.reset(100);
    require(scheduler.due(true, 100),
        "Fresh ambient texture revision was not due");
    require(scheduler.defer(true, 100) && scheduler.deadlineQpc() == 130,
        "Ambient defer did not create one future owner deadline");
    require(!scheduler.due(true, 129) && !scheduler.defer(true, 129) &&
            scheduler.deadlineQpc() == 130,
        "Repeated ambient defer slid or bypassed its owned deadline");
    require(scheduler.due(true, 130),
        "Deferred ambient revision was not due at its opportunity");
    require(!scheduler.defer(true, 130) && scheduler.deadlineQpc() == 160,
        "Continuing interaction did not advance exactly one period");
    require(scheduler.resume(true, 140) && scheduler.deadlineQpc() == 140 &&
            scheduler.due(true, 140),
        "Ambient resume did not expose the latest revision immediately");
    scheduler.accepted(140);
    require(!scheduler.deferred() && !scheduler.due(false, 1000),
        "Accepted ambient revision retained deferred demand");
}

void testCuratedC2Contracts()
{
    using nekorox::component::curated::Mode;
    using nekorox::component::curated::Preset;
    using nekorox::component::curated::Response;
    using nekorox::component::curated::Timeline;
    using nekorox::component::curated::acceptedRevisionAfterPresent;
    using nekorox::component::curated::gridFor;

    Timeline timeline;
    timeline.setActive(true, 0.0);
    timeline.defer(0.0);
    const auto ownedDeadline = timeline.deadline();
    for (int observation = 1; observation < 30; ++observation)
        timeline.defer(observation / 1000.0);
    require(std::abs(timeline.deadline() - ownedDeadline) < 1.0e-12,
        "C2 repeated defer slid the owned Curated deadline");
    timeline.resume(0.030);
    require(timeline.due(0.030),
        "C2 resume did not expose the latest Curated pose immediately");

    require(acceptedRevisionAfterPresent(7, 7, 11, true) == 7 &&
            acceptedRevisionAfterPresent(7, 11, 11, false) == 7 &&
            acceptedRevisionAfterPresent(7, 11, 11, true) == 11,
        "C2 presentation ACK advanced without both draw and accepted Present");

    const auto wide = gridFor(Preset::caustics, Mode::music, 16.0f / 9.0f);
    const auto portrait = gridFor(Preset::caustics, Mode::music, 9.0f / 16.0f);
    require(wide.width == portrait.width && wide.height == portrait.height,
        "C2 Curated state grid remained coupled to first viewport aspect");

    nekorox::component::curated::MusicSignals silent;
    silent.relativeFast = {4.0f, 4.0f, 4.0f};
    silent.relativeSlow = {4.0f, 4.0f, 4.0f};
    const auto noForce = nekorox::component::curated::bindMusic(
        Preset::caustics, silent, Response::legacyC2);
    require(noForce.continuous == std::array<float, 3>{} &&
            noForce.impulse == std::array<float, 3>{},
        "C2 relative silence baseline became Curated force");

    nekorox::component::curated::MusicSignals active;
    active.absolute = {.8f, .5f, .2f};
    active.relativeFast = {1.4f, 1.2f, 1.8f};
    active.relativeSlow = {1.1f, 1.3f, 1.2f};
    active.growth = {.7f, .3f, .9f};
    active.attack = {.9f, .4f, .8f};
    active.gate = active.intensity = 1.0f;
    const auto caustics = nekorox::component::curated::bindMusic(
        Preset::caustics, active, Response::legacyC2);
    const auto wave = nekorox::component::curated::bindMusic(
        Preset::lagrangian, active, Response::legacyC2);
    require(caustics.continuous != wave.continuous &&
            caustics.impulse != wave.impulse,
        "C2 presets did not retain explicit distinct music bindings");
}

void testA4GrainQualityPolicy()
{
    using nekorox::component::a4::QualityTier;
    using nekorox::component::a4::grainAmplitudeForTier;

    require(std::abs(grainAmplitudeForTier(QualityTier::full, .070f) - .070f) < 1e-6f,
        "A4 Full tier must preserve the requested film grain");
    require(std::abs(grainAmplitudeForTier(QualityTier::light, .070f) - .0385f) < 1e-6f,
        "A4 Light tier must retain reduced film grain instead of dropping it");
    require(std::abs(grainAmplitudeForTier(QualityTier::fieldOnly, .070f) - .0224f) < 1e-6f,
        "A4 FieldOnly tier must retain a restrained film grain");
    require(grainAmplitudeForTier(QualityTier::staticFallback, .070f) == 0.f,
        "A4 static fallback must remain free of film grain");
    require(grainAmplitudeForTier(QualityTier::full, 1.f) == .09f,
        "A4 film grain must remain capped");

    nekorox::component::a4::DecorativeBudget lockedFull(false);
    for (std::uint64_t frame = 1; frame <= 400; ++frame) {
        nekorox::component::a4::QualifiedGpuSample sample{};
        sample.frameId = frame;
        sample.epoch = lockedFull.epoch();
        sample.presentMask = sample.requiredMask = 15U;
        sample.valid = sample.eligible = true;
        sample.gpuUs = { 800., 800., 800., 800. };
        lockedFull.submit(sample);
    }
    require(lockedFull.tier() == QualityTier::full,
        "A4 installed policy must not downgrade Full quality from timing windows");
    require(lockedFull.snapshot().completedWindows > 0,
        "A4 locked-Full policy must keep reporting timing diagnostics");

    require(lockedFull.configure(false, QualityTier::light),
        "A4 fixed Light policy was not applied");
    require(lockedFull.tier() == QualityTier::light,
        "A4 fixed Light policy did not select Light immediately");
    lockedFull.restart(true);
    require(lockedFull.tier() == QualityTier::light,
        "A4 retry did not preserve the configured fixed quality");
    require(lockedFull.configure(true, QualityTier::full),
        "Retired A4 Auto alias did not select the configured Full tier");
    require(lockedFull.tier() == QualityTier::full,
        "Retired A4 Auto alias did not normalize to Full quality");
    for (std::uint64_t frame = 1; frame <= 98; ++frame) {
        nekorox::component::a4::QualifiedGpuSample sample{};
        sample.frameId = frame;
        sample.epoch = lockedFull.epoch();
        sample.presentMask = sample.requiredMask = 15U;
        sample.valid = sample.eligible = true;
        sample.gpuUs = { 800., 800., 800., 800. };
        lockedFull.submit(sample);
    }
    require(lockedFull.tier() == QualityTier::full,
        "A4 timing diagnostics downgraded Full quality without user input");
}

void testPlaybackPresentationGate()
{
    nekorox::PlaybackPresentationGate gate;
    constexpr auto refresh = 165000U;
    require(gate.shouldPresent(10, 0.0, refresh),
        "Fresh playback gate suppressed its first frame");
    gate.markPresented(10, 0.0, refresh);
    require(!gate.shouldPresent(10, 1.0 / 165.0, refresh),
        "Playback gate repainted an unchanged raster step");
    require(gate.shouldPresent(11, 1.0 / 165.0, refresh),
        "Real playhead movement was suppressed by the cadence gate");

    gate.reset();
    gate.markPresented(10, 0.0, refresh);
    int frames = 1;
    for (int tick = 1; tick <= 165; ++tick) {
        const auto now = static_cast<double>(tick) / 165.0;
        if (!gate.shouldPresent(10, now, refresh)) continue;
        gate.markPresented(10, now, refresh);
        ++frames;
    }
    require(frames == 1,
        "Playback gate repainted an unchanged playhead for one second");

    gate.reset();
    gate.markPresented(10, 0.0, refresh);
    require(!gate.shouldPresent(10, 1.0, refresh),
        "Playback gate repainted an unchanged step after a UI stall");
    require(gate.shouldPresent(12, 1.0, refresh),
        "Playback gate did not select the latest step after a UI stall");
}

void testScrubCrossfade()
{
    constexpr std::array<float, 7> samples{
        -1.0f, -0.63f, -0.1f, 0.0f, 0.17f, 0.72f, 1.0f};
    for (const auto previous : samples) {
        for (const auto next : samples) {
            const auto ceiling = std::max(
                std::abs(previous), std::abs(next));
            for (int step = 0; step <= 128; ++step) {
                const auto phase = static_cast<double>(step) / 128.0;
                const auto output = nekorox::scrubCrossfadeSample(
                    previous, next, phase);
                require(std::isfinite(output) &&
                        std::abs(output) <= ceiling + 1.0e-6f,
                    "Scrub crossfade exceeded its loudest input sample");
            }
        }
    }
    require(std::abs(nekorox::scrubCrossfadeSample(
                0.7f, -0.2f, 0.0) - 0.7f) < 1.0e-6f &&
            std::abs(nekorox::scrubCrossfadeSample(
                0.7f, -0.2f, 1.0) + 0.2f) < 1.0e-6f,
        "Scrub crossfade endpoints changed the source samples");
}

void testPresentationTiming()
{
    constexpr std::uint64_t qpc = 10'000'000U;
    require(nekorox::presentationPeriodTicks(qpc, 60'000U, 1001U) ==
            166'833U,
        "59.94 Hz presentation period lost its rational denominator");
    require(nekorox::presentationPeriodTicks(qpc, 120'000U, 1001U) ==
            83'417U,
        "119.88 Hz presentation period lost its rational denominator");
    require(nekorox::presentationPeriodTicks(qpc, 240U, 1U) == 41'667U,
        "240 Hz presentation period is incorrect");

    require(nekorox::pressureRefreshMilliHz(59'940U) == 59'940U,
        "Pressure mode exceeded a 59.94 Hz monitor");
    require(nekorox::pressureRefreshMilliHz(75'000U) == 60'000U,
        "75 Hz pressure cadence did not preserve the 60 Hz floor");
    require(nekorox::pressureRefreshMilliHz(165'000U) == 82'500U,
        "165 Hz pressure cadence was rounded to an integer refresh");
    require(nekorox::pressureRefreshMilliHz(240'000U) == 120'000U,
        "240 Hz pressure cadence is incorrect");

    constexpr auto tenHzCap = qpc / 10U;
    require(nekorox::pressurePresentationPeriodTicks(
                tenHzCap, qpc, 240'000U) == tenHzCap,
        "Pressure mode defeated a surface-specific 10 Hz cap");
}

void testPublicBranding()
{
    require(nekorox::godenPublicWindowTitle(L"foobar2000") ==
            L"Goden foobar2000",
        "Idle public window title did not use the Goden identity");
    require(nekorox::godenPublicWindowTitle(
                L"Track - Artist - Album - NekoRoX - foobar2000 v2.25.10") ==
            L"Track - Artist - Album - Goden foobar2000 v2.25.10",
        "Playback public window title retained the legacy identity");
    require(nekorox::godenPublicWindowTitle(L"Goden Listen") ==
            L"Goden Listen",
        "Public branding changed an unrelated window title");
}

void testDiscordRelayClientWhenRequested()
{
    std::array<wchar_t, 2048> relay{};
    const auto length = GetEnvironmentVariableW(L"GODEN_RELAY_TEST_ORIGIN",
        relay.data(), static_cast<DWORD>(relay.size()));
    if (length == 0 || length >= relay.size()) return;
    SetEnvironmentVariableW(L"GODEN_PRESENCE_RELAY_URL", relay.data());

    // Complete, CRC-valid 1x1 RGBA PNG.
    std::vector<std::uint8_t> png{
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,
        8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,8,215,99,248,207,
        192,240,31,0,5,0,1,255,114,156,82,103,0,0,0,0,73,69,78,68,174,
        66,96,130};
    nekorox::component::DiscordRelayClient client;
    std::stop_source stop;
    client.start();
    const auto cover = client.registerCover(png, stop.get_token(), client.admissionTicket());
    require(cover.succeeded(), "native relay client could not register cover: " + cover.error);

    const std::wstring url(cover.url.begin(), cover.url.end());
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    require(WinHttpCrackUrl(url.c_str(), 0, 0, &parts) != FALSE,
        "native relay cover URL was invalid");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    const auto session = WinHttpOpen(L"GODEN relay integration test",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    const auto connection = session ? WinHttpConnect(
        session, host.c_str(), parts.nPort, 0) : nullptr;
    const auto request = connection ? WinHttpOpenRequest(connection, L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    require(request && WinHttpSendRequest(request,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr),
        "native relay cover request failed");
    std::vector<std::uint8_t> received;
    for (;;) {
        DWORD available = 0;
        require(WinHttpQueryDataAvailable(request, &available) != FALSE,
            "native relay response length failed");
        if (available == 0) break;
        const auto offset = received.size();
        received.resize(offset + available);
        DWORD read = 0;
        require(WinHttpReadData(request, received.data() + offset,
                available, &read) != FALSE,
            "native relay response read failed");
        received.resize(offset + read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    require(received == png, "native relay did not return exact artwork bytes");
    for (unsigned attempt = 0; attempt < 40U &&
            client.trace().delivery ==
                nekorox::component::DiscordRelayDelivery::none; ++attempt) {
        Sleep(25);
    }
    require(client.trace().delivery ==
            nekorox::component::DiscordRelayDelivery::exact,
        "native relay did not correlate the delivery event");
    client.stop();
}

} // namespace

void runVisualCaptureTests();

void testPauseIntent()
{
    using Intent = nekorox::PauseIntent;
    using Action = Intent::CallbackAction;
    Intent intent;
    require(intent.request(false, 100U, 1250U), "Pause request was inverted");
    require(intent.observe(false, 101U, 1250U) == Action::reconcile,
        "Stale pause callback did not defer reconciliation");
    // A backend delivering the same obsolete edge reentrantly must never
    // provoke another backend call, even while the user intent remains fresh.
    for (unsigned i = 0; i < 10000U; ++i)
        require(intent.observe(false, 102U, 1250U) == Action::ignore,
            "Stale pause callbacks generated an unbounded retry loop");
    require(intent.takeReconciliation(103U, 1250U) == std::optional<bool>{true},
        "Deferred pause target changed");
    require(!intent.takeReconciliation(104U, 1250U),
        "A pause intent reconciled twice");
    require(intent.observe(true, 105U, 1250U) == Action::commit,
        "Committed pause was not accepted");
    require(!intent.request(true, 106U, 1250U), "Resume request failed");
    require(intent.request(true, 107U, 1250U),
        "Rapid toggles used a stale backend state");
    require(intent.observe(false, 108U, 1250U) == Action::reconcile,
        "Newest command lost its reconciliation allowance");
    require(!intent.request(false, 109U, 1250U), "Rapid toggle parity changed");
    require(!intent.takeReconciliation(110U, 1250U),
        "Superseded pause command ran after the newer intent");
    require(intent.observe(true, 111U, 1250U) == Action::reconcile,
        "New resume did not request one deferred reconciliation");
    intent.cancel();
    require(!intent.takeReconciliation(112U, 1250U),
        "Stop did not invalidate a queued pause command");
    require(intent.request(false, 200U, 1250U), "Pause request failed");
    require(intent.observe(false, 1451U, 1250U) == Action::commit,
        "Expired intent overrode a later external transport command");
    require(intent.request(false, 2000U, 1250U), "Pause request failed");
    require(intent.observe(false, 1999U, 1250U) == Action::commit,
        "Backward clock retained a stale pause command");
}

void runEfficiencyPolicyTests();
void runFullReviewRegressionTests();

void runAsyncTextViewTests();

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--database-open-probe")
            return runDatabaseInitializationChild(argv[2]);
        if (argc == 2 && std::string_view(argv[1]) == "--database-init-only") {
            runDatabaseInitializationTests();
            std::cout << "NekoRoX concurrent database initialization: PASS\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--async-text-only") {
            runAsyncTextViewTests();
            std::cout << "NekoRoX async text tests: PASS\n";
            return 0;
        }
        runEfficiencyPolicyTests();
        runFullReviewRegressionTests();
        testRealtimeRing();
        testRealtimeFrameBus();
        testDiscontinuityCommitLatch();
        testCrtXyModel();
        testRealtimeVisualModel();
        testScroll();
        testArtPalette();
        testVisualCollectionMigration();
        testArtworkResampler();
        testPacketParsers();
        testTrackIdentity();
        testDatabaseMigrationsAndHistory();
        runDatabaseInitializationTests();
        testAnalysisProtocol();
        testUiContracts();
        testLyricsTimeline();
        testWaveformPyramid();
        testGlyphFlow();
        testListenLifecycle();
        testDawOutputFaultInjection();
        testRolloutFlags();
        testInteractionLatencyGuard();
        testAmbientTextureScheduler();
        testCuratedC2Contracts();
        testA4GrainQualityPolicy();
        testPerformanceInputPresentationMarkers();
        runWorkpackLayoutTests();
        runWorkpackRetentionTests();
        runWorkpackScrollTests();
        runAlbumScrollTests();
        runWorkpackPresentationTests();
        runCorrectiveR2PacingTests();
        runWorkpackPcmTests();
        runWorkpackLifetimeTests();
        runWorkpackDspWakeTests();
        runWorkpackIdentityTests();
        runWorkpackTagTests();
        runWorkpackImportTests();
        runWorkpackShellTests();
        runWorkpackAlbumSessionTests();
        runCorrectiveR2AlbumSessionTests();
        runCorrectiveR2ShellIntegrationTests();
        runCorrectiveR2TagSelectionTests();
        runCorrectiveR2TagMediaTests();
        runCorrectiveR2ListenTests();
        runCorrectiveR2AuditionTests();
        runWorkpackAlbumViewTests();
        runWorkpackBookletTests();
        runWorkpackScrubTests();
        runWorkpackAuditionTests();
        runWorkpackScopeTests();
        runWorkpackSpectrumTests();
        runWorkpackArtworkTests();
        runR3ProbeTests();
        runR3TagIntegrityTests();
        runR3LifetimeTests();
        runR3AuditionTests();
        runR3LyricsTests();
        runC6HistoryTests();
        runVisualRefinementTests();
        runPixelUiDataTests();
        runR6AmbientFeaturesTests();
        runR7DynamicsTests();
        runC11CoreTests();
        runC13ResponseTests();
        runSpectrumRenderCacheTests();
        runVisualCaptureTests();
        testPlaybackPresentationGate();
        testPauseIntent();
        testPresentationTiming();
        testScrubCrossfade();
        testPublicBranding();
        testDiscordRelayClientWhenRequested();
        testCompressedBlobs();
        std::cout << "NekoRoX core tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NekoRoX core tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
