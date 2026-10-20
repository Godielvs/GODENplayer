#include "worker_reaper.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace nekorox::component {

struct WorkerReaper::Impl {
    struct Task { void* context = nullptr; Cleanup cleanup = nullptr; };
    static constexpr std::size_t capacity = 8U;
    std::array<Task, capacity> queue{};
    std::size_t read = 0;
    std::size_t count = 0;
    bool stopping = false;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread thread;
    std::atomic<std::size_t> pending{0};

    void run() noexcept
    {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || count != 0U; });
                if (count == 0U && stopping) break;
                task = queue[read];
                queue[read] = {};
                read = (read + 1U) % capacity;
                --count;
            }
            if (task.cleanup != nullptr) task.cleanup(task.context);
            pending.fetch_sub(1U, std::memory_order_release);
        }
    }
};

WorkerReaper& WorkerReaper::instance()
{
    static WorkerReaper service;
    return service;
}

WorkerReaper::~WorkerReaper() { shutdownAndDrain(); }

bool WorkerReaper::start()
{
    if (impl_ != nullptr) return true;
    auto* created = new (std::nothrow) Impl();
    if (created == nullptr) return false;
    try {
        created->thread = std::thread([created] { created->run(); });
    } catch (...) {
        delete created;
        return false;
    }
    impl_ = created;
    return true;
}

bool WorkerReaper::enqueue(void* context, Cleanup cleanup) noexcept
{
    auto* state = impl_;
    if (state == nullptr || context == nullptr || cleanup == nullptr) return false;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping || state->count == Impl::capacity) return false;
        const auto write = (state->read + state->count) % Impl::capacity;
        state->queue[write] = {context, cleanup};
        ++state->count;
        state->pending.fetch_add(1U, std::memory_order_release);
    }
    state->changed.notify_one();
    return true;
}

void WorkerReaper::shutdownAndDrain() noexcept
{
    auto* state = impl_;
    if (state == nullptr) return;
    {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
    }
    state->changed.notify_all();
    if (state->thread.joinable()) state->thread.join();
    impl_ = nullptr;
    delete state;
}

std::size_t WorkerReaper::pending() const noexcept
{
    return impl_ != nullptr
        ? impl_->pending.load(std::memory_order_acquire) : 0U;
}

} // namespace nekorox::component
