#include "master_dsp_engine.h"

#include "master_dsp_quantizer.h"
#include "master_dsp_history.h"

#include "master_dsp_controller.h"
#include "performance_telemetry.h"
#include "rollout_flags.h"
#include "sdk_compat.h"
#include "text_util.h"

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mixing/meter.h"
#include "rt/processor_base.h"
#include "rt/scoped_no_denormals.h"
#include "streaming/stream_resampler.h"
#include "util/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nekorox::goden_audio {
namespace {

constexpr std::size_t kMaxChannels = 8;
constexpr std::size_t kMicroBlockFrames = 1024;
constexpr std::size_t kMaxCallbackFrames = 32768;
// Covers more than 1.3 s at 48 kHz (and 170 ms at 384 kHz), comfortably above
// the qualified streaming processors. Keeping this bounded is important: the
// delay and graph-swap histories are permanently prepared so the callback never
// allocates when a rack changes latency.
constexpr std::size_t kMaxLatencyFrames = 1U << 16U;
constexpr std::uint64_t kDspDiagnosticMagic = 0x47445053ULL;

// Auto-reset events retain one wake even when no worker is waiting. The
// generation remains useful for coalescing and diagnostics, but correctness
// no longer depends on a condition-variable check/wait interval.
struct CompilerWake final {
    CompilerWake() noexcept
        : event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~CompilerWake() { if (event != nullptr) CloseHandle(event); }
    CompilerWake(const CompilerWake&) = delete;
    CompilerWake& operator=(const CompilerWake&) = delete;

    HANDLE event = nullptr;
    std::atomic<std::uint64_t> generation{1};

    void signal() noexcept
    {
        generation.fetch_add(1, std::memory_order_release);
        if (event != nullptr) SetEvent(event);
    }

    [[nodiscard]] bool valid() const noexcept { return event != nullptr; }
};

std::mutex gCompilerRegistryMutex;
std::vector<CompilerWake*> gCompilerWakes;
std::mutex gPlayerProcessorMutex;
std::vector<MasterDspProcessor*> gPlayerProcessors;
std::atomic<std::uint64_t> gTelemetrySequence{0};

constexpr std::size_t kRtCommandCapacity = 256;
enum class RtCommandKind : std::uint8_t { parameter, bypass };
struct RtCommand {
    std::uint64_t sequence = 0;
    std::uint64_t moduleHash = 0;
    std::uint64_t keyHash = 0;
    float value = 0.0f;
    RtCommandKind kind = RtCommandKind::parameter;
};
struct RtCommandSlot {
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> moduleHash{0};
    std::atomic<std::uint64_t> keyHash{0};
    std::atomic<float> value{0.0f};
    std::atomic<std::uint8_t> kind{
        static_cast<std::uint8_t>(RtCommandKind::parameter)};
};
std::array<RtCommandSlot, kRtCommandCapacity> gRtCommands{};
std::atomic<std::uint64_t> gRtCommandSequence{0};
std::mutex gRtCommandPublishMutex;

constexpr std::uint64_t hashText(std::string_view text) noexcept
{
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : text) {
        value ^= static_cast<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}
constexpr auto kGainDbHash = hashText("gainDb");
constexpr auto kBalanceHash = hashText("balance");
constexpr auto kAmountHash = hashText("amount");

bool publishCommand(const std::string& uuid, const std::string& key,
    float value, RtCommandKind kind) noexcept
{
    if (uuid.empty() || !std::isfinite(value)) return false;
    // Commands are produced on control/UI threads, never on the callback.
    // Serialising the uncommon producers keeps sequence publication ordered
    // even if two Goden windows edit the rack concurrently.
    std::lock_guard publishLock(gRtCommandPublishMutex);
    const auto sequence = gRtCommandSequence.load(
        std::memory_order_relaxed) + 1U;
    auto& slot = gRtCommands[sequence % kRtCommandCapacity];
    // Mark the slot unavailable before overwriting it. The payload fields are
    // atomic too: that keeps a wrapping producer and a lagging audio consumer
    // formally data-race-free, while the two published reads below still act
    // as the seqlock validation.
    slot.published.store(0, std::memory_order_release);
    slot.moduleHash.store(hashText(uuid), std::memory_order_relaxed);
    slot.keyHash.store(hashText(key), std::memory_order_relaxed);
    slot.value.store(value, std::memory_order_relaxed);
    slot.kind.store(static_cast<std::uint8_t>(kind),
        std::memory_order_relaxed);
    slot.published.store(sequence, std::memory_order_release);

    // Publish the global high-water mark last. Consumers that observe this
    // sequence are then guaranteed to see the complete ring payload.
    gRtCommandSequence.store(sequence, std::memory_order_release);
    return true;
}

float number(const std::string& json, const char* key, float fallback) noexcept
{
    const std::string marker = std::string("\"") + key + "\"";
    auto position = json.find(marker);
    if (position == std::string::npos) return fallback;
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return fallback;
    const auto begin = json.data() + position + 1;
    char* end = nullptr;
    const auto value = std::strtof(begin, &end);
    return end == begin || !std::isfinite(value) ? fallback : value;
}

class AudioNode {
public:
    virtual ~AudioNode() = default;
    virtual bool process(
        float* const* channels,
        std::uint32_t channelCount,
        std::size_t frames) noexcept = 0;
    virtual void reset() noexcept = 0;
    virtual std::uint32_t latency() const noexcept { return 0; }
    virtual std::uint32_t tail() const noexcept { return 0; }
    virtual float gainReduction() const noexcept { return 0.0f; }
    virtual bool setParameter(std::uint64_t, float) noexcept { return false; }
};

class SonareNode final : public AudioNode {
public:
    explicit SonareNode(std::unique_ptr<sonare::rt::ProcessorBase> processor)
        : processor_(std::move(processor))
    {
        for (const auto& descriptor : processor_->parameter_descriptors()) {
            if (processor_->parameter_is_realtime_safe(descriptor.id)) {
                parameters_.push_back({hashText(descriptor.key), descriptor.id});
            }
        }
    }

    void prepare(double sampleRate)
    {
        processor_->prepare(sampleRate, static_cast<int>(kMicroBlockFrames));
    }

    bool process(float* const* channels, std::uint32_t channelCount,
        std::size_t frames) noexcept override
    {
        if (faulted_) return false;
        try {
            processor_->process(channels, static_cast<int>(channelCount),
                static_cast<int>(frames));
            return true;
        } catch (...) {
            faulted_ = true;
            return false;
        }
    }

    void reset() noexcept override
    {
        try { processor_->reset(); } catch (...) {}
        faulted_ = false;
    }
    std::uint32_t latency() const noexcept override
    {
        return static_cast<std::uint32_t>(
            std::max(processor_->latency_samples(), 0));
    }
    std::uint32_t tail() const noexcept override
    {
        return static_cast<std::uint32_t>(
            std::max(processor_->tail_samples(), 0));
    }
    float gainReduction() const noexcept override
    {
        try { return processor_->last_gain_reduction_db(); }
        catch (...) { return 0.0f; }
    }
    bool faulted() const noexcept { return faulted_; }
    bool setParameter(std::uint64_t key, float value) noexcept override
    {
        const auto found = std::find_if(parameters_.begin(), parameters_.end(),
            [&](const auto& parameter) { return parameter.first == key; });
        if (found == parameters_.end()) return false;
        try { return processor_->set_parameter(found->second, value); }
        catch (...) { faulted_ = true; return false; }
    }

private:
    std::unique_ptr<sonare::rt::ProcessorBase> processor_;
    std::vector<std::pair<std::uint64_t, unsigned int>> parameters_;
    bool faulted_ = false;
};

class MultiIrNode final : public AudioNode {
public:
    explicit MultiIrNode(
        std::vector<std::unique_ptr<sonare::rt::ProcessorBase>> processors,
        double sampleRate)
    {
        nodes_.reserve(processors.size());
        for (auto& processor : processors) {
            auto node = std::make_unique<SonareNode>(std::move(processor));
            node->prepare(sampleRate);
            latency_ = std::max(latency_, node->latency());
            tail_ = std::max(tail_, node->tail());
            nodes_.push_back(std::move(node));
        }
    }

    bool process(float* const* channels, std::uint32_t channelCount,
        std::size_t frames) noexcept override
    {
        const auto count = std::min<std::size_t>(channelCount, nodes_.size());
        bool success = true;
        for (std::size_t channel = 0; channel < count; ++channel) {
            float* plane = channels[channel];
            success = nodes_[channel]->process(&plane, 1, frames) && success;
        }
        return success;
    }

    void reset() noexcept override
    {
        for (auto& node : nodes_) node->reset();
    }
    std::uint32_t latency() const noexcept override { return latency_; }
    std::uint32_t tail() const noexcept override { return tail_; }
    float gainReduction() const noexcept override
    {
        float value = 0.0f;
        for (const auto& node : nodes_)
            value = std::max(value, node->gainReduction());
        return value;
    }
    bool setParameter(std::uint64_t key, float value) noexcept override
    {
        bool accepted = false;
        for (auto& node : nodes_) accepted = node->setParameter(key, value) || accepted;
        return accepted;
    }

private:
    std::vector<std::unique_ptr<SonareNode>> nodes_;
    std::uint32_t latency_ = 0;
    std::uint32_t tail_ = 0;
};

class FrontStereoNode final : public AudioNode {
public:
    FrontStereoNode(std::unique_ptr<AudioNode> inner, std::uint32_t channels)
        : inner_(std::move(inner)), channels_(channels), latency_(inner_->latency())
    {
        if (channels_ > 2 && latency_ != 0)
            remainderDelay_.assign(
                (static_cast<std::size_t>(latency_) + 1U) * (channels_ - 2U),
                0.0f);
    }

    bool process(float* const* channels, std::uint32_t channelCount,
        std::size_t frames) noexcept override
    {
        const auto processedChannels = std::min<std::uint32_t>(channelCount, 2U);
        if (!inner_->process(channels, processedChannels, frames)) return false;
        if (latency_ == 0 || channelCount <= 2 || remainderDelay_.empty())
            return true;
        const auto span = static_cast<std::size_t>(latency_) + 1U;
        const auto remainder = channelCount - 2U;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto read = (cursor_ + 1U) % span;
            for (std::uint32_t channel = 0; channel < remainder; ++channel) {
                remainderDelay_[cursor_ * remainder + channel] =
                    channels[channel + 2U][frame];
                channels[channel + 2U][frame] =
                    remainderDelay_[read * remainder + channel];
            }
            cursor_ = read;
        }
        return true;
    }
    void reset() noexcept override
    {
        inner_->reset();
        std::fill(remainderDelay_.begin(), remainderDelay_.end(), 0.0f);
        cursor_ = 0;
    }
    std::uint32_t latency() const noexcept override { return latency_; }
    std::uint32_t tail() const noexcept override { return inner_->tail(); }
    float gainReduction() const noexcept override { return inner_->gainReduction(); }
    bool setParameter(std::uint64_t key, float value) noexcept override
    {
        return inner_->setParameter(key, value);
    }

private:
    std::unique_ptr<AudioNode> inner_;
    std::vector<float> remainderDelay_;
    std::uint32_t channels_ = 0;
    std::uint32_t latency_ = 0;
    std::size_t cursor_ = 0;
};

