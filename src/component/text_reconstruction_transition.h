#pragma once

#include "ui_preferences.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox::component {

class TextReconstructionTransition final {
public:
    void setText(std::wstring text, bool animate);
    [[nodiscard]] std::wstring_view frame(MotionMode mode);
    [[nodiscard]] bool active(MotionMode mode) const noexcept;
    [[nodiscard]] const std::wstring& text() const noexcept { return target_; }
    [[nodiscard]] const std::wstring& sourceText() const noexcept { return source_; }
    [[nodiscard]] double animationProgress(MotionMode mode) const noexcept;

private:
    [[nodiscard]] static std::uint64_t hashText(std::wstring_view text) noexcept;
    [[nodiscard]] static std::vector<std::wstring> graphemes(
        std::wstring_view text);
    void rebuildDiff();

    std::wstring source_;
    std::wstring target_;
    std::wstring display_;
    std::vector<std::wstring> sourceGraphemes_;
    std::vector<std::wstring> targetGraphemes_;
    std::vector<bool> targetPreserved_;
    std::chrono::steady_clock::time_point started_{};
    std::uint64_t seed_ = 0;
    bool transitioning_ = false;
};

} // namespace nekorox::component
