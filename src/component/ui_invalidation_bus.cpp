#include "ui_invalidation_bus.h"
#include "performance_telemetry.h"
#include "rollout_flags.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace nekorox::component {
namespace {

struct WindowEntry {
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<std::uint32_t> dirty{0};
    std::atomic<bool> posted{false};
    std::atomic<bool> active{true};
};

using EntryPtr = std::shared_ptr<WindowEntry>;
using EntryList = std::vector<EntryPtr>;

constexpr std::size_t surfaceIndex(UiInvalidationSurface surface) noexcept
{
    return static_cast<std::size_t>(surface);
}

TelemetrySurface telemetrySurface(UiInvalidationSurface surface) noexcept
{
    switch (surface) {
    case UiInvalidationSurface::workspace: return TelemetrySurface::workspace;
    case UiInvalidationSurface::waveSeek: return TelemetrySurface::waveSeek;
    case UiInvalidationSurface::listen: return TelemetrySurface::listen;
    case UiInvalidationSurface::masterDsp: return TelemetrySurface::workspace;
    default: return TelemetrySurface::headerMonitor;
    }
}

} // namespace

struct UiInvalidationBus::Impl {
    std::mutex mutationMutex;
    std::array<std::atomic<std::shared_ptr<const EntryList>>,
        surfaceIndex(UiInvalidationSurface::count)> registries;

    Impl()
    {
        for (auto& registry : registries) {
            registry.store(
                std::make_shared<const EntryList>(),
                std::memory_order_release);
        }
    }
};

UiInvalidationBus::UiInvalidationBus()
    : impl_(new Impl())
{
}

UiInvalidationBus::~UiInvalidationBus()
{
    delete impl_;
}

UiInvalidationBus& UiInvalidationBus::instance()
{
    static UiInvalidationBus bus;
    return bus;
}

void UiInvalidationBus::registerWindow(
    UiInvalidationSurface surface,
    HWND window,
    UINT message)
{
    if (window == nullptr || message == 0 ||
        surface == UiInvalidationSurface::count) {
        return;
    }
    const auto index = surfaceIndex(surface);
    std::lock_guard lock(impl_->mutationMutex);
    const auto current = impl_->registries[index].load(
        std::memory_order_acquire);
    auto next = std::make_shared<EntryList>(
        current ? *current : EntryList{});
    for (auto iterator = next->begin(); iterator != next->end(); ++iterator) {
        if ((*iterator)->window != window) continue;
        if ((*iterator)->message == message &&
            (*iterator)->active.load(std::memory_order_acquire)) return;
        (*iterator)->active.store(false, std::memory_order_release);
        next->erase(iterator);
        break;
    }
    auto entry = std::make_shared<WindowEntry>();
    entry->window = window;
    entry->message = message;
    next->push_back(std::move(entry));
    impl_->registries[index].store(
        std::const_pointer_cast<const EntryList>(next),
        std::memory_order_release);
}

void UiInvalidationBus::unregisterWindow(
    UiInvalidationSurface surface,
    HWND window) noexcept
{
    if (window == nullptr || surface == UiInvalidationSurface::count) return;
    const auto index = surfaceIndex(surface);
    std::lock_guard lock(impl_->mutationMutex);
    const auto current = impl_->registries[index].load(
        std::memory_order_acquire);
    if (!current || current->empty()) return;
    auto next = std::make_shared<EntryList>();
    next->reserve(current->size());
    for (const auto& entry : *current) {
        if (entry->window != window) {
            next->push_back(entry);
        } else {
            entry->active.store(false, std::memory_order_release);
        }
    }
    impl_->registries[index].store(
        std::const_pointer_cast<const EntryList>(next),
        std::memory_order_release);
}

void UiInvalidationBus::publish(
    UiInvalidationSurface surface,
    UiDirty dirty) noexcept
{
    if (!any(dirty) || surface == UiInvalidationSurface::count) return;
    const auto registry = impl_->registries[surfaceIndex(surface)].load(
        std::memory_order_acquire);
    if (!registry) return;
    const auto bits = static_cast<std::uint32_t>(dirty);
    const auto coalesce = rolloutFlagEnabled(RolloutFlag::uiDirtyBusV1);
    for (const auto& entry : *registry) {
        if (!entry->active.load(std::memory_order_acquire)) continue;
        entry->dirty.fetch_or(bits, std::memory_order_acq_rel);
        const auto alreadyPosted = entry->posted.exchange(
            true, std::memory_order_acq_rel);
        if ((!coalesce || !alreadyPosted) && IsWindow(entry->window)) {
            if (!PostMessageW(entry->window, entry->message, 0, 0)) {
                // Payload and post ownership are independent: retain every
                // dirty bit and permit any later publisher to retry the post.
                entry->posted.store(false, std::memory_order_release);
            } else {
                PerformanceTelemetry::instance().event(
                    nekorox::PerformanceEventKindV2::post,
                    telemetrySurface(surface), bits,
                    reinterpret_cast<std::uintptr_t>(entry->window));
            }
        } else if (!coalesce) {
            entry->posted.store(alreadyPosted, std::memory_order_release);
        }
    }
}

UiDirty UiInvalidationBus::consume(
    UiInvalidationSurface surface,
    HWND window) noexcept
{
    if (window == nullptr || surface == UiInvalidationSurface::count) {
        return UiDirty::none;
    }
    const auto registry = impl_->registries[surfaceIndex(surface)].load(
        std::memory_order_acquire);
    if (!registry) return UiDirty::none;
    for (const auto& entry : *registry) {
        if (entry->window == window) {
            const auto dirty = entry->dirty.exchange(
                0, std::memory_order_acq_rel);
            entry->posted.store(false, std::memory_order_release);
            // Close the exchange/post race: a publisher that arrived between
            // those two operations saw posted=true and intentionally did not
            // enqueue. Re-arm it now without clearing its payload.
            if (entry->active.load(std::memory_order_acquire) &&
                entry->dirty.load(std::memory_order_acquire) != 0U &&
                !entry->posted.exchange(true, std::memory_order_acq_rel)) {
                if (!PostMessageW(entry->window, entry->message, 0, 0)) {
                    entry->posted.store(false, std::memory_order_release);
                }
            }
            PerformanceTelemetry::instance().event(
                nekorox::PerformanceEventKindV2::consume,
                telemetrySurface(surface), dirty,
                reinterpret_cast<std::uintptr_t>(window));
            return static_cast<UiDirty>(dirty);
        }
    }
    return UiDirty::none;
}

} // namespace nekorox::component
