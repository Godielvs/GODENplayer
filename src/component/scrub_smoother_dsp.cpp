#include "scrub_smoother_dsp.h"

#include "scrub_coordinator.h"
#include "master_dsp_controller.h"
#include "master_dsp_engine.h"
#include "performance_telemetry.h"
#include "sdk_compat.h"

#include "nekorox/scrub_crossfade.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace nekorox::component {
namespace {

const GUID kScrubSmootherGuid = {0xa134a729, 0xfabc, 0x4986,
    {0x95, 0xe1, 0x2b, 0x8f, 0xc8, 0x12, 0x4d, 0xe4}};

class GodenScrubSmootherDSP : public dsp_impl_base {
public:
    explicit GodenScrubSmootherDSP(const dsp_preset&)
        : master_(goden_audio::MasterDspDomain::player),
          masterScratch_(32768U * kMaxChannels),
          nativeDryHistory_(kNativeHistoryFrames * kMaxChannels),
          nativeDelayed_(32768U * kMaxChannels)
    {
        PerformanceTelemetry::instance().event(
            nekorox::PerformanceEventKindV2::callback,
            TelemetrySurface::spectrum, kDspDiagnosticMagic, 1U);
    }

    static void g_get_name(pfc::string_base& output)
    {
        output = "GODENplayer Scrub Smoother";
    }
    static bool g_get_default_preset(dsp_preset& output)
    {
        output.set_owner(kScrubSmootherGuid);
        output.set_data(nullptr, 0);
        return true;
    }
    static GUID g_get_guid() { return kScrubSmootherGuid; }
    static bool g_have_config_popup() { return false; }
    static void g_show_config_popup(
        const dsp_preset&, HWND, dsp_preset_edit_callback&) {}

