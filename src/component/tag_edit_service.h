#pragma once

#include "sdk_compat.h"

#include <nekorox/tag_mutation.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include <vector>

namespace nekorox::component {

enum class TagFileOutcome : unsigned char {
    planned,
    backedUp,
    mutationIntentDurable,
    restoreIntentDurable,
    bindingPending,
    committed,
    restored,
    noChange,
    deferredUntilReleased,
    conflictExternalChange,
    failedBeforeWrite,
    recoverablePartial,
    cancelledBeforeWrite,
};

struct TagFileResult {
    std::filesystem::path path;
    std::filesystem::path backupPath;
    std::string sourceHash;
    std::string postWriteHash;
    std::vector<std::uint32_t> subsongs;
    TagFileOutcome outcome = TagFileOutcome::planned;
    std::wstring detail;
};

struct TagItemResult {
    std::string itemId;
    std::uint32_t subsong = 0;
    std::uint64_t draftRevision = 0;
    std::string beforeRevision;
    std::string afterRevision;
    TagFileOutcome outcome = TagFileOutcome::planned;
};

struct TagApplySummary {
    std::uint64_t operationSeq = 0;
    std::wstring operationId;
    std::filesystem::path journalPath;
    std::vector<nekorox::TagMutation> mutations;
    std::vector<TagFileResult> files;
    std::vector<TagItemResult> items;
};

struct TagEditPlanItem {
    metadb_handle_ptr track;
    std::string itemId;
    std::vector<nekorox::TagMutation> mutations;
    std::uint64_t draftRevision = 0;
};

class TagEditService final {
public:
    using Completion = std::function<void(TagApplySummary)>;

    static TagEditService& instance();
    ~TagEditService();

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool hasUndo() const noexcept;
    [[nodiscard]] std::uint64_t undoOperationSequence() const noexcept;
    void beginUndoRecovery(HWND observer, UINT completedMessage);
    void clearUndoRecoveryObserver(HWND observer) noexcept;
    [[nodiscard]] bool undoRecoveryPending() const noexcept;
    [[nodiscard]] bool undoRecoveryLimited() const noexcept;
    [[nodiscard]] bool start(
        metadb_handle_list_cref tracks,
        std::vector<nekorox::TagMutation> mutations,
        HWND owner,
        Completion completion);
    [[nodiscard]] bool start(std::vector<TagEditPlanItem> plan,
        HWND owner, Completion completion);
    void resumeDeferred();
    [[nodiscard]] bool undoLast(HWND owner, Completion completion);
    void cancelPending() noexcept;
    void shutdownAndDrain() noexcept;

private:
    struct Operation;
    struct RecoveryState;
    TagEditService() = default;
    void bindRecoveredUndo(std::uint64_t generation);
    void prepare(std::shared_ptr<Operation> operation, std::stop_token stop);
    void startNext(std::shared_ptr<Operation> operation);
    void updateCompleted(std::shared_ptr<Operation> operation,
        std::size_t itemIndex, unsigned status);
    void rereadCompleted(std::shared_ptr<Operation> operation,
        std::size_t itemIndex, unsigned status);
    void capturePostWrite(std::shared_ptr<Operation> operation,
        std::size_t itemIndex);
    void bindCompleted(std::shared_ptr<Operation> operation,
        bool success, std::string error);
    void prepareUndo(std::shared_ptr<Operation> operation,
        std::stop_token stop);
    [[nodiscard]] bool playbackProtects(const std::filesystem::path& path) const;
    void finish(std::shared_ptr<Operation> operation);

    mutable std::mutex mutex_;
    std::shared_ptr<Operation> active_;
    std::shared_ptr<Operation> lastCompleted_;
    std::filesystem::path playbackPath_;
    bool undoLoadAttempted_ = false;
    bool undoRecoveryPending_ = false;
    bool undoRecoveryLimited_ = false;
    HWND undoObserver_ = nullptr;
    UINT undoCompletedMessage_ = 0;
    std::unique_ptr<RecoveryState> recovery_;
    std::jthread undoWorker_;
    std::atomic<std::uint64_t> undoGeneration_{0};
    std::jthread worker_;
    std::atomic<std::uint64_t> generation_{0};
};

[[nodiscard]] const wchar_t* tagFileOutcomeText(TagFileOutcome value) noexcept;

} // namespace nekorox::component
