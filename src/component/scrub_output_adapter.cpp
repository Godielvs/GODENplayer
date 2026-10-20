#include "scrub_output_adapter.h"

#include "sdk_compat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace nekorox::component {
namespace {

template <typename T>
std::array<std::uint8_t, sizeof(T)> bytesOf(const T& value) noexcept
{
    std::array<std::uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

GUID guidOf(const std::array<std::uint8_t, 16>& bytes) noexcept
{
    GUID value{};
    static_assert(sizeof(value) == 16);
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

class SdkScrubOutputStream final : public ScrubOutputStream {
public:
    explicit SdkScrubOutputStream(output::ptr output) : output_(std::move(output)) {}
    ~SdkScrubOutputStream() override { stop(); }

    bool submit(const ScrubPcmBlock& block,
        std::string& error) noexcept override
    {
        if (!output_.is_valid() || block.frames == 0 ||
            block.channels == 0 || block.sampleRate == 0 ||
            block.samples.size() != block.frames * block.channels) {
            error = "invalid preview PCM block";
            return false;
        }
        try {
            audio_chunk_impl chunk;
            chunk.set_data_size(block.samples.size());
            chunk.set_sample_count(block.frames);
            chunk.set_sample_rate(block.sampleRate);
            chunk.set_channels(block.channels, block.channelMask);
            auto* destination = chunk.get_data();
            std::transform(block.samples.begin(), block.samples.end(),
                destination, [](float sample) {
                    return static_cast<audio_sample>(sample);
                });
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(500);
            for (;;) {
                bool ready = false;
                output_->update(ready);
                if (ready) {
                    output_->process_samples(chunk);
                    return true;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    error = "preview output readiness timeout";
                    return false;
                }
                const auto event = output_->get_trigger_event_();
                if (event != pfc::eventInvalid) {
                    WaitForSingleObject(event, 5);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "preview output rejected PCM";
        }
        return false;
    }

    void forcePlay() noexcept override
    {
        try { if (output_.is_valid()) output_->force_play(); } catch (...) {}
    }

    void flush() noexcept override
    {
        try { if (output_.is_valid()) output_->flush(); } catch (...) {}
    }

    void stop() noexcept override
    {
        try {
            if (output_.is_valid()) {
                output_->pause(true);
                output_->flush();
            }
        } catch (...) {}
        output_.release();
    }

private:
    output::ptr output_;
};

} // namespace

ScrubOutputRoute probeScrubOutputRoute() noexcept
{
    ScrubOutputRoute route;
    try {
        const auto config = output_manager::get()->getCoreConfig();
        output_entry::ptr entry;
        if (!output_entry::g_find(config.m_output, entry) ||
            !entry.is_valid()) {
            route.reason = "current output module could not be resolved";
            return route;
        }
        const auto flags = entry->get_config_flags_compat();
        if ((flags & output_entry::flag_supports_multiple_streams) == 0) {
            route.reason = "current output does not support parallel streams";
            return route;
        }
        if ((flags & output_entry::flag_high_latency) != 0) {
            route.reason = "current output is high latency";
            return route;
        }
        route.available = true;
        route.outputId = bytesOf(config.m_output);
        route.deviceId = bytesOf(config.m_device);
        route.bitDepth = config.m_bitDepth;
        route.dither = (config.m_flags & outputCoreConfig_t::flagUseDither) != 0;
        route.userVolumeDb = playback_control::get()->get_volume();
    } catch (const std::exception& exception) {
        route.reason = exception.what();
    } catch (...) {
        route.reason = "output capability probe failed";
    }
    return route;
}

std::unique_ptr<ScrubOutputStream> openScrubOutput(
    const ScrubOutputRoute& route, std::string& error) noexcept
{
    if (!route.available) {
        error = route.reason.empty() ? "preview output unavailable" : route.reason;
        return {};
    }
    try {
        output_entry::ptr entry;
        if (!output_entry::g_find(guidOf(route.outputId), entry) ||
            !entry.is_valid()) {
            error = "preview output module disappeared";
            return {};
        }
        output::ptr instance;
        entry->instantiate(instance, guidOf(route.deviceId), 0.050,
            route.dither, route.bitDepth);
        if (!instance.is_valid()) {
            error = "preview output returned no stream";
            return {};
        }
        instance->volume_set(route.userVolumeDb);
        return std::make_unique<SdkScrubOutputStream>(std::move(instance));
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "preview output open failed";
    }
    return {};
}

} // namespace nekorox::component
