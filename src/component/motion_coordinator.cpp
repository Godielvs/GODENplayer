#include "motion_coordinator.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nekorox::component {

MotionCoordinator::MotionCoordinator() noexcept
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    }
}

bool MotionCoordinator::begin(
    MotionKey key,
    MotionToken token,
    MotionMode mode,
    double from,
    double to,
    MotionEasing easing) noexcept
{
    const auto now = nowQpc();
    auto* transition = acquire(key, now);
    if (transition == nullptr) return false;

    const auto durationMs = UiDesignSystemV1::durationMs(token, mode);
    transition->key = key;
    transition->from = from;
    transition->to = to;
    transition->startedQpc = now;
    transition->durationQpc = durationMs == 0
        ? 0
        : std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(
                static_cast<long double>(qpcFrequency_) * durationMs /
                1000.0L));
    transition->easing = easing;
    transition->occupied = true;
    return transition->durationQpc != 0 && from != to;
}

bool MotionCoordinator::begin(
    MotionKey key,
    MotionToken token,
    MotionMode mode,
    double from,
    double to) noexcept
{
    return begin(key, token, mode, from, to, defaultEasing(token));
}

bool MotionCoordinator::retarget(
    MotionKey key,
    MotionToken token,
    MotionMode mode,
    double to,
    MotionEasing easing) noexcept
{
    const auto now = nowQpc();
    const auto* current = find(key);
    const auto from = current != nullptr
        ? sample(*current, now).value
        : to;
    return begin(key, token, mode, from, to, easing);
}

bool MotionCoordinator::retarget(
    MotionKey key,
    MotionToken token,
    MotionMode mode,
    double to) noexcept
{
    return retarget(key, token, mode, to, defaultEasing(token));
}

MotionSample MotionCoordinator::sample(MotionKey key) const noexcept
{
    return sampleAt(key, nowQpc());
}

MotionSample MotionCoordinator::sampleAt(
    MotionKey key,
    std::uint64_t qpcTicks) const noexcept
{
    const auto* transition = find(key);
    return transition != nullptr
        ? sample(*transition, qpcTicks)
        : MotionSample{};
}

bool MotionCoordinator::contains(MotionKey key) const noexcept
{
    return find(key) != nullptr;
}

bool MotionCoordinator::needsFrame() const noexcept
{
    return needsFrameAt(nowQpc());
}

bool MotionCoordinator::needsFrameAt(std::uint64_t qpcTicks) const noexcept
{
    return std::any_of(
        transitions_.begin(),
        transitions_.end(),
        [this, qpcTicks](const Transition& transition) {
            return transition.occupied &&
                sample(transition, qpcTicks).active;
        });
}

void MotionCoordinator::release(MotionKey key) noexcept
{
    if (auto* transition = find(key); transition != nullptr) {
        *transition = {};
    }
}

void MotionCoordinator::clear() noexcept
{
    transitions_.fill({});
}

std::uint64_t MotionCoordinator::nowQpc() const noexcept
{
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) && value.QuadPart > 0
        ? static_cast<std::uint64_t>(value.QuadPart)
        : 0;
}

std::uint64_t MotionCoordinator::qpcFrequency() const noexcept
{
    return qpcFrequency_;
}

MotionEasing MotionCoordinator::defaultEasing(MotionToken token) noexcept
{
    switch (token) {
    case MotionToken::hover:
    case MotionToken::press:
        return MotionEasing::easeOutCubic;
    case MotionToken::selection:
    case MotionToken::tab:
        return MotionEasing::smoothStep;
    case MotionToken::drawer:
    case MotionToken::artwork:
        return MotionEasing::easeInOutCubic;
    }
    return MotionEasing::linear;
}

double MotionCoordinator::applyEasing(
    MotionEasing easing,
    double progress) noexcept
{
    progress = std::clamp(progress, 0.0, 1.0);
    switch (easing) {
    case MotionEasing::linear:
        return progress;
    case MotionEasing::easeOutCubic: {
        const auto inverse = 1.0 - progress;
        return 1.0 - inverse * inverse * inverse;
    }
    case MotionEasing::smoothStep:
        return progress * progress * (3.0 - 2.0 * progress);
    case MotionEasing::easeInOutCubic:
        return progress < 0.5
            ? 4.0 * progress * progress * progress
            : 1.0 - std::pow(-2.0 * progress + 2.0, 3.0) / 2.0;
    }
    return progress;
}

MotionCoordinator::Transition* MotionCoordinator::find(
    MotionKey key) noexcept
{
    const auto iterator = std::find_if(
        transitions_.begin(),
        transitions_.end(),
        [key](const Transition& transition) {
            return transition.occupied && transition.key == key;
        });
    return iterator != transitions_.end() ? &*iterator : nullptr;
}

const MotionCoordinator::Transition* MotionCoordinator::find(
    MotionKey key) const noexcept
{
    const auto iterator = std::find_if(
        transitions_.begin(),
        transitions_.end(),
        [key](const Transition& transition) {
            return transition.occupied && transition.key == key;
        });
    return iterator != transitions_.end() ? &*iterator : nullptr;
}

MotionCoordinator::Transition* MotionCoordinator::acquire(
    MotionKey key,
    std::uint64_t now) noexcept
{
    if (auto* existing = find(key); existing != nullptr) return existing;
    const auto empty = std::find_if(
        transitions_.begin(),
        transitions_.end(),
        [](const Transition& transition) {
            return !transition.occupied;
        });
    if (empty != transitions_.end()) return &*empty;

    Transition* candidate = nullptr;
    auto oldest = std::numeric_limits<std::uint64_t>::max();
    for (auto& transition : transitions_) {
        const auto state = sample(transition, now);
        if (!state.active && transition.startedQpc < oldest) {
            candidate = &transition;
            oldest = transition.startedQpc;
        }
    }
    if (candidate != nullptr) return candidate;

    return &*std::min_element(
        transitions_.begin(),
        transitions_.end(),
        [](const Transition& left, const Transition& right) {
            return left.startedQpc < right.startedQpc;
        });
}

MotionSample MotionCoordinator::sample(
    const Transition& transition,
    std::uint64_t now) const noexcept
{
    if (!transition.occupied) return {};
    if (transition.durationQpc == 0 || transition.from == transition.to) {
        return {transition.to, 1.0, true, false};
    }

    const auto elapsed = now > transition.startedQpc
        ? now - transition.startedQpc
        : 0;
    const auto progress = std::clamp(
        static_cast<double>(elapsed) /
            static_cast<double>(transition.durationQpc),
        0.0,
        1.0);
    const auto eased = applyEasing(transition.easing, progress);
    return {
        transition.from + (transition.to - transition.from) * eased,
        progress,
        true,
        progress < 1.0,
    };
}

} // namespace nekorox::component