enum class UtilityKind {
    trim, output, balance, polarity, swap, mono, channelDelay,
    dcBlocker, crossfeed, monoBass, bitDepth, dither, passthrough,
};

class UtilityNode final : public AudioNode {
public:
    UtilityNode(UtilityKind kind, const std::string& json, double sampleRate)
        : kind_(kind), smoothingCoefficient_(1.0f - std::exp(-1.0f /
            std::max(1.0f, static_cast<float>(sampleRate) * 0.010f)))
    {
        gain_ = gainTarget_ = std::pow(
            10.0f, std::clamp(number(json, "gainDb", 0.0f),
                -120.0f, 48.0f) / 20.0f);
        numberCache_ = balanceTarget_ = number(json, "balance", 0.0f);
        amount_ = amountTarget_ = std::clamp(
            number(json, "amount", 0.3f), 0.0f, 1.0f);
        cutoff_ = std::clamp(number(json, "cutoffHz", 700.0f), 20.0f, 20000.0f);
        delayFrames_ = static_cast<std::size_t>(std::clamp(
            number(json, "delayUs", 280.0f) * sampleRate / 1000000.0,
            0.0, 4096.0));
        delayChannel_ = static_cast<std::uint32_t>(std::clamp(
            number(json, "channel", 0.0f), 0.0f,
            static_cast<float>(kMaxChannels - 1U)));
        bits_ = static_cast<int>(std::lround(std::clamp(
            number(json, "bits", 24.0f), 2.0f, 32.0f)));
        ditherType_ = static_cast<int>(std::clamp(
            number(json, "type", kind == UtilityKind::dither ? 2.0f : 0.0f),
            0.0f, 3.0f));
        monoBassHz_ = std::clamp(number(json, "frequencyHz", 120.0f), 20.0f, 500.0f);
        crossfeedCoefficient_ = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> *
            cutoff_ / static_cast<float>(sampleRate));
        monoBassCoefficient_ = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> *
            monoBassHz_ / static_cast<float>(sampleRate));
        delay_.assign((delayFrames_ + 1U) * kMaxChannels, 0.0f);
    }

    bool process(float* const* channels, std::uint32_t count,
        std::size_t frames) noexcept override
    {
        const sonare::rt::ScopedNoDenormals noDenormals;
        switch (kind_) {
        case UtilityKind::trim:
        case UtilityKind::output:
            for (std::size_t frame = 0; frame < frames; ++frame) {
                gain_ += (gainTarget_ - gain_) * smoothingCoefficient_;
                for (std::uint32_t channel = 0; channel < count; ++channel)
                    channels[channel][frame] *= gain_;
            }
            break;
        case UtilityKind::balance:
            if (count >= 2) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    numberCache_ += (balanceTarget_ - numberCache_) *
                        smoothingCoefficient_;
                    const auto balance = std::clamp(numberCache_, -1.0f, 1.0f);
                    const auto left = balance > 0.0f ? 1.0f - balance : 1.0f;
                    const auto right = balance < 0.0f ? 1.0f + balance : 1.0f;
                    channels[0][frame] *= left;
                    channels[1][frame] *= right;
                }
            }
            break;
        case UtilityKind::polarity:
            for (std::uint32_t channel = 0; channel < count; ++channel)
                for (std::size_t frame = 0; frame < frames; ++frame)
                    channels[channel][frame] = -channels[channel][frame];
            break;
        case UtilityKind::swap:
            if (count >= 2) for (std::size_t frame = 0; frame < frames; ++frame)
                std::swap(channels[0][frame], channels[1][frame]);
            break;
        case UtilityKind::mono:
            if (count >= 2) for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto mono = (channels[0][frame] + channels[1][frame]) * 0.5f;
                channels[0][frame] = channels[1][frame] = mono;
            }
            break;
        case UtilityKind::channelDelay:
            delayChannel(channels, count, frames);
            break;
        case UtilityKind::dcBlocker:
            dcBlock(channels, count, frames);
            break;
        case UtilityKind::crossfeed:
            crossfeed(channels, count, frames);
            break;
        case UtilityKind::monoBass:
            monoBass(channels, count, frames);
            break;
        case UtilityKind::bitDepth:
        case UtilityKind::dither:
            quantize(channels, count, frames, kind_ == UtilityKind::dither);
            break;
        case UtilityKind::passthrough:
            break;
        }
        return true;
    }

    void reset() noexcept override
    {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        delayCursor_ = 0;
        x1_.fill(0.0f); y1_.fill(0.0f);
        lowL_ = lowR_ = 0.0f;
        shapingError_.fill(0.0f);
    }

    std::uint32_t latency() const noexcept override
    {
        return kind_ == UtilityKind::channelDelay
            ? static_cast<std::uint32_t>(delayFrames_) : 0U;
    }

    bool setParameter(std::uint64_t key, float value) noexcept override
    {
        if (!std::isfinite(value)) return false;
        if ((kind_ == UtilityKind::trim || kind_ == UtilityKind::output) &&
                key == kGainDbHash) {
            gainTarget_ = std::pow(10.0f,
                std::clamp(value, -120.0f, 48.0f) / 20.0f);
            return true;
        }
        if (kind_ == UtilityKind::balance && key == kBalanceHash) {
            balanceTarget_ = std::clamp(value, -1.0f, 1.0f);
            return true;
        }
        if (kind_ == UtilityKind::crossfeed && key == kAmountHash) {
            amountTarget_ = std::clamp(value, 0.0f, 1.0f);
            return true;
        }
        return false;
    }

private:
    void delayChannel(float* const* channels, std::uint32_t count,
        std::size_t frames) noexcept
    {
        if (delayFrames_ == 0 || count == 0 || delayChannel_ >= count) return;
        const auto span = delayFrames_ + 1U;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto read = (delayCursor_ + 1U) % span;
            delay_[delayCursor_] = channels[delayChannel_][frame];
            channels[delayChannel_][frame] = delay_[read];
            delayCursor_ = read;
        }
    }

    void dcBlock(float* const* channels, std::uint32_t count,
        std::size_t frames) noexcept
    {
        constexpr float r = 0.995f;
        for (std::uint32_t channel = 0; channel < count; ++channel) {
            auto x1 = x1_[channel]; auto y1 = y1_[channel];
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto x = channels[channel][frame];
                const auto y = x - x1 + r * y1;
                channels[channel][frame] = y;
                x1 = x; y1 = y;
            }
            x1_[channel] = x1; y1_[channel] = y1;
        }
    }

    void crossfeed(float* const* channels, std::uint32_t count,
        std::size_t frames) noexcept
    {
        if (count < 2) return;
        const auto a = crossfeedCoefficient_;
        const auto span = delayFrames_ + 1U;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            amount_ += (amountTarget_ - amount_) * smoothingCoefficient_;
            const auto read = (delayCursor_ + 1U) % span;
            const auto left = channels[0][frame];
            const auto right = channels[1][frame];
            lowL_ += a * (left - lowL_);
            lowR_ += a * (right - lowR_);
            delay_[delayCursor_ * 2U] = lowL_;
            delay_[delayCursor_ * 2U + 1U] = lowR_;
            channels[0][frame] = left + delay_[read * 2U + 1U] * amount_;
            channels[1][frame] = right + delay_[read * 2U] * amount_;
            delayCursor_ = read;
        }
    }

    void monoBass(float* const* channels, std::uint32_t count,
        std::size_t frames) noexcept
    {
        if (count < 2) return;
        const auto a = monoBassCoefficient_;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            lowL_ += a * (channels[0][frame] - lowL_);
            lowR_ += a * (channels[1][frame] - lowR_);
            const auto mono = (lowL_ + lowR_) * 0.5f;
            channels[0][frame] += mono - lowL_;
            channels[1][frame] += mono - lowR_;
        }
    }

    void quantize(float* const* channels, std::uint32_t count,
        std::size_t frames, bool dither) noexcept
    {
        if (bits_ >= 32) return;
        const auto scale = std::ldexp(1.0f, bits_ - 1);
        for (std::uint32_t channel = 0; channel < count; ++channel) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                float noise = 0.0f;
                if (dither && ditherType_ != 0) {
                    rng_ = rng_ * 1664525U + 1013904223U;
                    const auto a = static_cast<float>(rng_ >> 8U) / 16777216.0f;
                    if (ditherType_ == 1) {
                        noise = a - 0.5f;
                    } else {
                        rng_ = rng_ * 1664525U + 1013904223U;
                        const auto b = static_cast<float>(rng_ >> 8U) / 16777216.0f;
                        noise = a - b;
                    }
                }
                auto shaped = std::clamp(channels[channel][frame],
                    -1.0f, 1.0f - 1.0f / scale);
                if (dither && ditherType_ == 3)
                    shaped += shapingError_[channel] * 0.85f;
                const auto quantized = dither
                    ? std::clamp(std::round(shaped * scale + noise) / scale,
                        -1.0f, 1.0f - 1.0f / scale)
                    : goden_audio::quantizeUnditheredSample(shaped, bits_);
                shapingError_[channel] = dither && ditherType_ == 3
                    ? shaped - quantized : 0.0f;
                channels[channel][frame] = quantized;
            }
        }
    }

    UtilityKind kind_ = UtilityKind::passthrough;
    const float smoothingCoefficient_;
    float crossfeedCoefficient_ = 0.0f;
    float monoBassCoefficient_ = 0.0f;
    float gain_ = 1.0f;
    float gainTarget_ = 1.0f;
    float amount_ = 0.3f;
    float amountTarget_ = 0.3f;
    float cutoff_ = 700.0f;
    float monoBassHz_ = 120.0f;
    float numberCache_ = 0.0f;
    float balanceTarget_ = 0.0f;
    int bits_ = 24;
    int ditherType_ = 0;
    std::size_t delayFrames_ = 0;
    std::size_t delayCursor_ = 0;
    std::uint32_t delayChannel_ = 0;
    std::vector<float> delay_;
    std::array<float, kMaxChannels> x1_{};
    std::array<float, kMaxChannels> y1_{};
    std::array<float, kMaxChannels> shapingError_{};
    float lowL_ = 0.0f;
    float lowR_ = 0.0f;
    std::uint32_t rng_ = 0x474f4445U;
};

