// Album-mode TRACKS / LYRICS wheel scroll contract.
//
// The two Album lists each own a ScrollController and drive it the same way:
// one impulse per WM_MOUSEWHEEL and one advanceTo() per presented frame, with
// setBounds()/setResponseScale() re-applied on every event. These tests pin the
// consumption rules that make a wheel burst one continuous movement:
//
//   * no valid delta is lost, at any cadence, including deltas below WHEEL_DELTA
//   * the target changes on the same input cycle, even while animating
//   * a continuing gesture carries the approved input lead into the rendered
//     offset, so the movement happens while the wheel turns instead of after it
//   * the lead cannot be multiplied by input rate, duplicate timestamps or
//     refresh rate
//   * the first delta of a gesture and any direction reversal never move the
//     rendered offset inside input handling
//   * bounds clamp without overshoot and with no residual velocity
//   * the two lists share no state

#include "nekorox/scroll_controller.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

// Album TRACKS geometry: 38 DIP header, 28 DIP rows, 24 tracks.
constexpr double kTrackRowHeight = 28.0;
constexpr double kTrackHeader = 38.0;
constexpr double kTrackViewport = 306.0;
constexpr double kTrackMaximum =
    kTrackRowHeight * 24.0 - kTrackViewport;
// Album LYRICS geometry: a long document in a short viewport.
constexpr double kLyricMaximum = 5927.349;
// One WM_MOUSEWHEEL notch with GET_WHEEL_DELTA_WPARAM == 120.
constexpr double kNotch = 56.0;

struct Harness {
    nekorox::ScrollController scroll;
    double maximum = 0.0;
    double position = 0.0;
    double previousPosition = 0.0;
    double maximumFrameStep = 0.0;
    double postBurstTravel = 0.0;
    int rejected = 0;

    Harness(double bound, double responseScale = 1.0)
        : maximum(bound), responseScale_(responseScale)
    {
        scroll.setBounds(0.0, bound);
        scroll.setPosition(0.0);
        scroll.setResponseScale(responseScale);
    }

    void frame(double nowSeconds)
    {
        scroll.setBounds(0.0, maximum);
        scroll.setResponseScale(responseScale_);
        scroll.advanceTo(nowSeconds);
        maximumFrameStep = std::max(maximumFrameStep,
            std::abs(scroll.position() - previousPosition));
        previousPosition = scroll.position();
    }

    // Returns false when the controller rejected the delta outright.
    bool impulse(double dip, double nowSeconds)
    {
        scroll.setBounds(0.0, maximum);
        scroll.setResponseScale(responseScale_);
        const auto accepted = scroll.addWheelImpulseAt(dip, nowSeconds);
        if (!accepted) ++rejected;
        return accepted;
    }

    // Impulses every eventPeriod, frames every framePeriod, interleaved by
    // absolute time exactly as the shell message loop and frame clock do.
    double burst(double frameHz, double eventHz, int events, double dip)
    {
        startBurst();
        const auto framePeriod = 1.0 / frameHz;
        const auto eventPeriod = 1.0 / eventHz;
        auto nextFrame = framePeriod;
        auto nextEvent = eventPeriod;
        for (int index = 0; index < events; ++index) {
            while (nextFrame < nextEvent) {
                frame(nextFrame);
                nextFrame += framePeriod;
            }
            impulse(dip, nextEvent);
            nextEvent += eventPeriod;
        }
        lastEventSeconds_ = nextEvent - eventPeriod;
        endPosition_ = scroll.position();
        return scroll.target() - scroll.position();
    }

    void startBurst()
    {
        position = scroll.position();
        previousPosition = scroll.position();
        maximumFrameStep = 0.0;
    }

    // Runs the frame clock until the motion settles and reports how far the
    // list still travelled after the last impulse.
    void settle(double frameHz, double nowSeconds)
    {
        const auto framePeriod = 1.0 / frameHz;
        for (int index = 0; index < 4000 && scroll.active(); ++index) {
            frame(nowSeconds + index * framePeriod);
        }
        postBurstTravel = std::abs(scroll.position() - endPosition_);
    }

private:
    double responseScale_ = 1.0;
    double lastEventSeconds_ = 0.0;
    double endPosition_ = 0.0;
};

