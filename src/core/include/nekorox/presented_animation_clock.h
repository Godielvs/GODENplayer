#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nekorox {
// Drawing/decoding does not consume visible animation time. A surface reports
// the generation it actually drew after EndDraw/Present has accepted the frame.
class PresentedAnimationClock final {
public:
    void reset(std::uint64_t generation = 0) noexcept {
        generation_ = generation; start_ = 0; started_ = false;
    }
    bool accept(std::uint64_t ticket, bool accepted, double now) noexcept {
        if (!accepted || ticket == 0 || ticket != generation_ || started_ || !std::isfinite(now)) return false;
        start_ = now; started_ = true; return true;
    }
    double elapsedMilliseconds(double now) const noexcept {
        return started_ && std::isfinite(now) ? std::max(0.0, (now - start_) * 1000.0) : 0.0;
    }
    bool started() const noexcept { return started_; }
    double startSeconds() const noexcept { return started_ ? start_ : 0.0; }
    std::uint64_t generation() const noexcept { return generation_; }
private:
    std::uint64_t generation_ = 0;
    double start_ = 0;
    bool started_ = false;
};
}