class AutoEqNode final : public AudioNode {
public:
    AutoEqNode(const std::string& json, double sampleRate)
        : sampleRate_(sampleRate)
    {
        const auto document = sonare::util::json::parse_strict(
            json.empty() ? "{}" : json);
        if (!document.is_object()) throw std::runtime_error("AutoEQ must be an object");
        if (const auto* preamp = document.find("preampDb");
                preamp && preamp->is_number()) {
            const auto db = std::clamp(preamp->as_float(), -120.0f, 48.0f);
            preamp_ = preampTarget_ = std::pow(10.0f, db / 20.0f);
        }

        std::size_t highestBand = 0;
        bool hasBand = false;
        for (const auto& [key, value] : document.as_object()) {
            if (key.rfind("band", 0) != 0) continue;
            const auto dot = key.find('.', 4);
            if (dot == std::string::npos || dot == 4) continue;
            std::size_t index = 0;
            const auto first = key.data() + 4;
            const auto last = key.data() + dot;
            const auto parsed = std::from_chars(first, last, index);
            if (parsed.ec != std::errc{} || parsed.ptr != last || index >= 128)
                continue;
            highestBand = std::max(highestBand, index);
            hasBand = true;
        }
        if (!hasBand) return;

        const auto groupCount = highestBand / 24U + 1U;
        groups_.reserve(groupCount);
        routes_.reserve(document.as_object().size());
        for (std::size_t group = 0; group < groupCount; ++group) {
            sonare::util::json::Object local;
            const auto begin = group * 24U;
            const auto end = std::min(begin + 24U, highestBand + 1U);
            for (const auto& [key, value] : document.as_object()) {
                if (key.rfind("band", 0) != 0) continue;
                const auto dot = key.find('.', 4);
                if (dot == std::string::npos) continue;
                std::size_t index = 0;
                const auto parsed = std::from_chars(
                    key.data() + 4, key.data() + dot, index);
                if (parsed.ec != std::errc{} || index < begin || index >= end)
                    continue;
                const auto localKey = "band" + std::to_string(index - begin) +
                    key.substr(dot);
                local[localKey] = value;
                routes_.push_back({
                    hashText(key),
                    static_cast<std::uint32_t>(group),
                    hashText(localKey),
                });
            }
            auto processor = sonare::mastering::api::make_insert(
                "eq.parametric", sonare::util::json::dump(
                    sonare::util::json::Value(std::move(local))));
            if (!processor) throw std::runtime_error("AutoEQ processor unavailable");
            auto node = std::make_unique<SonareNode>(std::move(processor));
            node->prepare(sampleRate);
            latency_ += node->latency();
            tail_ += node->tail();
            groups_.push_back(std::move(node));
        }
    }

    bool process(float* const* channels, std::uint32_t channelCount,
        std::size_t frames) noexcept override
    {
        const auto coefficient = 1.0f - std::exp(-1.0f /
            std::max(1.0f, static_cast<float>(sampleRate_) * 0.010f));
        if (preamp_ != 1.0f || preampTarget_ != 1.0f) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                preamp_ += (preampTarget_ - preamp_) * coefficient;
                for (std::uint32_t channel = 0; channel < channelCount; ++channel)
                    channels[channel][frame] *= preamp_;
            }
        }
        for (auto& group : groups_) {
            if (!group->process(channels, channelCount, frames)) return false;
        }
        return true;
    }

    void reset() noexcept override
    {
        for (auto& group : groups_) group->reset();
    }
    std::uint32_t latency() const noexcept override { return latency_; }
    std::uint32_t tail() const noexcept override { return tail_; }
    float gainReduction() const noexcept override
    {
        float result = 0.0f;
        for (const auto& group : groups_)
            result = std::max(result, group->gainReduction());
        return result;
    }
    bool setParameter(std::uint64_t key, float value) noexcept override
    {
        if (key == hashText("preampDb") && std::isfinite(value)) {
            preampTarget_ = std::pow(10.0f,
                std::clamp(value, -120.0f, 48.0f) / 20.0f);
            return true;
        }
        for (const auto& route : routes_) {
            if (route.publicKey != key || route.group >= groups_.size()) continue;
            return groups_[route.group]->setParameter(route.localKey, value);
        }
        return false;
    }

private:
    struct ParameterRoute {
        std::uint64_t publicKey = 0;
        std::uint32_t group = 0;
        std::uint64_t localKey = 0;
    };

    double sampleRate_ = 48000.0;
    float preamp_ = 1.0f;
    float preampTarget_ = 1.0f;
    std::vector<std::unique_ptr<SonareNode>> groups_;
    std::vector<ParameterRoute> routes_;
    std::uint32_t latency_ = 0;
    std::uint32_t tail_ = 0;
};

UtilityKind utilityKind(const std::string& type) noexcept
{
    if (type == "utility.trim") return UtilityKind::trim;
    if (type == "utility.output") return UtilityKind::output;
    if (type == "utility.balance") return UtilityKind::balance;
    if (type == "utility.polarity") return UtilityKind::polarity;
    if (type == "utility.swap") return UtilityKind::swap;
    if (type == "utility.mono") return UtilityKind::mono;
    if (type == "utility.channelDelay") return UtilityKind::channelDelay;
    if (type == "utility.dcBlocker") return UtilityKind::dcBlocker;
    if (type == "utility.crossfeed") return UtilityKind::crossfeed;
    if (type == "utility.monoBass") return UtilityKind::monoBass;
    if (type == "utility.bitDepth") return UtilityKind::bitDepth;
    if (type == "utility.dither") return UtilityKind::dither;
    return UtilityKind::passthrough;
}

std::filesystem::path irPath(const std::string& key)
{
    const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
    return std::filesystem::path(component::utf8ToWide(native.c_str())) / L"nekorox" /
        L"master-dsp" / L"ir" / std::filesystem::path(key);
}

bool validIrAssetKey(const std::string& key) noexcept
{
    if (key.size() != 68U && key.size() != 69U) return false;
    const std::filesystem::path name(key);
    if (name.filename() != name) return false;
    for (std::size_t index = 0; index < 64U; ++index) {
        const auto value = static_cast<unsigned char>(key[index]);
        if (!std::isxdigit(value)) return false;
    }
    auto extension = name.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return extension == ".wav" || extension == ".flac";
}

struct DecodedIr {
    std::vector<std::vector<float>> channels;
};

DecodedIr decodeIr(const std::string& key, std::uint32_t targetRate)
{
    if (key.empty()) return {};
    if (targetRate < 8000 || targetRate > 384000)
        throw std::runtime_error("IR target sample rate is outside 8-384 kHz");
    if (!validIrAssetKey(key)) {
        throw std::runtime_error("IR content key is invalid");
    }
    const auto path = irPath(key);
    const pfc::stringcvt::string_utf8_from_wide utf8(path.c_str());
    const std::string utf8Path(utf8.get_ptr());
    service_ptr_t<input_decoder> decoder;
    service_ptr_t<file> hint;
    abort_callback_dummy abort;
    input_entry::g_open_for_decoding(decoder, hint, utf8Path.c_str(), abort);
    decoder->initialize(0, input_flag_no_looping, abort);
    audio_chunk_impl chunk;
    std::vector<std::vector<float>> planes;
    std::uint32_t sourceRate = 0;
    while (decoder->run(chunk, abort)) {
        const auto frames = static_cast<std::size_t>(chunk.get_sample_count());
        const auto channels = chunk.get_channel_count();
        const auto rate = chunk.get_sample_rate();
        if (frames == 0 || channels == 0 || rate == 0) continue;
        if (sourceRate == 0) sourceRate = rate;
        if (sourceRate != rate) {
            throw std::runtime_error("IR sample rate changes inside the file");
        }
        if (rate < 8000 || rate > 384000)
            throw std::runtime_error("IR sample rate is outside 8-384 kHz");
        if (channels == 0 || channels > kMaxChannels)
            throw std::runtime_error("IR must contain between one and eight channels");
        if (planes.empty()) planes.resize(channels);
        if (planes.size() != channels) {
            throw std::runtime_error("IR channel layout changes inside the file");
        }
        const auto durationMaximum = static_cast<std::size_t>(sourceRate) * 60U;
        const auto memoryMaximum = (128U << 20U) /
            (sizeof(float) * std::max<std::size_t>(planes.size(), 1U));
        const auto maximum = std::min(durationMaximum, memoryMaximum);
        const auto accepted = std::min(frames, maximum -
            std::min(planes.front().size(), maximum));
        const auto* samples = chunk.get_data();
        for (std::size_t frame = 0; frame < accepted; ++frame) {
            for (std::size_t channel = 0; channel < planes.size(); ++channel) {
                const auto value = static_cast<float>(samples[frame * channels + channel]);
                if (!std::isfinite(value))
                    throw std::runtime_error("IR contains non-finite audio");
                planes[channel].push_back(value);
            }
        }
        if (planes.front().size() >= maximum) break;
    }
    if (sourceRate == 0 || planes.empty() || planes.front().empty()) {
        throw std::runtime_error("IR decoder produced no PCM");
    }
    if (sourceRate == targetRate) return {std::move(planes)};
    const auto outputFrames = static_cast<std::uint64_t>(std::ceil(
        planes.front().size() * (double(targetRate) / sourceRate)));
    const auto residentFrames = outputFrames + planes.front().size();
    if (residentFrames > (128ULL << 20U) / (sizeof(float) * planes.size()))
        throw std::runtime_error("IR resampling exceeds the 128 MiB PCM budget");
    DecodedIr result;
    result.channels.reserve(planes.size());
    for (const auto& plane : planes) {
        sonare::streaming_detail::StreamResampler resampler(
            static_cast<int>(sourceRate), static_cast<int>(targetRate));
        std::vector<float> output;
        output.reserve(static_cast<std::size_t>(std::ceil(
            plane.size() * (double(targetRate) / sourceRate))) + 256U);
        resampler.process(plane.data(), plane.size(), output);
        resampler.flush(output);
        if (output.empty() || output.size() != resampler.expected_output_samples() ||
                !std::all_of(output.begin(), output.end(),
                    [](float value) { return std::isfinite(value); })) {
            throw std::runtime_error("IR resampler produced invalid finite audio");
        }
        result.channels.push_back(std::move(output));
    }
    return result;
}

struct NodeSlot {
    std::unique_ptr<AudioNode> node;
    std::uint64_t uuidHash = 0;
    bool bypassed = false;
    std::vector<float> dryDelay;
    std::size_t dryCursor = 0;
    std::uint32_t latency = 0;
    float bypassMix = 0.0f;
    float bypassStep = 1.0f;
    bool forcedFaultBypass = false;
    bool faultReported = false;

    void prepareDry(std::uint32_t channels, std::uint32_t sampleRate)
    {
        latency = node ? node->latency() : 0;
        if (latency > 0 && latency <= kMaxLatencyFrames)
            dryDelay.assign((static_cast<std::size_t>(latency) + 1U) * channels, 0.0f);
        bypassMix = bypassed ? 1.0f : 0.0f;
        bypassStep = 1.0f / std::max(
            static_cast<float>(sampleRate) * 0.015f, 1.0f);
    }

