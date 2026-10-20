#include "library_roots_provider.h"
#include "text_util.h"
#include <algorithm>
#include <map>
#include <system_error>

namespace nekorox::component {
#ifdef NEKOROX_LIBRARY_ROOTS_TEST
namespace testing {
void captureLibraryRoots(metadb_handle_list&, LibraryRootsSnapshot&);
bool relativeLibraryPath(const metadb_handle_ptr&, pfc::string_base&);
bool libraryRootOnline(const std::filesystem::path&);
bool libraryRootAddable(const char*);
}
#endif
LibraryRootsProvider& LibraryRootsProvider::instance()
{
    static LibraryRootsProvider value;
    return value;
}
LibraryRootsProvider::LibraryRootsProvider() = default;
LibraryRootsProvider::~LibraryRootsProvider() { shutdownAndDrain(); }
std::shared_ptr<const LibraryRootsSnapshot> LibraryRootsProvider::snapshot() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}
void LibraryRootsProvider::observe(Completion completion)
{
    if (stopping_) return;
    if (!completion) { suspend(); return; }
    observer_ = std::move(completion);
    if (registered_) return;
    register_callback();
    registered_ = true;
    // Reopening also rechecks preferences and filesystem availability.
    refresh();
}
void LibraryRootsProvider::suspend()
{
    observer_ = {};
    if (registered_) { unregister_callback(); registered_ = false; }
    ++generation_;
    dirty_ = true;
    // Even a cancelled scan reports completion so busy_ is cleared if the
    // editor reopens before it finishes. Never join on a visibility change.
}
void LibraryRootsProvider::shutdownAndDrain() noexcept
{
    if (stopping_) return;
    stopping_ = true;
    suspend();
    alive_->store(false, std::memory_order_release);
    worker_.shutdownAndDrain();
}
void LibraryRootsProvider::refresh()
{
    if (stopping_) return;
    ++generation_;
    dirty_ = true;
    schedule();
}
void LibraryRootsProvider::schedule()
{
    if (stopping_ || !observer_ || !dirty_ || posted_ || busy_) return;
    posted_ = true;
    try {
        fb2k::inMainThread([this, alive = alive_] {
            if (!alive->load(std::memory_order_acquire)) return;
            posted_ = false;
            if (!stopping_ && observer_ && dirty_ && !busy_) beginRefresh();
        });
    } catch (...) { posted_ = false; throw; }
}
void LibraryRootsProvider::beginRefresh()
{
    const auto generation = generation_.load();
    auto next = std::make_shared<LibraryRootsSnapshot>();
    next->configEpoch = generation;
    dirty_ = false;
    busy_ = true;
    try {
        // Main-only APIs: copy handles once, without visiting their paths.
        // Changes during a scan only mark it stale; they cannot queue N copies.
        auto manager = library_manager::get();
        auto items = std::make_shared<metadb_handle_list>();
#ifdef NEKOROX_LIBRARY_ROOTS_TEST
        testing::captureLibraryRoots(*items, *next);
#else
        next->libraryEnabled = manager->is_library_enabled();
        manager->get_all_items(*items);
        auto managerV3 = library_manager_v3::tryGet();
        if (managerV3.is_valid()) {
            pfc::string8 directory, pattern;
            if (managerV3->get_new_file_pattern_tracks(directory, pattern)) {
                try {
                    next->configuredDestination = utf8ToWide(
                        filesystem::g_get_native_path(directory.c_str()).c_str());
                } catch (...) {
                    next->configuredDestination = utf8ToWide(directory.c_str());
                }
                next->configuredPattern = pattern.c_str();
            }
        }
#endif
        const auto submitted = worker_.submit([this, alive = alive_, next, items,
                manager, generation](std::stop_token stop) mutable {
            const auto cancelled = [&] {
                return stop.stop_requested() || generation != generation_.load();
            };
            try {
                std::map<std::wstring, std::filesystem::path, std::less<>> unique;
                for (t_size index = 0; index < items->get_count() && !cancelled(); ++index) {
                    pfc::string8 relative;
                    // foobar2000 2.x: relative-path and addability queries are
                    // explicitly permitted on any thread by library_manager.
#ifdef NEKOROX_LIBRARY_ROOTS_TEST
                    if (!testing::relativeLibraryPath((*items)[index], relative)) continue;
#else
                    if (!manager->get_relative_path((*items)[index], relative)) continue;
#endif
                    if (cancelled()) break;
                    try {
                        const auto full = filesystem::g_get_native_path((*items)[index]->get_path());
                        if (full.is_empty() || relative.is_empty()) continue;
                        auto root = std::filesystem::path(utf8ToWide(full.c_str())).lexically_normal();
                        for (const auto& ignored : std::filesystem::path(utf8ToWide(relative.c_str()))) {
                            (void)ignored;
                            root = root.parent_path();
                        }
                        if (root.empty()) continue;
                        auto key = root.wstring();
                        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
                        unique.emplace(std::move(key), std::move(root));
                    } catch (...) { /* Non-native paths have no observed filesystem root. */ }
                }
                for (auto& [key, path] : unique) {
                    if (cancelled()) break;
                    (void)key;
                    LibraryRootEntry entry;
                    entry.path = std::move(path);
#ifdef NEKOROX_LIBRARY_ROOTS_TEST
                    entry.online = testing::libraryRootOnline(entry.path);
#else
                    std::error_code error;
                    entry.online = std::filesystem::is_directory(entry.path, error) && !error;
#endif
                    if (cancelled()) break;
                    try {
                        const auto candidate = entry.path / L"goden-library-policy-probe.flac";
                        pfc::string8 canonical;
                        filesystem::g_get_canonical_path(wideToUtf8(candidate.wstring()).c_str(), canonical);
#ifdef NEKOROX_LIBRARY_ROOTS_TEST
                        entry.policyAddable = testing::libraryRootAddable(canonical.c_str());
#else
                        entry.policyAddable = manager->is_path_addable(canonical.c_str());
#endif
                    } catch (...) { entry.policyAddable = false; }
                    next->roots.push_back(std::move(entry));
                }
            } catch (const std::exception& error) { next->error = error.what(); }
            catch (...) { next->error = "Library roots could not be refreshed."; }
            if (!alive->load(std::memory_order_acquire)) return;
            fb2k::inMainThread([this, alive, generation, next] {
                if (alive->load(std::memory_order_acquire)) finishRefresh(generation, next);
            });
        });
        if (!submitted) {
            next->error = "Library roots worker is unavailable.";
            finishRefresh(generation, next);
        }
    } catch (const std::exception& error) {
        next->error = error.what();
        finishRefresh(generation, next);
    } catch (...) {
        next->error = "Library roots could not be refreshed.";
        finishRefresh(generation, next);
    }
}
void LibraryRootsProvider::finishRefresh(std::uint64_t generation,
    std::shared_ptr<LibraryRootsSnapshot> next)
{
    busy_ = false;
    if (stopping_) return;
    if (observer_ && generation == generation_.load()) {
        { std::lock_guard lock(mutex_); snapshot_ = next; }
        // Permit observer-driven suspend/refresh without destroying the
        // callable while it is executing.
        const auto completion = observer_;
        completion(next);
    }
    schedule();
}
void LibraryRootsProvider::openLibraryPreferences()
{
    library_manager::get()->show_preferences();
}
void LibraryRootsProvider::on_items_added(metadb_handle_list_cref) { refresh(); }
void LibraryRootsProvider::on_items_removed(metadb_handle_list_cref) { refresh(); }
void LibraryRootsProvider::on_library_initialized() { refresh(); }
} // namespace nekorox::component
