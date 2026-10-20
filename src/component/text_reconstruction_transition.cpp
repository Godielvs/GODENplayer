#include "text_reconstruction_transition.h"

#include "ui_design_system.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nekorox::component {
namespace {

constexpr std::array<wchar_t, 14> kReconstructionGlyphs{
    L'.', L':', L'=', L'-', L'+', L'*', L'#',
    L'%', L'&', L'@', L'/', L'\\', L'[', L']',
};

double clampUnit(double value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

std::uint64_t mix(std::uint64_t value) noexcept
{
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

} // namespace

void TextReconstructionTransition::setText(std::wstring text, bool animate)
{
    if (text == target_) return;

    source_ = transitioning_ && !display_.empty() ? display_ : target_;
    target_ = std::move(text);
    rebuildDiff();
    seed_ = hashText(target_);
    started_ = std::chrono::steady_clock::now();
    transitioning_ = animate && !source_.empty() && source_ != target_;
    if (!transitioning_) {
        source_ = target_;
        display_ = target_;
    }
}

std::wstring_view TextReconstructionTransition::frame(MotionMode mode)
{
    if (!active(mode)) {
        transitioning_ = false;
        source_ = target_;
        display_ = target_;
        return display_;
    }

    const auto amount = animationProgress(mode);
    if (mode == MotionMode::reduced) {
        display_ = target_;
        return display_;
    }

    const auto slots = std::max(
        sourceGraphemes_.size(), targetGraphemes_.size());
    display_.clear();
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto sourceOffset =
            (slots - sourceGraphemes_.size()) / 2U;
        const auto targetOffset =
            (slots - targetGraphemes_.size()) / 2U;
        const auto sourceIndex = slot >= sourceOffset
            ? slot - sourceOffset : sourceGraphemes_.size();
        const auto targetIndex = slot >= targetOffset
            ? slot - targetOffset : targetGraphemes_.size();
        const auto oldCluster = sourceIndex < sourceGraphemes_.size()
            ? sourceGraphemes_[sourceIndex] : std::wstring(L" ");
        const auto newCluster = targetIndex < targetGraphemes_.size()
            ? targetGraphemes_[targetIndex] : std::wstring(L" ");
        if (targetIndex < targetPreserved_.size() &&
            targetPreserved_[targetIndex]) {
            display_ += newCluster;
            continue;
        }

        const auto random = mix(seed_ ^ (slot * 0x9e3779b97f4a7c15ULL));
        const auto stagger = static_cast<double>(random & 0xffffU) /
            static_cast<double>(0xffffU) * 0.18;
        const auto local = clampUnit((amount - stagger) / 0.77);
        if (local < 0.24) {
            display_ += oldCluster;
        } else if (local < 0.82) {
            const auto phase = static_cast<std::uint64_t>(
                std::floor(local * 31.0));
            const auto glyph = mix(random ^ phase) % kReconstructionGlyphs.size();
            display_ += kReconstructionGlyphs[glyph];
        } else {
            display_ += newCluster;
        }
    }

    // The clean destination is already present in nearly every slot before
    // the semantic clock ends, avoiding an old/scramble/new three-stage cut.
    if (amount >= 0.95) display_ = target_;
    return display_;
}

bool TextReconstructionTransition::active(MotionMode mode) const noexcept
{
    return transitioning_ && animationProgress(mode) < 1.0;
}

double TextReconstructionTransition::animationProgress(
    MotionMode mode) const noexcept
{
    if (!transitioning_ || mode == MotionMode::off) return 1.0;
    const auto duration = mode == MotionMode::reduced ? 180U : 650U;
    if (duration == 0) return 1.0;
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_).count();
    return clampUnit(elapsed / static_cast<double>(duration));
}

std::uint64_t TextReconstructionTransition::hashText(
    std::wstring_view text) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : text) {
        hash ^= static_cast<std::uint64_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<std::wstring> TextReconstructionTransition::graphemes(
    std::wstring_view text)
{
    std::vector<std::wstring> result;
    const auto combining = [](std::uint32_t value) noexcept {
        return (value >= 0x0300U && value <= 0x036fU) ||
            (value >= 0x1ab0U && value <= 0x1affU) ||
            (value >= 0x1dc0U && value <= 0x1dffU) ||
            (value >= 0x20d0U && value <= 0x20ffU) ||
            (value >= 0xfe00U && value <= 0xfe0fU) ||
            (value >= 0xfe20U && value <= 0xfe2fU) ||
            (value >= 0xe0100U && value <= 0xe01efU);
    };
    bool joinNext = false;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<std::uint32_t>(text[index]);
        std::uint32_t codepoint = first;
        std::size_t units = 1;
        if (first >= 0xd800U && first <= 0xdbffU &&
            index + 1U < text.size()) {
            const auto second = static_cast<std::uint32_t>(text[index + 1U]);
            if (second >= 0xdc00U && second <= 0xdfffU) {
                codepoint = 0x10000U + ((first - 0xd800U) << 10U) +
                    (second - 0xdc00U);
                units = 2;
            }
        }
        const auto attach = result.empty() || joinNext || combining(codepoint);
        if (!attach) result.emplace_back();
        if (result.empty()) result.emplace_back();
        result.back().append(text.substr(index, units));
        joinNext = codepoint == 0x200dU;
        index += units;
    }
    return result;
}

void TextReconstructionTransition::rebuildDiff()
{
    sourceGraphemes_ = graphemes(source_);
    targetGraphemes_ = graphemes(target_);
    targetPreserved_.assign(targetGraphemes_.size(), false);
    const auto rows = sourceGraphemes_.size() + 1U;
    const auto columns = targetGraphemes_.size() + 1U;
    std::vector<std::size_t> lcs(rows * columns, 0U);
    for (std::size_t source = 1; source < rows; ++source) {
        for (std::size_t target = 1; target < columns; ++target) {
            lcs[source * columns + target] =
                sourceGraphemes_[source - 1U] == targetGraphemes_[target - 1U]
                ? lcs[(source - 1U) * columns + target - 1U] + 1U
                : std::max(lcs[(source - 1U) * columns + target],
                    lcs[source * columns + target - 1U]);
        }
    }
    auto source = sourceGraphemes_.size();
    auto target = targetGraphemes_.size();
    while (source > 0U && target > 0U) {
        if (sourceGraphemes_[source - 1U] == targetGraphemes_[target - 1U]) {
            targetPreserved_[target - 1U] = true;
            --source;
            --target;
        } else if (lcs[(source - 1U) * columns + target] >=
            lcs[source * columns + target - 1U]) {
            --source;
        } else {
            --target;
        }
    }
}

} // namespace nekorox::component