    bool on_chunk(audio_chunk* chunk, abort_callback&) override
    {
        if (chunk == nullptr || chunk->is_empty()) return true;
        const auto channels = chunk->get_channels();
        const auto sampleRate = chunk->get_sample_rate();
        const auto frames = chunk->get_sample_count();
        if (!reportedFirstChunk_) {
            reportedFirstChunk_ = true;
            PerformanceTelemetry::instance().event(
                nekorox::PerformanceEventKindV2::callback,
                TelemetrySurface::spectrum, kDspDiagnosticMagic, 2U,
                sampleRate, (static_cast<std::uint64_t>(channels) << 32U) |
                    static_cast<std::uint64_t>(frames));
        }
        if (channels == 0 || channels > kMaxChannels || sampleRate == 0) {
            resetTail();
            return true;
        }
        if ((masterSampleRate_ != 0 && masterSampleRate_ != sampleRate) ||
                (masterChannels_ != 0 && masterChannels_ != channels))
            resetNativeDry();

        auto& controller = MasterDspController::instance();
        if (!controller.enabledRt() &&
                (masterSampleRate_ != sampleRate ||
                    masterChannels_ != channels)) {
            // This is a dry atomic observation, not graph preparation. It lets
            // POWER compile from the current format immediately when toggled
            // between host callbacks instead of waiting for another chunk.
            (void)master_.prewarm(sampleRate, channels);
        }
        // A configured-but-disabled rack is truly dormant. Only an enabled
        // rack, or a rack retaining PDC until the next flush, enters float32.
        if ((controller.enabledRt() &&
                controller.activeModuleCountRt() != 0) ||
                master_.latencySamples() != 0) {
            // foobar normally starts with a small decoder block, then may
            // deliver much larger aggregate chunks (41,472 frames was observed
            // in the installed player).  Never skip those chunks: partition
            // them through the preallocated realtime engine while preserving
            // filter state across each contiguous segment.
            constexpr std::size_t maxMasterFrames = 32768U;
            const auto totalFrames = static_cast<std::size_t>(frames);
            auto* audio = chunk->get_data();
            for (std::size_t frameOffset = 0; frameOffset < totalFrames;) {
                const auto blockFrames = std::min(
                    maxMasterFrames, totalFrames - frameOffset);
                const auto blockSamples = blockFrames * channels;
                const auto sampleOffset = frameOffset * channels;
                for (std::size_t index = 0; index < blockSamples; ++index) {
                    masterScratch_[index] = static_cast<float>(
                        audio[sampleOffset + index]);
                }
                const auto result = master_.processV2(masterScratch_.data(),
                    blockFrames, channels, sampleRate);
                advanceNativeDry(audio + sampleOffset, blockFrames, channels,
                    result.effectiveLatencySamples);
                if (result.disposition ==
                        goden_audio::MasterDspProcessDisposition::processed) {
                    for (std::size_t index = 0; index < blockSamples; ++index) {
                        audio[sampleOffset + index] =
                            static_cast<audio_sample>(masterScratch_[index]);
                    }
                } else if (result.disposition ==
                        goden_audio::MasterDspProcessDisposition::delayedDry) {
                    std::copy_n(nativeDelayed_.data(), blockSamples,
                        audio + sampleOffset);
                }
                frameOffset += blockFrames;
            }
        }
        masterSampleRate_ = sampleRate;
        masterChannels_ = channels;
        masterChannelConfig_ = chunk->get_channel_config();

        const auto generation = ScrubCoordinator::audioSmoothingGeneration();
        const auto smoothing = ScrubCoordinator::audioSmoothingActive();
        const auto formatMatches = tailFrames_ > 0 &&
            tailChannels_ == channels && tailSampleRate_ == sampleRate;
        if (smoothing && generation != observedGeneration_) {
            auto* data = chunk->get_data();
            const auto crossfadeFrames = std::min<std::size_t>(
                {tailFrames_, static_cast<std::size_t>(frames),
                    static_cast<std::size_t>(
                        std::llround(sampleRate * 0.012))});
            if (formatMatches && crossfadeFrames > 0) {
                const auto tailStart = tailFrames_ - crossfadeFrames;
                for (std::size_t frame = 0; frame < crossfadeFrames; ++frame) {
                    const auto phase = static_cast<double>(frame + 1) /
                        static_cast<double>(crossfadeFrames + 1);
                    for (unsigned channel = 0; channel < channels; ++channel) {
                        const auto index = frame * channels + channel;
                        const auto oldIndex =
                            (tailStart + frame) * channels + channel;
                        const auto oldSample = tail_[oldIndex];
                        const auto newSample = data[index];
                        data[index] = nekorox::scrubCrossfadeSample(
                            oldSample, newSample, phase);
                    }
                }
            } else {
                const auto fadeFrames = std::min<std::size_t>(
                    static_cast<std::size_t>(frames),
                    static_cast<std::size_t>(
                        std::llround(sampleRate * 0.004)));
                for (std::size_t frame = 0; frame < fadeFrames; ++frame) {
                    const auto gain = static_cast<audio_sample>(
                        static_cast<double>(frame + 1) /
                        static_cast<double>(fadeFrames + 1));
                    for (unsigned channel = 0; channel < channels; ++channel) {
                        data[frame * channels + channel] *= gain;
                    }
                }
            }
            observedGeneration_ = generation;
        }
        rememberTail(*chunk);
        return true;
    }

    void on_endoftrack(abort_callback&) override { resetTail(); }
    void on_endofplayback(abort_callback& aborter) override
    {
        drainMasterTail(aborter);
        resetTail();
        master_.reset();
    }
    void flush() override
    {
        master_.flush();
        resetNativeDry();
        if (!ScrubCoordinator::audioSmoothingActive()) resetTail();
    }
    double get_latency() override
    {
        return masterSampleRate_ == 0 ? 0.0 :
            static_cast<double>(master_.latencySamples()) /
                static_cast<double>(masterSampleRate_);
    }
    bool need_track_change_mark() override { return false; }

private:
    static constexpr std::uint64_t kDspDiagnosticMagic = 0x47445053ULL;
    static constexpr std::size_t kMaxChannels = 8;
    static constexpr std::size_t kMaxTailFrames = 6144;
    static constexpr std::size_t kNativeHistoryFrames =
        (1U << 16U) + 32768U + 1U;