    void advanceDry(float* const* planes, std::uint32_t channels,
        std::size_t frames, float* const* delayed) noexcept
    {
        if (latency == 0 || dryDelay.empty()) {
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                std::copy_n(planes[channel], frames, delayed[channel]);
            return;
        }
        const auto span = static_cast<std::size_t>(latency) + 1U;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto read = (dryCursor + 1U) % span;
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                dryDelay[dryCursor * channels + channel] = planes[channel][frame];
                delayed[channel][frame] = dryDelay[read * channels + channel];
            }
            dryCursor = read;
        }
    }

    void reset() noexcept
    {
        if (node) node->reset();
        std::fill(dryDelay.begin(), dryDelay.end(), 0.0f);
        dryCursor = 0;
    }

    [[nodiscard]] bool needsWet() const noexcept
    {
        return (!bypassed && !forcedFaultBypass) || bypassMix < 1.0f;
    }

    void blendBypass(float* const* wet, float* const* dry,
        std::uint32_t channels, std::size_t frames) noexcept
    {
        const auto target = (bypassed || forcedFaultBypass) ? 1.0f : 0.0f;
        if (bypassMix == target && target == 0.0f) return;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (bypassMix < target) {
                bypassMix = std::min(target, bypassMix + bypassStep);
            } else if (bypassMix > target) {
                bypassMix = std::max(target, bypassMix - bypassStep);
            }
            const auto dryGain = std::sin(
                bypassMix * std::numbers::pi_v<float> * 0.5f);
            const auto wetGain = std::cos(
                bypassMix * std::numbers::pi_v<float> * 0.5f);
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                wet[channel][frame] = wet[channel][frame] * wetGain +
                    dry[channel][frame] * dryGain;
            }
        }
    }

    bool apply(const RtCommand& command) noexcept
    {
        if (uuidHash != command.moduleHash) return false;
        if (command.kind == RtCommandKind::bypass) {
            bypassed = command.value >= 0.5f;
            return true;
        }
        return node && node->setParameter(command.keyHash, command.value);
    }
};

class PreparedMasterRack final {
public:
    PreparedMasterRack(const MasterDspRackV1& rack, std::uint32_t sampleRate,
        std::uint32_t channels, std::uint64_t revision,
        std::uint64_t commandBaseline)
        : sampleRate_(sampleRate), channels_(channels), revision_(revision),
          commandBaseline_(commandBaseline),
          planar_(kMaxChannels * kMicroBlockFrames),
          delayed_(kMaxChannels * kMicroBlockFrames),
          output_(kMaxCallbackFrames * kMaxChannels)
    {
        if (channels == 0 || channels > kMaxChannels || sampleRate < 8000 ||
            sampleRate > 384000) throw std::runtime_error("unsupported stream format");
        meter_.prepare(sampleRate, static_cast<int>(kMicroBlockFrames));
        for (const auto& module : rack.modules) {
            NodeSlot slot;
            slot.uuidHash = hashText(module.uuid);
            slot.bypassed = module.bypassed;
            try {
            if (module.type == "utility.src") {
                // A true terminal sample-rate change alters host block length
                // and output format; it cannot be represented by this fixed-
                // frame PreparedMasterRack facade. Preserve imported presets as
                // a bypassed placeholder until the variable-rate host adapter
                // is qualified, instead of exposing a deceptive no-op.
                slot.node = std::make_unique<UtilityNode>(
                    UtilityKind::passthrough, "{}", sampleRate);
                slot.bypassed = true;
                ++faultCount_;
            } else if (module.type.rfind("utility.", 0) == 0) {
                slot.node = std::make_unique<UtilityNode>(
                    utilityKind(module.type), module.parametersJson, sampleRate);
            } else if (module.type == "autoeq.equalizerApo") {
                slot.node = std::make_unique<AutoEqNode>(
                    module.parametersJson, sampleRate);
            } else {
                std::unique_ptr<sonare::rt::ProcessorBase> processor;
                if (module.type == "effects.reverb.convolution" &&
                    !module.irContentKey.empty()) {
                    const auto ir = decodeIr(module.irContentKey, sampleRate);
                    if (ir.channels.size() != 1U && ir.channels.size() != channels)
                        throw std::runtime_error(
                            "IR channel count must be mono or match the stream");
                    std::vector<std::unique_ptr<sonare::rt::ProcessorBase>> processors;
                    processors.reserve(channels);
                    for (std::uint32_t channel = 0; channel < channels; ++channel) {
                        const auto& plane = ir.channels[std::min<std::size_t>(
                            channel, ir.channels.size() - 1U)];
                        auto instance = sonare::mastering::api::make_insert_with_ir(
                            module.type, module.parametersJson, plane.data(),
                            static_cast<int>(plane.size()));
                        if (!instance) throw std::runtime_error(
                            "Convolution processor unavailable");
                        processors.push_back(std::move(instance));
                    }
                    slot.node = std::make_unique<MultiIrNode>(
                        std::move(processors), sampleRate);
                } else {
                    processor = sonare::mastering::api::make_insert(
                        module.type, module.parametersJson);
                }
                if (slot.node) {
                    // Multichannel convolution was prepared above.
                } else if (!processor) {
                    // Unknown/future modules remain represented but bypassed.
                    slot.node = std::make_unique<UtilityNode>(
                        UtilityKind::passthrough, "{}", sampleRate);
                    slot.bypassed = true;
                    ++faultCount_;
                } else {
                    auto wrapper = std::make_unique<SonareNode>(std::move(processor));
                    wrapper->prepare(sampleRate);
                    if (channels > 2 &&
                        sonare::mastering::api::channel_policy(module.type) ==
                            sonare::mastering::api::ChannelPolicy::StereoPairOnly)
                        slot.node = std::make_unique<FrontStereoNode>(
                            std::move(wrapper), channels);
                    else
                        slot.node = std::move(wrapper);
                }
            }
            } catch (...) {
                // Asset/configuration failure is isolated to this module.
                // The graph remains publishable and both hosts hear aligned
                // dry audio instead of losing the complete rack/session.
                slot.node = std::make_unique<UtilityNode>(
                    UtilityKind::passthrough, "{}", sampleRate);
                slot.bypassed = true;
                slot.forcedFaultBypass = true;
                slot.faultReported = true;
                ++faultCount_;
            }
            slot.prepareDry(channels, sampleRate);
            if (slot.latency > kMaxLatencyFrames ||
                latency_ > kMaxLatencyFrames - slot.latency) {
                slot.node = std::make_unique<UtilityNode>(
                    UtilityKind::passthrough, "{}", sampleRate);
                slot.bypassed = true;
                slot.forcedFaultBypass = true;
                slot.faultReported = true;
                slot.prepareDry(channels, sampleRate);
                ++faultCount_;
            }
            latency_ += slot.latency;
            // In a serial rack, a downstream delay/reverb can extend the tail
            // emitted by every upstream processor. Sum declared tails (with
            // the host's hard 60 s drain ceiling) instead of taking only the
            // longest individual module.
            const auto tailLimit = static_cast<std::uint64_t>(sampleRate) * 60U;
            tail_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                tailLimit, static_cast<std::uint64_t>(tail_) +
                    slot.node->tail()));
            slots_.push_back(std::move(slot));
        }
        tail_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(sampleRate) * 60U,
            static_cast<std::uint64_t>(tail_) + latency_));
    }

    const float* render(const float* input, std::size_t frames, bool collectTelemetry) noexcept
    {
        if (frames > kMaxCallbackFrames) return nullptr;
        gainReduction_ = 0.0f;
        std::size_t offset = 0;
        while (offset < frames) {
            const auto block = std::min(kMicroBlockFrames, frames - offset);
            std::array<float*, kMaxChannels> planes{};
            std::array<float*, kMaxChannels> delayed{};
            for (std::uint32_t channel = 0; channel < channels_; ++channel) {
                planes[channel] = planar_.data() + channel * kMicroBlockFrames;
                delayed[channel] = delayed_.data() + channel * kMicroBlockFrames;
                for (std::size_t frame = 0; frame < block; ++frame)
                    planes[channel][frame] = input[(offset + frame) * channels_ + channel];
            }
            for (auto& slot : slots_) {
                slot.advanceDry(planes.data(), channels_, block, delayed.data());
                if (!slot.needsWet()) {
                    for (std::uint32_t channel = 0; channel < channels_; ++channel)
                        std::copy_n(delayed[channel], block, planes[channel]);
                } else {
                    const auto processed = slot.node->process(
                        planes.data(), channels_, block);
                    bool finite = processed;
                    for (std::uint32_t channel = 0; channel < channels_ && finite; ++channel)
                        for (std::size_t frame = 0; frame < block; ++frame)
                            if (!std::isfinite(planes[channel][frame])) { finite = false; break; }
                    if (!finite) {
                        for (std::uint32_t channel = 0; channel < channels_; ++channel)
                            std::copy_n(delayed[channel], block, planes[channel]);
                        // Preserve the already-running PDC history. A module
                        // exception/non-finite output is converted to a
                        // permanent latency-aligned dry fallback for this
                        // prepared graph, so the callback never throws the
                        // same exception repeatedly.
                        slot.forcedFaultBypass = true;
                        slot.bypassMix = 1.0f;
                        if (!slot.faultReported) {
                            slot.faultReported = true;
                            ++faultCount_;
                        }
                    } else {
                        slot.blendBypass(planes.data(), delayed.data(),
                            channels_, block);
                    }
                }
                if (collectTelemetry)
                    gainReduction_ = std::max(gainReduction_, slot.node->gainReduction());
            }
            for (std::size_t frame = 0; frame < block; ++frame)
                for (std::uint32_t channel = 0; channel < channels_; ++channel)
                    output_[(offset + frame) * channels_ + channel] = planes[channel][frame];
            offset += block;
        }
        return output_.data();
    }

    void measure(const float* interleaved, std::size_t frames) noexcept
    {
        try {
            std::size_t offset = 0;
            while (offset < frames) {
                const auto block = std::min(kMicroBlockFrames, frames - offset);
                std::array<float*, kMaxChannels> planes{};
                for (std::uint32_t channel = 0; channel < channels_; ++channel) {
                    planes[channel] = planar_.data() + channel * kMicroBlockFrames;
                    for (std::size_t frame = 0; frame < block; ++frame) {
                        planes[channel][frame] =
                            interleaved[(offset + frame) * channels_ + channel];
                    }
                }
                meter_.process(planes.data(), static_cast<int>(channels_),
                    static_cast<int>(block));
                offset += block;
            }
        } catch (...) {
            meter_.reset();
            ++faultCount_;
        }
    }
    sonare::mixing::MeterSnapshot meterSnapshot() const noexcept
    {
        return meter_.snapshot();
    }
    void apply(const RtCommand& command) noexcept
    {
        for (auto& slot : slots_) {
            if (slot.apply(command)) return;
        }
    }
    void reset() noexcept
    {
        for (auto& slot : slots_) slot.reset();
        meter_.reset();
    }
    std::uint32_t latency() const noexcept { return latency_; }
    std::uint32_t tail() const noexcept { return tail_; }
    std::uint32_t faults() const noexcept { return faultCount_; }
    float gainReduction() const noexcept { return gainReduction_; }
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t commandBaseline() const noexcept
    {
        return commandBaseline_;
    }
    bool empty() const noexcept { return slots_.empty(); }
    std::uint32_t sampleRate() const noexcept { return sampleRate_; }
    std::uint32_t channels() const noexcept { return channels_; }
    void requestRetirement() noexcept
    {
        retired_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool retirementRequested() const noexcept
    {
        return retired_.load(std::memory_order_acquire);
    }

private:
    std::uint32_t sampleRate_ = 0;
    std::uint32_t channels_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t commandBaseline_ = 0;
    std::uint32_t latency_ = 0;
    std::uint32_t tail_ = 0;
    std::uint32_t faultCount_ = 0;
    std::atomic<bool> retired_{false};
    float gainReduction_ = 0.0f;
    std::vector<NodeSlot> slots_;
    std::vector<float> planar_;
    std::vector<float> delayed_;
    std::vector<float> output_;
    sonare::mixing::MeterProcessor meter_{
        sonare::mixing::MeterConfig{true, true, 4, 0.0f}};
};

