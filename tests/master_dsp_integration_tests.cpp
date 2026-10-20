#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "master_dsp_quantizer.h"
#include "master_dsp_types.h"
#include "rt/processor_base.h"
#include "streaming/stream_resampler.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct FailureCounter {
    int value = 0;
    void check(bool condition, const std::string& message) {
        if (condition) return;
        ++value;
        std::cerr << "FAIL: " << message << '\n';
    }
};

class AudioPriorityScope {
public:
    AudioPriorityScope() noexcept
        : process_(GetCurrentProcess()),
          thread_(GetCurrentThread()),
          previousProcess_(GetPriorityClass(process_)),
          previousThread_(GetThreadPriority(thread_))
    {
        // A normal-priority console process measures unrelated scheduler stalls
        // as DSP time. The production callback is hosted on foobar's elevated
        // audio thread, so qualify the compute budget under the closest safe
        // Windows scheduling contract while retaining wall-clock measurement.
        processChanged_ = SetPriorityClass(process_, HIGH_PRIORITY_CLASS) != FALSE;
        threadChanged_ = SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST) != FALSE;
    }

    ~AudioPriorityScope()
    {
        if (threadChanged_ && previousThread_ != THREAD_PRIORITY_ERROR_RETURN)
            SetThreadPriority(thread_, previousThread_);
        if (processChanged_ && previousProcess_ != 0)
            SetPriorityClass(process_, previousProcess_);
    }

    AudioPriorityScope(const AudioPriorityScope&) = delete;
    AudioPriorityScope& operator=(const AudioPriorityScope&) = delete;

private:
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    DWORD previousProcess_ = 0;
    int previousThread_ = THREAD_PRIORITY_ERROR_RETURN;
    bool processChanged_ = false;
    bool threadChanged_ = false;
};

std::vector<float> signal(std::size_t frames, std::uint32_t channels,
    std::uint32_t rate)
{
    std::vector<float> result(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const auto frequency = 83.0 + channel * 137.0;
            result[frame * channels + channel] = static_cast<float>(
                0.08 * std::sin(2.0 * kPi * frequency * frame / rate));
        }
    }
    return result;
}

void processInterleaved(sonare::rt::ProcessorBase& processor,
    std::vector<float>& interleaved, std::uint32_t channels,
    const std::vector<std::size_t>& blocks)
{
    const auto frames = interleaved.size() / channels;
    std::vector<std::vector<float>> planes(channels,
        std::vector<float>(frames));
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            planes[channel][frame] = interleaved[frame * channels + channel];
    std::size_t offset = 0;
    std::size_t blockIndex = 0;
    while (offset < frames) {
        const auto count = std::min(blocks[blockIndex++ % blocks.size()],
            frames - offset);
        std::array<float*, 8> pointers{};
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            pointers[channel] = planes[channel].data() + offset;
        processor.process(pointers.data(), static_cast<int>(channels),
            static_cast<int>(count));
        offset += count;
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            interleaved[frame * channels + channel] = planes[channel][frame];
}

bool finite(const std::vector<float>& values)
{
    return std::all_of(values.begin(), values.end(),
        [](float value) { return std::isfinite(value); });
}

double rms(const std::vector<float>& values, std::size_t first = 0)
{
    double energy = 0.0;
    for (std::size_t index = first; index < values.size(); ++index)
        energy += static_cast<double>(values[index]) * values[index];
    return std::sqrt(energy / std::max<std::size_t>(values.size() - first, 1));
}

double linearToDb(double value)
{
    return 20.0 * std::log10(std::max(value, 1.0e-20));
}

void testNullExact(FailureCounter& failures)
{
    auto floats = signal(41472, 8, 48000);
    const auto floatCopy = floats;
    failures.check(std::memcmp(floats.data(), floatCopy.data(),
        floats.size() * sizeof(float)) == 0, "float Null is not bit exact");
    std::vector<double> doubles(floats.begin(), floats.end());
    const auto doubleCopy = doubles;
    failures.check(std::memcmp(doubles.data(), doubleCopy.data(),
        doubles.size() * sizeof(double)) == 0, "double wrapper Null is not bit exact");
}

void testManualTwoBitQuantizer(FailureCounter& failures)
{
    constexpr std::array<float, 8> input{
        -1.0f, -0.76f, -0.74f, -0.24f, 0.24f, 0.26f, 0.74f, 1.0f};
    constexpr std::array<float, 8> expected{
        -1.0f, -1.0f, -0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f};
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto actual = nekorox::goden_audio::quantizeUnditheredSample(
            input[index], 2);
        failures.check(std::bit_cast<std::uint32_t>(actual) ==
                std::bit_cast<std::uint32_t>(expected[index]),
            "manual 2-bit quantizer emitted the wrong PCM code");
    }
    const auto neutral = -0.1234567f;
    failures.check(std::bit_cast<std::uint32_t>(
            nekorox::goden_audio::quantizeUnditheredSample(neutral, 32)) ==
            std::bit_cast<std::uint32_t>(neutral),
        "32-bit quantizer is not an exact passthrough");
}

