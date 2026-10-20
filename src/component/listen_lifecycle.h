#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace nekorox::component {

class ListenLifecycleState final {
public:
    static constexpr double kAnalysisDwellSeconds = 1.5;

    void onTrackChanged(
        bool analyzableTrack,
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) noexcept
    {
        hasAnalyzableTrack_ = analyzableTrack;
        if (visible_ && playing_ && hasAnalyzableTrack_) {
            arm(nowQpc, qpcFrequency);
        } else {
            disarm();
        }
    }

    bool setVisible(
        bool visible,
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) noexcept
    {
        if (visible_ == visible) return false;
        visible_ = visible;
        if (visible_ && playing_ && hasAnalyzableTrack_) {
            arm(nowQpc, qpcFrequency);
        } else {
            disarm();
        }
        return true;
    }

    bool setPlaying(
        bool playing,
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) noexcept
    {
        if (playing_ == playing) return false;
        playing_ = playing;
        if (visible_ && playing_ && hasAnalyzableTrack_) {
            arm(nowQpc, qpcFrequency);
        } else {
            disarm();
        }
        return true;
    }

    void requestAnalysis(
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) noexcept
    {
        if (!visible_ || !playing_ || !hasAnalyzableTrack_) return;
        if (eligibleQpc_ == 0) setEligibility(nowQpc, qpcFrequency);
        analysisPending_ = true;
    }

    void cancelPendingAnalysis() noexcept
    {
        analysisPending_ = false;
    }

    [[nodiscard]] bool consumeAnalysisDue(std::uint64_t nowQpc) noexcept
    {
        if (!analysisPending_ || !visible_ || !playing_ ||
            !hasAnalyzableTrack_ ||
            nowQpc < eligibleQpc_) {
            return false;
        }
        analysisPending_ = false;
        return true;
    }

    [[nodiscard]] std::uint32_t remainingMilliseconds(
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) const noexcept
    {
        if (!analysisPending_ || eligibleQpc_ <= nowQpc ||
            qpcFrequency == 0) {
            return analysisPending_ ? 1U : 0U;
        }
        const auto remainingTicks = eligibleQpc_ - nowQpc;
        const auto milliseconds = std::ceil(
            static_cast<long double>(remainingTicks) * 1000.0L /
            static_cast<long double>(qpcFrequency));
        return static_cast<std::uint32_t>(std::clamp<long double>(
            milliseconds,
            1.0L,
            static_cast<long double>(
                (std::numeric_limits<std::uint32_t>::max)())));
    }

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] bool analysisPending() const noexcept
    {
        return analysisPending_;
    }

private:
    void arm(
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) noexcept
    {
        setEligibility(nowQpc, qpcFrequency);
        analysisPending_ = true;
    }

    void setEligibility(
        std::uint64_t nowQpc,
        std::uint64_t qpcFrequency) noexcept
    {
        const auto dwellTicks = static_cast<std::uint64_t>(std::ceil(
            static_cast<long double>(qpcFrequency) *
            kAnalysisDwellSeconds));
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        eligibleQpc_ = nowQpc > maximum - dwellTicks
            ? maximum
            : nowQpc + dwellTicks;
    }

    void disarm() noexcept
    {
        analysisPending_ = false;
        eligibleQpc_ = 0;
    }

    bool visible_ = false;
    bool playing_ = false;
    bool hasAnalyzableTrack_ = false;
    bool analysisPending_ = false;
    std::uint64_t eligibleQpc_ = 0;
};

} // namespace nekorox::component