using FixedHistory = FixedDspHistory<kMaxLatencyFrames, kMaxCallbackFrames,
    kMaxChannels>;

} // namespace

struct MasterDspProcessor::Impl {
    explicit Impl(MasterDspDomain value)
        : domain(value), wetOutput(kMaxCallbackFrames * kMaxChannels),
          oldInput(kMaxCallbackFrames * kMaxChannels),
          newInput(kMaxCallbackFrames * kMaxChannels),
          oldAligned(kMaxCallbackFrames * kMaxChannels),
          dryOutput(kMaxCallbackFrames * kMaxChannels)
    {
        if (!compilerWake.valid()) {
            prepareState.store(MasterDspPrepareState::fault,
                std::memory_order_release);
            prepareFault.store(ERROR_NOT_ENOUGH_MEMORY,
                std::memory_order_release);
            component::MasterDspController::instance().publishFault(
                "DSP compiler wake event could not be created; playback remains dry.");
            return;
        }
        {
            std::lock_guard lock(gCompilerRegistryMutex);
            gCompilerWakes.push_back(&compilerWake);
        }
        worker = std::thread([this] { compilerLoop(); });
    }

    ~Impl()
    {
        {
            std::lock_guard lock(gCompilerRegistryMutex);
            const auto found = std::find(gCompilerWakes.begin(),
                gCompilerWakes.end(), &compilerWake);
            if (found != gCompilerWakes.end()) gCompilerWakes.erase(found);
        }
        stop.store(true, std::memory_order_release);
        compilerWake.signal();
        if (worker.joinable()) worker.join();
        staged.store(nullptr, std::memory_order_release);
        active = nullptr;
        transitionOld = nullptr;
        programs.clear();
    }

    void compilerLoop()
    {
        auto& controller = component::MasterDspController::instance();
        // Monotonic for this worker lifetime: an empty staged graph has not
        // necessarily replaced the nonempty active graph yet. Clearing this
        // flag at staging could strand that old graph after a second clear.
        bool compiledRackHadSlots = false;
        while (!stop.load(std::memory_order_acquire)) {
            // Capture this before inspecting controller/revision state.  If a
            // control-thread edit races any work below, the predicate at the
            // bottom observes the newer generation and does not go to sleep.
            const auto observedWake = compilerWake.generation.load(
                std::memory_order_acquire);
            std::erase_if(programs, [](const auto& value) {
                return value && value->retirementRequested();
            });
            const auto requestedRevision = controller.revision();
            {
                // Retire stale stages even while enabled. The callback repeats
                // the identity check to close its race with this CAS.
                auto* pending = staged.load(std::memory_order_acquire);
                if (pending != nullptr &&
                    (pending->revision() != requestedRevision ||
                     pending->sampleRate() != requestedRate.load(
                         std::memory_order_acquire) ||
                     pending->channels() != requestedChannels.load(
                         std::memory_order_acquire))) {
                    if (staged.compare_exchange_strong(
                            pending, nullptr,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        pending->requestRetirement();
                    }
                }
            }
            if ((controller.activeModuleCountRt() != 0 || compiledRackHadSlots) &&
                component::rolloutFlagEnabled(component::RolloutFlag::masterDspEngineV1)) {
                const auto rate = requestedRate.load(std::memory_order_acquire);
                const auto channels = requestedChannels.load(std::memory_order_acquire);
                const auto revision = controller.revision();
                const auto forced = forceCompileGeneration.load(
                    std::memory_order_acquire);
                if (rate != 0 && channels != 0 &&
                    (revision != compiledRevision || rate != compiledRate ||
                        channels != compiledChannels ||
                        forced != compiledForceGeneration) &&
                    staged.load(std::memory_order_acquire) == nullptr) {
                    if (controller.enabledRt()) {
                        controller.publishPreparing(true);
                    }
                    try {
                        // Capture before the controller snapshot. A parameter
                        // edit either predates both (already represented in
                        // JSON) or follows this mark (and must be replayed).
                        const auto commandBaseline = gRtCommandSequence.load(
                            std::memory_order_acquire);
                        const auto state = controller.snapshot();
                        const auto& rack = state.selectedB ? state.rackB : state.rackA;
                        auto program = std::make_unique<PreparedMasterRack>(
                            rack, rate, channels, revision, commandBaseline);
                        auto* raw = program.get();
                        // Construction faults are immutable at this point.
                        // Capture them before publication; after staged.store
                        // the audio callback owns all mutable graph state.
                        const auto constructionFaults = raw->faults();
                        // Compilation can be expensive (IR, linear phase,
                        // multiband). Never publish a graph that an edit or a
                        // format transition made stale while it was building.
                        if (stop.load(std::memory_order_acquire) ||
                                controller.revision() != revision ||
                                requestedRate.load(std::memory_order_acquire) != rate ||
                                requestedChannels.load(std::memory_order_acquire) != channels ||
                                forceCompileGeneration.load(
                                    std::memory_order_acquire) != forced) {
                            continue;
                        }
                        programs.push_back(std::move(program));
                        staged.store(raw, std::memory_order_release);
                        compiledRackHadSlots = compiledRackHadSlots || !rack.modules.empty();
                        component::PerformanceTelemetry::instance().event(
                            nekorox::PerformanceEventKindV2::callback,
                            component::TelemetrySurface::spectrum,
                            kDspDiagnosticMagic, 4U, revision,
                            (static_cast<std::uint64_t>(rate) << 32U) |
                                channels);
                        // CPU telemetry calibration is deliberately after the
                        // graph becomes visible to audio.  The former 10 ms
                        // startup sleep delayed first sound processing and let
                        // the host prebuffer an entirely dry window.
                        calibrateTscIfNeeded();
                        compiledRevision = revision;
                        compiledRate = rate;
                        compiledChannels = channels;
                        compiledForceGeneration = forced;
                        preparedRate.store(rate, std::memory_order_release);
                        preparedChannels.store(channels,
                            std::memory_order_release);
                        preparedRevision.store(revision,
                            std::memory_order_release);
                        prepareState.store(MasterDspPrepareState::ready,
                            std::memory_order_release);
                        prepareFault.store(constructionFaults,
                            std::memory_order_release);
                        prepareCondition.notify_all();
                        if (constructionFaults != 0) {
                            controller.publishFault(
                                "One or more DSP modules are unavailable and bypassed.");
                        } else if (controller.enabledRt()) {
                            controller.publishPreparing(false);
                        }
                    } catch (const std::exception& exception) {
                        compiledRevision = revision;
                        compiledRate = rate;
                        compiledChannels = channels;
                        compiledForceGeneration = forced;
                        preparedRate.store(rate, std::memory_order_release);
                        preparedChannels.store(channels,
                            std::memory_order_release);
                        preparedRevision.store(revision,
                            std::memory_order_release);
                        prepareState.store(MasterDspPrepareState::fault,
                            std::memory_order_release);
                        prepareFault.store(1, std::memory_order_release);
                        prepareCondition.notify_all();
                        controller.publishFault(exception.what());
                    } catch (...) {
                        compiledRevision = revision;
                        compiledRate = rate;
                        compiledChannels = channels;
                        compiledForceGeneration = forced;
                        preparedRate.store(rate, std::memory_order_release);
                        preparedChannels.store(channels,
                            std::memory_order_release);
                        preparedRevision.store(revision,
                            std::memory_order_release);
                        prepareState.store(MasterDspPrepareState::fault,
                            std::memory_order_release);
                        prepareFault.store(1, std::memory_order_release);
                        prepareCondition.notify_all();
                        controller.publishFault("DSP rack compilation failed.");
                    }
                }
            }
            if (controller.telemetryDemandRt()) {
                const auto telemetry = readTelemetry();
                if (telemetry.revision != 0 &&
                        telemetry.revision != publishedTelemetryRevision) {
                    publishedTelemetryRevision = telemetry.revision;
                    controller.publishTelemetry(telemetry);
                }
            }
            const auto observedFaults = faults.load(std::memory_order_acquire);
            if (observedFaults > publishedFaultCount) {
                publishedFaultCount = observedFaults;
                controller.publishFault(
                    "A DSP module produced invalid audio and was reset to dry.");
            }
            if (stop.load(std::memory_order_acquire)) break;
            // Meter demand alone owns the 20 ms cadence. Enabled DSP without
            // a visible meter sleeps until a retained semantic wake arrives.
            const auto timeout = controller.telemetryDemandRt() ? 20U : INFINITE;
            if (WaitForSingleObject(compilerWake.event, timeout) == WAIT_FAILED) {
                controller.publishFault(
                    "DSP compiler wake wait failed; playback remains on the last safe graph.");
                break;
            }
        }
    }

    void calibrateTscIfNeeded() noexcept
    {
        if (tscHz.load(std::memory_order_acquire) > 0.0) return;
        LARGE_INTEGER frequency{}, qpcStart{}, qpcEnd{};
        if (!QueryPerformanceFrequency(&frequency) ||
                !QueryPerformanceCounter(&qpcStart)) return;
        const auto cycleStart = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto cycleEnd = __rdtsc();
        if (!QueryPerformanceCounter(&qpcEnd) ||
                qpcEnd.QuadPart <= qpcStart.QuadPart) return;
        const auto seconds = static_cast<double>(
            qpcEnd.QuadPart - qpcStart.QuadPart) /
            static_cast<double>(frequency.QuadPart);
        if (seconds > 0.0) tscHz.store(
            static_cast<double>(cycleEnd - cycleStart) / seconds,
            std::memory_order_release);
    }

    MasterDspTelemetryV1 readTelemetry() const noexcept
    {
        MasterDspTelemetryV1 value;
        value.revision = telemetryRevision.load(std::memory_order_relaxed);
        value.inputPeak[0] = inputPeakL.load(std::memory_order_relaxed);
        value.inputPeak[1] = inputPeakR.load(std::memory_order_relaxed);
        value.outputPeak[0] = outputPeakL.load(std::memory_order_relaxed);
        value.outputPeak[1] = outputPeakR.load(std::memory_order_relaxed);
        value.rms[0] = rmsL.load(std::memory_order_relaxed);
        value.rms[1] = rmsR.load(std::memory_order_relaxed);
        value.correlation = correlation.load(std::memory_order_relaxed);
        value.gainReductionDb = gainReduction.load(std::memory_order_relaxed);
        value.truePeakDbtp = truePeakDb.load(std::memory_order_relaxed);
        value.integratedLufs = integratedLufs.load(std::memory_order_relaxed);
        value.callbackCpuPercent = callbackCpuPercent.load(
            std::memory_order_relaxed);
        value.latencySamples = latency.load(std::memory_order_relaxed);
        value.tailSamples = tail.load(std::memory_order_relaxed);
        value.sampleRate = requestedRate.load(std::memory_order_relaxed);
        value.faultCount = faults.load(std::memory_order_relaxed);
        return value;
    }

    void consumeRtCommands() noexcept
    {
        const auto newest = gRtCommandSequence.load(std::memory_order_acquire);
        if (newest <= lastRtCommandSequence) return;

        // A drag can publish much faster than foobar calls us, especially when
        // its output buffer is a 41k-frame aggregate. Replaying old values here
        // makes the audible control trail the pointer for seconds. Scan the
        // bounded ring newest-first and apply only the newest value for each
        // control. This is fixed-storage and remains lock/allocation free.
        constexpr std::size_t kMaximumUniqueCommands = 64;
        std::array<RtCommand, kMaximumUniqueCommands> latest{};
        std::size_t latestCount = 0;
        bool incomplete = false;
        const auto oldestAvailable = newest >= kRtCommandCapacity
            ? newest - kRtCommandCapacity + 1U : 1U;
        const auto first = std::max(lastRtCommandSequence + 1U,
            oldestAvailable);
        if (first > lastRtCommandSequence + 1U) incomplete = true;

        for (auto sequence = newest; sequence >= first; --sequence) {
            auto& slot = gRtCommands[sequence % kRtCommandCapacity];
            const auto before = slot.published.load(std::memory_order_acquire);
            if (before != sequence) {
                incomplete = true;
                if (sequence == first) break;
                continue;
            }
            const RtCommand command{
                sequence,
                slot.moduleHash.load(std::memory_order_relaxed),
                slot.keyHash.load(std::memory_order_relaxed),
                slot.value.load(std::memory_order_relaxed),
                static_cast<RtCommandKind>(
                    slot.kind.load(std::memory_order_relaxed))};
            const auto after = slot.published.load(std::memory_order_acquire);
            if (before != after) {
                incomplete = true;
                if (sequence == first) break;
                continue;
            }
            const auto duplicate = std::any_of(latest.begin(),
                latest.begin() + static_cast<std::ptrdiff_t>(latestCount),
                [&](const RtCommand& value) {
                    return value.moduleHash == command.moduleHash &&
                        value.keyHash == command.keyHash &&
                        value.kind == command.kind;
                });
            if (!duplicate) {
                if (latestCount == latest.size()) {
                    incomplete = true;
                    break;
                }
                latest[latestCount++] = command;
            }
            if (sequence == first) break;
        }

        // The scan was newest-first. Applying the surviving values in reverse
        // preserves ordering between different controls without replaying any
        // superseded intermediate points.
        for (auto index = latestCount; index > 0; --index) {
            const auto& command = latest[index - 1U];
            if (active) active->apply(command);
            if (transitionOld) transitionOld->apply(command);
        }
        lastRtCommandSequence = newest;
        if (incomplete) {
            forceCompileGeneration.fetch_add(1, std::memory_order_release);
            compilerWake.signal();
        }
    }

    void recordCpu(std::uint64_t cycleStart, std::size_t frames,
        std::uint32_t sampleRate) noexcept
    {
        const auto frequency = tscHz.load(std::memory_order_acquire);
        if (cycleStart == 0 || frequency <= 0.0 || sampleRate == 0 ||
                frames == 0) return;
        const auto cycles = static_cast<double>(__rdtsc() - cycleStart);
        const auto deadline = static_cast<double>(frames) /
            static_cast<double>(sampleRate);
        const auto percent = static_cast<float>(std::clamp(
            100.0 * cycles / (frequency * deadline), 0.0, 999.0));
        const auto previous = callbackCpuPercent.load(
            std::memory_order_relaxed);
        callbackCpuPercent.store(previous + (percent - previous) * 0.2f,
            std::memory_order_relaxed);
    }

    void updateMeters(const float* input, const float* output,
        std::size_t frames, std::uint32_t channels,
        const sonare::mixing::MeterSnapshot& measured) noexcept
    {
        float inL = 0, inR = 0, outL = 0, outR = 0;
        double squareL = 0, squareR = 0, cross = 0;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto il = input[frame * channels];
            const auto ir = channels > 1 ? input[frame * channels + 1] : il;
            const auto ol = output[frame * channels];
            const auto orr = channels > 1 ? output[frame * channels + 1] : ol;
            inL = std::max(inL, std::abs(il)); inR = std::max(inR, std::abs(ir));
            outL = std::max(outL, std::abs(ol)); outR = std::max(outR, std::abs(orr));
            squareL += double(ol) * ol; squareR += double(orr) * orr;
            cross += double(ol) * orr;
        }
        const auto divisor = frames > 0 ? static_cast<double>(frames) : 1.0;
        inputPeakL.store(inL, std::memory_order_relaxed);
        inputPeakR.store(inR, std::memory_order_relaxed);
        outputPeakL.store(outL, std::memory_order_relaxed);
        outputPeakR.store(outR, std::memory_order_relaxed);
        rmsL.store(static_cast<float>(std::sqrt(squareL / divisor)), std::memory_order_relaxed);
        rmsR.store(static_cast<float>(std::sqrt(squareR / divisor)), std::memory_order_relaxed);
        correlation.store(static_cast<float>(cross /
            std::sqrt(std::max(squareL * squareR, 1.0e-20))), std::memory_order_relaxed);
        truePeakDb.store(measured.max_true_peak_db, std::memory_order_relaxed);
        integratedLufs.store(measured.integrated_lufs,
            std::memory_order_relaxed);
        telemetryRevision.store(
            gTelemetrySequence.fetch_add(1, std::memory_order_relaxed) + 1U,
            std::memory_order_release);
    }