    void advanceNativeDry(const audio_sample* source, std::size_t frames,
        std::uint32_t channels, std::uint32_t latency) noexcept
    {
        if (channels == 0 || channels > kMaxChannels) return;
        latency = std::min<std::uint32_t>(latency,
            static_cast<std::uint32_t>(kNativeHistoryFrames - 1U));
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto readFrame = (nativeDryCursor_ + kNativeHistoryFrames -
                latency) % kNativeHistoryFrames;
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const auto input = source[frame * channels + channel];
                nativeDryHistory_[nativeDryCursor_ * kMaxChannels + channel] = input;
                nativeDelayed_[frame * channels + channel] = latency == 0
                    ? input
                    : nativeDryHistory_[readFrame * kMaxChannels + channel];
            }
            nativeDryCursor_ = (nativeDryCursor_ + 1U) % kNativeHistoryFrames;
        }
    }

    void resetNativeDry() noexcept
    {
        std::fill(nativeDryHistory_.begin(), nativeDryHistory_.end(), 0.0);
        nativeDryCursor_ = 0;
    }

    void rememberTail(const audio_chunk& chunk) noexcept
    {
        const auto channels = chunk.get_channels();
        const auto sampleRate = chunk.get_sample_rate();
        const auto frames = static_cast<std::size_t>(chunk.get_sample_count());
        const auto keep = std::min<std::size_t>(
            {frames, kMaxTailFrames,
                static_cast<std::size_t>(
                    std::llround(sampleRate * 0.012))});
        if (keep == 0 || channels == 0 || channels > kMaxChannels) {
            resetTail();
            return;
        }
        const auto* source = chunk.get_data() + (frames - keep) * channels;
        std::copy_n(source, keep * channels, tail_.begin());
        tailFrames_ = keep;
        tailChannels_ = channels;
        tailSampleRate_ = sampleRate;
    }

    void resetTail() noexcept
    {
        tailFrames_ = 0;
        tailChannels_ = 0;
        tailSampleRate_ = 0;
    }

    void drainMasterTail(abort_callback& aborter)
    {
        if (!MasterDspController::instance().enabledRt() ||
            masterSampleRate_ == 0 || masterChannels_ == 0 ||
            masterChannels_ > kMaxChannels) {
            return;
        }
        const auto maximum = std::min<std::uint64_t>(
            master_.tailSamples(),
            static_cast<std::uint64_t>(masterSampleRate_) * 60U);
        if (maximum == 0) return;
        constexpr std::size_t blockFrames = 8192;
        const auto silenceFrames = static_cast<std::uint64_t>(
            masterSampleRate_) / 2U;
        std::uint64_t rendered = 0;
        std::uint64_t quiet = 0;
        const auto started = std::chrono::steady_clock::now();
        bool budgetExceeded = false;
        while (rendered < maximum && quiet < silenceFrames &&
                !aborter.is_aborting()) {
            if (std::chrono::steady_clock::now() - started >=
                    std::chrono::milliseconds(250)) {
                budgetExceeded = true;
                break;
            }
            const auto frames = static_cast<std::size_t>(
                std::min<std::uint64_t>(blockFrames, maximum - rendered));
            const auto samples = frames * masterChannels_;
            std::fill_n(masterScratch_.data(), samples, 0.0f);
            if (!master_.process(masterScratch_.data(), frames,
                    masterChannels_, masterSampleRate_)) {
                break;
            }
            float peak = 0.0f;
            for (std::size_t index = 0; index < samples; ++index) {
                peak = std::max(peak, std::abs(masterScratch_[index]));
            }
            quiet = peak < 1.0e-6f ? quiet + frames : 0U;
            auto* chunk = insert_chunk(samples);
            chunk->set_data_size(samples);
            chunk->set_sample_count(frames);
            chunk->set_sample_rate(masterSampleRate_);
            chunk->set_channels(masterChannels_, masterChannelConfig_);
            auto* output = chunk->get_data();
            for (std::size_t index = 0; index < samples; ++index) {
                output[index] = static_cast<audio_sample>(masterScratch_[index]);
            }
            rendered += frames;
        }
        if (budgetExceeded) {
            MasterDspController::instance().publishFault(
                "DSP tail was truncated to keep playback responsive.");
        }
    }

    std::array<audio_sample, kMaxTailFrames * kMaxChannels> tail_{};
    std::size_t tailFrames_ = 0;
    unsigned tailChannels_ = 0;
    unsigned tailSampleRate_ = 0;
    std::uint64_t observedGeneration_ = 0;
    goden_audio::MasterDspProcessor master_;
    std::vector<float> masterScratch_;
    std::vector<audio_sample> nativeDryHistory_;
    std::vector<audio_sample> nativeDelayed_;
    std::size_t nativeDryCursor_ = 0;
    std::uint32_t masterSampleRate_ = 0;
    unsigned masterChannels_ = 0;
    unsigned masterChannelConfig_ = 0;
    bool reportedFirstChunk_ = false;
};

