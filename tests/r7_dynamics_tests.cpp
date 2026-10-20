#include "nekorox/dynamics_history.h"
#include "nekorox/history_retry.h"

#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <windows.h>

namespace {

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

nekorox::RealtimeFrameV1 frame(std::uint32_t rate, std::uint16_t channels,
    std::uint32_t frames, std::uint64_t first, std::uint64_t stream,
    float (*sample)(std::uint64_t, std::uint16_t))
{
    nekorox::RealtimeFrameV1 value;
    value.sequence = first + 1U;
    value.streamGeneration = stream;
    value.playbackSerial = 1U;
    value.recordingIdentityHash = 1U;
    value.firstFrameIndex = first;
    value.sampleRate = rate;
    value.channels = channels;
    value.flags = nekorox::kRealtimeFrameFlagPcm;
    value.frameCount = frames;
    value.sampleCount = frames * channels;
    for (std::uint32_t n = 0; n < frames; ++n)
        for (std::uint16_t c = 0; c < channels; ++c)
            value.samples[n * channels + c] = sample(first + n, c);
    return value;
}

float sine(std::uint64_t n, std::uint16_t) {
    return static_cast<float>(std::sin(2.0 * 3.14159265358979323846 *
        static_cast<double>(n % 48U) / 48.0));
}
float square(std::uint64_t n, std::uint16_t) { return n % 2U ? 1.0f : -1.0f; }
float duty25(std::uint64_t n, std::uint16_t) { return n % 4U == 0 ? 1.0f : 0.0f; }
float overRange(std::uint64_t, std::uint16_t) { return 1.5f; }
float antiphase(std::uint64_t n, std::uint16_t c) {
    const auto value = sine(n, c);
    return c == 0 ? value : -value;
}

void ingestSeconds(nekorox::DynamicsHistory& history,
    std::uint32_t rate, std::uint16_t channels, std::uint64_t stream,
    float (*sample)(std::uint64_t, std::uint16_t),
    std::uint32_t block)
{
    std::uint64_t cursor = 0;
    while (cursor < rate) {
        const auto amount = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            block, rate - cursor));
        (void)history.ingest(frame(rate, channels, amount, cursor,
            stream, sample));
        cursor += amount;
    }
}

} // namespace

