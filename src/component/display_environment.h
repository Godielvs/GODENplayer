#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace nekorox::component {

struct DisplayEnvironmentSnapshotV1 {
    HMONITOR monitor = nullptr;
    LUID adapterLuid{};
    std::uint32_t dpiX = 96;
    std::uint32_t dpiY = 96;
    std::uint32_t refreshNumerator = 120;
    std::uint32_t refreshDenominator = 1;
    std::uint64_t topologyGeneration = 1;
    std::uint64_t windowPresentationRevision = 0;
    std::uint64_t rootLifetimeToken = 0;
    bool visible = false;
    bool minimized = false;
    bool occluded = false;
    bool suspended = false;
    bool remoteSession = false;

    [[nodiscard]] double refreshHz() const noexcept;
    [[nodiscard]] std::uint32_t refreshMilliHz() const noexcept;
};

struct WindowPresentationIdentityV1 {
    std::uint64_t topologyGeneration = 1;
    std::uint64_t windowPresentationRevision = 0;
    std::uint64_t rootLifetimeToken = 0;
};

// Central process-wide display topology source. It listens for topology,
// power/session and top-level window-location changes; consumers still poll at
// a low fallback cadence so a missed shell notification cannot strand a root.
class DisplayEnvironmentService final {
public:
    static DisplayEnvironmentService& instance() noexcept;

    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] DisplayEnvironmentSnapshotV1 snapshot(HWND window) noexcept;
    [[nodiscard]] std::uint64_t topologyGeneration() const noexcept;
    [[nodiscard]] WindowPresentationIdentityV1 windowIdentity(
        HWND window) const noexcept;
    [[nodiscard]] bool suspended() const noexcept;
    // Cheap presentation gate. Unlike snapshot(), this never enumerates the
    // display topology and is safe to call from every retained surface.
    [[nodiscard]] bool presentationVisible(HWND window) const noexcept;
    [[nodiscard]] std::uint64_t displayConfigQueryCount() const noexcept
    { return displayConfigQueries_.load(std::memory_order_relaxed); }
    // Only explicitly registered product roots participate. Native menus,
    // tooltips and combo popups are intentionally ignored.
    void registerPersistentRoot(HWND window) noexcept;
    void unregisterPersistentRoot(HWND window) noexcept;
    bool notifyWindowChanged(
        HWND window = nullptr, bool* presentationChanged = nullptr) noexcept;

private:
    DisplayEnvironmentService() = default;
    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    static void CALLBACK locationHook(
        HWINEVENTHOOK hook, DWORD event, HWND window, LONG object,
        LONG child, DWORD thread, DWORD milliseconds) noexcept;
    void onEnvironmentMessage(UINT message, WPARAM wParam) noexcept;

    mutable std::mutex lifecycleMutex_;
    HWND messageWindow_ = nullptr;
    HWINEVENTHOOK locationHook_ = nullptr;
    HWINEVENTHOOK visibilityHook_ = nullptr;
    struct WindowIdentity {
        HWND root = nullptr;
        HMONITOR monitor = nullptr;
        UINT dpi = 96;
        std::uint64_t presentationRevision = 1;
        std::uint64_t lifetimeToken = 0;
    };
    struct DisplayPathCache {
        HMONITOR monitor = nullptr;
        std::uint64_t topologyGeneration = 0;
        LUID adapterLuid{};
        std::uint32_t refreshNumerator = 120;
        std::uint32_t refreshDenominator = 1;
    };
    mutable std::mutex identityMutex_;
    std::vector<WindowIdentity> identities_;
    std::vector<DisplayPathCache> displayPathCache_;
    std::atomic<std::uint64_t> topologyGeneration_{1};
    std::atomic<std::uint64_t> nextRootLifetimeToken_{1};
    std::atomic<bool> suspended_{false};
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> displayConfigQueries_{0};
};

} // namespace nekorox::component