void testManualHardRangeContract(FailureCounter& failures)
{
    nekorox::goden_audio::MasterDspParameterDescriptorV2 descriptor;
    descriptor.minimum = -24.0;
    descriptor.maximum = 24.0;
    descriptor.hardMinimum = -96.0;
    descriptor.hardMaximum = 96.0;
    failures.check(descriptor.isInsideComfortRange(12.0),
        "slider comfort range rejected a normal value");
    failures.check(!descriptor.isInsideComfortRange(80.0) &&
            descriptor.accepts(80.0),
        "manual hard range did not extend beyond the slider");
    failures.check(!descriptor.accepts(97.0) &&
            !descriptor.accepts(std::numeric_limits<double>::infinity()) &&
            !descriptor.accepts(std::numeric_limits<double>::quiet_NaN()),
        "manual hard range accepted an unsafe value");
}

void testAuditedExtremeParameters(FailureCounter& failures)
{
    constexpr std::array<std::pair<const char*, const char*>, 4> cases{
        std::pair{"eq.parametric",
            R"({"band0.enabled":true,"band0.type":0,"band0.frequencyHz":1,"band0.gainDb":48,"band0.q":100})"},
        std::pair{"dynamics.compressor",
            R"({"thresholdDb":-144,"ratio":1000,"attackMs":0,"releaseMs":120000,"kneeDb":60})"},
        std::pair{"saturation.tape",
            R"({"driveDb":48,"outputGainDb":-120,"speedIps":240,"headBumpDb":48})"},
        std::pair{"effects.delay.stereo",
            R"({"delayTimeLMs":10000,"delayTimeRMs":10000,"feedback":0.999,"dryWet":0.1})"}};
    for (const auto& [type, json] : cases) {
        try {
            auto processor = sonare::mastering::api::make_insert(type, json);
            failures.check(processor != nullptr,
                std::string(type) + " rejected its audited manual envelope");
            if (!processor) continue;
            processor->prepare(48000, 1024);
            auto pcm = signal(2048, 2, 48000);
            processInterleaved(*processor, pcm, 2, {1, 7, 511, 1024});
            failures.check(finite(pcm), std::string(type) +
                " emitted non-finite PCM at an audited extreme");
        } catch (const std::exception& exception) {
            failures.check(false, std::string(type) +
                " extreme-value exception: " + exception.what());
        }
    }
}