void runR7DynamicsTests()
{
    using nekorox::HistoryFailureClass;
    const auto locked = nekorox::historyRetryDecision(
        "database is locked", 4);
    require(locked.failure == HistoryFailureClass::busyOrLocked &&
        locked.retry && locked.delay == std::chrono::milliseconds(30000),
        "R7-H02 locked database retry policy drifted");
    require(!nekorox::historyRetryDecision("database or disk is full", 1).retry,
        "R7-H02 full storage was retried automatically");
    require(!nekorox::historyRetryDecision("attempt to write a readonly database", 1).retry,
        "R7-H02 readonly storage was retried automatically");
    require(!nekorox::historyRetryDecision("database disk image is malformed", 1).retry,
        "R7-H02 corrupt storage was retried automatically");
    require(!nekorox::historyRetryDecision("database schema is newer", 1).retry,
        "R7-H02 future schema was retried automatically");
    require(!nekorox::historyRetryDecision("temporary I/O error", 5).retry,
        "R7-H02 transient retry budget was unbounded");
    for (const auto rate : {44100U, 48000U, 96000U, 192000U}) {
        for (const auto block : {1U, 317U, 997U}) {
            nekorox::DynamicsHistory history;
            ingestSeconds(history, rate, 2, 7, sine, block);
            require(history.size() == 40U,
                "R7-D03 one second did not close exactly 40 bins");
            const auto reading = history.reading(16);
            require(reading.complete && reading.rmsDb() && reading.crestDb() &&
                std::abs(*reading.rmsDb() + 3.0103) < 0.02 &&
                std::abs(*reading.crestDb() - 3.0103) < 0.02,
                "R7-D01 full-scale sine measurement is incorrect");
        }
    }
    nekorox::DynamicsHistory squareHistory;
    ingestSeconds(squareHistory, 44100, 1, 8, square, 997);
    const auto squareReading = squareHistory.reading(16);
    require(squareReading.rmsDb() && squareReading.crestDb() &&
        std::abs(*squareReading.rmsDb()) < 0.001 &&
        std::abs(*squareReading.crestDb()) < 0.001,
        "R7-D01 square peak/RMS diverged");

    nekorox::DynamicsHistory dutyHistory;
    ingestSeconds(dutyHistory, 48000, 1, 8, duty25, 317);
    const auto dutyReading = dutyHistory.reading(16);
    require(dutyReading.crestDb() &&
        std::abs(*dutyReading.crestDb() - 6.0206) < 0.02,
        "R7-D01 25-percent duty crest measurement is incorrect");

    nekorox::DynamicsHistory overHistory;
    ingestSeconds(overHistory, 48000, 1, 8, overRange, 997);
    require(overHistory.reading(16).peakDb() &&
        *overHistory.reading(16).peakDb() > 3.5,
        "R7-D04 PCM above unity was silently clamped");

    nekorox::DynamicsHistory inPhase;
    nekorox::DynamicsHistory anti;
    ingestSeconds(inPhase, 48000, 2, 9, sine, 317);
    ingestSeconds(anti, 48000, 2, 9, antiphase, 317);
    require(inPhase.reading(16).rmsDb() && anti.reading(16).rmsDb() &&
        std::abs(*inPhase.reading(16).rmsDb() -
            *anti.reading(16).rmsDb()) < 0.001,
        "R7-D02 antiphase channels cancelled in dynamics");

    const auto before = anti.revision();
    auto gap = frame(48000, 2, 400, 49000, 9, antiphase);
    gap.flags |= nekorox::kRealtimeFrameFlagDiscontinuity;
    (void)anti.ingest(gap, true);
    require(anti.revision() > before && anti.size() >= 40U,
        "R8-D01 discontinuity erased retained history");
    require(!anti.reading(16).complete,
        "R8-D02 incomplete post-gap window was reported continuous");
    const auto gapSnapshot = anti.snapshot(gap.sequence, gap.qpcTicks);
    require(gapSnapshot.count >= 40U && gapSnapshot.dataGap &&
            gapSnapshot.unknownFrames != 0,
        "R8-D03 gap marker was not published with retained bins");

    std::uint64_t cursor = gap.firstFrameIndex + gap.frameCount;
    for (unsigned block = 0; block < 48; ++block) {
        auto continued = frame(48000, 2, 400, cursor, 9, antiphase);
        (void)anti.ingest(continued);
        cursor += continued.frameCount;
    }
    require(anti.size() > 40U && anti.reading(16).complete,
        "R8-D04 continuous post-gap window did not recover naturally");
    const auto recovered = anti.snapshot(cursor, 0);
    bool observedBoundary = false;
    for (std::size_t index = 1; index < recovered.count; ++index) {
        if (recovered.bins[index - 1U].segmentId !=
                recovered.bins[index].segmentId) {
            observedBoundary = true;
            break;
        }
    }
    require(observedBoundary,
        "R8-D05 retained history lost its explicit segment boundary");

    // R9: view/source churn is a segment, while only an authoritative
    // playback admission starts a new epoch.
    nekorox::DynamicsHistory continuity;
    std::uint64_t first = 0;
    for (unsigned block = 0; block < 120; ++block) {
        auto value = frame(48000, 2, 400, first, 20, sine);
        value.playbackSerial = 10;
        value.recordingIdentityHash = 0xabc;
        (void)continuity.ingest(value);
        first += value.frameCount;
    }
    const auto retained = continuity.size();
    const auto retainedEpoch = continuity.epoch();
    auto seek = frame(44100, 2, 441, 0, 21, sine);
    seek.playbackSerial = 10;
    seek.recordingIdentityHash = 0xabc;
    seek.flags |= nekorox::kRealtimeFrameFlagDiscontinuity;
    (void)continuity.ingest(seek, true);
    require(continuity.size() == retained &&
        continuity.epoch() == retainedEpoch,
        "R9-D01 seek/format boundary hard-reset Dynamics");
    auto nextTrack = frame(48000, 2, 1200, 0, 22, sine);
    nextTrack.playbackSerial = 11;
    nextTrack.recordingIdentityHash = 0xdef;
    (void)continuity.ingest(nextTrack, true);
    require(continuity.epoch() == retainedEpoch + 1U &&
        continuity.size() == 1U,
        "R9-D02 authoritative playback did not reset exactly once");
    auto stale = frame(48000, 2, 1200, 48000, 20, sine);
    stale.playbackSerial = 10;
    stale.recordingIdentityHash = 0xabc;
    require(!continuity.ingest(stale) &&
        continuity.epoch() == retainedEpoch + 1U,
        "R9-D03 delayed old PCM crossed playback admission");

    const auto protectedEpoch = continuity.epoch();
    auto zeroIdentity = frame(48000, 2, 1200, 1200, 23, sine);
    zeroIdentity.playbackSerial = 0;
    zeroIdentity.recordingIdentityHash = 0;
    require(!continuity.ingest(zeroIdentity) &&
            continuity.epoch() == protectedEpoch,
        "R10-DY01 zero identity cleared the admitted epoch");
    auto conflicting = frame(48000, 2, 1200, 1200, 23, sine);
    conflicting.playbackSerial = 11;
    conflicting.recordingIdentityHash = 0xbeef;
    require(!continuity.ingest(conflicting) &&
            continuity.epoch() == protectedEpoch,
        "R10-DY01 same-serial conflicting identity cleared the epoch");

    LARGE_INTEGER frequency{};
    require(QueryPerformanceFrequency(&frequency) != FALSE,
        "R10-DY03 QPC unavailable");
    const auto hz = static_cast<std::uint64_t>(frequency.QuadPart);
    nekorox::DynamicsHistory temporal;
    auto firstTimed = frame(48000, 2, 1200, 0, 30, sine);
    firstTimed.playbackSerial = 30;
    firstTimed.recordingIdentityHash = 0x3030;
    firstTimed.qpcTicks = hz;
    require(temporal.ingest(firstTimed),
        "R10-DY03 initial timed bin did not close");
    auto fiveSecondGap = frame(48000, 2, 1200, 1200, 31, sine);
    fiveSecondGap.playbackSerial = 30;
    fiveSecondGap.recordingIdentityHash = 0x3030;
    fiveSecondGap.qpcTicks = hz * 6U + hz / 40U;
    require(temporal.ingest(fiveSecondGap, true),
        "R10-DY03 post-gap timed bin did not close");
    const auto temporalSnapshot = temporal.snapshot(
        fiveSecondGap.sequence, fiveSecondGap.qpcTicks);
    bool explicitUnknown = false;
    for (std::size_t index = 0; index < temporalSnapshot.count; ++index) {
        const auto& bin = temporalSnapshot.bins[index];
        if ((bin.flags & nekorox::kDynamicsBinUnknown) != 0 &&
                bin.endQpc > bin.startQpc &&
                bin.endQpc - bin.startQpc >= hz * 49U / 10U) {
            explicitUnknown = true;
            break;
        }
    }
    require(explicitUnknown,
        "R10-DY03 five-second gap was compressed out of the timeline");
    auto expired = frame(48000, 2, 1200, 2400, 32, sine);
    expired.playbackSerial = 30;
    expired.recordingIdentityHash = 0x3030;
    expired.qpcTicks = hz * 15U + hz / 40U;
    require(temporal.ingest(expired, true) && temporal.size() <= 2U,
        "R10-DY03 >=8 second gap did not expire old bins naturally");

    // C10: retain a partial measured bin, represent the missing interval as
    // unknown (not silence), and sanitize invalid scalars locally.
    nekorox::DynamicsHistory partial;
    auto seventyPercent = frame(48000, 1, 840, 0, 50, square);
    seventyPercent.playbackSerial = 50;
    seventyPercent.recordingIdentityHash = 0x5050;
    (void)partial.ingest(seventyPercent);
    auto afterGap = frame(48000, 1, 1200, 1200, 50, square);
    afterGap.playbackSerial = 50;
    afterGap.recordingIdentityHash = 0x5050;
    require(partial.ingest(afterGap, true),
        "C10-DYN01 partial/gap sequence published no bins");
    const auto partialSnapshot = partial.snapshot(afterGap.sequence, 0);
    require(partialSnapshot.count >= 3U &&
            (partialSnapshot.bins[0].flags &
                nekorox::kDynamicsBinPartial) != 0 &&
            std::abs(partialSnapshot.bins[0].coverage() - 1.0) < 0.001 &&
            partialSnapshot.bins[0].nominalFrames == 840U,
        "C11-DYN01 known partial bin was mislabeled as unknown audio");
    require((partialSnapshot.bins[1].flags &
                nekorox::kDynamicsBinUnknown) != 0 &&
            !partialSnapshot.bins[1].measured(),
        "C10-DYN02 unknown gap was presented as measured silence");

    nekorox::DynamicsHistory silence;
    auto zero = frame(48000, 1, 1200, 0, 51,
        +[](std::uint64_t, std::uint16_t) { return 0.0f; });
    zero.playbackSerial = 51;
    zero.recordingIdentityHash = 0x5151;
    require(silence.ingest(zero) && silence.snapshot(1, 0).bins[0].measured(),
        "C10-DYN02 measured silence was classified as unknown");

    auto invalid = frame(48000, 1, 1200, 1200, 51, square);
    invalid.playbackSerial = 51;
    invalid.recordingIdentityHash = 0x5151;
    invalid.samples[5] = std::numeric_limits<float>::quiet_NaN();
    require(silence.ingest(invalid) &&
            silence.snapshot(2, 0).sanitizedSamples == 1U,
        "C10-DYN03 invalid sample rejected the full PCM frame");
}
