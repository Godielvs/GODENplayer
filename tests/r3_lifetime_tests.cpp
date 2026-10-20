#include "latest_worker.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

bool waitFor(const std::atomic_int& value, int expected)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

void runR3LifetimeTests()
{
    using nekorox::component::LatestWorker;
    LatestWorker worker;
    std::atomic_int state{0};
    std::atomic_bool release{false};
    std::atomic_bool cancellationObserved{false};
    check(worker.submit([&](std::stop_token stop) {
        state.store(1, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        cancellationObserved.store(stop.stop_requested(),
            std::memory_order_release);
    }), "DATA07 persistent worker could not start");
    check(waitFor(state, 1), "DATA07 worker did not start its active request");

    for (int revision = 2; revision <= 32; ++revision) {
        check(worker.submit([&, revision](std::stop_token) {
            state.store(revision, std::memory_order_release);
        }), "DATA07 latest-wins submit failed");
    }
    release.store(true, std::memory_order_release);
    check(waitFor(state, 32),
        "DATA07 latest-wins queue executed stale pending work");
    worker.shutdownAndDrain();
    check(cancellationObserved.load(std::memory_order_acquire),
        "DATA07 replacement did not cancel the active request token");
    check(!worker.submit([](std::stop_token) {}),
        "DATA07 worker accepted work after shutdown");
}
