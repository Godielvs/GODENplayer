#include "mastering/api/insert_factory.h"
#include "rt/processor_base.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_countAllocations{false};
std::atomic<std::uint64_t> g_allocations{0};

void* allocate(std::size_t size)
{
    if (g_countAllocations.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (auto* value = std::malloc(size == 0 ? 1 : size)) return value;
    throw std::bad_alloc();
}

bool finite(const std::vector<float>& values)
{
    return std::all_of(values.begin(), values.end(),
        [](float value) { return std::isfinite(value); });
}

void fillSignal(std::vector<float>& left, std::vector<float>& right)
{
    constexpr double rate = 48000.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto time = static_cast<double>(index) / rate;
        left[index] = static_cast<float>(0.18 * std::sin(2.0 * 3.141592653589793 * 997.0 * time));
        right[index] = static_cast<float>(0.16 * std::sin(2.0 * 3.141592653589793 * 613.0 * time + 0.3));
    }
}

int verifyProcessor(const std::string& name)
{
    auto processor = sonare::mastering::api::make_insert(name, "{}");
    if (!processor) {
        std::cerr << "factory returned null: " << name << '\n';
        return 1;
    }
    constexpr std::size_t frames = 8192;
    std::vector<float> left(frames), right(frames);
    fillSignal(left, right);
    processor->prepare(48000.0, static_cast<int>(frames));
    processor->reset();
    const auto realtimeParameters = processor->parameter_descriptors();
    const std::array<std::size_t, 7> partitions{1, 7, 31, 127, 511, 1024, 257};
    std::size_t offset = 0;
    std::size_t partition = 0;
    g_allocations.store(0, std::memory_order_relaxed);
    g_countAllocations.store(true, std::memory_order_release);
    // The Goden command path calls only descriptors explicitly declared RT
    // safe. Exercise those setters and generic bypass while allocation
    // counting is armed, before testing irregular host partitions.
    for (const auto& descriptor : realtimeParameters) {
        if (processor->parameter_is_realtime_safe(descriptor.id)) {
            (void)processor->set_parameter(descriptor.id, 0.5f);
        }
    }
    (void)processor->set_bypassed(true, false);
    (void)processor->set_bypassed(false, false);
    while (offset < frames) {
        const auto count = std::min(partitions[partition++ % partitions.size()], frames - offset);
        float* channels[]{left.data() + offset, right.data() + offset};
        processor->process(channels, 2, static_cast<int>(count));
        offset += count;
    }
    g_countAllocations.store(false, std::memory_order_release);
    if (g_allocations.load(std::memory_order_relaxed) != 0) {
        std::cerr << "realtime allocation: " << name << " ("
                  << g_allocations.load() << ")\n";
        return 1;
    }
    if (!finite(left) || !finite(right)) {
        std::cerr << "non-finite output: " << name << '\n';
        return 1;
    }
    // Maximum qualified host block.
    processor->reset();
    fillSignal(left, right);
    g_allocations.store(0, std::memory_order_relaxed);
    g_countAllocations.store(true, std::memory_order_release);
    {
        float* channels[]{left.data(), right.data()};
        processor->process(channels, 2, static_cast<int>(frames));
    }
    g_countAllocations.store(false, std::memory_order_release);
    if (g_allocations.load(std::memory_order_relaxed) != 0) {
        std::cerr << "realtime allocation at 8192 frames: " << name << " ("
                  << g_allocations.load() << ")\n";
        return 1;
    }
    if (!finite(left) || !finite(right)) {
        std::cerr << "non-finite output at 8192 frames: " << name << '\n';
        return 1;
    }
    const auto latency = processor->latency_samples();
    const auto tail = processor->tail_samples();
    if (latency < 0 || tail < 0 || latency > (1 << 18) || tail > 48000 * 60) {
        std::cerr << "invalid latency/tail: " << name << '\n';
        return 1;
    }
    return 0;
}

