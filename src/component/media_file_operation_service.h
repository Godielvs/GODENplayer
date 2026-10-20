#pragma once

#include "media_file_operations.h"
#include "sdk_compat.h"

#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <cstdint>

namespace nekorox::component {

class MediaFileOperationService final {
public:
    using Completion = std::function<void(std::vector<MediaCopyResult>)>;

    static MediaFileOperationService& instance();
    ~MediaFileOperationService();

    [[nodiscard]] bool busy() const noexcept;
    void beginBindingRecovery();
    [[nodiscard]] bool mutationAllowed(const std::filesystem::path& path) const noexcept;
    [[nodiscard]] bool hasInPlaceUndo() const noexcept;
    [[nodiscard]] std::uint64_t undoOperationSequence() const noexcept;
    [[nodiscard]] bool startCopy(MediaPlan plan, Completion completion);
    [[nodiscard]] bool startPlan(MediaPlan plan, Completion completion,
        MediaCopyExecutionOptions options = {});
    [[nodiscard]] bool undoLastInPlace(Completion completion);
    void onPlaybackChanged(const metadb_handle_ptr& track);
    void beginShutdown() noexcept;
    void shutdownAndDrain() noexcept;

private:
    struct PendingWork {
        MediaPlanItem item;
        std::size_t sequence = 0;
        unsigned attempt = 0;
    };

    MediaFileOperationService() = default;
    void pump();
    void finishOperation(std::vector<MediaCopyResult> results,
        Completion completion, std::uint64_t operation, bool undo,
        metadb_hint_list::ptr hints, bool identitiesSettled = false);
    [[nodiscard]] bool persistQueueLocked() const;
    [[nodiscard]] std::filesystem::path itemJournalPath(
        const PendingWork& work) const;
    [[nodiscard]] bool playbackProtects(
        const std::filesystem::path& path) const noexcept;

    mutable std::mutex mutex_;
    bool active_ = false;
    bool workerActive_ = false;
    bool identityBindingPending_ = false;
    bool shuttingDown_ = false;
    bool recoveryActive_ = false;
    bool recoveryLimited_ = false;
    std::uint64_t recoveryGeneration_ = 0;
    std::vector<std::filesystem::path> blockedBindingPaths_;
    MediaPlan activePlan_;
    std::vector<PendingWork> pending_;
    std::optional<PendingWork> current_;
    std::vector<MediaCopyResult> results_;
    std::vector<MediaCopyResult> inPlaceUndo_;
    std::uint64_t activeOperationSequence_ = 0;
    std::uint64_t inPlaceUndoSequence_ = 0;
    MediaCopyExecutionOptions options_;
    Completion completion_;
    std::filesystem::path playbackPath_;
    std::filesystem::path queueJournalPath_;
    std::jthread worker_;
    metadb_hint_list::ptr metadataHints_;
};

} // namespace nekorox::component