    MasterDspDomain domain;
    std::atomic<bool> stop{false};
    std::thread worker;
    CompilerWake compilerWake;
    std::atomic<std::uint32_t> requestedRate{0};
    std::atomic<std::uint32_t> requestedChannels{0};
    std::uint64_t compiledRevision = 0;
    std::uint32_t compiledRate = 0;
    std::uint32_t compiledChannels = 0;
    std::uint64_t publishedTelemetryRevision = 0;
    std::uint32_t publishedFaultCount = 0;
    std::uint64_t lastRtCommandSequence = 0;
    std::deque<std::unique_ptr<PreparedMasterRack>> programs;
    std::atomic<PreparedMasterRack*> staged{nullptr};
    PreparedMasterRack* active = nullptr;
    PreparedMasterRack* transitionOld = nullptr;
    std::uint32_t presentationLatency = 0;
    std::uint32_t transitionOldPresentationLatency = 0;
    std::uint32_t transitionTargetLatency = 0;
    unsigned int activeHistoryIndex = 0;
    unsigned int transitionNewHistoryIndex = 1;
    std::size_t transitionFrame = 0;
    std::size_t transitionLength = 0;
    std::vector<float> wetOutput;
    std::vector<float> oldInput;
    std::vector<float> newInput;
    std::vector<float> oldAligned;
    std::vector<float> dryOutput;
    FixedHistory inputHistory;
    std::array<FixedHistory, 2> wetHistory;
    float dryMix = 1.0f;
    bool latencyReleased = true;
    std::atomic<std::uint64_t> telemetryRevision{0};
    std::atomic<float> inputPeakL{0}, inputPeakR{0};
    std::atomic<float> outputPeakL{0}, outputPeakR{0};
    std::atomic<float> rmsL{0}, rmsR{0}, correlation{1};
    std::atomic<float> gainReduction{0};
    std::atomic<float> truePeakDb{-120.0f};
    std::atomic<float> integratedLufs{-120.0f};
    std::atomic<float> callbackCpuPercent{0.0f};
    std::atomic<double> tscHz{0.0};
    std::atomic<std::uint32_t> latency{0}, tail{0}, faults{0};
    std::atomic<std::uint32_t> inputFaults{0};
    std::atomic<std::uint64_t> prepareGeneration{0};
    std::atomic<std::uint32_t> preparedRate{0};
    std::atomic<std::uint32_t> preparedChannels{0};
    std::atomic<std::uint64_t> preparedRevision{0};
    std::atomic<MasterDspPrepareState> prepareState{
        MasterDspPrepareState::pending};
    std::atomic<std::uint32_t> prepareFault{0};
    std::mutex prepareMutex;
    std::condition_variable prepareCondition;
    std::atomic<std::uint64_t> forceCompileGeneration{0};
    std::uint64_t compiledForceGeneration = 0;
    bool reportedFirstProcess = false;
    bool reportedFirstActivation = false;
};

void notifyMasterDspCompilerWorkers() noexcept
{
    std::lock_guard lock(gCompilerRegistryMutex);
    for (auto* value : gCompilerWakes) if (value) value->signal();
}

bool publishMasterDspParameterCommand(const std::string& moduleUuid,
    const std::string& parameterKey, float value) noexcept
{
    return publishCommand(moduleUuid, parameterKey, value,
        RtCommandKind::parameter);
}

bool publishMasterDspBypassCommand(
    const std::string& moduleUuid, bool bypassed) noexcept
{
    return publishCommand(moduleUuid, {}, bypassed ? 1.0f : 0.0f,
        RtCommandKind::bypass);
}

MasterDspProcessor::MasterDspProcessor(MasterDspDomain domain)
    : impl_(std::make_unique<Impl>(domain))
{
    if (domain == MasterDspDomain::player) {
        std::lock_guard lock(gPlayerProcessorMutex);
        gPlayerProcessors.push_back(this);
    }
}

MasterDspProcessor::~MasterDspProcessor()
{
    if (impl_ && impl_->domain == MasterDspDomain::player) {
        std::lock_guard lock(gPlayerProcessorMutex);
        std::erase(gPlayerProcessors, this);
    }
}