// Mirrors the shell paint path: reconcileAlbumTrackScroll() runs once per
// presented frame and re-applies bounds and response scale.
double driveTracks(double frameHz, double eventHz, int notches)
{
    Harness harness(kTrackMaximum);
    harness.burst(frameHz, eventHz, notches, kNotch);
    return harness.scroll.target();
}

} // namespace

void runAlbumScrollTests()
{
    using nekorox::ScrollController;

    // 1. Every valid delta is consumed: 8 and 12 rapid impulses in the same
    //    direction land exactly on the accumulated target, at both refresh
    //    rates and at a real-wheel cadence.
    for (const auto frameHz : {60.0, 120.0, 165.0}) {
        for (const auto eventHz : {8.0, 20.0, 60.0, 125.0, 250.0, 500.0}) {
            for (const auto notches : {8, 12}) {
                Harness harness(kLyricMaximum);
                const auto lag = harness.burst(
                    frameHz, eventHz, notches, kNotch);
                check(harness.rejected == 0,
                    "album scroll rejected a valid wheel delta");
                check(std::abs(harness.scroll.target() -
                        notches * kNotch) < 0.001,
                    "album scroll lost part of a wheel burst");
                check(lag >= 0.0 && lag < notches * kNotch,
                    "album scroll target escaped its input");
            }
        }
    }

    // 2. The target moves on the same input cycle even while the previous
    //    animation is still being presented.
    {
        Harness harness(kLyricMaximum);
        harness.impulse(kNotch, 0.010);
        for (int index = 1; index <= 4; ++index) harness.frame(index / 165.0);
        check(harness.scroll.active(),
            "album scroll settled before the continuation delta");
        const auto before = harness.scroll.target();
        harness.impulse(kNotch, 5.0 / 165.0);
        check(std::abs(harness.scroll.target() - (before + kNotch)) < 0.001,
            "album scroll deferred a delta until the animation settled");
    }

    // 3. A continuing gesture advances the rendered offset in the same frame
    //    cycle instead of queueing the delta behind the running animation.
    //    Without the lead the rendered offset trails the accumulated target by
    //    most of the burst and only catches up after the input stops.
    {
        Harness harness(kLyricMaximum);
        harness.burst(165.0, 125.0, 10, kNotch);
        const auto lag = harness.scroll.target() - harness.scroll.position();
        check(lag < 0.45 * 10.0 * kNotch,
            "fast wheel burst left the rendered offset trailing the target");
        harness.settle(165.0, 11.0 / 125.0);
        check(harness.postBurstTravel < 0.45 * 10.0 * kNotch,
            "most of a wheel burst was still rendered after the input stopped");
        check(std::abs(harness.scroll.position() -
                harness.scroll.target()) < 0.001,
            "album scroll did not settle on its logical target");
    }

    // 4. Denser input may not inflate the per-frame step: the lead is applied
    //    at most once per presented frame.
    {
        Harness once(kLyricMaximum);
        once.burst(60.0, 125.0, 10, kNotch);
        const auto onceStep = once.maximumFrameStep;
        Harness dense(kLyricMaximum);
        dense.burst(60.0, 1000.0, 10, kNotch);
        check(dense.maximumFrameStep <= onceStep + 0.001,
            "input rate multiplied the rendered step");
        once.settle(60.0, 11.0 / 125.0);
        check(once.maximumFrameStep < 6.0 * kNotch,
            "album scroll rendered a single-frame jump");
    }

    // 5. The first delta of a gesture still moves only the modelled target:
    //    input handling never invents an un-presented visual offset, and the
    //    first response cannot depend on how long the list sat idle.
    {
        ScrollController idle;
        ScrollController busy;
        for (auto* controller : {&idle, &busy}) {
            controller->setBounds(0.0, kLyricMaximum);
            controller->setPosition(500.0);
            controller->setResponseScale(1.0);
        }
        check(idle.addWheelImpulseAt(kNotch, 2.0),
            "first album delta was rejected after an idle period");
        check(busy.addWheelImpulseAt(kNotch, 1.0 / 165.0),
            "first album delta was rejected at the nominal cadence");
        check(idle.position() == 500.0 && busy.position() == 500.0,
            "first album delta moved the rendered offset before a frame");
        check(std::abs(idle.position() - busy.position()) < 0.001,
            "first album response depends on the preceding idle duration");
    }

    // 6. Duplicate timestamps cannot multiply the lead: N fractional deltas at
    //    one instant must equal one whole notch. High-resolution wheels and
    //    touchpads deliver exactly this shape.
    {
        ScrollController whole;
        ScrollController fractions;
        for (auto* controller : {&whole, &fractions}) {
            controller->setBounds(0.0, kLyricMaximum);
            controller->setPosition(500.0);
            controller->setResponseScale(1.0);
        }
        whole.addWheelImpulseAt(84.0, 10.0);
        for (int index = 0; index < 4; ++index) {
            fractions.addWheelImpulseAt(21.0, 10.0);
        }
        check(whole.target() == fractions.target(),
            "fractional album deltas diverged on the logical target");
        check(whole.position() == fractions.position(),
            "fractional album deltas diverged on the rendered offset");
    }

    // 7. Reversal responds on the input cycle and on the next frame, and never
    //    teleports the rendered offset backwards inside input handling.
    {
        Harness harness(kLyricMaximum);
        for (int index = 0; index < 3; ++index) {
            harness.impulse(kNotch, 0.008 + index * 0.008);
            harness.frame(0.008 + index * 0.008 + 0.006);
        }
        const auto beforeReversal = harness.scroll.position();
        check(harness.impulse(-kNotch, 0.040),
            "album reversal was rejected");
        check(harness.scroll.target() < beforeReversal,
            "album reversal did not move the logical target");
        check(harness.scroll.position() >= beforeReversal,
            "album reversal teleported the rendered offset inside input handling");
        const auto atReversal = harness.scroll.position();
        harness.frame(0.046);
        check(harness.scroll.position() < atReversal,
            "album reversal did not respond on the next frame");
    }

    // 8. Alternating bursts never overshoot their bounds and never leave
    //    residual velocity pressing against a limit; the input loop stays
    //    strictly time-ordered.
    {
        Harness harness(kTrackMaximum);
        auto now = 0.0;
        for (int index = 0; index < 200; ++index) {
            now += 0.004;
            harness.frame(now);
            harness.impulse(index % 2 == 0 ? kNotch : -kNotch, now);
            check(harness.scroll.position() >= 0.0 &&
                    harness.scroll.position() <= kTrackMaximum,
                "album scroll escaped the tracklist bounds");
            check(harness.scroll.target() >= 0.0 &&
                    harness.scroll.target() <= kTrackMaximum,
                "album scroll target escaped the tracklist bounds");
        }
        check(harness.scroll.velocity() > -2.0,
            "album scroll kept inward velocity at the top bound");
    }

    // 9. Both bounds, driven by a real burst, clamp exactly and settle with no
    //    residual velocity or overshoot.
    {
        Harness down(kTrackMaximum);
        down.burst(165.0, 125.0, 40, kNotch);
        down.settle(165.0, 41.0 / 125.0);
        check(std::abs(down.scroll.position() - kTrackMaximum) < 0.001 &&
                std::abs(down.scroll.target() - kTrackMaximum) < 0.001,
            "album scroll burst did not clamp at the bottom bound");
        check(down.scroll.velocity() == 0.0,
            "album scroll kept outward velocity at the bottom bound");
        Harness up(kTrackMaximum);
        up.scroll.setPosition(kTrackMaximum);
        up.burst(165.0, 125.0, 40, -kNotch);
        up.settle(165.0, 41.0 / 125.0);
        check(std::abs(up.scroll.position()) < 0.001 &&
                std::abs(up.scroll.target()) < 0.001,
            "album scroll burst did not clamp at the top bound");
        check(up.scroll.velocity() == 0.0,
            "album scroll kept outward velocity at the top bound");
    }

    // 10. High-resolution wheels: deltas below WHEEL_DELTA accumulate in the
    //     same proportion as whole notches.
    {
        Harness fine(kLyricMaximum);
        fine.burst(165.0, 250.0, 40, kNotch / 4.0);
        check(std::abs(fine.scroll.target() - 10.0 * kNotch) < 0.001,
            "sub-notch album deltas lost distance");
        Harness coarse(kLyricMaximum);
        coarse.burst(165.0, 250.0, 10, kNotch);
        check(std::abs(fine.scroll.target() - coarse.scroll.target()) < 0.001,
            "sub-notch album deltas diverged from whole notches");
        check(fine.rejected == 0 && coarse.rejected == 0,
            "album scroll rejected a valid high-resolution delta");
    }

    // 11. Reduced motion keeps the same consumption guarantees with a shorter,
    //     still smooth trajectory; motion off snaps every delta.
    {
        Harness reduced(kLyricMaximum, 0.55);
        reduced.burst(165.0, 125.0, 10, kNotch);
        check(std::abs(reduced.scroll.target() - 10.0 * kNotch) < 0.001,
            "reduced-motion album scroll lost a delta");
        check(reduced.scroll.active(),
            "reduced-motion album scroll did not retain short settling");
        reduced.settle(165.0, 11.0 / 125.0);
        check(std::abs(reduced.scroll.position() - 10.0 * kNotch) < 0.001,
            "reduced-motion album scroll settled away from its target");
        Harness off(kLyricMaximum, 0.0);
        off.burst(165.0, 125.0, 10, kNotch);
        check(!off.scroll.active() &&
                std::abs(off.scroll.position() - 10.0 * kNotch) < 0.001,
            "motion-off album scroll did not snap to its target");
    }

    // 12. TRACKS and LYRICS own independent controllers: driving one list must
    //     not move the other, and a session change resets only its own list.
    {
        Harness tracks(kTrackMaximum);
        Harness lyrics(kLyricMaximum);
        tracks.burst(165.0, 125.0, 4, kNotch);
        check(tracks.scroll.target() > 0.0,
            "TRACKS burst did not move the tracklist");
        check(lyrics.scroll.position() == 0.0 && lyrics.scroll.target() == 0.0,
            "TRACKS input moved the LYRICS list");
        lyrics.burst(165.0, 125.0, 4, kNotch);
        const auto tracksAfter = tracks.scroll.target();
        check(lyrics.scroll.target() > 0.0,
            "LYRICS burst did not move the lyric list");
        check(tracks.scroll.target() == tracksAfter,
            "LYRICS input moved the TRACKS list");
        check(tracks.scroll.target() <= kTrackMaximum &&
                lyrics.scroll.target() <= kLyricMaximum,
            "album lists used each other's bounds");
    }

    // 13. The track viewport clamps a burst that exceeds the album length, and
    //     a shrink during motion discards outward state instead of overshooting.
    {
        check(driveTracks(165.0, 125.0, 40) == kTrackMaximum,
            "TRACKS burst did not clamp to the tracklist maximum");
        Harness harness(kTrackMaximum);
        harness.burst(165.0, 125.0, 30, kNotch);
        check(harness.scroll.target() == kTrackMaximum,
            "TRACKS burst exceeded the tracklist maximum");
        harness.scroll.setBounds(0.0, kTrackMaximum * 0.25);
        check(harness.scroll.position() <= kTrackMaximum * 0.25 + 0.001 &&
                harness.scroll.target() <= kTrackMaximum * 0.25 + 0.001,
            "TRACKS resize retained an offset beyond the new maximum");
        check(harness.scroll.velocity() <= 0.0,
            "TRACKS resize retained outward velocity");
    }

    // 14. A hostile resize during motion, applied between frames, never yields
    //     a non-finite or out-of-range offset.
    {
        Harness harness(kLyricMaximum);
        harness.burst(165.0, 125.0, 6, kNotch);
        for (int index = 0; index < 120; ++index) {
            const auto bound = index % 3 == 0 ? 0.0
                : index % 3 == 1 ? kLyricMaximum : kLyricMaximum * 0.1;
            harness.scroll.setBounds(0.0, bound);
            harness.frame(0.05 + index / 165.0);
            check(std::isfinite(harness.scroll.position()) &&
                    std::isfinite(harness.scroll.target()),
                "album scroll produced a non-finite offset during resize");
            check(harness.scroll.position() >= 0.0 &&
                    harness.scroll.position() <= bound + 0.001,
                "album scroll escaped its bounds during resize");
        }
    }
}
