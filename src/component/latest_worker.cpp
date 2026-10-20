#include "latest_worker.h"

namespace nekorox::component {

LatestWorker::~LatestWorker()
{
    shutdownAndDrain();
}

bool LatestWorker::submit(Task task) noexcept
{
    if (!task) return false;
    std::unique_lock lock(mutex_);
    if (stopping_) return false;
    activeStop_.request_stop();
    pending_ = std::move(task);
    if (!worker_.joinable()) {
        try {
            worker_ = std::jthread([this] { run(); });
        } catch (...) {
            pending_.reset();
            return false;
        }
    }
    lock.unlock();
    changed_.notify_one();
    return true;
}

void LatestWorker::cancelPending() noexcept
{
    std::lock_guard lock(mutex_);
    activeStop_.request_stop();
    pending_.reset();
}

void LatestWorker::shutdownAndDrain() noexcept
{
    std::jthread worker;
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && !worker_.joinable()) return;
        stopping_ = true;
        activeStop_.request_stop();
        pending_.reset();
        worker = std::move(worker_);
    }
    changed_.notify_all();
    if (worker.joinable()) worker.join();
}

void LatestWorker::run() noexcept
{
    for (;;) {
        Task task;
        std::stop_token token;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            task = std::move(*pending_);
            pending_.reset();
            activeStop_ = std::stop_source{};
            token = activeStop_.get_token();
        }
        try {
            task(token);
        } catch (...) {
            // Domain tasks publish their own error states. The worker must
            // remain available for the next latest-wins request.
        }
    }
}

} // namespace nekorox::component
