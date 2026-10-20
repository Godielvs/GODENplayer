#pragma once

#include "ui_design_system.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace nekorox::component {

using MotionKey = std::uint64_t;

enum class MotionEasing : std::uint8_t {
    linear = 0,
    easeOutCubic,
    smoothStep,
    easeInOutCubic,
};

struct MotionSample {
    double value = 0.0;
    double progress = 1.0;
    bool found = false;
    bool active = false;
};

// Stores semantic transitions but owns no HWND, timer, wait or render loop.
// A host starts its presentation clock only while needsFrame() is true.
class MotionCoordinator final {
public:
    static constexpr std::size_t capacity = 64;

    MotionCoordinator() noexcept;

    MotionCoordinator(const MotionCoordinator&) = delete;
    MotionCoordinator& operator=(const MotionCoordinator&) = delete;

    bool begin(
        MotionKey key,
        MotionToken token,
        MotionMode mode,
        double from,
        double to,
        MotionEasing easing) noexcept;
    bool begin(
        MotionKey key,
        MotionToken token,
        MotionMode mode,
        double from,
        double to) noexcept;
    bool retarget(
        MotionKey key,
        MotionToken token,
        MotionMode mode,
        double to,
        MotionEasing easing) noexcept;
    bool retarget(
        MotionKey key,
        MotionToken token,
        MotionMode mode,
        double to) noexcept;

    [[nodiscard]] MotionSample sample(MotionKey key) const noexcept;
    [[nodiscard]] MotionSample sampleAt(
        MotionKey key,
        std::uint64_t qpcTicks) const noexcept;
    [[nodiscard]] bool contains(MotionKey key) const noexcept;
    [[nodiscard]] bool needsFrame() const noexcept;
    [[nodiscard]] bool needsFrameAt(std::uint64_t qpcTicks) const noexcept;

    void release(MotionKey key) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::uint64_t nowQpc() const noexcept;
    [[nodiscard]] std::uint64_t qpcFrequency() const noexcept;
    [[nodiscard]] static MotionEasing defaultEasing(
        MotionToken token) noexcept;
    [[nodiscard]] static double applyEasing(
        MotionEasing easing,
        double progress) noexcept;

private:
    struct Transition {
        MotionKey key = 0;
        double from = 0.0;
        double to = 0.0;
        std::uint64_t startedQpc = 0;
        std::uint64_t durationQpc = 0;
        MotionEasing easing = MotionEasing::linear;
        bool occupied = false;
    };

    [[nodiscard]] Transition* find(MotionKey key) noexcept;
    [[nodiscard]] const Transition* find(MotionKey key) const noexcept;
    [[nodiscard]] Transition* acquire(
        MotionKey key,
        std::uint64_t now) noexcept;
    [[nodiscard]] MotionSample sample(
        const Transition& transition,
        std::uint64_t now) const noexcept;

    std::array<Transition, capacity> transitions_{};
    std::uint64_t qpcFrequency_ = 1;
};

} // namespace nekorox::component
