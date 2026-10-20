#pragma once

#include <cmath>

namespace nekorox::component {

// One source repair per activation/discontinuity. Ordinary publication jitter and
// pause are not failures; only sustained absence of fresh PCM can spend it.
class VisualSourceRecovery final {
    double waitingSince_=-1;
    bool attempted_=false;
public:
    void activate(double now) noexcept {waitingSince_=now;attempted_=false;}
    void reset() noexcept {*this={};}
    void commit() noexcept {attempted_=true;}
    bool observe(double now,bool fresh) noexcept {
        if(!std::isfinite(now))return false;
        if(fresh){waitingSince_=-1;return false;}
        if(waitingSince_<0||now<waitingSince_)waitingSince_=now;
        if(attempted_||now-waitingSince_<.5)return false;
        // A global cooldown may deny the repair. Spend the one-shot budget
        // only after the native source was actually released/rebased.
        return true;
    }
};

// Observe debt AFTER the bounded read batch. Successful reads do not imply
// fresh audio: at 25 Hz a four-by-10 ms pump cannot drain an existing backlog.
// Give normal catch-up two observations, then bound its time to fresh data.
class VisualCaptureRecovery final {
public:
    void reset() noexcept { *this = {}; }

    bool takeRebase() noexcept {
        const bool result = rebase_;
        if (result) reset();
        return result;
    }

    void observe(double clock, double sourceAge) noexcept {
        if (!std::isfinite(clock) || !std::isfinite(sourceAge) ||
                sourceAge <= targetAge) {
            reset();
            return;
        }
        if (excess_ && clock > previousClock_) {
            const double recovered = previousAge_ - sourceAge;
            const double interval = clock - previousClock_;
            rebase_ = recovered <= 1.e-7 ||
                (sourceAge - targetAge) * interval >
                    recoveryWindow * recovered + 1.e-7;
        }
        excess_ = true;
        previousClock_ = clock;
        previousAge_ = sourceAge;
    }

    static constexpr double targetAge = .100;
    static constexpr double recoveryWindow = .200;

private:
    double previousClock_ = 0, previousAge_ = 0;
    bool excess_ = false, rebase_ = false;
};

} // namespace nekorox::component