dsp_factory_t<GodenScrubSmootherDSP> g_scrubSmootherFactory;

constexpr GUID kScrubChainLeaseGuid = {0xbbfce990, 0x7ba8, 0x4d5d,
    {0x90, 0x3e, 0x58, 0xa8, 0xb3, 0x31, 0xca, 0x51}};
cfg_var_modern::cfg_blob g_scrubChainLease(kScrubChainLeaseGuid);

constexpr std::uint32_t kLeaseMagic = 0x314c4347U; // GCL1
constexpr std::uint32_t kLeaseVersion = 1U;
constexpr std::size_t kMaxLeaseChainBytes = 8U << 20U;

struct LeaseHeader {
    std::uint32_t magic = kLeaseMagic;
    std::uint32_t version = kLeaseVersion;
    std::uint32_t previousBytes = 0;
    std::uint32_t managedBytes = 0;
    std::uint32_t payloadCrc = 0;
};

std::uint32_t leaseCrc(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            result = (result >> 1U) ^
                (0xedb88320U & (0U - (result & 1U)));
    }
    return ~result;
}

std::vector<std::uint8_t> chainBytes(const dsp_chain_config& chain)
{
    const auto blob = chain.to_blob();
    if (!blob.is_valid() || blob->size() == 0) return {};
    if (blob->size() > kMaxLeaseChainBytes)
        throw std::runtime_error("DSP chain is too large to lease safely");
    const auto* first = static_cast<const std::uint8_t*>(blob->data());
    return {first, first + blob->size()};
}

bool decodeLease(
    std::vector<std::uint8_t>& previous,
    std::vector<std::uint8_t>& managed) noexcept
{
    previous.clear();
    managed.clear();
    const auto blob = g_scrubChainLease.get();
    if (!blob.is_valid() || blob->size() == 0) return false;
    if (blob->size() < sizeof(LeaseHeader)) return false;
    LeaseHeader header{};
    std::memcpy(&header, blob->data(), sizeof(header));
    const auto payloadBytes = static_cast<std::size_t>(header.previousBytes) +
        static_cast<std::size_t>(header.managedBytes);
    if (header.magic != kLeaseMagic || header.version != kLeaseVersion ||
        header.previousBytes > kMaxLeaseChainBytes ||
        header.managedBytes > kMaxLeaseChainBytes ||
        payloadBytes != blob->size() - sizeof(header)) {
        return false;
    }
    const auto* payload = static_cast<const std::uint8_t*>(blob->data()) +
        sizeof(header);
    if (leaseCrc({payload, payloadBytes}) != header.payloadCrc) return false;
    previous.assign(payload, payload + header.previousBytes);
    managed.assign(payload + header.previousBytes, payload + payloadBytes);
    return true;
}