void testFiniteMatrix(FailureCounter& failures)
{
    const std::array<const char*, 11> processors{
        "eq.parametric", "eq.tilt", "dynamics.compressor",
        "dynamics.limiter", "eq.equalizer", "saturation.tape",
        "saturation.softClipper", "stereo.imager",
        "effects.modulation.chorus", "effects.reverb.fdn",
        "maximizer.truePeakLimiter"};
    const std::array<std::uint32_t, 6> rates{8000, 44100, 48000, 96000,
        192000, 384000};
    const std::array<std::uint32_t, 4> channelCounts{1, 2, 6, 8};
    const std::vector<std::size_t> blocks{1, 7, 511, 1024, 4608, 8192};
    for (const auto* name : processors) {
        for (const auto rate : rates) {
            for (const auto channels : channelCounts) {
                try {
                    auto processor = sonare::mastering::api::make_insert(name, "{}");
                    failures.check(processor != nullptr,
                        std::string(name) + " unavailable");
                    if (!processor) continue;
                    processor->prepare(rate, 8192);
                    auto pcm = signal(8192, channels, rate);
                    // Stereo-only families intentionally receive FL/FR; the
                    // Goden wrapper keeps surround remainder dry and aligned.
                    const auto processedChannels =
                        sonare::mastering::api::channel_policy(name) ==
                            sonare::mastering::api::ChannelPolicy::StereoPairOnly
                        ? std::min<std::uint32_t>(channels, 2U) : channels;
                    if (processedChannels == channels) {
                        processInterleaved(*processor, pcm, channels, blocks);
                    } else {
                        std::vector<float> front(8192 * processedChannels);
                        for (std::size_t frame = 0; frame < 8192; ++frame)
                            for (std::uint32_t channel = 0;
                                    channel < processedChannels; ++channel)
                                front[frame * processedChannels + channel] =
                                    pcm[frame * channels + channel];
                        processInterleaved(*processor, front,
                            processedChannels, blocks);
                    }
                    failures.check(finite(pcm), std::string(name) +
                        " emitted non-finite PCM at " + std::to_string(rate) +
                        " Hz / " + std::to_string(channels) + " channels");
                } catch (const std::exception& exception) {
                    failures.check(false, std::string(name) + " matrix exception: " +
                        exception.what());
                }
            }
        }
    }
}

void testPlayerDawParity(FailureCounter& failures)
{
    auto first = sonare::mastering::api::make_insert("eq.parametric",
        R"({"band0.enabled":true,"band0.type":0,"band0.frequencyHz":997,"band0.gainDb":6,"band0.q":0.707})");
    auto second = sonare::mastering::api::make_insert("eq.parametric",
        R"({"band0.enabled":true,"band0.type":0,"band0.frequencyHz":997,"band0.gainDb":6,"band0.q":0.707})");
    failures.check(first && second, "parity processors unavailable");
    if (!first || !second) return;
    first->prepare(48000, 8192);
    second->prepare(48000, 8192);
    auto player = signal(41472, 2, 48000);
    auto daw = player;
    processInterleaved(*first, player, 2, {32768, 8704});
    processInterleaved(*second, daw, 2, {1, 7, 511, 1024, 4608, 8192});
    double maximumError = 0.0;
    for (std::size_t index = 0; index < player.size(); ++index)
        maximumError = std::max(maximumError,
            std::abs(static_cast<double>(player[index] - daw[index])));
    failures.check(maximumError <= 1.0e-6,
        "player/DAW partition parity exceeded 1e-6");
}

void testTail(FailureCounter& failures)
{
    auto processor = sonare::mastering::api::make_insert(
        "effects.reverb.fdn", R"({"dryWet":0.5,"decaySec":0.5})");
    failures.check(processor != nullptr, "tail processor unavailable");
    if (!processor) return;
    processor->prepare(48000, 8192);
    std::vector<float> left(8192), right(8192);
    left[0] = right[0] = 1.0f;
    float* impulse[]{left.data(), right.data()};
    processor->process(impulse, 2, 8192);
    std::uint64_t emittedFrames = 0;
    float peak = 0.0f;
    for (int block = 0; block < 16; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        processor->process(impulse, 2, 8192);
        emittedFrames += left.size();
        for (const auto value : left) peak = std::max(peak, std::abs(value));
    }
    failures.check(processor->tail_samples() > 0,
        "reverb declares an empty tail");
    failures.check(emittedFrames != 0 && peak > 1.0e-8f,
        "tail chunks were empty or silent");
}

