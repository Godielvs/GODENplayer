#pragma once

#include <algorithm>

namespace nekorox::component {

struct UiFormRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// A small, deterministic DIP-space flow primitive shared by native forms.
// It deliberately knows nothing about HWNDs, DPI, scrolling, or data state.
class UiFormFlow final {
public:
    explicit UiFormFlow(int topDip = 0) noexcept : cursor_(topDip) {}

    [[nodiscard]] UiFormRect take(int xDip, int widthDip,
        int heightDip, int gapAfterDip = 0) noexcept
    {
        const UiFormRect result{xDip, cursor_, (std::max)(1, widthDip),
            (std::max)(1, heightDip)};
        cursor_ += result.height + (std::max)(0, gapAfterDip);
        return result;
    }

    void gap(int valueDip) noexcept { cursor_ += (std::max)(0, valueDip); }
    [[nodiscard]] int cursor() const noexcept { return cursor_; }

private:
    int cursor_ = 0;
};

} // namespace nekorox::component
