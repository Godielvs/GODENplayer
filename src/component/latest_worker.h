#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace nekorox::component {

// One persistent worker with a single latest-wins pending slot. Replacing a
// request cancels the active token and never joins a thread on the caller.
class LatestWorker final {
public:
    using Task = std::function<void(std::stop_token)>;

    LatestWorker() = default;
    ~LatestWorker();
    LatestWorker(const LatestWorker&) = delete;
    LatestWorker& operator=(const LatestWorker&) = delete;

    [[nodiscard]] bool submit(Task task) noexcept;
    void cancelPending() noexcept;
    void shutdownAndDrain() noexcept;

private:
    void run() noexcept;

    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Task> pending_;
    std::stop_source activeStop_;
    std::jthread worker_;
    bool stopping_ = false;
};

} // namespace nekorox::component