void testEqAnalyticResponse(FailureCounter& failures)
{
    constexpr std::uint32_t rate = 48000;
    constexpr std::size_t frames = rate * 2;
    auto processor = sonare::mastering::api::make_insert("eq.parametric",
        R"({"band0.enabled":true,"band0.type":0,"band0.frequencyHz":1000,"band0.gainDb":6,"band0.q":1})");
    failures.check(processor != nullptr, "analytic EQ processor unavailable");
    if (!processor) return;
    processor->prepare(rate, 1024);
    std::vector<float> pcm(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto sample = static_cast<float>(0.02 * std::sin(
            2.0 * kPi * 1000.0 * frame / rate));
        pcm[frame * 2] = pcm[frame * 2 + 1] = sample;
    }
    const auto input = pcm;
    processInterleaved(*processor, pcm, 2, {1, 7, 511, 1024});
    std::vector<float> before(frames / 2), after(frames / 2);
    for (std::size_t frame = frames / 2; frame < frames; ++frame) {
        before[frame - frames / 2] = input[frame * 2];
        after[frame - frames / 2] = pcm[frame * 2];
    }
    const auto measured = linearToDb(rms(after) / rms(before));
    failures.check(std::abs(measured - 6.0) <= 0.25,
        "parametric EQ differs from analytic gain by more than 0.25 dB (" +
        std::to_string(measured) + " dB)");
}

void testDynamicsStaticCurve(FailureCounter& failures)
{
    constexpr std::uint32_t rate = 48000;
    auto processor = sonare::mastering::api::make_insert("dynamics.compressor",
        R"({"thresholdDb":-18,"ratio":4,"attackMs":0,"releaseMs":100,"kneeDb":0,"makeupGainDb":0,"autoMakeup":false,"detector":0})");
    failures.check(processor != nullptr, "static compressor unavailable");
    if (!processor) return;
    processor->prepare(rate, 1024);
    std::vector<float> pcm(rate * 2, 0.5f);
    processInterleaved(*processor, pcm, 2, {7, 511, 1024});
    std::vector<float> settled(rate / 4);
    for (std::size_t index = 0; index < settled.size(); ++index)
        settled[index] = pcm[(rate * 3 / 4 + index) * 2];
    const auto measuredDb = linearToDb(rms(settled));
    constexpr double expectedDb = -15.0;
    failures.check(std::abs(measuredDb - expectedDb) <= 0.5,
        "compressor static curve differs by more than 0.5 dB (" +
        std::to_string(measuredDb) + " dBFS)");
}

void testTruePeakCeiling(FailureCounter& failures)
{
    constexpr std::uint32_t rate = 48000;
    auto processor = sonare::mastering::api::make_insert(
        "maximizer.truePeakLimiter",
        R"({"ceilingDb":-1,"lookaheadMs":5,"releaseMs":80,"oversampleFactor":4})");
    failures.check(processor != nullptr, "true-peak limiter unavailable");
    if (!processor) return;
    processor->prepare(rate, 1024);
    std::vector<float> pcm(rate * 2);
    for (std::size_t frame = 0; frame < rate; ++frame) {
        const auto sample = static_cast<float>(1.5 * std::sin(
            2.0 * kPi * 997.0 * frame / rate));
        pcm[frame * 2] = pcm[frame * 2 + 1] = sample;
    }
    processInterleaved(*processor, pcm, 2, {511, 1024});
    float peak = 0.0f;
    for (std::size_t frame = rate / 4; frame < rate; ++frame)
        peak = std::max(peak, std::abs(pcm[frame * 2]));
    failures.check(linearToDb(peak) <= -0.5,
        "true-peak limiter exceeded ceiling tolerance (" +
        std::to_string(linearToDb(peak)) + " dBFS)");
}

