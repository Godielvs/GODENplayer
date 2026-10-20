#pragma once

#include "sdk_compat.h"
#include "latest_worker.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nekorox::component {

enum class LibraryRootCoverage : std::uint8_t { observedOnly };

struct LibraryRootEntry {
    std::filesystem::path path;
    bool online = false;
    bool policyAddable = false;
    std::wstring origin = L"Observed from library items";
};

struct LibraryRootsSnapshot {
    std::uint64_t configEpoch = 0;
    LibraryRootCoverage coverage = LibraryRootCoverage::observedOnly;
    bool libraryEnabled = false;
    std::vector<LibraryRootEntry> roots;
    std::filesystem::path configuredDestination;
    std::string configuredPattern;
    std::string error;
};

class LibraryRootsProvider final : private library_callback_v2_dynamic {
public:
    using Completion = std::function<void(
        std::shared_ptr<const LibraryRootsSnapshot>)>;

    static LibraryRootsProvider& instance();
    ~LibraryRootsProvider();

    std::shared_ptr<const LibraryRootsSnapshot> snapshot() const;
    // Main-thread demand. Hidden editors do not subscribe or scan.
    void observe(Completion completion);
    void suspend();
    void refresh();
    void shutdownAndDrain() noexcept;
    void openLibraryPreferences();

private:
    LibraryRootsProvider();
    void schedule();
    void beginRefresh();
    void finishRefresh(std::uint64_t generation,
        std::shared_ptr<LibraryRootsSnapshot> next);
    void on_items_added(metadb_handle_list_cref) override;
    void on_items_removed(metadb_handle_list_cref) override;
    void on_items_modified_v2(
        metadb_handle_list_cref, metadb_io_callback_v2_data&) override {}
    void on_library_initialized() override;

    mutable std::mutex mutex_;
    std::shared_ptr<const LibraryRootsSnapshot> snapshot_ =
        std::make_shared<LibraryRootsSnapshot>();
    LatestWorker worker_;
    std::atomic<std::uint64_t> generation_{0};
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    Completion observer_;
    bool registered_ = false;
    bool dirty_ = true;
    bool posted_ = false;
    bool busy_ = false;
    bool stopping_ = false;
#ifdef NEKOROX_LIBRARY_ROOTS_TEST
    friend struct LibraryRootsTestPeer;
#endif
};

} // namespace nekorox::component
