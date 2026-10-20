#pragma once

#include <atomic>
#include <cstdint>

namespace nekorox::component {

class ScrubPlaybackAdapter {
public:
    virtual ~ScrubPlaybackAdapter() = default;
    [[nodiscard]] virtual bool isPlaying() const noexcept = 0;
    [[nodiscard]] virtual bool isPaused() const noexcept = 0;
    [[nodiscard]] virtual bool canSeek() const noexcept = 0;
    [[nodiscard]] virtual double durationSeconds() const noexcept = 0;
    [[nodiscard]] virtual double positionSeconds() const noexcept = 0;
    virtual void seek(double seconds) noexcept = 0;
    virtual void pause(bool paused) noexcept = 0;
};

class ScrubClock {
public:
    virtual ~ScrubClock() = default;
    [[nodiscard]] virtual double nowSeconds() const noexcept = 0;
};

struct ScrubSnapshot {
    enum class State : std::uint8_t {
        idle,
        selecting,
        commitPending,
    };

    bool active = false;
    bool auditioning = false;
    bool commitFailed = false;
    bool auditionUnavailable = false;
    State state = State::idle;
    double visualSeconds = 0.0;
    double audibleSeconds = 0.0;
    double durationSeconds = 0.0;
    std::uint64_t generation = 0;
};

class ScrubCoordinator final {
public:
    static ScrubCoordinator& instance();

    using SessionToken = std::uint64_t;
    [[nodiscard]] SessionToken begin(double normalizedPosition, bool auditionRequested = false);
    void update(SessionToken session, double normalizedPosition);
    void pump();
    void end(SessionToken session, double normalizedPosition);
    void commitCurrent(SessionToken session);
    void cancel(SessionToken session);
    [[nodiscard]] bool owns(SessionToken session) const noexcept;
    // Playback stop/start callbacks already retire the old transport and may
    // not call pause(). Other cancellation paths restore the preview lease.
    void abandon(bool restoreAuditionPause = true) noexcept;
    void notifyBackendSeek(double seconds) noexcept;

    [[nodiscard]] ScrubSnapshot snapshot() const noexcept;
    [[nodiscard]] double effectivePosition(double backendSeconds) const noexcept;
    [[nodiscard]] bool active() const noexcept;

    // Lock-free state consumed by the optional scrub smoother DSP.
    [[nodiscard]] static bool audioSmoothingActive() noexcept;
    [[nodiscard]] static std::uint64_t audioSmoothingGeneration() noexcept;
    long installQaHooks(
        ScrubPlaybackAdapter* playback,
        ScrubClock* clock) noexcept;
    void resetQaHooks() noexcept;

private:
    ScrubCoordinator() = default;
    void pumpCommit(double now);
    void completeCommit(bool failed = false) noexcept;

    std::atomic<bool> publishedActive_{false};
    std::atomic<bool> publishedAuditioning_{false};
    std::atomic<bool> publishedCommitFailed_{false};
    std::atomic<bool> publishedAuditionUnavailable_{false};
    std::atomic<ScrubSnapshot::State> publishedState_{
        ScrubSnapshot::State::idle};
    std::atomic<double> publishedVisualSeconds_{0.0};
    std::atomic<double> publishedAudibleSeconds_{0.0};
    std::atomic<double> publishedDurationSeconds_{0.0};
    std::atomic<std::uint64_t> publishedGeneration_{0};

    bool sessionActive_ = false;
    bool originallyPlaying_ = false;
    bool originallyPaused_ = false;
    bool auditionRequested_ = false;
    bool auditionPausedMain_ = false;
    double durationSeconds_ = 0.0;
    double visualNormalized_ = 0.0;
    std::uint64_t generation_ = 0;
    bool commitPending_ = false;
    double commitTargetSeconds_ = 0.0;
    double commitStartedSeconds_ = 0.0;
    double commitLastSeekSeconds_ = 0.0;
    unsigned commitAttempts_ = 0;
    std::uint64_t lastPreviewCursor_ = 0;
    std::uint64_t previewAcceptedAtLease_ = 0;
    double lastPreviewProgressSeconds_ = 0.0;
};

} // namespace nekorox::component