int verifyConfiguredAutoEqSignalPath()
{
    // Mirrors the rack state created by the graphical AutoEQ editor.  The
    // catalog smoke test proves that the factory is reachable; this verifies
    // that its flat dotted keys actually produce an audible transfer rather
    // than a successfully constructed no-op processor.
    auto processor = sonare::mastering::api::make_insert(
        "eq.parametric",
        R"({"band0.enabled":true,"band0.type":0,"band0.frequencyHz":53.06,"band0.gainDb":15.11,"band0.q":0.29})");
    if (!processor) {
        std::cerr << "configured AutoEQ factory returned null\n";
        return 1;
    }

    constexpr std::size_t frames = 48000;
    constexpr std::size_t block = 1024;
    constexpr double rate = 48000.0;
    std::vector<float> left(frames), right(frames);
    double inputEnergy = 0.0;
    for (std::size_t index = 0; index < frames; ++index) {
        const auto sample = static_cast<float>(0.02 * std::sin(
            2.0 * 3.141592653589793 * 53.06 *
            static_cast<double>(index) / rate));
        left[index] = right[index] = sample;
        if (index >= frames / 2) inputEnergy += sample * sample;
    }
    processor->prepare(rate, static_cast<int>(block));
    for (std::size_t offset = 0; offset < frames; offset += block) {
        const auto count = std::min(block, frames - offset);
        float* channels[]{left.data() + offset, right.data() + offset};
        processor->process(channels, 2, static_cast<int>(count));
    }
    double outputEnergy = 0.0;
    for (std::size_t index = frames / 2; index < frames; ++index)
        outputEnergy += left[index] * left[index];
    const auto gain = std::sqrt(outputEnergy / std::max(inputEnergy, 1.0e-30));
    if (!std::isfinite(gain) || gain < 3.0) {
        std::cerr << "configured AutoEQ did not affect signal (linear gain "
                  << gain << ")\n";
        return 1;
    }
    return 0;
}

int verifyDetectOnlyTruePeakFirstCallback()
{
    auto processor = sonare::mastering::api::make_insert(
        "maximizer.truePeakLimiter",
        R"({"applyGainAtInputRate":true,"ceilingDb":-1,"lookaheadMs":100,"oversampleFactor":1,"releaseMs":100})");
    if (!processor) {
        std::cerr << "detect-only true-peak limiter unavailable\n";
        return 1;
    }
    constexpr int frames = 8192;
    processor->prepare(48000.0, frames);
    std::vector<float> left(frames, 0.02f), right(frames, -0.02f);
    float* channels[]{left.data(), right.data()};
    g_allocations.store(0, std::memory_order_relaxed);
    g_countAllocations.store(true, std::memory_order_release);
    processor->process(channels, 2, frames);
    g_countAllocations.store(false, std::memory_order_release);
    const auto allocations = g_allocations.load(std::memory_order_relaxed);
    if (allocations != 0) {
        std::cerr << "detect-only true-peak first callback allocated ("
                  << allocations << ")\n";
        return 1;
    }
    return 0;
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

int main(int argc, char** argv)
{
    try {
        const auto names = sonare::mastering::api::insert_factory_names();
        if (names.empty()) {
            std::cerr << "insert factory is empty\n";
            return 1;
        }
        if (argc == 2 && std::string(argv[1]) == "--dump-catalog") {
            for (const auto& name : names) {
                if (name == "dynamics.sidechainRouter") continue;
                std::cout << name;
                for (const auto& parameter :
                        sonare::mastering::api::insert_param_names(name))
                    std::cout << '\t' << parameter;
                std::cout << '\n';
            }
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--dump-defaults") {
            for (const auto& name : names) {
                if (name == "dynamics.sidechainRouter") continue;
                std::cout << name << '\t'
                          << sonare::mastering::api::insert_param_defaults_json(name)
                          << '\n';
            }
            return 0;
        }
        int failures = 0;
        failures += verifyConfiguredAutoEqSignalPath();
        failures += verifyDetectOnlyTruePeakFirstCallback();
        for (const auto& name : names) {
            if (name == "dynamics.sidechainRouter") continue;
            failures += verifyProcessor(name);
        }
        if (failures != 0) {
            std::cerr << failures << " DSP catalog checks failed\n";
            return 1;
        }
        std::cout << "GODENplayer Master DSP catalog: PASS ("
                  << names.size() - 1 << " realtime processors)\n";
        return 0;
    } catch (const std::exception& exception) {
        g_countAllocations.store(false, std::memory_order_release);
        std::cerr << "DSP smoke test exception: " << exception.what() << '\n';
        return 1;
    }
}
