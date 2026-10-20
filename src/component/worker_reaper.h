#pragma once

#include <cstddef>

namespace nekorox::component {

class WorkerReaper final {
public:
    using Cleanup = void (*)(void*) noexcept;
    static WorkerReaper& instance();

    bool start();
    bool enqueue(void* context, Cleanup cleanup) noexcept;
    void shutdownAndDrain() noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;

private:
    WorkerReaper() = default;
    ~WorkerReaper();
    WorkerReaper(const WorkerReaper&) = delete;
    WorkerReaper& operator=(const WorkerReaper&) = delete;
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace nekorox::component
