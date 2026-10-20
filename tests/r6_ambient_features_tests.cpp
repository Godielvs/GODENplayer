#include "nekorox/ambient_features.h"
#include "nekorox/flow_signal.h"
#include "nekorox/pcm_frame_carry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

std::vector<float> tone(std::uint32_t sampleRate, std::uint16_t channels,
    double frequency, double seconds, bool antiphase = false)
{
    const auto frames = static_cast<std::size_t>(sampleRate * seconds);
    std::vector<float> samples(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<float>(0.35 * std::sin(
            2.0 * std::numbers::pi * frequency * frame / sampleRate));
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            samples[frame * channels + channel] =
                antiphase && (channel & 1U) ? -value : value;
        }
    }
    return samples;
}

nekorox::AmbientFeaturesV1 extract(std::span<const float> samples,
    std::uint32_t sampleRate, std::uint16_t channels,
    std::size_t packetFrames)
{
    nekorox::AmbientFeaturesModel model;
    require(model.configure(sampleRate, channels, 7, 0),
        "ambient model configure failed");
    std::uint64_t frame = 0;
    std::uint64_t sequence = 1;
    const auto frames = samples.size() / channels;
    while (frame < frames) {
        const auto count = std::min(packetFrames,
            static_cast<std::size_t>(frames - frame));
        const auto offset = static_cast<std::size_t>(frame) * channels;
        require(model.ingest(samples.subspan(offset, count * channels),
            frame, 7, sequence++, 1000 + frame),
            "ambient packet rejected");
        frame += count;
    }
    return model.snapshot();
}

void testAntiphaseAndPacketization()
{
    const auto inPhase = tone(48000, 2, 70.0, 1.0, false);
    const auto antiphase = tone(48000, 2, 70.0, 1.0, true);
    const auto a = extract(inPhase, 48000, 2, 317);
    const auto b = extract(antiphase, 48000, 2, 317);
    const auto c = extract(inPhase, 48000, 2, 1);
    const auto d = extract(inPhase, 48000, 2, 997);
    require(std::abs(a.level[0] - b.level[0]) < 1.0e-6f,
        "antiphase cancelled ambient power");
    for (std::size_t band = 0; band < nekorox::kAmbientBandCount; ++band) {
        require(std::abs(a.level[band] - c.level[band]) < 1.0e-6f &&
            std::abs(a.level[band] - d.level[band]) < 1.0e-6f,
            "packet split changed ambient envelope");
        require(std::abs(a.absoluteEnergy[band] - c.absoluteEnergy[band]) < 1.0e-6f &&
                std::abs(a.relativeFast[band] - c.relativeFast[band]) < 1.0e-6f &&
                std::abs(a.relativeSlow[band] - c.relativeSlow[band]) < 1.0e-6f &&
                std::abs(a.growth[band] - c.growth[band]) < 1.0e-6f &&
                std::abs(a.attack[band] - c.attack[band]) < 1.0e-6f,
            "packet split changed C2 musical features");
    }
}

void testBandDominanceAndFormats()
{
    constexpr std::array frequencies{70.0, 800.0, 7000.0};
    for (std::size_t expected = 0; expected < frequencies.size(); ++expected) {
        const auto samples = tone(48000, 2, frequencies[expected], 1.0);
        const auto value = extract(samples, 48000, 2, 317);
        const auto dominant = static_cast<std::size_t>(std::distance(
            value.level.begin(),
            std::max_element(value.level.begin(), value.level.end())));
        require(dominant == expected, "tone selected wrong ambient band");
        require(nekorox::validAmbientFeatures(value),
            "ambient snapshot contract invalid");
    }
    for (const auto sampleRate : {8000U, 44100U, 96000U, 192000U, 384000U}) {
        for (const auto channels : {std::uint16_t{1}, std::uint16_t{2},
                 std::uint16_t{6}}) {
            const auto samples = tone(sampleRate, channels, 70.0, 0.15);
            const auto value = extract(samples, sampleRate, channels, 317);
            require(nekorox::validAmbientFeatures(value),
                "supported ambient format became non-finite");
        }
    }
    nekorox::AmbientFeaturesModel invalid;
    require(!invalid.configure(7999, 2, 1),
        "invalid sample rate was accepted");
    require(!invalid.configure(48000, 9, 1),
        "invalid channel count was accepted");
}