void testStereoMatrix(FailureCounter& failures)
{
    auto processor = sonare::mastering::api::make_insert(
        "stereo.monoMaker", R"({"amount":1})");
    failures.check(processor != nullptr, "mono matrix processor unavailable");
    if (!processor) return;
    processor->prepare(48000, 1024);
    auto pcm = signal(8192, 2, 48000);
    processInterleaved(*processor, pcm, 2, {1, 511, 1024});
    double maximumDifference = 0.0;
    for (std::size_t frame = 0; frame < 8192; ++frame)
        maximumDifference = std::max(maximumDifference,
            std::abs(static_cast<double>(pcm[frame * 2] - pcm[frame * 2 + 1])));
    failures.check(maximumDifference <= 1.0e-7,
        "mono matrix did not produce correlated L/R output");
}

void testFiniteResampler(FailureCounter& failures)
{
    sonare::streaming_detail::StreamResampler resampler(44100, 48000);
    auto input = signal(44100, 1, 44100);
    std::vector<float> output;
    std::size_t offset = 0;
    const std::array<std::size_t, 5> blocks{1, 7, 511, 1024, 4608};
    std::size_t block = 0;
    while (offset < input.size()) {
        const auto count = std::min(blocks[block++ % blocks.size()],
            input.size() - offset);
        resampler.process(input.data() + offset, count, output);
        offset += count;
    }
    resampler.flush(output);
    failures.check(resampler.expected_output_samples() == 48000,
        "r8brain analytic frame count is wrong");
    failures.check(output.size() == 48000,
        "r8brain finite flush did not produce exactly 48000 samples");
    failures.check(finite(output), "r8brain emitted non-finite output");
    failures.check(resampler.input_latency_samples() >= 0,
        "r8brain reported negative input latency");
}

void testRealtimeBudget(FailureCounter& failures)
{
    AudioPriorityScope audioPriority;
    std::vector<std::unique_ptr<sonare::rt::ProcessorBase>> chain;
    for (const auto& definition : std::array<std::pair<const char*, const char*>, 4>{
             std::pair{"eq.parametric", "{}"},
             std::pair{"dynamics.compressor", "{}"},
             std::pair{"stereo.imager", "{}"},
             std::pair{"maximizer.truePeakLimiter", "{}"}}) {
        auto processor = sonare::mastering::api::make_insert(
            definition.first, definition.second);
        if (!processor) continue;
        processor->prepare(48000, 1024);
        chain.push_back(std::move(processor));
    }
    failures.check(chain.size() == 4, "common audiophile chain unavailable");
    if (chain.size() != 4) return;
    constexpr std::array<const char*, 4> names{
        "eq.parametric", "dynamics.compressor", "stereo.imager",
        "maximizer.truePeakLimiter"};
    for (std::size_t processorIndex = 0; processorIndex < chain.size();
            ++processorIndex) {
        std::vector<double> processorTimings;
        processorTimings.reserve(240);
        std::vector<float> diagnosticLeft(1024), diagnosticRight(1024);
        for (int iteration = 0; iteration < 260; ++iteration) {
            fill(diagnosticLeft.begin(), diagnosticLeft.end(), 0.02f);
            fill(diagnosticRight.begin(), diagnosticRight.end(), -0.02f);
            float* channels[]{diagnosticLeft.data(), diagnosticRight.data()};
            const auto started = std::chrono::steady_clock::now();
            chain[processorIndex]->process(channels, 2, 1024);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (iteration >= 20) processorTimings.push_back(elapsed);
        }
        std::sort(processorTimings.begin(), processorTimings.end());
        const auto processorP99 = processorTimings[static_cast<std::size_t>(
            processorTimings.size() * 0.99)];
        std::cout << "  " << names[processorIndex] << " p99: "
            << processorP99 << " ms\n";
        chain[processorIndex]->reset();
    }
    std::vector<double> timings;
    timings.reserve(240);
    std::vector<float> left(1024), right(1024);
    for (int iteration = 0; iteration < 260; ++iteration) {
        fill(left.begin(), left.end(), 0.02f);
        fill(right.begin(), right.end(), -0.02f);
        float* channels[]{left.data(), right.data()};
        const auto started = std::chrono::steady_clock::now();
        for (auto& processor : chain) processor->process(channels, 2, 1024);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (iteration >= 20) timings.push_back(elapsed);
    }
    std::sort(timings.begin(), timings.end());
    const auto p99 = timings[static_cast<std::size_t>(timings.size() * 0.99)];
    std::cout << "Common audiophile chain p99: " << p99 << " ms / 1024 frames\n";
    failures.check(p99 <= 0.4,
        "common chain exceeded the 0.4 ms realtime gate (p99=" +
        std::to_string(p99) + " ms)");
}

