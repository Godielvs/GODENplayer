#pragma once

#include "nekorox/waveform_pyramid.h"
#include "sdk_compat.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nekorox::component {

struct WaveformCacheKey {
    std::string path;
    std::uint32_t subsong = 0;
    double durationSeconds = 0.0;
};

enum class WaveformCacheState : std::uint8_t {
    idle = 0,
    dwelling = 1,
    loadingCache = 2,
    decoding = 3,
    complete = 4,
    cancelled = 5,
    failed = 6,
};

struct WaveformCacheSnapshot {
    std::uint64_t generation = 0;
    WaveformCacheKey key;
    WaveformCacheState state = WaveformCacheState::idle;
    double progress = 0.0;
    std::shared_ptr<const WaveformPyramidV1> pyramid;
    std::string error;
};

// Implementations persist the versioned pyramid through NekoDatabase analysis
// layers. Methods run on the waveform worker and may block. A cache miss
// returns false with an empty error; corruption or I/O failure returns false
// with a non-empty error.
class WaveformPersistenceHook {
public:
    virtual ~WaveformPersistenceHook() = default;

    virtual bool load(
        const WaveformCacheKey& key,
        std::vector<std::byte>& encoded,
        std::string& error) = 0;
    virtual bool store(
        const WaveformCacheKey& key,
        std::span<const std::byte> encoded,
        std::string& error) = 0;
};

class WaveformCacheService final {
public:
    using SnapshotPtr = std::shared_ptr<const WaveformCacheSnapshot>;
    // Callbacks can run on the requesting thread for the dwell snapshot and
    // on the worker thread thereafter. UI integrations must marshal them.
    using SnapshotCallback = std::function<void(SnapshotPtr)>;

    WaveformCacheService();
    ~WaveformCacheService();

    WaveformCacheService(const WaveformCacheService&) = delete;
    WaveformCacheService& operator=(const WaveformCacheService&) = delete;

    void start();
    void stop();

    // Each request supersedes the previous generation. A 12 ms dwell absorbs
    // accidental track churn; decoding publishes a real prefix before any
    // persistent fingerprint or SQLite lookup can delay first paint.
    [[nodiscard]] std::uint64_t request(
        const metadb_handle_ptr& track,
        SnapshotCallback callback = {});
    void cancel();

    void setPersistenceHook(
        std::shared_ptr<WaveformPersistenceHook> hook);
    [[nodiscard]] SnapshotPtr snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nekorox::component