void testSilenceDiscontinuityAndPolicy()
{
    std::vector<float> silence(48000 * 2, 0.0f);
    const auto quiet = extract(silence, 48000, 2, 997);
    require(quiet.gate < 1.0e-6f && quiet.level[0] < 1.0e-6f &&
        quiet.level[1] < 1.0e-6f && quiet.level[2] < 1.0e-6f,
        "silence animated ambient");
    require(*std::max_element(quiet.absoluteEnergy.begin(),
                quiet.absoluteEnergy.end()) < 1.0e-6f &&
            *std::max_element(quiet.attack.begin(), quiet.attack.end()) < 1.0e-6f,
        "relative silence escaped the C2 absolute gate");

    nekorox::AmbientFeaturesModel model;
    require(model.configure(48000, 2, 2, 100), "configure failed");
    std::array<float, 20> samples{};
    require(!model.ingest(samples, 101, 2, 1, 0),
        "frame gap was accepted");
    require(!model.ingest(samples, 100, 3, 1, 0),
        "stream mismatch was accepted");

    std::vector<float> dc(48000U * 2U * 3U, 0.35f);
    const auto dcResult = extract(dc, 48000, 2, 317);
    require(*std::max_element(dcResult.level.begin(), dcResult.level.end()) <
            0.02f,
        "DC animated ambient after filter settling");
    auto nonFinite = tone(48000, 2, 800.0, 0.25);
    nonFinite[7] = std::numeric_limits<float>::quiet_NaN();
    nonFinite[103] = std::numeric_limits<float>::infinity();
    const auto finiteResult = extract(nonFinite, 48000, 2, 317);
    require(nekorox::validAmbientFeatures(finiteResult),
        "NaN/Inf escaped the ambient input guard");
    const auto sustained = extract(tone(48000, 2, 800.0, 4.0),
        48000, 2, 997);
    require(sustained.level[1] < 0.9f,
        "sustained tone pinned novelty/envelope at full drive");

    nekorox::AlbumAmbientEligibility policy;
    require(policy.mode == nekorox::AlbumAmbientMode::reactive,
        "new ambient sessions did not default to reactive mode");
    policy.visible = policy.playing = policy.hardware =
        policy.hasArtwork = policy.albumMode = true;
    require(policy.wantsAudio(), "eligible ambient policy stayed off");
    policy.bookletOpen = true;
    require(!policy.wantsAudio(), "booklet did not freeze ambient");
    require(std::abs(nekorox::ambientBackgroundHz(75.0) - 25.0) < 0.001 &&
        std::abs(nekorox::ambientBackgroundHz(144.0) - 28.8) < 0.001,
        "display divisor exceeded ambient cadence");
    const auto fitted = nekorox::fitAmbientTexture(3840, 2160);
    require(fitted.width == 512 && fitted.height == 288,
        "ambient texture was not bounded");
    const auto degraded = nekorox::fitAmbientTexture(3840, 1600, true);
    require(degraded.width <= 320 && degraded.height <= 180,
        "degraded ambient texture exceeded its bound");
    const auto tail = nekorox::ambientTail({1.0f, 0.5f, 0.25f}, 2.0);
    require(tail[0] == 0.0f && tail[1] == 0.0f && tail[2] == 0.0f,
        "ambient tail did not settle");
}