void clearLease() noexcept
{
    try {
        g_scrubChainLease.set(fb2k::memBlockRef{});
    } catch (...) {
    }
}

void persistLease(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& managed)
{
    if (previous.size() > kMaxLeaseChainBytes ||
        managed.size() > kMaxLeaseChainBytes) {
        throw std::runtime_error("DSP chain lease exceeds its safety limit");
    }
    LeaseHeader header;
    header.previousBytes = static_cast<std::uint32_t>(previous.size());
    header.managedBytes = static_cast<std::uint32_t>(managed.size());
    std::vector<std::uint8_t> bytes(sizeof(header) + previous.size() +
        managed.size());
    auto* payload = bytes.data() + sizeof(header);
    if (!previous.empty())
        std::memcpy(payload, previous.data(), previous.size());
    if (!managed.empty())
        std::memcpy(payload + previous.size(), managed.data(), managed.size());
    header.payloadCrc = leaseCrc({payload, previous.size() + managed.size()});
    std::memcpy(bytes.data(), &header, sizeof(header));
    g_scrubChainLease.set(bytes.data(), bytes.size());
}

bool removeManagedEntries(dsp_chain_config_impl& chain)
{
    auto changed = false;
    for (auto index = chain.get_count(); index != 0; --index) {
        if (chain.get_item(index - 1U).get_owner() == kScrubSmootherGuid) {
            chain.remove_item(index - 1U);
            changed = true;
        }
    }
    return changed;
}

} // namespace

void restoreScrubSmootherDspChain() noexcept
{
    try {
        std::vector<std::uint8_t> previous;
        std::vector<std::uint8_t> managed;
        if (!decodeLease(previous, managed)) {
            clearLease();
            return;
        }
        const auto manager = dsp_config_manager::get();
        dsp_chain_config_impl current;
        manager->get_core_settings(current);
        const auto currentBytes = chainBytes(current);
        if (currentBytes == managed) {
            dsp_chain_config_impl restored;
            restored.from_blob(previous.data(), previous.size());
            manager->set_core_settings(restored);
        } else if (removeManagedEntries(current)) {
            // The user changed the chain while the lease was active. Preserve
            // those edits and remove only the entry owned by GODENplayer.
            manager->set_core_settings(current);
        }
        clearLease();
    } catch (...) {
        // Shutdown and crash recovery must never make process teardown fail.
    }
}

void ensureScrubSmootherDspEnabled(bool allowMutation)
{
    // First recover a lease left by a crash or forced termination. Safe mode
    // intentionally performs only this recovery step.
    restoreScrubSmootherDspChain();
    if (!allowMutation) return;

    const auto manager = dsp_config_manager::get();
    dsp_chain_config_impl chain;
    manager->get_core_settings(chain);
    PerformanceTelemetry::instance().event(
        nekorox::PerformanceEventKindV2::callback,
        TelemetrySurface::spectrum, 0x47445053ULL, 10U,
        dsp_entry::g_dsp_exists(kScrubSmootherGuid) ? 1U : 0U,
        chain.get_count());

    // Respect an entry that the user already owns, including its chosen order.
    if (chain.contains_dsp(kScrubSmootherGuid)) return;

    dsp_preset_impl preset;
    if (dsp_entry::g_get_default_preset(preset, kScrubSmootherGuid)) {
        const auto previous = chainBytes(chain);
        chain.add_item(preset);
        const auto managed = chainBytes(chain);
        persistLease(previous, managed);
        try {
            manager->set_core_settings(chain);
        } catch (...) {
            clearLease();
            throw;
        }
    }
}

} // namespace nekorox::component