void prewarmMasterDspPlayer(
    std::uint32_t sampleRate, std::uint32_t channels) noexcept
{
    if (sampleRate < 8000 || sampleRate > 384000 || channels == 0 ||
            channels > kMaxChannels) return;
    std::lock_guard lock(gPlayerProcessorMutex);
    for (auto* processor : gPlayerProcessors)
        if (processor) (void)processor->prewarm(sampleRate, channels);
}

MasterDspPrepareTicket MasterDspProcessor::prewarm(
    std::uint32_t sampleRate, std::uint32_t channels) noexcept
{
    if (!impl_ || sampleRate < 8000 || sampleRate > 384000 ||
            channels == 0 || channels > kMaxChannels ||
            !impl_->compilerWake.valid()) {
        MasterDspPrepareTicket invalid;
        invalid.domain = impl_ ? static_cast<std::uint8_t>(impl_->domain) : 0;
        invalid.sampleRate = sampleRate;
        invalid.channels = channels;
        invalid.state = MasterDspPrepareState::fault;
        invalid.faultCode = 1;
        return invalid;
    }
    auto& controller = component::MasterDspController::instance();
    // Observe the stream format even while the rack is dormant. Enabling a
    // configured rack mid-track can then compile immediately from the control
    // thread instead of waiting for the next (potentially very large) foobar
    // audio chunk to reveal its format.
    const auto previousRate = impl_->requestedRate.exchange(
        sampleRate, std::memory_order_acq_rel);
    const auto previousChannels = impl_->requestedChannels.exchange(
        channels, std::memory_order_acq_rel);
    const auto revision = controller.revision();
    if (controller.activeModuleCountRt() == 0) {
        MasterDspPrepareTicket dry;
        dry.domain = static_cast<std::uint8_t>(impl_->domain);
        dry.sampleRate = sampleRate;
        dry.channels = channels;
        dry.rackRevision = revision;
        dry.generation = impl_->prepareGeneration.load(
            std::memory_order_acquire);
        dry.state = MasterDspPrepareState::ready;
        return dry;
    }
    const auto preparedKeyMatches =
        impl_->preparedRate.load(std::memory_order_acquire) == sampleRate &&
        impl_->preparedChannels.load(std::memory_order_acquire) == channels &&
        impl_->preparedRevision.load(std::memory_order_acquire) == revision;
    const auto currentState = impl_->prepareState.load(
        std::memory_order_acquire);
    // A failed (revision, format) is a stable memoized result. Repeated
    // prewarm calls must not turn it back into pending and wake the compiler
    // every callback. A rack edit, format change or explicit force generation
    // naturally changes the key and permits another attempt.
    if (preparedKeyMatches && currentState == MasterDspPrepareState::fault)
        return prepareTicket();
    const auto alreadyReady = preparedKeyMatches &&
        currentState == MasterDspPrepareState::ready;
    if (!alreadyReady || previousRate != sampleRate ||
            previousChannels != channels) {
        impl_->prepareState.store(MasterDspPrepareState::pending,
            std::memory_order_release);
        impl_->prepareFault.store(0, std::memory_order_release);
        impl_->prepareGeneration.fetch_add(1, std::memory_order_acq_rel);
        impl_->compilerWake.signal();
    }
    return prepareTicket();
}

MasterDspPrepareTicket MasterDspProcessor::prepareTicket() const noexcept
{
    MasterDspPrepareTicket ticket;
    if (!impl_) {
        ticket.state = MasterDspPrepareState::fault;
        ticket.faultCode = 1;
        return ticket;
    }
    ticket.domain = static_cast<std::uint8_t>(impl_->domain);
    ticket.sampleRate = impl_->requestedRate.load(std::memory_order_acquire);
    ticket.channels = impl_->requestedChannels.load(std::memory_order_acquire);
    ticket.rackRevision = impl_->preparedRevision.load(
        std::memory_order_acquire);
    ticket.generation = impl_->prepareGeneration.load(
        std::memory_order_acquire);
    ticket.state = impl_->prepareState.load(std::memory_order_acquire);
    ticket.faultCode = impl_->prepareFault.load(std::memory_order_acquire);
    return ticket;
}

bool MasterDspProcessor::waitUntilPrepared(
    std::chrono::milliseconds timeout) noexcept
{
    if (!impl_) return false;
    const auto expectedRate = impl_->requestedRate.load(
        std::memory_order_acquire);
    const auto expectedChannels = impl_->requestedChannels.load(
        std::memory_order_acquire);
    const auto expectedRevision =
        component::MasterDspController::instance().revision();
    std::unique_lock lock(impl_->prepareMutex);
    const auto completed = impl_->prepareCondition.wait_for(lock, timeout, [&] {
        const auto state = impl_->prepareState.load(std::memory_order_acquire);
        return state == MasterDspPrepareState::fault ||
            (state == MasterDspPrepareState::ready &&
             impl_->preparedRate.load(std::memory_order_acquire) == expectedRate &&
             impl_->preparedChannels.load(std::memory_order_acquire) ==
                expectedChannels &&
             impl_->preparedRevision.load(std::memory_order_acquire) ==
                expectedRevision);
    });
    return completed && impl_->prepareState.load(std::memory_order_acquire) ==
        MasterDspPrepareState::ready;
}

bool MasterDspProcessor::process(float* interleaved, std::size_t frames,
    std::uint32_t channels, std::uint32_t sampleRate) noexcept
{
    return processV2(interleaved, frames, channels, sampleRate).changed();
}

