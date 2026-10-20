#include <windows.h>
#include <psapi.h>

#include "mf_audio_decoder.h"

#include "analysis/analysis_json.h"
#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "nekorox/contracts.h"
#include "nekorox/packet_cost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string_view>
#include <string>
#include <vector>

namespace {

bool analysisTraceEnabled() noexcept
{
    static const bool enabled = [] {
        wchar_t value[4]{};
        return GetEnvironmentVariableW(L"NEKOROX_ANALYSIS_TRACE",
            value, static_cast<DWORD>(std::size(value))) > 0;
    }();
    return enabled;
}

void traceAnalysisProgress(nekorox::AnalysisStage stage,
    float stageProgress, float overallProgress) noexcept
{
    if (!analysisTraceEnabled()) return;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const auto milliseconds = frequency.QuadPart > 0
        ? counter.QuadPart * 1000LL / frequency.QuadPart : 0LL;
    char line[192]{};
    const auto length = std::snprintf(line, std::size(line),
        "%lld stage=%u stageProgress=%.4f overall=%.4f privateMiB=%.1f wsMiB=%.1f\n",
        milliseconds, static_cast<unsigned>(stage), stageProgress, overallProgress,
        static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0),
        static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0));
    if (length <= 0) return;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line,
        static_cast<DWORD>(std::min<int>(length, static_cast<int>(std::size(line)))),
        &written, nullptr);
}

class AnalysisWriter {
public:
    explicit AnalysisWriter(std::uint64_t requestId)
        : output_(GetStdHandle(STD_OUTPUT_HANDLE)), requestId_(requestId)
    {
    }

