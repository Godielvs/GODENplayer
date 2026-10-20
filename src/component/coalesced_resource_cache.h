#pragma once

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace nekorox::component {

// Background-only single-flight cache. Results are weak: resident accounting
// and ownership remain with the existing consumer. Cancellation never runs
// under this mutex, and an exception releases the flight for another owner.
template<class Key, class Value, std::size_t MaximumEntries = 512>
class CoalescedResourceCache final {
    struct Entry {
        std::condition_variable ready;
        std::weak_ptr<Value> value;
        bool loading = false;
    };
public:
    class Lease final {
    public:
        Lease(CoalescedResourceCache* owner, std::shared_ptr<Entry> entry,
            std::shared_ptr<Value> cached = {})
            : owner_(owner), entry_(std::move(entry)), cached_(std::move(cached)) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            if (!owner_) return;
            {
                std::lock_guard lock(owner_->mutex_);
                entry_->loading = false;
            }
            entry_->ready.notify_all();
        }
        const std::shared_ptr<Value>& cached() const noexcept { return cached_; }
        void publish(const std::shared_ptr<Value>& value) {
            std::lock_guard lock(owner_->mutex_);
            entry_->value = value;
        }
    private:
        CoalescedResourceCache* owner_;
        std::shared_ptr<Entry> entry_;
        std::shared_ptr<Value> cached_;
    };

    template<class CheckCancelled>
    Lease acquire(const Key& key, CheckCancelled checkCancelled) {
        checkCancelled();
        std::unique_lock lock(mutex_);
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            if (entries_.size() >= MaximumEntries) {
                for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                    if (!it->second->loading && it->second.use_count() == 1) {
                        entries_.erase(it);
                        break;
                    }
                }
            }
            // All records can be in use only with more concurrent workers than
            // the configured ceiling. Fail rather than growing without bound.
            if (entries_.size() >= MaximumEntries) throw std::bad_alloc();
            found = entries_.emplace(key, std::make_shared<Entry>()).first;
        }
        const auto entry = found->second;
        while (entry->loading) {
            entry->ready.wait_for(lock, std::chrono::milliseconds(15));
            lock.unlock();
            checkCancelled();
            lock.lock();
        }
        if (auto value = entry->value.lock()) return Lease(nullptr, entry, std::move(value));
        entry->loading = true;
        return Lease(this, entry);
    }
private:
    std::mutex mutex_;
    std::map<Key, std::shared_ptr<Entry>> entries_;
};

} // namespace nekorox::component
