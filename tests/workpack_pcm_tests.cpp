#include "nekorox/realtime_ring.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>

void runWorkpackPcmTests()
{
    using Bus = nekorox::RealtimeFrameBus<8>;
    auto invalid = std::make_unique<Bus>();
    std::array<float, 8> samples{};
    if (invalid->publish(samples.data(), samples.size(), 0, 2, 4,
            0, 0.0, 0) ||
        invalid->publish(samples.data(), 7U, 48000, 2, 4,
            0, 0.0, 0) ||
        invalid->publish(samples.data(), samples.size(), 48000, 0, 4,
            0, 0.0, 0)) {
        throw std::runtime_error("WP05 accepted invalid PCM geometry");
    }

    auto bus = std::make_unique<Bus>();
    std::atomic<bool> producerDone{false};
    std::atomic<bool> corrupt{false};
    constexpr std::uint64_t publications = 12000U;
    auto reader = [&](std::uint64_t salt) {
        auto frame = std::make_unique<nekorox::RealtimeFrameV1>();
        std::uint64_t cursor = 0;
        std::uint64_t idle = 0;
        while (!producerDone.load(std::memory_order_acquire) || idle < 2000U) {
            if (!bus->consumeLatest(*frame, cursor)) {
                ++idle;
                std::this_thread::yield();
                continue;
            }
            idle = 0;
            if (frame->sampleRate != 48000U || frame->channels != 2U ||
                frame->frameCount != 4U || frame->sampleCount != 8U) {
                corrupt.store(true, std::memory_order_release);
                break;
            }
            const auto expected = static_cast<float>(frame->qpcTicks);
            for (std::size_t index = 0; index < frame->sampleCount; ++index) {
                if (frame->samples[index] != expected) {
                    corrupt.store(true, std::memory_order_release);
                    break;
                }
            }
            if ((cursor + salt) == 0U) corrupt.store(true);
        }
    };
    std::thread first(reader, 1U);
    std::thread second(reader, 2U);
    for (std::uint64_t attempt = 1; attempt <= publications; ++attempt) {
        for (;;) {
            samples.fill(static_cast<float>(attempt));
            if (bus->publish(samples.data(), samples.size(), 48000, 2, 4,
                    attempt, static_cast<double>(attempt), 1U)) break;
            std::this_thread::yield();
        }
    }
    producerDone.store(true, std::memory_order_release);
    first.join();
    second.join();
    if (corrupt.load(std::memory_order_acquire) ||
        bus->publishedSequence() < publications) {
        throw std::runtime_error("WP05 multicast stress observed torn PCM");
    }
}
