#pragma once

#include <cstdint>

namespace nekorox::component {

// Values 0..2 are persisted by existing profiles and remain unchanged.
enum class MotionMode : std::uint32_t { normal = 0, reduced = 1, off = 2 };
enum class MotionPreference : std::uint32_t {
    normal = 0, reduced = 1, off = 2, followWindows = 3,
};

constexpr MotionPreference sanitizeMotionPreference(std::uint32_t value) noexcept
{
    return value <= static_cast<std::uint32_t>(MotionPreference::followWindows)
        ? static_cast<MotionPreference>(value) : MotionPreference::followWindows;
}

constexpr MotionMode resolveMotionPreference(MotionPreference preference,
    bool systemQuerySucceeded, bool systemAnimationsEnabled) noexcept
{
    if (preference == MotionPreference::normal) return MotionMode::normal;
    if (preference == MotionPreference::reduced) return MotionMode::reduced;
    if (preference == MotionPreference::off) return MotionMode::off;
    // A failed query must not silently disable the player's animations.
    return systemQuerySucceeded && !systemAnimationsEnabled
        ? MotionMode::reduced : MotionMode::normal;
}

// UI-thread environment callback; the cfg/SDK owner implements this.
void refreshSystemMotionPreference() noexcept;

} // namespace nekorox::component
