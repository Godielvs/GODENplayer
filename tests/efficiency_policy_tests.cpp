#include "../src/component/ui_animation_policy.h"
#include "../src/component/coalesced_resource_cache.h"

#include <atomic>
#include <future>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
void requireEfficiency(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void runEfficiencyPolicyTests() {
    using namespace nekorox::component;
    for (const double hz : {60.0, 90.0, 165.0}) {
        std::set<std::uint64_t> buckets;
        float previous = 0.f;
        for (unsigned frame = 0; frame <= unsigned(hz); ++frame) {
            const auto step = themeSampleStep(frame / hz, 0.0, 0.620);
            requireEfficiency(step.progress >= previous && step.progress <= 1.f,
                "Theme sampling reversed or exceeded its target");
            previous = step.progress;
            buckets.insert(step.bucket);
        }
        requireEfficiency(buckets.size() == 20, "Theme work followed monitor refresh");
    }
    const auto first = themeSampleStep(10.010, 10.0, .620);
    const auto same = themeSampleStep(10.020, 10.0, .620);
    requireEfficiency(first.bucket == same.bucket && first.progress == same.progress,
        "Consumers disagreed on the same pigment sample");
    requireEfficiency(!themeSampleStep(10.620, 10.0, .620).active,
        "Theme failed to deliver its final sample");
    requireEfficiency(!themeSampleStep(.120, 0., .120).active,
        "Reduced-motion theme did not settle");
    for (const double hz : {60.0, 90.0, 165.0, 240.0}) {
        for (unsigned frame = 0; frame < unsigned(hz * .620); ++frame) {
            const auto now = 10.0 + frame / hz;
            const auto next = nextThemeSampleSeconds(now, 10.0, .620);
            requireEfficiency(next > now && next <= 10.620,
                "Theme-only deadline drifted or stalled");
            const auto current = themeSampleStep(now, 10.0, .620);
            const auto due = themeSampleStep(next + 1e-8, 10.0, .620);
            requireEfficiency(!due.active || due.bucket > current.bucket,
                "Theme-only deadline repeats the same pigment sample");
        }
    }
    requireEfficiency(nextThemeSampleSeconds(10.619, 10.0, .620) == 10.620,
        "Theme endpoint waited for a later periodic tick");
    requireEfficiency(nextThemeSampleSeconds(10.620, 10.0, .620) == 0.0 &&
        nextThemeSampleSeconds(10.0, 10.0, 0.0) == 0.0,
        "Settled/reduced motion theme acquired a deadline");
    requireEfficiency(themeSampleStep(9., 10., .620).progress == 0.f,
        "Backward time generated an invalid bucket");
    requireEfficiency(!themeSampleStep(std::numeric_limits<double>::quiet_NaN(), 0., 1.).active,
        "Non-finite time reached the integer clock");
    requireEfficiency(cappedPresentationDeadline(1000, 10000000, 30000) == 334334,
        "Slow content deadline was rounded early");
    requireEfficiency(cappedPresentationDeadline(1000, 10000000, 0) == 0,
        "Native-refresh visual acquired a slow deadline");
    requireEfficiency(displayPresentationDeadline(1000,10000000,0,60606)==61606,
        "Free DXGI credit bypassed the native monitor period");
    requireEfficiency(displayPresentationDeadline(1000,10000000,30000,60606)==334334,
        "Content pacing ignored its slower requested period");
    requireEfficiency(displayPresentationDeadline(1000,10000000,300000,60606)==61606,
        "Continuous rendering exceeded the physical monitor refresh");

    CoalescedResourceCache<int, int, 2> cache;
    auto value = std::make_shared<int>(42);
    std::future<std::shared_ptr<int>> follower;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    {
        auto owner = cache.acquire(1, [] {});
        requireEfficiency(!owner.cached(), "Fresh decode was cached");
        follower = std::async(std::launch::async, [&] {
            bool firstCheck = true;
            auto lease = cache.acquire(1, [&] {
                if (firstCheck) { entered.set_value(); firstCheck = false; }
            });
            return lease.cached();
        });
        enteredFuture.get();
        requireEfficiency(follower.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
            "Concurrent consumer decoded before its owner completed");
        owner.publish(value);
    }
    requireEfficiency(follower.get() == value, "Concurrent consumers did not share pixels");
    requireEfficiency(cache.acquire(1, [] {}).cached() == value, "Live pixel ownership was lost");
    value.reset();
    { auto expired = cache.acquire(1, [] {});
      requireEfficiency(!expired.cached(), "Weak cache retained unaccounted pixels"); }

    std::future<bool> cancelled;
    std::atomic<bool> cancel{false};
    std::promise<void> waiting;
    auto waitingFuture = waiting.get_future();
    {
        auto owner = cache.acquire(2, [] {});
        cancelled = std::async(std::launch::async, [&] {
            bool firstCheck = true;
            try {
                auto lease = cache.acquire(2, [&] {
                    if (firstCheck) { waiting.set_value(); firstCheck = false; }
                    if (cancel.load()) throw std::runtime_error("cancelled");
                });
                return false;
            } catch (const std::runtime_error&) { return true; }
        });
        waitingFuture.get();
        cancel.store(true);
        requireEfficiency(cancelled.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "Cancelled artwork waited for another decoder");
        requireEfficiency(cancelled.get(), "Cancellation was swallowed");
    }
    { auto retry = cache.acquire(2, [] {});
      requireEfficiency(!retry.cached(), "Failed owner left the flight locked"); }

    CoalescedResourceCache<int, int, 1> bounded;
    { auto owner = bounded.acquire(1, [] {});
      bool rejected = false;
      try { auto excess = bounded.acquire(2, [] {}); }
      catch (const std::bad_alloc&) { rejected = true; }
      requireEfficiency(rejected, "Flight metadata exceeded its bound"); }
    { auto next = bounded.acquire(2, [] {});
      requireEfficiency(!next.cached(), "Idle metadata could not be evicted"); }
}