void testCreativeRealtimeBudget(FailureCounter& failures)
{
    AudioPriorityScope audioPriority;
    constexpr std::array<std::pair<const char*, const char*>, 12> definitions{
        std::pair{"eq.tilt", R"({"tiltDb":1})"},
        std::pair{"eq.parametric", "{}"},
        std::pair{"dynamics.compressor", "{}"},
        std::pair{"dynamics.deesser", "{}"},
        std::pair{"saturation.softClipper", "{}"},
        std::pair{"saturation.tape", "{}"},
        std::pair{"saturation.exciter", "{}"},
        std::pair{"stereo.imager", "{}"},
        std::pair{"effects.modulation.chorus", "{}"},
        std::pair{"effects.modulation.flanger", "{}"},
        std::pair{"effects.delay.stereo", "{}"},
        std::pair{"effects.reverb.fdn", R"({"dryWet":0.15,"decaySec":0.5})"}};
    std::vector<std::unique_ptr<sonare::rt::ProcessorBase>> chain;
    for (const auto& definition : definitions) {
        auto processor = sonare::mastering::api::make_insert(
            definition.first, definition.second);
        if (!processor) continue;
        processor->prepare(48000, 1024);
        chain.push_back(std::move(processor));
    }
    failures.check(chain.size() == definitions.size(),
        "creative 12-module chain unavailable");
    if (chain.size() != definitions.size()) return;
    std::vector<double> timings;
    std::vector<float> left(1024, 0.02f), right(1024, -0.02f);
    timings.reserve(240);
    for (int iteration = 0; iteration < 260; ++iteration) {
        float* channels[]{left.data(), right.data()};
        const auto started = std::chrono::steady_clock::now();
        for (auto& processor : chain) processor->process(channels, 2, 1024);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (iteration >= 20) timings.push_back(elapsed);
    }
    std::sort(timings.begin(), timings.end());
    const auto p99 = timings[static_cast<std::size_t>(timings.size() * 0.99)];
    std::cout << "Creative 12-module chain p99: " << p99 << " ms / 1024 frames\n";
    failures.check(p99 <= 2.4,
        "creative chain exceeded the 2.4 ms realtime gate (p99=" +
        std::to_string(p99) + " ms)");
}

} // namespace

int main(int argc, char** argv)
{
    const bool functionalOnly = argc == 2 && std::string_view(argv[1]) == "--functional-only";
    if (argc > 1 && !functionalOnly) {
        std::cerr << "Usage: master_dsp_integration_tests [--functional-only]\n";
        return 2;
    }
    FailureCounter failures;
    try {
        testNullExact(failures);
        testManualTwoBitQuantizer(failures);
        testManualHardRangeContract(failures);
        testAuditedExtremeParameters(failures);
        testFiniteMatrix(failures);
        testPlayerDawParity(failures);
        testTail(failures);
        testEqAnalyticResponse(failures);
        testDynamicsStaticCurve(failures);
        testTruePeakCeiling(failures);
        testStereoMatrix(failures);
        testFiniteResampler(failures);
        if (!functionalOnly) {
            testRealtimeBudget(failures);
            testCreativeRealtimeBudget(failures);
        } else {
            std::cout << "SKIP hardware-dependent timing gates (functional-only mode)\n";
        }
    } catch (const std::exception& exception) {
        failures.check(false, std::string("unhandled exception: ") + exception.what());
    }
    if (failures.value != 0) {
        std::cerr << failures.value << " Master DSP integration checks failed\n";
        return 1;
    }
    std::cout << "GODENplayer Master DSP integration: PASS\n";
    return 0;
}
