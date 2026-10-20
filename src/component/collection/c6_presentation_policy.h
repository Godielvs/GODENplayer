#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace nekorox::component::c6 {
// UI-owned. Merely observing pending input must not slide a deadline.
// A decoration may yield briefly to urgent input, but its last real production
// bounds the suspension. Hover and inertial scrolling are not urgent input.
class InteractionAdmission final {
public:
    static constexpr std::uint64_t maximumInputDeferMs = 12;
    [[nodiscard]] bool shouldDefer(std::uint64_t nowMs,
        bool resizing, bool urgentInput) const noexcept {
        if (resizing) return true;
        if (!urgentInput || !haveProduction_ || nowMs < lastProductionMs_)
            return false;
        return nowMs - lastProductionMs_ < maximumInputDeferMs;
    }
    void produced(std::uint64_t nowMs) noexcept {
        lastProductionMs_ = nowMs;
        haveProduction_ = true;
    }
    void reset() noexcept { haveProduction_ = false; lastProductionMs_ = 0; }
private:
    std::uint64_t lastProductionMs_ = 0;
    bool haveProduction_ = false;
};

// Missing render observations are not proof of missing PCM. This policy keeps
// already-known history; only an identity change or elapsed capacity expires it.
// It is a reusable contract, NOT connected to the audio bridge by this patch.
struct HistoryDecision {
    bool newRecording = false;
    bool duplicate = false;
    bool discontinuity = false;
    std::uint64_t skippedObservations = 0;
};
class HistoryIdentity final {
public:
    HistoryDecision admit(std::uint64_t serial, std::uint64_t recording,
        std::uint64_t sequence, bool sourceGap) noexcept {
        HistoryDecision d;
        if (!serial || !recording || !sequence) { d.duplicate = true; return d; }
        if (serial_ && (serial < serial_ ||
            (serial == serial_ && recording != recording_))) {
            d.duplicate = true; return d;
        }
        if (serial != serial_ || recording != recording_) {
            d.newRecording = true;
            serial_ = serial; recording_ = recording; last_ = sequence;
            d.discontinuity = sourceGap; return d;
        }
        if (sequence <= last_) { d.duplicate = true; return d; }
        d.skippedObservations = sequence - last_ - 1;
        d.discontinuity = sourceGap;
        last_ = sequence; return d;
    }
private:
    std::uint64_t serial_ = 0, recording_ = 0, last_ = 0;
};
} // namespace nekorox::component::c6
