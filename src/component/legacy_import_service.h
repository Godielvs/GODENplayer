#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace nekorox::component {

struct LegacyImportSnapshot {
    bool running = false;
    bool complete = false;
    bool sourceAvailable = false;
    std::uint64_t files = 0;
    std::uint64_t totalFiles = 0;
    std::uint64_t bytes = 0;
    std::uint64_t personas = 0;
    std::uint64_t artAssets = 0;
    std::string phase;
    std::string currentFile;
    std::string error;
};

class LegacyImportService final {
public:
    static LegacyImportService& instance();

    void start();
    void stop();
    LegacyImportSnapshot snapshot() const;

private:
    LegacyImportService() = default;
    void run(std::filesystem::path profile);
    void publish(LegacyImportSnapshot snapshot);

    mutable std::mutex mutex_;
    LegacyImportSnapshot snapshot_{};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

} // namespace nekorox::component
