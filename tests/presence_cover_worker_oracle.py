"""Compile the complete production cover worker with deterministic host seams.

This is a worker oracle, not a player integration test. It extracts the whole
DiscordPresenceService::coverWorkerMain body from the current source, substitutes
only clock reads, and provides a scripted cover service plus predicate-aware
waits. It performs no network or player/SDK calls.

Usage: python tests/presence_cover_worker_oracle.py
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/component/discord_presence_service.cpp"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    body = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    i = body
    while i < len(source):
        char = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state == "string":
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                state = "code"
        elif state == "char":
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == "'":
                state = "code"
        elif char == "/" and nxt == "/":
            state = "line_comment"
            i += 1
        elif char == "/" and nxt == "*":
            state = "block_comment"
            i += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1
    raise RuntimeError(f"Could not find the end of {signature}")


def verify_pipe_disconnect_handoff(source: str) -> tuple[str, str]:
    worker = extract_function(
        source,
        "void DiscordPresenceService::workerMain(std::stop_token stopToken)",
    )
    pipe_close = worker.rfind("CloseHandle(pipe);")
    if pipe_close < 0:
        raise RuntimeError("workerMain has no per-connection CloseHandle(pipe)")
    handoff_start = worker.rfind(
        "rpcReady_.store(false, std::memory_order_release);", 0, pipe_close)
    if handoff_start < 0:
        raise RuntimeError("workerMain pipe-close handoff has no rpcReady reset")
    close_call = "CloseHandle(pipe);"
    per_pipe_close_segment = worker[handoff_start:pipe_close + len(close_call)]
    if "coverService_.cancel()" in per_pipe_close_segment:
        raise RuntimeError("per-pipe RPC disconnect still cancels admitted cover work")
    if "changed_.notify_all();" not in per_pipe_close_segment:
        raise RuntimeError("workerMain pipe-close handoff no longer wakes cover waiters")
    shutdown_cancel = worker.find("coverService_.cancel();", pipe_close)
    if shutdown_cancel < 0:
        raise RuntimeError("workerMain no longer cancels cover work at final shutdown")
    return worker, per_pipe_close_segment


FIXTURE_PREFIX = r'''#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
int GetCurrentThread() { return 0; }
bool SetThreadPriority(int, int) { return true; }
constexpr int THREAD_PRIORITY_BELOW_NORMAL = 0;

namespace nekorox::component {

enum class PresenceCoverProvider {unconfigured,catbox,privateRelay,uguu,imgbb,catalogue,automatic};
struct DiscordCoverMetadata { std::string artist,album,releaseId; };
enum class DiscordCoverFailure : std::uint8_t {
    none = 0, upload, origin, delivery, noArtwork, recoveryLimit,
};
enum class DiscordCoverStatus { disabled, idle, resolving, fallback, ready };
enum class DiscordRelayStatus { disconnected, connected };
enum class DiscordPresenceStatus { disabled };
struct DiscordRelayTrace {
    DiscordRelayStatus status = DiscordRelayStatus::disconnected;
    std::uint64_t connectionGeneration = 0;
};
struct FakeTrackHandle {
    bool valid = true;
    bool is_valid() const noexcept { return valid; }
};
struct DiscordPresenceUpdate {
    std::uint64_t sessionGeneration = 1;
    std::uint64_t coverEpoch = 1;
    std::uint64_t trackKey = 11;
    std::uint64_t presenceRevision = 1;
    bool allowCover = true;
    bool enabled = true;
    bool started = true;
};
struct DiscordPresenceUpdateV3 {
    DiscordPresenceUpdate base{};
    std::uint64_t trackGeneration = 0;
    std::uint64_t deliveryRevision = 0;
    std::string coverToken;
};
struct DiscordTrackPresence {
    DiscordPresenceUpdate update{};
    struct { std::array<char,256> artist{},album{}; } playback;
    std::string albumArtist,musicBrainzReleaseId;
    std::string coverUrl;
    std::string coverToken;
    std::int64_t relayExpiresAtMilliseconds = 0;
    std::uint64_t relayConnectionGeneration = 0;
    FakeTrackHandle trackHandle{};
    DiscordPresenceUpdateV3 updateV3{};
};
struct DiscordCoverResult {
    enum class Source : std::uint8_t { none = 0, cache, upload, relay };
    std::string url;
    std::string token;
    std::string error;
    std::uint32_t artworkMilliseconds = 0;
    std::uint32_t encodeMilliseconds = 0;
    std::uint32_t uploadMilliseconds = 0;
    std::uint32_t verificationMilliseconds = 0;
    std::int64_t expiresAtMilliseconds = 0;
    std::uint64_t connectionGeneration = 0;
    std::uint64_t contentHash = 0;
    std::size_t encodedBytes = 0;
    Source source = Source::none;
    PresenceCoverProvider provider=PresenceCoverProvider::catbox;
    DiscordCoverFailure failure = DiscordCoverFailure::none;
    std::uint32_t retryAfterMilliseconds = 0;
    bool retryWithNewEncoding = false;
    bool succeeded() const noexcept { return !url.empty(); }
};

struct DiscordPresenceService;
using WorkerFixture = DiscordPresenceService;
struct FakeChanged {
    explicit FakeChanged(WorkerFixture* value) : owner(value) {}
    WorkerFixture* owner;
    void notify_all() noexcept {}
    template<class Lock, class Predicate>
    bool wait(Lock& lock, std::stop_token token, Predicate predicate);
    template<class Lock, class Rep, class Period, class Predicate>
    bool wait_for(Lock& lock, std::stop_token token,
        std::chrono::duration<Rep, Period> duration, Predicate predicate);
    template<class Lock, class Clock, class Duration, class Predicate>
    bool wait_until(Lock& lock, std::stop_token token,
        std::chrono::time_point<Clock, Duration> deadline, Predicate predicate);
};

std::atomic<bool> g_presenceEnabled{true};
std::atomic<bool> g_presenceTemporaryCovers{true};
void qaPresenceLog(const std::string&) {}

struct ScriptedCoverService {
    explicit ScriptedCoverService(WorkerFixture* value) : owner(value) {}
    WorkerFixture* owner;
    std::deque<DiscordCoverResult> results;
    std::uint64_t admissionTicket() const noexcept { return 7; }
    DiscordRelayTrace relayTrace() const noexcept { return {}; }
    DiscordCoverResult resolve(const FakeTrackHandle&, std::uint64_t trackKey,
        std::stop_token token, std::uint64_t admission, const DiscordCoverMetadata&);
};

struct WaitRecord {
    std::string kind;
    std::int64_t beforeMs = 0;
    std::int64_t afterMs = 0;
    std::int64_t deadlineMs = 0;
    std::size_t calls = 0;
};

struct DiscordPresenceService {
    DiscordPresenceService();
    void run() { coverWorkerMain(source_.get_token()); }
    void onWait(const std::string& kind, std::int64_t deadlineMs);
    std::chrono::steady_clock::time_point steadyNow{};
    std::chrono::system_clock::time_point systemNow{};
    std::vector<WaitRecord> waits;
    std::vector<std::uint64_t> resolvedTrackKeys;
    std::function<void(std::size_t, std::uint64_t)> resolveHook;
    std::function<void(const std::string&, std::int64_t)> waitHook;
    std::size_t calls = 0;
    std::mutex mutex_;
    FakeChanged changed_;
    DiscordTrackPresence pending_{};
    std::uint64_t pendingRevision_ = 1;
    std::uint64_t sentRevision_ = 0;
    std::atomic<std::uint64_t> staleCoverDiscards_{0};
    bool ephemeralRelay_ = false;
    bool started_ = true;
    bool drainPending_ = false;
    std::atomic<bool> rpcReady_{true};
    std::atomic<DiscordCoverStatus> coverStatus_{DiscordCoverStatus::idle};
    std::atomic<DiscordCoverFailure> coverFailure_{DiscordCoverFailure::none};
    std::atomic<bool> coverRetryPending_{false};
    std::atomic<PresenceCoverProvider> deliveredCoverProvider_{PresenceCoverProvider::unconfigured};
    ScriptedCoverService coverService_{this};
    std::stop_source source_;
    std::chrono::steady_clock::time_point fakeSteadyNow() const { return steadyNow; }
    std::chrono::system_clock::time_point fakeSystemNow() const { return systemNow; }
    bool ephemeralRelayEnabled() const noexcept { return ephemeralRelay_; }
    void coverWorkerMain(std::stop_token stopToken);
};

WorkerFixture* activeFixture = nullptr;
inline std::chrono::steady_clock::time_point fakeSteadyNow() {
    return activeFixture->fakeSteadyNow();
}
inline std::chrono::system_clock::time_point fakeSystemNow() {
    return activeFixture->fakeSystemNow();
}

inline DiscordPresenceService::DiscordPresenceService() : changed_(this) {
    activeFixture = this;
    pending_.update.enabled = true;
    pending_.update.started = true;
    pending_.update.allowCover = true;
    pending_.update.trackKey = 11;
    pending_.trackHandle.valid = true;
    g_presenceEnabled.store(true);
    g_presenceTemporaryCovers.store(true);
}

inline void WorkerFixture::onWait(const std::string& kind, std::int64_t deadlineMs) {
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        steadyNow.time_since_epoch()).count();
    waits.push_back({kind, before, before, deadlineMs, calls});
    if (waitHook) waitHook(kind, deadlineMs);
    waits.back().afterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        steadyNow.time_since_epoch()).count();
}

inline DiscordCoverResult ScriptedCoverService::resolve(
        const FakeTrackHandle&, std::uint64_t trackKey, std::stop_token,
        std::uint64_t, const DiscordCoverMetadata&) {
    ++owner->calls;
    owner->resolvedTrackKeys.push_back(trackKey);
    if (owner->resolveHook) owner->resolveHook(owner->calls, trackKey);
    if (results.empty()) {
        DiscordCoverResult exhausted;
        exhausted.error = "fixture result script exhausted";
        exhausted.failure = DiscordCoverFailure::origin;
        return exhausted;
    }
    auto result = results.front();
    results.pop_front();
    return result;
}

template<class Lock, class Predicate>
inline bool FakeChanged::wait(Lock&, std::stop_token token, Predicate predicate) {
    if (predicate() || token.stop_requested()) return predicate();
    owner->onWait("indefinite", -1);
    if (predicate()) return true;
    owner->source_.request_stop();
    return false;
}

template<class Lock, class Rep, class Period, class Predicate>
inline bool FakeChanged::wait_for(Lock&, std::stop_token token,
        std::chrono::duration<Rep, Period> duration, Predicate predicate) {
    if (predicate() || token.stop_requested()) return predicate();
    const auto target = owner->steadyNow + duration;
    const auto targetMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        target.time_since_epoch()).count();
    owner->onWait("for", targetMs);
    if (token.stop_requested()) return false;
    if (predicate()) return true;
    if (owner->steadyNow < target) owner->steadyNow = target;
    owner->waits.back().afterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        owner->steadyNow.time_since_epoch()).count();
    return predicate();
}

template<class Lock, class Clock, class Duration, class Predicate>
inline bool FakeChanged::wait_until(Lock&, std::stop_token token,
        std::chrono::time_point<Clock, Duration> deadline, Predicate predicate) {
    if (predicate() || token.stop_requested()) return predicate();
    const auto deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline.time_since_epoch()).count();
    owner->onWait("until", deadlineMs);
    if (token.stop_requested()) return false;
    if (predicate()) return true;
    if (!token.stop_requested()) {
        if constexpr (std::is_same_v<Clock, std::chrono::steady_clock>) {
            if (owner->steadyNow < deadline)
                owner->steadyNow = std::chrono::steady_clock::time_point(deadline.time_since_epoch());
        } else if constexpr (std::is_same_v<Clock, std::chrono::system_clock>) {
            if (owner->systemNow < deadline)
                owner->systemNow = std::chrono::system_clock::time_point(deadline.time_since_epoch());
        }
    }
    owner->waits.back().afterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        owner->steadyNow.time_since_epoch()).count();
    if (predicate()) return true;
    // A missed/spurious wait cannot spin forever in this fixture. The harness
    // requests a stop and reports the wait for diagnosis.
    owner->source_.request_stop();
    return false;
}

static int assertions = 0;
static void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
static DiscordCoverResult failed(std::uint32_t hintMs = 0,
        DiscordCoverFailure failure = DiscordCoverFailure::origin) {
    DiscordCoverResult result;
    result.error = "scripted cover failure";
    result.failure = failure;
    result.retryAfterMilliseconds = hintMs;
    return result;
}
static DiscordCoverResult succeeded(const std::string& url = "https://cdn.invalid/cover.png") {
    DiscordCoverResult result;
    result.url = url;
    result.token = "fixture-token";
    result.source = DiscordCoverResult::Source::upload;
    return result;
}
static void queueFailures(WorkerFixture& fixture, unsigned count,
        std::uint32_t hintMs, DiscordCoverFailure failure = DiscordCoverFailure::origin) {
    for (unsigned i = 0; i < count; ++i)
        fixture.coverService_.results.push_back(failed(hintMs, failure));
}
static void requirePromptDeferredInterruption(const WorkerFixture& fixture) {
    std::vector<std::int64_t> positiveRetryDelays;
    bool deferredWakeWasImmediate = false;
    for (const auto& wait : fixture.waits) {
        if (wait.kind != "until") continue;
        const auto elapsed = wait.afterMs - wait.beforeMs;
        if (elapsed > 0) positiveRetryDelays.push_back(elapsed);
        if (wait.calls == 4 && elapsed == 0 &&
                wait.deadlineMs - wait.beforeMs >= 600000)
            deferredWakeWasImmediate = true;
        require(elapsed < 600000,
            "interruption must not advance the fake clock through the grace period");
    }
    require(positiveRetryDelays == std::vector<std::int64_t>({1000, 5000, 30000}),
        "only the original 1/5/30 second retry waits elapse before interruption");
    require(deferredWakeWasImmediate,
        "disable/cancel/track event is rechecked before the deferred deadline advances");
    const auto elapsedTotal = std::chrono::duration_cast<std::chrono::milliseconds>(
        fixture.steadyNow.time_since_epoch()).count();
    require(elapsedTotal < 60000,
        "deferred interruption occurs after normal backoffs and before the grace deadline");
}
'''


FIXTURE_SUFFIX = r'''
} // namespace nekorox::component
using namespace nekorox::component;

static void testHintedRecovery() {
    WorkerFixture f;
    queueFailures(f, 3, 0);
    f.coverService_.results.push_back(failed(600000));
    f.coverService_.results.push_back(succeeded());
    f.run();
    require(f.calls == 5, "four normal failures should lead to one deferred success");
    require(f.pending_.coverUrl == "https://cdn.invalid/cover.png", "deferred success URL retained");
    require(f.coverStatus_.load() == DiscordCoverStatus::ready, "deferred success status");
    bool sawFutureHint = false;
    for (const auto& wait : f.waits) {
        if (wait.kind == "until" && wait.afterMs - wait.beforeMs >= 600000)
            sawFutureHint = true;
    }
    require(sawFutureHint, "10 minute retry hint advances fake time without sleeping");
}

static void testSixAttemptLimit() {
    WorkerFixture f;
    queueFailures(f, 3, 0);
    f.coverService_.results.push_back(failed(600000));
    // The first deferred attempt has already qualified the continuation; a
    // subsequent transient result need not renew the original origin hint.
    f.coverService_.results.push_back(failed(0));
    f.coverService_.results.push_back(failed(0));
    f.run();
    require(f.calls == 6, "persistent hinted origin failure is capped at six resolves");
    require(f.coverRetryPending_.load() == false, "retry pending clears at deferred limit");
    require(f.coverFailure_.load() == DiscordCoverFailure::origin, "terminal origin failure retained");
}

static void testUnhintedRecovery() {
    WorkerFixture f;
    queueFailures(f, 4, 0, DiscordCoverFailure::upload);
    f.coverService_.results.push_back(succeeded());
    f.run();
    require(f.calls == 5, "unhinted transport outage gets a slow recovery attempt");
    require(f.pending_.coverUrl == "https://cdn.invalid/cover.png", "late network success published");
    bool slow = false;
    for (const auto& wait : f.waits)
        if (wait.kind == "until" && wait.calls == 4 && wait.afterMs-wait.beforeMs == 120000) slow = true;
    require(slow, "unhinted first recovery waits two minutes");
}

static void testUnhintedLimitAndReconnect() {
    WorkerFixture f;
    queueFailures(f, 6, 0, DiscordCoverFailure::delivery);
    bool readyWake = false;
    f.waitHook = [&](const std::string& kind, std::int64_t) {
        if (kind == "indefinite" && f.calls == 6 && !readyWake) {
            readyWake = true;
            f.rpcReady_.store(true);
            ++f.pending_.update.presenceRevision;
            ++f.pendingRevision_;
        }
    };
    f.run();
    require(readyWake && f.calls == 6, "READY/metadata cannot replenish exhausted transport budget");
    require(!f.coverRetryPending_.load(), "sixth failure clears recovery timer");
    bool longWait = false;
    for (const auto& wait : f.waits)
        if (wait.kind == "until" && wait.calls == 5 && wait.afterMs-wait.beforeMs == 600000) longWait = true;
    require(longWait, "second recovery waits ten minutes instead of polling");
}

static void testTerminalFailuresDoNotRetry() {
    for (const auto failure : {DiscordCoverFailure::noArtwork, DiscordCoverFailure::recoveryLimit,
            DiscordCoverFailure::none}) {
        WorkerFixture f;
        f.coverService_.results.push_back(failed(600000, failure));
        bool metadata = false;
        f.waitHook = [&](const std::string& kind, std::int64_t) {
            if (kind == "indefinite" && f.calls == 1 && !metadata) {
                metadata = true;
                ++f.pending_.update.presenceRevision;
                ++f.pendingRevision_;
            }
        };
        f.run();
        require(f.calls == 1, "terminal/artwork/cancelled failure cannot acquire network retries");
        require(!f.coverRetryPending_.load(), "terminal failure leaves no recovery timer");
    }
}

static void testRelayBudgetUnchanged() {
    for (bool renewing : {false, true}) {
        WorkerFixture f;
        f.ephemeralRelay_ = true;
        if (renewing) {
            f.pending_.coverUrl = "https://relay.invalid/still-valid.png";
            f.pending_.coverToken = "relay-token";
            f.pending_.relayExpiresAtMilliseconds = 10000;
        }
        queueFailures(f, 4, 0, DiscordCoverFailure::upload);
        f.run();
        require(f.calls == 4, "relay registration/renewal retains the existing four-attempt budget");
        require(!f.coverRetryPending_.load(), "public slow recovery cannot schedule a relay renewal loop");
    }
}

static void testMetadataDoesNotResetBudgetOrDeadline() {
    WorkerFixture f;
    queueFailures(f, 3, 0);
    f.coverService_.results.push_back(failed(600000));
    f.coverService_.results.push_back(succeeded());
    bool changed = false;
    f.waitHook = [&](const std::string& kind, std::int64_t) {
        if (kind == "until" && f.calls == 4 && !changed) {
            changed = true;
            ++f.pending_.update.presenceRevision;
            ++f.pendingRevision_;
        }
    };
    f.run();
    require(changed, "metadata refinement was injected during deferred wait");
    require(f.calls == 5, "metadata revision does not reset request budget");
    require(f.resolvedTrackKeys == std::vector<std::uint64_t>({11, 11, 11, 11, 11}),
        "metadata refinement stays on the same cover key");
    bool waitedFullHint = false;
    for (const auto& wait : f.waits) {
        if (wait.kind == "until" && wait.afterMs - wait.beforeMs >= 600000)
            waitedFullHint = true;
    }
    require(waitedFullHint, "metadata refinement preserves the future deferred deadline");
}

static void testDisableInterruptsDeferredWait() {
    WorkerFixture f;
    queueFailures(f, 3, 0);
    f.coverService_.results.push_back(failed(600000));
    bool disabled = false;
    f.waitHook = [&](const std::string& kind, std::int64_t) {
        if (kind == "until" && f.calls == 4 && !disabled) {
            disabled = true;
            g_presenceTemporaryCovers.store(false);
        } else if (kind == "indefinite") {
            f.source_.request_stop();
        }
    };
    f.run();
    require(disabled && f.calls == 4, "disabling covers interrupts deferred retry");
    require(f.pending_.coverUrl.empty(), "disable leaves no deferred URL");
    requirePromptDeferredInterruption(f);
}

static void testCancellationInterruptsDeferredWait() {
    WorkerFixture f;
    queueFailures(f, 3, 0);
    f.coverService_.results.push_back(failed(600000));
    bool cancelled = false;
    f.waitHook = [&](const std::string& kind, std::int64_t) {
        if (kind == "until" && f.calls == 4 && !cancelled) {
            cancelled = true;
            f.source_.request_stop();
        }
    };
    f.run();
    require(cancelled && f.calls == 4, "stop token cancels deferred wait immediately");
    require(f.pending_.coverUrl.empty(), "cancelled wait cannot publish a URL");
    requirePromptDeferredInterruption(f);
}

static void testTrackChangeInterruptsDeferredWait() {
    WorkerFixture f;
    queueFailures(f, 3, 0);
    f.coverService_.results.push_back(failed(600000));
    f.coverService_.results.push_back(succeeded("https://cdn.invalid/new-track.png"));
    bool changed = false;
    f.waitHook = [&](const std::string& kind, std::int64_t) {
        if (kind == "until" && f.calls == 4 && !changed) {
            changed = true;
            f.pending_.update.trackKey = 22;
            ++f.pending_.update.coverEpoch;
            ++f.pendingRevision_;
        }
    };
    f.run();
    require(changed && f.calls == 5, "track change interrupts deferred wait and starts new request");
    require(f.resolvedTrackKeys.back() == 22, "replacement cover belongs to the new track");
    require(f.pending_.coverUrl == "https://cdn.invalid/new-track.png", "new-track URL accepted");
    requirePromptDeferredInterruption(f);
}

static void testDisconnectSuccessIsRetained() {
    WorkerFixture f;
    f.coverService_.results.push_back(succeeded("https://cdn.invalid/disconnected.png"));
    f.resolveHook = [&](std::size_t, std::uint64_t) { f.rpcReady_.store(false); };
    f.run();
    require(f.pending_.coverUrl == "https://cdn.invalid/disconnected.png",
        "same-generation success is retained while RPC is disconnected");
    require(f.coverStatus_.load() == DiscordCoverStatus::ready, "disconnected success is ready for replay");
}

static void testStaleSuccessRejected(bool stopHandle) {
    WorkerFixture f;
    f.coverService_.results.push_back(succeeded("https://cdn.invalid/stale.png"));
    f.resolveHook = [&](std::size_t, std::uint64_t) {
        if (stopHandle) {
            f.pending_.trackHandle.valid = false;
        } else {
            f.pending_.update.allowCover = false;
        }
        ++f.pendingRevision_;
    };
    f.run();
    require(f.pending_.coverUrl.empty(), stopHandle
        ? "stopped handle rejects stale URL" : "disabled cover policy rejects stale URL");
    require(f.staleCoverDiscards_.load() == 1, "stale success is counted as discarded");
}

static void testTemporaryCoverRenewal() {
    WorkerFixture f;
    f.pending_.coverUrl="https://cdn.invalid/temporary.png";
    f.pending_.relayExpiresAtMilliseconds=180000;
    auto replacement=succeeded("https://cdn.invalid/replacement.png");replacement.token.clear();
    replacement.expiresAtMilliseconds=720000;replacement.provider=PresenceCoverProvider::uguu;
    f.coverService_.results.push_back(replacement);
    f.waitHook=[&](const std::string& kind,std::int64_t deadline) {
        if(kind=="until" && f.calls==1) f.source_.request_stop();
    };
    f.run();
    require(f.calls==1,"temporary URL renews once, not a busy loop");
    require(f.pending_.coverUrl==replacement.url && f.pending_.relayExpiresAtMilliseconds==720000,
        "temporary renewal retains replacement and its real expiry");
    bool sawDeadline=false;
    for(const auto& wait:f.waits) if(wait.kind=="until"&&wait.calls==0) {
        require(wait.deadlineMs==120000,"temporary cover renews one minute before safe expiry");sawDeadline=true;
    }
    require(sawDeadline,"temporary renewal needs an expiry timer, not metadata polling");
    require(f.deliveredCoverProvider_==PresenceCoverProvider::uguu,"delivery status identifies fallback provider");
}
static void testTerminalTemporaryExpiry() {
    WorkerFixture f;
    f.pending_.coverUrl="https://cdn.invalid/temporary.png";
    f.pending_.relayExpiresAtMilliseconds=180000;
    f.coverService_.results.push_back(failed(0,DiscordCoverFailure::noArtwork));
    f.run();
    require(f.calls==1,"terminal renewal does not grant new attempts");
    require(f.pending_.coverUrl.empty(),"terminal renewal still clears URL at its expiration");
    require(f.pending_.update.allowCover && g_presenceEnabled,"expiry does not disable text Presence");
}

static void testTemporaryRenewalFailureBounded() {
    WorkerFixture f;
    f.pending_.coverUrl="https://cdn.invalid/temporary.png";
    f.pending_.relayExpiresAtMilliseconds=180000;
    queueFailures(f,8,0);
    f.waitHook=[&](const std::string& kind,std::int64_t deadline) {
        if(kind=="until" && f.calls>0) {
            const auto delta=std::chrono::milliseconds(deadline)-f.steadyNow.time_since_epoch();
            if(delta.count()>0) f.systemNow+=std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
        }
    };
    f.run();
    require(f.calls==6,"renewal outage obeys six-attempt public budget");
    require(f.pending_.coverUrl.empty(),"expired public URL cleared after failed renewal");
    require(f.pending_.update.allowCover && g_presenceEnabled,"failed renewal keeps text Presence enabled");
}

int main() {
    try {
            testTerminalTemporaryExpiry();
        testTemporaryCoverRenewal();
        testTemporaryRenewalFailureBounded();
        testHintedRecovery();
        testSixAttemptLimit();
        testUnhintedRecovery();
        testUnhintedLimitAndReconnect();
        testTerminalFailuresDoNotRetry();
        testRelayBudgetUnchanged();
        testMetadataDoesNotResetBudgetOrDeadline();
        testDisableInterruptsDeferredWait();
        testCancellationInterruptsDeferredWait();
        testTrackChangeInterruptsDeferredWait();
        testDisconnectSuccessIsRetained();
        testStaleSuccessRejected(true);
        testStaleSuccessRejected(false);
        std::cout << "PASS assertions=" << assertions << " scenarios=16\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL assertions=" << assertions << " error=" << error.what() << '\n';
        return 1;
    }
}
'''


def main() -> int:
    source_text = SOURCE.read_text(encoding="utf-8-sig")
    worker_main, disconnect_segment = verify_pipe_disconnect_handoff(source_text)
    actual_function = extract_function(
        source_text,
        "void DiscordPresenceService::coverWorkerMain(std::stop_token stopToken)",
    )
    instrumented = actual_function
    seam_counts = {
        "steady_clock_now": instrumented.count("std::chrono::steady_clock::now()"),
        "system_clock_now": instrumented.count("std::chrono::system_clock::now()"),
    }
    if not all(seam_counts.values()):
        raise RuntimeError(f"Expected clock seams are missing: {seam_counts}")
    instrumented = instrumented.replace(
        "std::chrono::steady_clock::now()", "fakeSteadyNow()")
    instrumented = instrumented.replace(
        "std::chrono::system_clock::now()", "fakeSystemNow()")

    compiler_setting = os.environ.get("CXX", "")
    compiler = (
        [part.strip('"') for part in shlex.split(
            compiler_setting, posix=os.name != "nt")]
        if compiler_setting else None
    )
    if compiler:
        compiler_path = shutil.which(compiler[0])
        if not compiler_path:
            raise RuntimeError(f"CXX compiler not found: {compiler[0]}")
        compiler[0] = compiler_path
    else:
        selected = (shutil.which("cl") or shutil.which("g++") or
                    shutil.which("clang++") or shutil.which("clang-cl"))
        if not selected:
            raise RuntimeError("A C++20 compiler (cl, g++, clang++, or clang-cl) is required")
        compiler = [selected]
    compiler_name = Path(compiler[0]).name.lower()
    msvc_style = compiler_name in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}

    seams = {
        "sourceExtraction": "complete coverWorkerMain method with lexical brace matching",
        "rpcReconnect": "actual workerMain pipe-close segment verified to omit coverService cancellation",
        "clock": "fake steady/system clock; source clock reads rewritten only at call sites",
        "waits": "predicate-aware wait_for/wait_until/wait with bounded scripted events",
        "coverService": "scripted deterministic results; no HTTP, relay, SDK, player, or disk access",
        "policy": "atomic presence flags and mutable pending track/revision fixture",
    }
    generated = FIXTURE_PREFIX + "\n" + instrumented + "\n" + FIXTURE_SUFFIX
    with tempfile.TemporaryDirectory(prefix="goden-cover-worker-oracle-") as tmp:
        tmpdir = Path(tmp)
        cpp = tmpdir / "cover_worker_oracle.cpp"
        exe = tmpdir / ("cover_worker_oracle.exe" if os.name == "nt" else "cover_worker_oracle")
        cpp.write_text(generated, encoding="utf-8", newline="\n")
        if msvc_style:
            command = compiler + [
                "/std:c++20", "/EHsc", "/nologo", "/W4", "/utf-8",
                f"/Fe:{exe}", f"/Fo:{tmpdir}{os.sep}", str(cpp),
            ]
        else:
            command = compiler + [
                "-std=c++20", "-O0", "-Wall", "-Wextra", str(cpp), "-o", str(exe),
            ]
        built = subprocess.run(command, text=True, capture_output=True, check=False)
        if built.returncode:
            sys.stderr.write(built.stdout)
            sys.stderr.write(built.stderr)
            return built.returncode
        completed = subprocess.run([str(exe)], text=True, capture_output=True, check=False)
        if completed.returncode:
            sys.stderr.write(completed.stdout)
            sys.stderr.write(completed.stderr)
            return completed.returncode

    report = {
        "schema": "goden.presence-cover-worker-oracle.v1",
        "result": "PASS",
        "workerSha256": hashlib.sha256(actual_function.encode("utf-8")).hexdigest(),
        "instrumentedWorkerSha256": hashlib.sha256(instrumented.encode("utf-8")).hexdigest(),
        "seams": seams,
        "seamsSha256": hashlib.sha256(FIXTURE_PREFIX.encode("utf-8")).hexdigest(),
        "seamDescriptionSha256": hashlib.sha256(
            json.dumps(seams, sort_keys=True).encode()).hexdigest(),
        "compiler": Path(compiler[0]).name,
        "clockSeamCallCounts": seam_counts,
        "workerMainSha256": hashlib.sha256(worker_main.encode("utf-8")).hexdigest(),
        "rpcPipeDisconnectSegmentSha256": hashlib.sha256(
            disconnect_segment.encode("utf-8")).hexdigest(),
        "rpcReconnectContract": "PASS",
        "fullPlayerTested": False,
        "realNetwork": False,
        "results": completed.stdout.strip().splitlines(),
    }
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
