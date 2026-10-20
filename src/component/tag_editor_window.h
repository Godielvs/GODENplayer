#pragma once

#include "sdk_compat.h"
#include "media_file_operations.h"
#include "staging_media_editor.h"
#include "latest_worker.h"
#include "ui_form_viewport.h"

#include <nekorox/tag_mutation.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace nekorox::component {

class TagEditorWindow final {
public:
    static TagEditorWindow& instance();
    ~TagEditorWindow();

    void show(HWND owner);
    void hide();
    void shutdownAndDrain() noexcept;

private:
    struct MoveIdentitySeed {
        std::filesystem::path source;
        std::string previousRevisionTrackId;
        std::uint32_t subsong = 0;
    };
    struct ArtworkDraft {
        ArtworkMutation mutation;
        std::uint64_t revision = 0;
        std::string contentHash;
    };
    struct PreviewPreparation {
        std::size_t index = 0;
        bool buildingRequests = false;
        std::set<std::wstring, std::less<>> discs;
        std::set<std::wstring, std::less<>> seen;
        std::vector<MoveIdentitySeed> identities;
        std::vector<MediaPlanRequest> requests;
        std::vector<nekorox::TagMutation> mutations;
        std::filesystem::path destination;
        MediaOrganizationMode mode = MediaOrganizationMode::copy;
        MediaCollisionPolicy collision = MediaCollisionPolicy::skip;
    };
    struct PendingLocations;
    TagEditorWindow() = default;
    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layout();
    void updateFont();
    void beginDrop(HDROP drop);
    void beginPaths(std::vector<std::filesystem::path> roots);
    void resolveFiles(std::vector<std::filesystem::path> files,
        std::uint64_t generation);
    void continueUiBatch(HWND window, UINT_PTR timer, UINT message, std::uint64_t generation) noexcept;
    void scheduleLocations();
    void publishLocations(std::uint64_t generation);
    void acceptTracks(std::shared_ptr<metadb_handle_list> tracks,
        std::uint64_t generation);
    void scheduleTrackRows();
    void publishTrackRows(std::uint64_t generation);
    void refreshTrackMetadata(const std::set<std::string>& itemIds);
    void refreshTrackMetadataForPaths(const std::set<std::wstring>& paths);
    void publishTrackMetadata(std::uint64_t generation);
    void setTrackLoading(bool loading);
    void refreshFields();
    void selectionChanged();
    void commitFieldIntent(std::size_t field);
    void updateDirtyUi();
    [[nodiscard]] std::string itemId(const metadb_handle_ptr& track) const;
    void refreshLibraryRoots();
    void showLibraryRoots();
    void chooseDestination();
    void previewOrganization();
    void schedulePreviewPreparation();
    void preparePreviewBatch(std::uint64_t generation);
    void schedulePreviewRows();
    void publishPreviewRows(std::uint64_t generation);
    void scheduleResultRows();
    void publishResultRows(std::uint64_t generation);
    void invalidateOrganizationPreview();
    void executeOrganization();
    void chooseArtwork(std::size_t role);
    void beginArtwork(std::filesystem::path path, std::size_t role);
    void refreshArtworkStates();
    [[nodiscard]] bool collectTagMutations(
        std::vector<nekorox::TagMutation>& output);
    void apply();
    void undoLast();
    void setStatus(std::wstring text);
    void setDirty(bool value);

    HWND window_ = nullptr;
    bool libraryRootsStarted_ = false;
    HWND lastOwner_ = nullptr;
    FormViewport viewport_;
    HWND title_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND apply_ = nullptr;
    HWND revert_ = nullptr;
    HWND undo_ = nullptr;
    HWND rootsLabel_ = nullptr;
    HWND roots_ = nullptr;
    HWND organizationPreview_ = nullptr;
    HWND chooseDestination_ = nullptr;
    HWND libraryPreferences_ = nullptr;
    HWND previewCopy_ = nullptr;
    HWND executeCopy_ = nullptr;
    HWND organizationMode_ = nullptr;
    HWND collisionMode_ = nullptr;
    std::array<HWND, 4> artworkLabels_{};
    std::array<HWND, 4> artworkModes_{};
    std::array<HWND, 4> artworkChoose_{};
    HWND artworkPreview_ = nullptr;
    HBITMAP artworkPreviewBitmap_ = nullptr;
    std::vector<HWND> labels_;
    std::vector<HWND> modes_;
    std::vector<HWND> edits_;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    HBRUSH fieldBackground_ = nullptr;
    metadb_handle_list tracks_;
    std::vector<std::string> itemIds_;
    nekorox::TagDocument tagDocument_;
    std::unique_ptr<PendingLocations> pendingLocations_;
    bool locationsPosted_ = false;
    std::shared_ptr<metadb_handle_list> pendingTracks_;
    std::vector<nekorox::TagDocumentItem> pendingDocumentItems_;
    titleformat_object::ptr pendingTitleScript_;
    std::size_t pendingTrackIndex_ = 0;
    bool trackLoading_ = false;
    bool trackRowsPosted_ = false;
    bool refreshingTrackMetadata_ = false;
    std::vector<std::size_t> metadataRefreshRows_;
    std::optional<PreviewPreparation> previewPreparation_;
    bool previewPreparationPosted_ = false;
    std::size_t previewRowIndex_ = 0;
    std::size_t previewReady_ = 0;
    std::size_t previewSkipped_ = 0;
    std::size_t previewRejected_ = 0;
    bool previewRowsPosted_ = false;
    std::shared_ptr<std::vector<MediaCopyResult>> resultRows_;
    std::size_t resultRowIndex_ = 0;
    std::uint64_t resultGeneration_ = 0;
    bool resultRowsPosted_ = false;
    std::wstring resultSummary_;
    LatestWorker uiBatchWorker_;
    LatestWorker scanWorker_;
    LatestWorker artworkWorker_;
    LatestWorker organizationWorker_;
    std::atomic<std::uint64_t> scanGeneration_{0};
    std::atomic<std::uint64_t> artworkGeneration_{0};
    std::atomic<std::uint64_t> organizationGeneration_{0};
    std::atomic_bool shuttingDown_{false};
    bool organizationPreviewPending_ = false;
    bool dirty_ = false;
    bool applying_ = false;
    bool internalControlUpdate_ = false;
    std::filesystem::path selectedDestination_;
    bool selectedDestinationAddable_ = false;
    std::optional<MediaPlan> copyPlan_;
    std::vector<nekorox::TagMutation> plannedTagMutations_;
    std::array<ArtworkMutation, 4> artworkMutations_{};
    std::map<std::string, std::array<ArtworkDraft, 4>> artworkDrafts_;
    std::uint64_t nextArtworkDraftRevision_ = 1;
    std::vector<ArtworkMutation> plannedArtworkMutations_;
    std::vector<MoveIdentitySeed> plannedMoveIdentities_;
};

} // namespace nekorox::component
