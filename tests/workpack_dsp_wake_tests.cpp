#include <atomic>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace {

struct HandleOwner {
    HANDLE value = nullptr;
    ~HandleOwner() { if (value != nullptr) CloseHandle(value); }
};

void requireWait(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

void runWorkpackDspWakeTests()
{
    HandleOwner retained{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    requireWait(retained.value != nullptr, "WP07 auto-reset event creation failed");

    // A signal issued before the waiter exists must remain observable.
    requireWait(SetEvent(retained.value) != FALSE, "WP07 retained signal failed");
    std::atomic<DWORD> result{WAIT_FAILED};
    std::thread waiter([&] {
        result.store(WaitForSingleObject(retained.value, 250),
            std::memory_order_release);
    });
    waiter.join();
    requireWait(result.load(std::memory_order_acquire) == WAIT_OBJECT_0,
        "WP07 signal-before-wait was lost");

    // Auto-reset coalesces redundant notifications into one bounded wake.
    requireWait(SetEvent(retained.value) != FALSE &&
            SetEvent(retained.value) != FALSE,
        "WP07 coalesced signal failed");
    requireWait(WaitForSingleObject(retained.value, 0) == WAIT_OBJECT_0,
        "WP07 coalesced wake was not observable");
    requireWait(WaitForSingleObject(retained.value, 0) == WAIT_TIMEOUT,
        "WP07 auto-reset event produced an unbounded second wake");

    // The same retained transition is sufficient to stop an idle worker.
    std::atomic<bool> stopped{false};
    std::thread idle([&] {
        if (WaitForSingleObject(retained.value, INFINITE) == WAIT_OBJECT_0)
            stopped.store(true, std::memory_order_release);
    });
    requireWait(SetEvent(retained.value) != FALSE, "WP07 stop wake failed");
    idle.join();
    requireWait(stopped.load(std::memory_order_acquire),
        "WP07 idle worker did not stop deterministically");
}
