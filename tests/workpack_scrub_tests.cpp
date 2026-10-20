#include "../src/component/scrub_coordinator.h"

#include <cmath>
#include <stdexcept>
#include <vector>
#include <windows.h>

namespace {

void requireScrub(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class FakeClock final : public nekorox::component::ScrubClock {
public:
    double nowSeconds() const noexcept override { return now; }
    double now = 0.0;
};

class FakePlayback final : public nekorox::component::ScrubPlaybackAdapter {
public:
    bool isPlaying() const noexcept override { return playing; }
    bool isPaused() const noexcept override { return paused; }
    bool canSeek() const noexcept override { return seekable; }
    double durationSeconds() const noexcept override { return duration; }
    double positionSeconds() const noexcept override { return position; }
    void seek(double seconds) noexcept override { seeks.push_back(seconds); }
    void pause(bool value) noexcept override
    {
        pauses.push_back(value);
        paused = value;
    }

    bool playing = true;
    bool paused = false;
    bool seekable = true;
    double duration = 100.0;
    double position = 12.0;
    std::vector<double> seeks;
    std::vector<bool> pauses;
};

struct QaSession {
    QaSession(FakePlayback& playback, FakeClock& clock)
    {
        SetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", L"1");
        SetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", L"1");
        requireScrub(nekorox::component::ScrubCoordinator::instance()
                .installQaHooks(&playback, &clock) == S_OK,
            "WP16 QA adapter was rejected");
    }
    ~QaSession()
    {
        nekorox::component::ScrubCoordinator::instance().resetQaHooks();
        SetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", nullptr);
        SetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", nullptr);
    }
};

} // namespace

void runWorkpackScrubTests()
{
    using nekorox::component::ScrubCoordinator;
    using State = nekorox::component::ScrubSnapshot::State;

    FakePlayback playback;
    FakeClock clock;
    QaSession qa(playback, clock);
    auto& scrub = ScrubCoordinator::instance();

    auto session = scrub.begin(0.20);
    requireScrub(session != 0, "WP16 could not begin selection");
    for (int move = 0; move < 1000; ++move) {
        clock.now += 0.001;
        scrub.update(session, static_cast<double>(move % 1000) / 999.0);
        scrub.pump();
    }
    clock.now += 0.5;
    scrub.pump();
    requireScrub(playback.seeks.empty() && playback.pauses.empty(),
        "WP16 pointer movement or stationary hold touched transport");
    scrub.end(session, 0.73);
    requireScrub(playback.seeks.size() == 1 &&
            std::abs(playback.seeks.front() - 73.0) < 1e-9 &&
            playback.pauses.empty() &&
            scrub.snapshot().state == State::commitPending,
        "WP16 release did not emit exactly one nominal seek");
    scrub.end(session, 0.40);
    requireScrub(playback.seeks.size() == 1,
        "WP16 duplicate release duplicated the commit");
    scrub.notifyBackendSeek(12.0);
    requireScrub(scrub.snapshot().state == State::commitPending,
        "WP16 stale callback completed a newer target");
    scrub.notifyBackendSeek(73.0);
    requireScrub(!scrub.active() &&
            !scrub.owns(session) &&
            scrub.snapshot().state == State::idle &&
            !ScrubCoordinator::audioSmoothingActive(),
        "WP16 acknowledged commit stayed latched");

    playback.paused = true;
    playback.seeks.clear();
    session = scrub.begin(0.5);
    requireScrub(session != 0, "WP16 paused selection could not begin");
    scrub.update(session, 0.6);
    scrub.cancel(session);
    requireScrub(playback.seeks.empty() && playback.pauses.empty(),
        "WP16 cancel restored transport by seeking or pausing");

    session = scrub.begin(0.4);
    requireScrub(session != 0, "WP16 paused commit could not begin");
    scrub.end(session, 0.4);
    requireScrub(playback.seeks.size() == 1 && playback.pauses.empty() &&
            playback.paused,
        "WP16 changed paused transport during commit");
    scrub.notifyBackendSeek(40.0);

    playback.paused = false;
    playback.seeks.clear();
    playback.position = 5.0;
    session = scrub.begin(0.8);
    requireScrub(session != 0, "WP16 timeout selection could not begin");
    scrub.end(session, 0.8);
    for (const double now : {clock.now + 0.11, clock.now + 0.76,
             clock.now + 1.60}) {
        clock.now = now;
        scrub.pump();
    }
    const auto failed = scrub.snapshot();
    requireScrub(playback.seeks.size() == 2,
        "WP16 exceeded or missed its single bounded retry");
    requireScrub(!failed.active && failed.commitFailed &&
            std::abs(failed.visualSeconds - playback.position) < 1e-9,
        "WP16 timeout did not expose failure and real position");

    playback.seeks.clear();
    session = scrub.begin(0.2);
    requireScrub(session != 0, "WP16 revoke selection could not begin");
    scrub.end(session, 0.2);
    scrub.abandon();
    clock.now += 2.0;
    scrub.notifyBackendSeek(20.0);
    scrub.pump();
    requireScrub(playback.seeks.size() == 1 && !scrub.active(),
        "WP16 revoked track/stop commit resumed after abandon");

    playback.seeks.clear();
    const auto oldOwner = scrub.begin(0.3);
    const auto newOwner = scrub.begin(0.6);
    requireScrub(oldOwner != 0 && newOwner != 0 && oldOwner != newOwner &&
            !scrub.owns(oldOwner) && scrub.owns(newOwner),
        "scrub ownership did not change with a new surface session");
    // SetCapture can synchronously notify the old surface after begin() on
    // the new one. None of the old surface's delayed operations may win.
    for (const auto stale : {oldOwner, std::uint64_t{0}}) {
        scrub.update(stale, 0.9);
        scrub.end(stale, 0.9);
        scrub.commitCurrent(stale);
        scrub.cancel(stale);
    }
    requireScrub(scrub.owns(newOwner) && playback.seeks.empty() &&
            std::abs(scrub.snapshot().visualSeconds - 60.0) < 1e-9,
        "stale surface mutated or cancelled the new scrub");
    scrub.update(newOwner, 0.65);
    scrub.end(newOwner, 0.65);
    scrub.cancel(oldOwner);
    requireScrub(scrub.owns(newOwner) && playback.seeks.size() == 1 &&
            std::abs(playback.seeks.front() - 65.0) < 1e-9 &&
            ScrubCoordinator::audioSmoothingActive(),
        "pending seek lost ownership on pointer release");
    scrub.cancel(newOwner);
    scrub.notifyBackendSeek(65.0);
    scrub.pump();
    requireScrub(!scrub.active() && !scrub.owns(newOwner) &&
            !ScrubCoordinator::audioSmoothingActive() && playback.seeks.size() == 1,
        "owner teardown kept a pending commit or smoothing flag alive");
    session = scrub.begin(0.4);
    playback.seekable = false;
    requireScrub(scrub.begin(0.2) == 0 && scrub.owns(session),
        "failed begin revoked the existing owner");
    scrub.abandon();
    requireScrub(!scrub.owns(session), "track abandonment retained scrub ownership");
    requireScrub(scrub.begin(0.2) == 0, "failed scrub begin returned an owner token");
}
