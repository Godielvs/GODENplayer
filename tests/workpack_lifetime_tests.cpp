#include "../src/component/worker_reaper.h"

#include <atomic>
#include <stdexcept>
#include <thread>

namespace {
struct ReapProbe { std::atomic<unsigned>* completed = nullptr; };
void finishProbe(void* value) noexcept
{
    auto* probe = static_cast<ReapProbe*>(value);
    probe->completed->fetch_add(1U, std::memory_order_release);
    delete probe;
}
}

void runWorkpackLifetimeTests()
{
    auto& reaper = nekorox::component::WorkerReaper::instance();
    if (!reaper.start()) throw std::runtime_error("WP06 reaper did not start");
    std::atomic<unsigned> completed{0};
    for (unsigned index = 0; index < 100U; ++index) {
        auto* probe = new ReapProbe{&completed};
        while (!reaper.enqueue(probe, &finishProbe)) std::this_thread::yield();
    }
    reaper.shutdownAndDrain();
    if (completed.load(std::memory_order_acquire) != 100U ||
        reaper.pending() != 0U) {
        throw std::runtime_error("WP06 shutdown did not drain every owner");
    }
    if (!reaper.start()) throw std::runtime_error("WP06 reaper did not restart");
    reaper.shutdownAndDrain();
}