MasterDspProcessResultV2 MasterDspProcessor::processV2(
    float* interleaved, std::size_t frames,
    std::uint32_t channels, std::uint32_t sampleRate) noexcept
{
    if (!impl_ || interleaved == nullptr || frames == 0 ||
        channels == 0 || channels > kMaxChannels ||
        frames > kMaxCallbackFrames ||
        !component::rolloutFlagEnabled(component::RolloutFlag::masterDspEngineV1) ||
        !component::rolloutFlagEnabled(component::RolloutFlag::masterDspEngineV2)) {
        return {};
    }
    auto& controller = component::MasterDspController::instance();
    const auto domainFlag = impl_->domain == MasterDspDomain::player
        ? component::RolloutFlag::masterDspPlayerV1
        : component::RolloutFlag::masterDspDawV1;
    if (!component::rolloutFlagEnabled(domainFlag)) return {};

    if (impl_->domain == MasterDspDomain::player &&
            !impl_->reportedFirstProcess) {
        impl_->reportedFirstProcess = true;
        component::PerformanceTelemetry::instance().event(
            nekorox::PerformanceEventKindV2::callback,
            component::TelemetrySurface::spectrum,
            kDspDiagnosticMagic, 3U, sampleRate,
            (static_cast<std::uint64_t>(channels) << 32U) |
                static_cast<std::uint64_t>(frames));
    }

    const auto previousRate = impl_->requestedRate.exchange(
        sampleRate, std::memory_order_acq_rel);
    const auto previousChannels = impl_->requestedChannels.exchange(
        channels, std::memory_order_acq_rel);
    if (previousRate != sampleRate || previousChannels != channels) {
        // One coalesced wake per stream-format transition. Without this, the
        // 20 ms idle compiler cadence lets foobar fill its output buffer with
        // dry blocks before the first graph is staged; long device buffers can
        // then make POWER appear ineffective for an arbitrarily long time.
        // Normal callbacks stay entirely on the atomic fast path.
        impl_->prepareGeneration.fetch_add(1, std::memory_order_acq_rel);
        impl_->prepareState.store(MasterDspPrepareState::pending,
            std::memory_order_release);
        impl_->prepareFault.store(0, std::memory_order_release);
        impl_->compilerWake.signal();
    }

    const auto enabled = controller.enabledRt();
    // Keep a newly compiled program staged while another graph swap is still
    // fading. This bounds lifetime hand-off to one retired graph at a time.
    if (enabled && impl_->transitionOld == nullptr) if (auto* next =
            impl_->staged.exchange(nullptr, std::memory_order_acq_rel)) {
        if (next->revision() != controller.revision() ||
                next->sampleRate() != sampleRate || next->channels() != channels) {
            next->requestRetirement();
            impl_->compilerWake.signal();
        } else {
        auto* previous = impl_->active;
        // A sample-rate transition may arrive before the shadow graph for the
        // new rate. The preceding graph is safe to bridge for a few blocks as
        // long as the interleaving is unchanged; its frequency/time constants
        // are only temporarily scaled. Treat it as transition-compatible so
        // the first note is never emitted dry, then crossfade to the correctly
        // prepared graph. A channel-layout change is not memory-compatible.
        const auto compatible = previous != nullptr &&
            previous->channels() == next->channels();
        impl_->active = next;
        if (impl_->domain == MasterDspDomain::player &&
                !impl_->reportedFirstActivation) {
            impl_->reportedFirstActivation = true;
            component::PerformanceTelemetry::instance().event(
                nekorox::PerformanceEventKindV2::callback,
                component::TelemetrySurface::spectrum,
                kDspDiagnosticMagic, 5U, next->revision(),
                (static_cast<std::uint64_t>(next->sampleRate()) << 32U) |
                    next->channels());
        }
        impl_->transitionFrame = 0;
        impl_->transitionLength = std::max<std::size_t>(
            static_cast<std::size_t>(sampleRate * 0.015), 1U);
        if (previous == nullptr || !compatible) {
            impl_->transitionOld = nullptr;
            impl_->presentationLatency = next->latency();
            impl_->transitionOldPresentationLatency = next->latency();
            impl_->transitionTargetLatency = next->latency();
            impl_->inputHistory.reset();
            impl_->wetHistory[0].reset();
            impl_->wetHistory[1].reset();
            impl_->activeHistoryIndex = 0;
            impl_->transitionNewHistoryIndex = 1;
            impl_->lastRtCommandSequence = std::max(
                impl_->lastRtCommandSequence, next->commandBaseline());
        } else {
            impl_->transitionOld = previous;
            impl_->transitionOldPresentationLatency =
                std::max(impl_->presentationLatency, previous->latency());
            impl_->transitionTargetLatency = std::max(
                impl_->transitionOldPresentationLatency, next->latency());
            impl_->transitionNewHistoryIndex = 1U - impl_->activeHistoryIndex;
            impl_->wetHistory[impl_->transitionNewHistoryIndex].reset();
        }
        if (previous != nullptr && !compatible) {
            previous->requestRetirement();
            impl_->compilerWake.signal();
            impl_->dryMix = 1.0f;
        }
        impl_->latency.store(impl_->transitionOld
                ? impl_->transitionTargetLatency : impl_->presentationLatency,
            std::memory_order_release);
        impl_->tail.store(next->tail(), std::memory_order_release);
        impl_->faults.store(next->faults() +
                impl_->inputFaults.load(std::memory_order_relaxed),
            std::memory_order_release);
        }
    }

    const auto bypassed = controller.bypassedRt();
    if (enabled) impl_->latencyReleased = false;
    const auto requestedRevision = controller.revision();
    const auto failedCurrentGraph =
        impl_->prepareState.load(std::memory_order_acquire) ==
            MasterDspPrepareState::fault &&
        impl_->preparedRate.load(std::memory_order_acquire) == sampleRate &&
        impl_->preparedChannels.load(std::memory_order_acquire) == channels &&
        impl_->preparedRevision.load(std::memory_order_acquire) ==
            requestedRevision;
    if (failedCurrentGraph) {
        // A bad module/asset never takes playback down and an older graph is
        // never allowed to masquerade as the newly requested revision. Keep
        // the already-presented latency until flush, but render aligned dry.
        const auto heldLatency = impl_->transitionOld
            ? impl_->transitionTargetLatency : impl_->presentationLatency;
        for (std::size_t index = 0; index < frames * channels; ++index) {
            if (!std::isfinite(interleaved[index])) interleaved[index] = 0.0f;
        }
        if (heldLatency == 0) {
            return {MasterDspProcessDisposition::untouched, 0,
                requestedRevision,
                impl_->prepareFault.load(std::memory_order_relaxed)};
        }
        impl_->inputHistory.push(interleaved, frames, channels);
        impl_->inputHistory.read(heldLatency, impl_->dryOutput.data(),
            frames, channels);
        std::copy_n(impl_->dryOutput.data(), frames * channels, interleaved);
        return {MasterDspProcessDisposition::delayedDry, heldLatency,
            requestedRevision,
            impl_->prepareFault.load(std::memory_order_relaxed)};
    }
    if (!impl_->active || (!enabled && impl_->latencyReleased &&
            impl_->transitionOld == nullptr)) {
        return {};
    }
    if (impl_->active->channels() != channels) {
        return {};
    }
    const auto earlyHeldLatency = impl_->transitionOld
        ? impl_->transitionTargetLatency : impl_->presentationLatency;
    const auto earlyDry = !enabled || bypassed ||
        (impl_->active->empty() && impl_->transitionOld == nullptr);
    if (earlyDry && impl_->dryMix == 1.0f && earlyHeldLatency == 0) {
        return {MasterDspProcessDisposition::untouched, 0,
            impl_->active->revision(),
            impl_->faults.load(std::memory_order_relaxed)};
    }
    const auto collectTelemetry = controller.telemetryDemandRt();
    const auto cpuCycleStart = collectTelemetry
        ? __rdtsc() : 0U;
    bool sanitizedInput = false;
    for (std::size_t index = 0; index < frames * channels; ++index) {
        if (std::isfinite(interleaved[index])) continue;
        interleaved[index] = 0.0f;
        sanitizedInput = true;
    }
    if (sanitizedInput) {
        impl_->inputFaults.fetch_add(1, std::memory_order_relaxed);
    }
    impl_->consumeRtCommands();
    const auto hadTopologyTransition = impl_->transitionOld != nullptr;
    impl_->inputHistory.push(interleaved, frames, channels);
    const float* wet = nullptr;
    if (impl_->transitionOld) {
        const auto oldInputDelay = impl_->transitionOldPresentationLatency >=
                impl_->transitionOld->latency()
            ? impl_->transitionOldPresentationLatency -
                impl_->transitionOld->latency() : 0U;
        const float* oldInput = interleaved;
        if (oldInputDelay != 0) {
            impl_->inputHistory.read(oldInputDelay, impl_->oldInput.data(),
                frames, channels);
            oldInput = impl_->oldInput.data();
        }
        const auto* previous = impl_->transitionOld->render(oldInput, frames, collectTelemetry);
        if (!previous) previous = oldInput;
        impl_->wetHistory[impl_->activeHistoryIndex].push(
            previous, frames, channels);
        const auto oldOutputDelay = impl_->transitionTargetLatency -
            impl_->transitionOldPresentationLatency;
        impl_->wetHistory[impl_->activeHistoryIndex].read(oldOutputDelay,
            impl_->oldAligned.data(), frames, channels);

        const auto newInputDelay = impl_->transitionTargetLatency >=
                impl_->active->latency()
            ? impl_->transitionTargetLatency - impl_->active->latency() : 0U;
        const float* nextInput = interleaved;
        if (newInputDelay != 0) {
            impl_->inputHistory.read(newInputDelay, impl_->newInput.data(),
                frames, channels);
            nextInput = impl_->newInput.data();
        }
        const auto* processed = impl_->active->render(nextInput, frames, collectTelemetry);
        if (!processed) processed = nextInput;
        impl_->wetHistory[impl_->transitionNewHistoryIndex].push(
            processed, frames, channels);

        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto phase = std::min(1.0,
                double(impl_->transitionFrame + frame) /
                double(impl_->transitionLength));
            const auto oldGain = static_cast<float>(std::cos(
                phase * std::numbers::pi_v<double> * 0.5));
            const auto newGain = static_cast<float>(std::sin(
                phase * std::numbers::pi_v<double> * 0.5));
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const auto index = frame * channels + channel;
                impl_->wetOutput[index] = impl_->oldAligned[index] * oldGain +
                    processed[index] * newGain;
            }
        }
        wet = impl_->wetOutput.data();
        impl_->transitionFrame += frames;
        if (impl_->transitionFrame >= impl_->transitionLength) {
            auto* old = impl_->transitionOld;
            impl_->transitionOld = nullptr;
            impl_->presentationLatency = impl_->transitionTargetLatency;
            impl_->activeHistoryIndex = impl_->transitionNewHistoryIndex;
            impl_->latency.store(impl_->presentationLatency,
                std::memory_order_release);
            impl_->tail.store(impl_->active->tail(), std::memory_order_release);
            old->requestRetirement();
            impl_->compilerWake.signal();
        }
    } else {
        const auto activeInputDelay = impl_->presentationLatency >=
                impl_->active->latency()
            ? impl_->presentationLatency - impl_->active->latency() : 0U;
        const float* activeInput = interleaved;
        if (activeInputDelay != 0) {
            impl_->inputHistory.read(activeInputDelay, impl_->newInput.data(),
                frames, channels);
            activeInput = impl_->newInput.data();
        }
        wet = impl_->active->render(activeInput, frames, collectTelemetry);
        if (!wet) wet = activeInput;
        impl_->wetHistory[impl_->activeHistoryIndex].push(wet, frames, channels);
    }

    if (collectTelemetry) {
        impl_->active->measure(wet, frames);
        impl_->updateMeters(interleaved, wet, frames, channels,
            impl_->active->meterSnapshot());
        impl_->gainReduction.store(
            impl_->active->gainReduction(), std::memory_order_relaxed);
    }
    impl_->faults.store(impl_->active->faults() +
            impl_->inputFaults.load(std::memory_order_relaxed),
        std::memory_order_release);

    const auto heldLatency = impl_->transitionOld
        ? impl_->transitionTargetLatency : impl_->presentationLatency;
    const float* dry = interleaved;
    if (heldLatency != 0) {
        impl_->inputHistory.read(heldLatency, impl_->dryOutput.data(),
            frames, channels);
        dry = impl_->dryOutput.data();
    }
    const auto nullRack = impl_->active->empty() && !hadTopologyTransition;
    if (nullRack) impl_->dryMix = 1.0f;
    const auto targetDry = (!enabled || bypassed || nullRack) ? 1.0f : 0.0f;
    const auto mixStep = 1.0f / std::max(
        static_cast<float>(sampleRate) * 0.015f, 1.0f);
    MasterDspProcessDisposition disposition =
        MasterDspProcessDisposition::processed;
    if (impl_->dryMix == targetDry && targetDry == 0.0f) {
        std::copy_n(wet, frames * channels, interleaved);
    } else if (impl_->dryMix == targetDry && targetDry == 1.0f) {
        if (heldLatency != 0) {
            std::copy_n(dry, frames * channels, interleaved);
            disposition = MasterDspProcessDisposition::delayedDry;
        } else {
            disposition = MasterDspProcessDisposition::untouched;
        }
    } else {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (impl_->dryMix < targetDry) {
                impl_->dryMix = std::min(targetDry, impl_->dryMix + mixStep);
            } else if (impl_->dryMix > targetDry) {
                impl_->dryMix = std::max(targetDry, impl_->dryMix - mixStep);
            }
            const auto dryGain = std::sin(
                impl_->dryMix * std::numbers::pi_v<float> * 0.5f);
            const auto wetGain = std::cos(
                impl_->dryMix * std::numbers::pi_v<float> * 0.5f);
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const auto index = frame * channels + channel;
                interleaved[index] = wet[index] * wetGain +
                    dry[index] * dryGain;
            }
        }
    }
    impl_->recordCpu(cpuCycleStart, frames, sampleRate);
    return {disposition, heldLatency, impl_->active->revision(),
        impl_->faults.load(std::memory_order_relaxed)};
}

void MasterDspProcessor::reset() noexcept
{
    if (!impl_) return;
    if (impl_->active) impl_->active->reset();
    if (impl_->transitionOld) impl_->transitionOld->reset();
    impl_->inputHistory.reset();
    impl_->wetHistory[0].reset();
    impl_->wetHistory[1].reset();
}

void MasterDspProcessor::flush() noexcept
{
    reset();
    if (impl_->transitionOld) {
        impl_->transitionOld->requestRetirement();
        impl_->compilerWake.signal();
        impl_->transitionOld = nullptr;
    }
    const auto& controller = component::MasterDspController::instance();
    const auto activeLatency = controller.enabledRt() && impl_->active &&
            !impl_->active->empty()
        ? impl_->active->latency() : 0U;
    impl_->latencyReleased = activeLatency == 0;
    impl_->dryMix = !controller.enabledRt() || controller.bypassedRt() ||
            !impl_->active || impl_->active->empty()
        ? 1.0f : 0.0f;
    impl_->presentationLatency = activeLatency;
    impl_->transitionOldPresentationLatency = activeLatency;
    impl_->transitionTargetLatency = activeLatency;
    impl_->latency.store(activeLatency, std::memory_order_release);
    impl_->tail.store(activeLatency == 0 || !impl_->active
            ? 0U : impl_->active->tail(),
        std::memory_order_release);
}

std::uint32_t MasterDspProcessor::latencySamples() const noexcept
{
    return impl_ ? impl_->latency.load(std::memory_order_acquire) : 0;
}

std::uint32_t MasterDspProcessor::tailSamples() const noexcept
{
    return impl_ ? impl_->tail.load(std::memory_order_acquire) : 0;
}

MasterDspTelemetryV1 MasterDspProcessor::telemetry() const noexcept
{
    return impl_ ? impl_->readTelemetry() : MasterDspTelemetryV1{};
}

} // namespace nekorox::goden_audio