void testFlowA3Signal()
{
    nekorox::FlowSignal flow;
    nekorox::FlowPose pose;
    for (int index = 0; index < 500; ++index) {
        const auto now = index * 0.02;
        require(flow.ingest({{{0.01, 0.002, 0.0005}}, now, 9U}),
            "R10-SI01 source publication was rejected");
        pose = flow.sample(now, true, true, false);
    }
    require(pose.needsFrame && pose.level[0] > 0.3f &&
        pose.level[0] < 0.8f,
        "R9-F01 steady music saturated or failed to animate");
    const auto oldPhase = pose.phase;
    require(flow.ingest({{{0.01, 0.002, 0.0005}}, 10.0, 9U}),
        "R10-SI01 next publication was rejected");
    pose = flow.sample(10.0, true, true, false);
    require(pose.phase != oldPhase, "R9-F02 independent flow phase stopped");
    for (int index = 0; index < 80; ++index) {
        const auto now = 10.02 + index * 0.02;
        pose = flow.sample(now, true, true, false);
    }
    require(!pose.needsFrame,
        "R9-F03 stale source did not release to static");
    require(flow.ingest({{{0.02, 0.0, 0.0}}, 12.0, 10U}),
        "R10-SI04 new playback publication was rejected");
    pose = flow.sample(12.0, true, true, false);
    require(pose.level[1] < 0.01f && pose.level[2] < 0.01f,
        "R9-F04 new playback reused prior-band temporal state");
}

void testFlowA31CadenceAndCarry()
{
    nekorox::FlowSignal sparse;
    nekorox::FlowSignal dense;
    double amMinimum = 1.0;
    double amMaximum = 0.0;
    for (int index = 0; index < 400; ++index) {
        const auto now = index * 0.02;
        const auto modulationDb = 1.5 * std::sin(2.0 * std::numbers::pi *
            2.0 * now);
        const auto power = 0.01 * std::pow(10.0, modulationDb / 10.0);
        const nekorox::FlowBandFeatures features{{{power, 0.001, 0.0001}},
            now, 77U};
        require(sparse.ingest(features) && dense.ingest(features),
            "R10-SI01 cadence fixture publication failed");
        for (int sample = 0; sample < 6; ++sample) {
            (void)dense.sample(now + sample / 300.0, true, true, false);
        }
        const auto left = sparse.sample(now, true, true, false);
        const auto right = dense.sample(now, true, true, false);
        require(std::abs(left.level[0] - right.level[0]) < 1.0e-6f &&
                std::abs(left.phase[2] - right.phase[2]) < 1.0e-6f,
            "R10-SI01 pose changed with paint cadence");
        if (now >= 4.0) {
            amMinimum = std::min(amMinimum, static_cast<double>(left.level[0]));
            amMaximum = std::max(amMaximum, static_cast<double>(left.level[0]));
        }
    }
    require(amMaximum - amMinimum >= 0.10,
        "R10-SI02 +/-1.5 dB AM did not produce visible drive range");

    std::vector<double> source(4096U * 2U);
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<double>(index) / source.size();
    nekorox::PcmFrameCarry carry;
    require(carry.replace(source.data(), 4096U, 48000U, 2U, 3U,
            source.size() * sizeof(float)),
        "R10-AU01 double decoder chunk was rejected");
    std::vector<float> drained;
    require(carry.appendTo(drained, 1440U) == 1440U &&
            carry.appendTo(drained, 1440U) == 1440U &&
            carry.appendTo(drained, 1440U) == 1216U && carry.empty(),
        "R10-AU01 4096->1440 carry lost or duplicated frames");
    require(drained.size() == source.size(),
        "R10-AU02 carry changed scalar count");
    for (std::size_t index = 0; index < drained.size(); ++index) {
        require(std::abs(drained[index] - static_cast<float>(source[index])) <
                1.0e-7f,
            "R10-AU02 carry changed sample order or value");
    }
}

} // namespace

void runR6AmbientFeaturesTests()
{
    testAntiphaseAndPacketization();
    testBandDominanceAndFormats();
    testSilenceDiscontinuityAndPolicy();
    testFlowA3Signal();
    testFlowA31CadenceAndCarry();
}