    bool frame(nekorox::AnalysisMessageKind kind, const void* payload,
        std::size_t payloadBytes) const
    {
        if (payloadBytes > nekorox::kAnalysisMaxPayloadBytes ||
            payloadBytes > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        nekorox::AnalysisFrameHeaderV1 header;
        header.kind = kind;
        header.payloadBytes = static_cast<std::uint32_t>(payloadBytes);
        header.requestId = requestId_;
        return writeAll(&header, sizeof(header)) &&
            (payloadBytes == 0 || writeAll(payload, payloadBytes));
    }

    bool progress(nekorox::AnalysisStage stage, float stageProgress,
        float overallProgress, std::uint64_t completed = 0,
        std::uint64_t total = 0) const
    {
        nekorox::AnalysisProgressV1 value;
        value.stage = stage;
        value.stageProgress = std::clamp(stageProgress, 0.0f, 1.0f);
        value.overallProgress = std::clamp(overallProgress, 0.0f, 1.0f);
        value.completedUnits = completed;
        value.totalUnits = total;
        const auto sent = frame(nekorox::AnalysisMessageKind::progress,
            &value, sizeof(value));
        traceAnalysisProgress(stage, value.stageProgress, value.overallProgress);
        return sent;
    }

    bool error(std::string_view text) const
    {
        return frame(nekorox::AnalysisMessageKind::error,
            text.data(), text.size());
    }

private:
    bool writeAll(const void* data, std::size_t bytes) const
    {
        const auto* cursor = static_cast<const std::byte*>(data);
        while (bytes > 0) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(output_, cursor, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            cursor += written;
            bytes -= written;
        }
        return true;
    }

    HANDLE output_ = INVALID_HANDLE_VALUE;
    std::uint64_t requestId_ = 0;
};

nekorox::AnalysisStage stageFromSonare(std::string_view stage)
{
    using enum nekorox::AnalysisStage;
    if (stage == "features") return features;
    if (stage == "bpm") return bpm;
    if (stage == "key") return key;
    if (stage == "beats") return beats;
    if (stage == "chords") return harmony;
    if (stage == "sections") return segmentation;
    if (stage == "timbre") return timbre;
    if (stage == "dynamics") return dynamics;
    if (stage == "rhythm") return rhythm;
    if (stage == "melody") return melody;
    if (stage == "complete") return stabilization;
    return features;
}

struct AnalysisAccumulator {
    sonare::AnalysisResult result{};
    std::vector<std::pair<float, double>> tempos;
    std::array<double, 12U * 7U> keyVotes{};
    struct SignatureVote {
        int numerator = 0;
        int denominator = 0;
        double weight = 0.0;
    };
    std::vector<SignatureVote> signatures;
    std::vector<std::pair<std::string, double>> grooves;
    double totalWeight = 0.0;
    double bpmConfidence = 0.0;
    double timbreBrightness = 0.0;
    double timbreWarmth = 0.0;
    double timbreDensity = 0.0;
    double timbreRoughness = 0.0;
    double timbreComplexity = 0.0;
    double dynamicRange = 0.0;
    double dynamicPeak = -std::numeric_limits<double>::infinity();
    double dynamicRms = 0.0;
    double crestFactor = 0.0;
    double loudnessRange = 0.0;
    double compressedWeight = 0.0;
    double syncopation = 0.0;
    double patternRegularity = 0.0;
    double tempoStability = 0.0;
    double melodyStability = 0.0;
    double melodyMeanFrequency = 0.0;
    double melodyVibrato = 0.0;
    float melodyRange = 0.0f;
};

void addSignatureVote(AnalysisAccumulator& accumulator,
    const sonare::TimeSignature& signature, double duration)
{
    const auto weight = duration * std::max(0.05f, signature.confidence);
    const auto iterator = std::find_if(accumulator.signatures.begin(),
        accumulator.signatures.end(), [&](const auto& candidate) {
            return candidate.numerator == signature.numerator &&
                candidate.denominator == signature.denominator;
        });
    if (iterator != accumulator.signatures.end()) iterator->weight += weight;
    else accumulator.signatures.push_back(
        {signature.numerator, signature.denominator, weight});
}

void addGrooveVote(AnalysisAccumulator& accumulator,
    const std::string& groove, double duration)
{
    const auto iterator = std::find_if(accumulator.grooves.begin(),
        accumulator.grooves.end(), [&](const auto& candidate) {
            return candidate.first == groove;
        });
    if (iterator != accumulator.grooves.end()) iterator->second += duration;
    else accumulator.grooves.emplace_back(groove, duration);
}

void mergeChunk(AnalysisAccumulator& accumulator,
    const sonare::AnalysisResult& chunk, double windowStart,
    double coreStart, double coreEnd, std::uint32_t sampleRate,
    int hopLength)
{
    const auto duration = std::max(0.0, coreEnd - coreStart);
    if (!(duration > 0.0)) return;
    accumulator.totalWeight += duration;
    if (chunk.bpm > 0.0f && std::isfinite(chunk.bpm)) {
        accumulator.tempos.emplace_back(chunk.bpm,
            duration * std::max(0.05f, chunk.bpm_confidence));
    }
    accumulator.bpmConfidence +=
        duration * std::clamp(chunk.bpm_confidence, 0.0f, 1.0f);
    const auto root = std::clamp(static_cast<int>(chunk.key.root), 0, 11);
    const auto mode = std::clamp(static_cast<int>(chunk.key.mode), 0, 6);
    accumulator.keyVotes[static_cast<std::size_t>(mode * 12 + root)] +=
        duration * std::max(0.05f, chunk.key.confidence);
    addSignatureVote(accumulator, chunk.time_signature, duration);
    addGrooveVote(accumulator, chunk.rhythm.groove_type, duration);

    accumulator.timbreBrightness += duration * chunk.timbre.brightness;
    accumulator.timbreWarmth += duration * chunk.timbre.warmth;
    accumulator.timbreDensity += duration * chunk.timbre.density;
    accumulator.timbreRoughness += duration * chunk.timbre.roughness;
    accumulator.timbreComplexity += duration * chunk.timbre.complexity;
    accumulator.dynamicRange += duration * chunk.dynamics.dynamic_range_db;
    accumulator.dynamicPeak = std::max(accumulator.dynamicPeak,
        static_cast<double>(chunk.dynamics.peak_db));
    accumulator.dynamicRms += duration * chunk.dynamics.rms_db;
    accumulator.crestFactor += duration * chunk.dynamics.crest_factor;
    accumulator.loudnessRange += duration * chunk.dynamics.loudness_range_db;
    if (chunk.dynamics.is_compressed) accumulator.compressedWeight += duration;
    accumulator.syncopation += duration * chunk.rhythm.syncopation;
    accumulator.patternRegularity += duration * chunk.rhythm.pattern_regularity;
    accumulator.tempoStability += duration * chunk.rhythm.tempo_stability;
    accumulator.melodyStability += duration * chunk.melody.pitch_stability;
    accumulator.melodyMeanFrequency += duration * chunk.melody.mean_frequency;
    accumulator.melodyVibrato += duration * chunk.melody.vibrato_rate;
    accumulator.melodyRange = std::max(
        accumulator.melodyRange, chunk.melody.pitch_range_octaves);

    const auto includeTime = [&](double value) {
        const auto global = windowStart + value;
        return global >= coreStart && global < coreEnd;
    };
    for (const auto& source : chunk.beats) {
        if (!includeTime(source.time)) continue;
        auto beat = source;
        beat.time = static_cast<float>(windowStart + source.time);
        beat.frame = static_cast<int>(std::llround(
            beat.time * static_cast<float>(sampleRate) /
            static_cast<float>(std::max(1, hopLength))));
        accumulator.result.beats.push_back(beat);
    }
    for (const auto& source : chunk.chords) {
        auto chord = source;
        chord.start = static_cast<float>(std::max(
            coreStart, windowStart + static_cast<double>(source.start)));
        chord.end = static_cast<float>(std::min(
            coreEnd, windowStart + static_cast<double>(source.end)));
        if (chord.end - chord.start < 0.05f) continue;
        if (!accumulator.result.chords.empty()) {
            auto& previous = accumulator.result.chords.back();
            if (previous.root == chord.root && previous.quality == chord.quality &&
                previous.bass == chord.bass && chord.start - previous.end <= 0.08f) {
                const auto previousDuration = std::max(0.0f,
                    previous.end - previous.start);
                const auto chordDuration = chord.end - chord.start;
                previous.confidence = (previous.confidence * previousDuration +
                    chord.confidence * chordDuration) /
                    std::max(0.001f, previousDuration + chordDuration);
                previous.end = std::max(previous.end, chord.end);
                continue;
            }
        }
        accumulator.result.chords.push_back(chord);
    }
    for (const auto& source : chunk.sections) {
        auto section = source;
        section.start = static_cast<float>(std::max(
            coreStart, windowStart + static_cast<double>(source.start)));
        section.end = static_cast<float>(std::min(
            coreEnd, windowStart + static_cast<double>(source.end)));
        if (section.end - section.start < 0.25f) continue;
        if (!accumulator.result.sections.empty()) {
            auto& previous = accumulator.result.sections.back();
            if (previous.type == section.type &&
                section.start - previous.end <= 0.25f) {
                const auto previousDuration = std::max(0.0f,
                    previous.end - previous.start);
                const auto sectionDuration = section.end - section.start;
                const auto combined = std::max(
                    0.001f, previousDuration + sectionDuration);
                previous.energy_level =
                    (previous.energy_level * previousDuration +
                        section.energy_level * sectionDuration) / combined;
                previous.confidence =
                    (previous.confidence * previousDuration +
                        section.confidence * sectionDuration) / combined;
                previous.end = std::max(previous.end, section.end);
                continue;
            }
        }
        accumulator.result.sections.push_back(section);
    }
    for (const auto& source : chunk.melody.pitches) {
        if (!includeTime(source.time)) continue;
        auto point = source;
        point.time = static_cast<float>(windowStart + source.time);
        accumulator.result.melody.pitches.push_back(point);
    }
}

sonare::AnalysisResult finishChunks(AnalysisAccumulator accumulator)
{
    auto& result = accumulator.result;
    const auto total = std::max(0.001, accumulator.totalWeight);
    if (!accumulator.tempos.empty()) {
        std::sort(accumulator.tempos.begin(), accumulator.tempos.end(),
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        const auto tempoWeight = std::accumulate(accumulator.tempos.begin(),
            accumulator.tempos.end(), 0.0,
            [](double value, const auto& tempo) { return value + tempo.second; });
        double consumed = 0.0;
        result.bpm = accumulator.tempos.back().first;
        for (const auto& tempo : accumulator.tempos) {
            consumed += tempo.second;
            if (consumed * 2.0 >= tempoWeight) {
                result.bpm = tempo.first;
                break;
            }
        }
    }
    result.bpm_confidence = static_cast<float>(
        accumulator.bpmConfidence / total);
    const auto key = static_cast<std::size_t>(std::distance(
        accumulator.keyVotes.begin(),
        std::max_element(accumulator.keyVotes.begin(),
            accumulator.keyVotes.end())));
    result.key.root = static_cast<sonare::PitchClass>(key % 12U);
    result.key.mode = static_cast<sonare::Mode>(key / 12U);
    result.key.confidence = static_cast<float>(
        accumulator.keyVotes[key] / total);
    result.time_signature = {4, 4, 0.0f};
    if (!accumulator.signatures.empty()) {
        const auto signature = std::max_element(accumulator.signatures.begin(),
            accumulator.signatures.end(), [](const auto& left, const auto& right) {
                return left.weight < right.weight;
            });
        result.time_signature = {signature->numerator, signature->denominator,
            static_cast<float>(std::clamp(signature->weight / total, 0.0, 1.0))};
    }
    result.timbre.brightness = static_cast<float>(accumulator.timbreBrightness / total);
    result.timbre.warmth = static_cast<float>(accumulator.timbreWarmth / total);
    result.timbre.density = static_cast<float>(accumulator.timbreDensity / total);
    result.timbre.roughness = static_cast<float>(accumulator.timbreRoughness / total);
    result.timbre.complexity = static_cast<float>(accumulator.timbreComplexity / total);
    result.dynamics.dynamic_range_db = static_cast<float>(accumulator.dynamicRange / total);
    result.dynamics.peak_db = std::isfinite(accumulator.dynamicPeak)
        ? static_cast<float>(accumulator.dynamicPeak) : 0.0f;
    result.dynamics.rms_db = static_cast<float>(accumulator.dynamicRms / total);
    result.dynamics.crest_factor = static_cast<float>(accumulator.crestFactor / total);
    result.dynamics.loudness_range_db = static_cast<float>(accumulator.loudnessRange / total);
    result.dynamics.is_compressed = accumulator.compressedWeight * 2.0 >= total;
    result.rhythm.time_signature = result.time_signature;
    result.rhythm.syncopation = static_cast<float>(accumulator.syncopation / total);
    result.rhythm.pattern_regularity = static_cast<float>(
        accumulator.patternRegularity / total);
    result.rhythm.tempo_stability = static_cast<float>(
        accumulator.tempoStability / total);
    if (!accumulator.grooves.empty()) {
        result.rhythm.groove_type = std::max_element(accumulator.grooves.begin(),
            accumulator.grooves.end(), [](const auto& left, const auto& right) {
                return left.second < right.second;
            })->first;
    }
    result.melody.pitch_range_octaves = accumulator.melodyRange;
    result.melody.pitch_stability = static_cast<float>(
        accumulator.melodyStability / total);
    result.melody.mean_frequency = static_cast<float>(
        accumulator.melodyMeanFrequency / total);
    result.melody.vibrato_rate = static_cast<float>(
        accumulator.melodyVibrato / total);
    for (const auto& section : result.sections) {
        result.form.push_back(sonare::section_type_to_char(section.type));
    }
    return result;
}

sonare::AnalysisResult analyzeMusic(std::vector<float>& samples,
    std::uint32_t sampleRate, const sonare::MusicAnalyzerConfig& config,
    const AnalysisWriter& writer)
{
    constexpr double kChunkThresholdSeconds = 12.0 * 60.0;
    constexpr double kChunkCoreSeconds = 8.0 * 60.0;
    constexpr double kChunkContextSeconds = 12.0;
    const auto duration = static_cast<double>(samples.size()) /
        static_cast<double>(sampleRate);
    if (duration <= kChunkThresholdSeconds) {
        auto audio = sonare::Audio::from_vector(std::move(samples),
            static_cast<int>(sampleRate));
        sonare::MusicAnalyzer analyzer(audio, config);
        analyzer.set_progress_callback([&](float progress, const char* stage) {
            const auto safeStage = stage != nullptr
                ? std::string_view(stage) : std::string_view();
            writer.progress(stageFromSonare(safeStage), progress,
                0.15f + std::clamp(progress, 0.0f, 1.0f) * 0.80f);
        });
        return analyzer.analyze();
    }

    const auto chunkCount = static_cast<std::size_t>(std::ceil(
        duration / kChunkCoreSeconds));
    AnalysisAccumulator accumulator;
    for (std::size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const auto coreStart = chunkIndex * kChunkCoreSeconds;
        const auto coreEnd = std::min(duration, coreStart + kChunkCoreSeconds);
        const auto windowStart = std::max(0.0,
            coreStart - kChunkContextSeconds);
        const auto windowEnd = std::min(duration,
            coreEnd + kChunkContextSeconds);
        const auto firstSample = static_cast<std::size_t>(std::floor(
            windowStart * static_cast<double>(sampleRate)));
        const auto lastSample = std::min(samples.size(),
            static_cast<std::size_t>(std::ceil(
                windowEnd * static_cast<double>(sampleRate))));
        std::vector<float> chunk(
            samples.begin() + static_cast<std::ptrdiff_t>(firstSample),
            samples.begin() + static_cast<std::ptrdiff_t>(lastSample));
        auto audio = sonare::Audio::from_vector(std::move(chunk),
            static_cast<int>(sampleRate));
        sonare::MusicAnalyzer analyzer(audio, config);
        analyzer.set_progress_callback([&, chunkIndex](float progress,
            const char* stage) {
            const auto safeStage = stage != nullptr
                ? std::string_view(stage) : std::string_view();
            const auto aggregateProgress =
                (static_cast<float>(chunkIndex) +
                    std::clamp(progress, 0.0f, 1.0f)) /
                static_cast<float>(chunkCount);
            writer.progress(stageFromSonare(safeStage), progress,
                0.15f + aggregateProgress * 0.80f,
                chunkIndex, chunkCount);
        });
        const auto result = analyzer.analyze();
        mergeChunk(accumulator, result, windowStart, coreStart, coreEnd,
            sampleRate, config.hop_length);
    }
    return finishChunks(std::move(accumulator));
}

std::vector<nekorox::AnalysisWaveformBinV1> makeWaveform(
    const std::vector<float>& samples, std::uint32_t sampleRate,
    const AnalysisWriter& writer)
{
    const auto samplesPerBin = std::max<std::size_t>(sampleRate / 10U, 1U);
    const auto binCount = (samples.size() + samplesPerBin - 1U) / samplesPerBin;
    std::vector<nekorox::AnalysisWaveformBinV1> bins(binCount);
    for (std::size_t binIndex = 0; binIndex < binCount; ++binIndex) {
        const auto begin = binIndex * samplesPerBin;
        const auto end = std::min(samples.size(), begin + samplesPerBin);
        double squared = 0.0;
        float peak = 0.0f;
        for (std::size_t index = begin; index < end; ++index) {
            const auto magnitude = std::abs(samples[index]);
            peak = std::max(peak, magnitude);
            squared += static_cast<double>(samples[index]) * samples[index];
        }
        bins[binIndex].peak = peak;
        bins[binIndex].rms = end > begin
            ? static_cast<float>(std::sqrt(squared / static_cast<double>(end - begin)))
            : 0.0f;
        if ((binIndex % 64U) == 0U || binIndex + 1U == binCount) {
            const auto completed = binIndex + 1U;
            const auto ratio = binCount > 0
                ? static_cast<float>(completed) / static_cast<float>(binCount)
                : 1.0f;
            writer.progress(nekorox::AnalysisStage::waveform, ratio,
                0.12f + ratio * 0.03f, completed, binCount);
        }
    }
    return bins;
}

bool sendMediaInfo(const AnalysisWriter& writer,
    const nekorox::analysis::DecodedAudio& audio,
    const std::vector<nekorox::AnalysisWaveformBinV1>& waveform)
{
    nekorox::AnalysisMediaInfoV1 info;
    info.sampleRate = audio.sampleRate;
    info.sampleCount = audio.samples.size();
    info.durationSeconds = audio.durationSeconds;
    info.waveformBinCount = static_cast<std::uint32_t>(waveform.size());
    const auto bytes = sizeof(info) + waveform.size() * sizeof(waveform.front());
    std::vector<std::byte> payload(bytes);
    std::memcpy(payload.data(), &info, sizeof(info));
    if (!waveform.empty()) {
        std::memcpy(payload.data() + sizeof(info), waveform.data(),
            waveform.size() * sizeof(waveform.front()));
    }
    return writer.frame(nekorox::AnalysisMessageKind::partialResult,
        payload.data(), payload.size());
}

int runAnalyze(std::uint64_t requestId, const std::filesystem::path& path)
{
    AnalysisWriter writer(requestId);
    writer.progress(nekorox::AnalysisStage::queued, 1.0f, 0.0f);

    try {
        // Decoding and waveform construction allocate the largest buffers in
        // the worker and can throw too. Keeping them outside this boundary
        // turned an ordinary per-track failure into std::terminate/abort
        // (0xc0000409), leaving Listen with no terminal frame to retry from.
        nekorox::analysis::DecodedAudio decoded;
        std::string error;
        std::uint64_t lastDecodeProgress = 0;
        const bool decodedOk = nekorox::analysis::decodeLocalAudio(path, decoded,
            [&](std::uint64_t completed, std::uint64_t total) {
                const auto ratio = total > 0
                    ? static_cast<float>(completed) /
                        static_cast<float>(total)
                    : 0.0f;
                const auto quantized =
                    static_cast<std::uint64_t>(ratio * 200.0f);
                if (quantized != lastDecodeProgress || completed == total) {
                    lastDecodeProgress = quantized;
                    writer.progress(nekorox::AnalysisStage::decode, ratio,
                        ratio * 0.12f, completed, total);
                }
            }, error);
        if (!decodedOk) {
            writer.error(error);
            return 2;
        }

        auto waveform = makeWaveform(
            decoded.samples, decoded.sampleRate, writer);
        if (!sendMediaInfo(writer, decoded, waveform)) return 3;
        waveform.clear();
        waveform.shrink_to_fit();

        sonare::MusicAnalyzerConfig config;
        config.n_fft = 1024;
        config.hop_length = 512;
        config.use_hpss = false;
        config.use_bass_weighted = false;
        config.chroma_hop_multiplier = 8;
        const auto result = analyzeMusic(
            decoded.samples, decoded.sampleRate, config, writer);
        writer.progress(nekorox::AnalysisStage::stabilization, 0.5f, 0.975f);
        auto json = sonare::analysis_result_to_json(result);
        if (!json.empty() && json.back() == '}') {
            json.pop_back();
            std::ostringstream v2;
            v2 << std::fixed << std::setprecision(6)
               << ",\"godenV2\":{\"version\":2,\"stereo\":{";
            if (decoded.hasStereoMetrics) {
                v2 << "\"correlation\":" << decoded.stereoCorrelation
                   << ",\"midEnergy\":" << decoded.midEnergy
                   << ",\"sideEnergy\":" << decoded.sideEnergy
                   << ",\"outOfPhaseFraction\":"
                   << decoded.outOfPhaseFraction;
            } else {
                v2 << "\"correlation\":null,\"midEnergy\":null,"
                      "\"sideEnergy\":null,\"outOfPhaseFraction\":null";
            }
            v2 << "}}}";
            json += v2.str();
        }
        if (!writer.frame(nekorox::AnalysisMessageKind::complete,
                json.data(), json.size())) {
            return 3;
        }
        writer.progress(nekorox::AnalysisStage::complete, 1.0f, 1.0f);
        return 0;
    } catch (const std::exception& exception) {
        writer.error(exception.what());
        return 2;
    }
}

const char* codecName(nekorox::PacketCodec codec)
{
    switch (codec) {
    case nekorox::PacketCodec::flac: return "flac";
    case nekorox::PacketCodec::mp3: return "mp3";
    case nekorox::PacketCodec::aacAdts: return "aac-adts";
    case nekorox::PacketCodec::isoBmff: return "iso-bmff";
    case nekorox::PacketCodec::oggVorbis: return "ogg-vorbis";
    case nekorox::PacketCodec::oggOpus: return "ogg-opus";
    case nekorox::PacketCodec::wave: return "wave";
    case nekorox::PacketCodec::aiff: return "aiff";
    case nekorox::PacketCodec::wavPack: return "wavpack";
    case nekorox::PacketCodec::ac3: return "ac3";
    case nekorox::PacketCodec::eac3: return "eac3";
    case nekorox::PacketCodec::monkeysAudio: return "monkeys-audio";
    case nekorox::PacketCodec::tta: return "tta";
    case nekorox::PacketCodec::dsd: return "dsd";
    case nekorox::PacketCodec::caf: return "caf";
    default: return "unsupported";
    }
}

void printJsonString(const std::string& value)
{
    std::cout << '"';
    for (const char valueByte : value) {
        switch (valueByte) {
        case '\\': std::cout << "\\\\"; break;
        case '"': std::cout << "\\\""; break;
        case '\n': std::cout << "\\n"; break;
        case '\r': std::cout << "\\r"; break;
        case '\t': std::cout << "\\t"; break;
        default: std::cout << valueByte; break;
        }
    }
    std::cout << '"';
}

int runCost(std::uint64_t requestId, const std::filesystem::path& path)
{
    AnalysisWriter writer(requestId);
    const auto cost = nekorox::parsePacketCost(path);
    std::vector<std::byte> payload;
    std::string error;
    if (!nekorox::serializePacketCost(cost, payload, error)) {
        writer.error(error);
        return 2;
    }
    if (!writer.frame(nekorox::AnalysisMessageKind::complete,
            payload.data(), payload.size())) {
        return 3;
    }
    return 0;
}

int runCostJson(const std::filesystem::path& path)
{
    const auto cost = nekorox::parsePacketCost(path);
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\"schema\":\"nekorox.packet-cost.v1\",\"supported\":"
              << (cost.supported ? "true" : "false")
              << ",\"codec\":\"" << codecName(cost.codec) << "\""
              << ",\"durationSeconds\":" << cost.durationSeconds
              << ",\"error\":";
    printJsonString(cost.error);
    std::cout << ",\"stats\":{"
              << "\"minimumKbps\":" << cost.stats.minimumKbps << ','
              << "\"p05Kbps\":" << cost.stats.p05Kbps << ','
              << "\"medianKbps\":" << cost.stats.medianKbps << ','
              << "\"averageKbps\":" << cost.stats.averageKbps << ','
              << "\"p90Kbps\":" << cost.stats.p90Kbps << ','
              << "\"p95Kbps\":" << cost.stats.p95Kbps << ','
              << "\"peakKbps\":" << cost.stats.peakKbps << "},\"levels\":[";
    for (std::size_t levelIndex = 0; levelIndex < cost.levels.size(); ++levelIndex) {
        if (levelIndex != 0) std::cout << ',';
        const auto& level = cost.levels[levelIndex];
        std::cout << "{\"resolutionMilliseconds\":"
                  << level.resolutionMilliseconds << ",\"bins\":[";
        for (std::size_t binIndex = 0; binIndex < level.bins.size(); ++binIndex) {
            if (binIndex != 0) std::cout << ',';
            const auto& bin = level.bins[binIndex];
            std::cout << '[' << bin.startSeconds << ',' << bin.durationSeconds
                      << ',' << bin.kilobitsPerSecond << ',' << bin.normalized << ']';
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
    return cost.supported ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    if (argc == 4 && std::wstring_view(argv[1]) == L"analyze") {
        wchar_t* end = nullptr;
        const auto requestId = std::wcstoull(argv[2], &end, 10);
        if (end == argv[2] || *end != L'\0' || requestId == 0) return 64;
        return runAnalyze(requestId, argv[3]);
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"cost") {
        wchar_t* end = nullptr;
        const auto requestId = std::wcstoull(argv[2], &end, 10);
        if (end == argv[2] || *end != L'\0' || requestId == 0) return 64;
        return runCost(requestId, argv[3]);
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"cost-json") {
        return runCostJson(argv[2]);
    }
    std::wcerr << L"Usage:\n"
               << L"  nekorox-analysis analyze <request-id> <local-file>\n"
               << L"  nekorox-analysis cost <request-id> <local-audio-file>\n"
               << L"  nekorox-analysis cost-json <local-audio-file>\n";
    return 64;
}
