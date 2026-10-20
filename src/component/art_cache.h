#pragma once

#include "artwork_broker_policy.h"
#include "sdk_compat.h"

#include "nekorox/art_palette.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox::component {
namespace a4 { struct Material; }

struct ArtPixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t contentHash = 0;
    std::vector<std::uint8_t> bgra;
    // Palette extraction is part of the background decode/resample job. UI
    // surfaces can therefore adopt artwork colours without running thousands
    // of OKLab transforms on the presentation thread.
    ArtPalette palette{};
    std::shared_ptr<const a4::Material> material;
    bool palettePrepared = false;
};

struct ArtworkBrokerDiagnostics {
    std::size_t readyBytes = 0;
    std::size_t temporaryBytes = 0;
    std::size_t peakTemporaryBytes = 0;
    std::size_t entryCount = 0;
    std::size_t queuedRequests = 0;
    std::uint64_t rejectedEntries = 0;
    std::uint64_t rejectedTemporaryReservations = 0;
    std::uint64_t displacedPrefetchRequests = 0;
    std::uint64_t loadRequests = 0;
    std::uint64_t localPaths = 0;
    std::uint64_t sidecarCandidates = 0;
    std::uint64_t fileReads = 0;
    std::uint64_t decodeAttempts = 0;
    std::uint64_t decodeSuccesses = 0;
    std::uint64_t busyRetryRequeues = 0;
    std::uint64_t failedNotices = 0;
    std::uint64_t resampleAttempts = 0;
    std::uint64_t resampleMemoHits = 0;
};

[[nodiscard]] ArtworkBrokerDiagnostics artworkBrokerDiagnostics() noexcept;

// Stable identity for artwork resolved from one foobar track handle. Keeping
// the subsong in the key prevents two cue-sheet tracks (or other container
// subsongs) from sharing an artwork result accidentally.
[[nodiscard]] std::wstring artworkTrackIdentity(
    const metadb_handle_ptr& track);

// Resolves the same track-first artwork used by the native Goden surfaces.
// This is intentionally blocking and must only be called from a background
// worker. It exists for consumers such as Discord cover preparation that need
// a one-shot immutable image rather than HWND-driven cache notifications.
std::shared_ptr<const ArtPixels> loadResolvedArtworkBlocking(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge = 320);
// Presence supplies an abort callback so SDK extraction and intervening
// filesystem/decode stages can stop when the user disables cover work.
std::shared_ptr<const ArtPixels> loadResolvedArtworkBlocking(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge,
    abort_callback& abort);

enum class ArtCacheStatus {
    absent,
    loading,
    ready,
    missing,
};

enum class ArtRequestPriority : std::uint8_t {
    visibleMain = 0,
    visibleRow = 1,
    prefetch = 2,
};
enum class ArtAnalysisFlags : std::uint8_t { legacy=0, alloyV2=1 };

class ArtCache final {
public:
    struct State;

    explicit ArtCache(UINT refreshMessage);
    ~ArtCache();

    void attach(HWND window);
    void detach();
    // Withdraw only speculative work outside the current neighbour window.
    // Overlapping requests, visible consumers and resident pixels are retained.
    void retainPrefetch(const std::vector<std::wstring>& identities,
        std::uint32_t decodeEdge,
        ArtAnalysisFlags analysis = ArtAnalysisFlags::legacy);
    // Bracket one complete paint of this owner's visible rows. Requests for
    // both track/file masters and their variants are retained as one batch.
    // Main covers, prefetches and other owners remain independent.
    void beginVisibleRequests();
    void endVisibleRequests();
    void invalidatePrefix(std::wstring_view keyPrefix);
    // Artwork edits affect the physical file, including cached sibling subsongs.
    static void invalidateTrack(const metadb_handle_ptr& track);
    ArtCacheStatus status(
        const std::wstring& key,
        std::uint32_t decodeEdge,
        ArtAnalysisFlags analysis = ArtAnalysisFlags::legacy) const;
    std::shared_ptr<const ArtPixels> getOrRequest(
        const std::wstring& key,
        const metadb_handle_ptr& track,
        std::uint32_t decodeEdge = 160,
        ArtRequestPriority priority = ArtRequestPriority::visibleRow,
        ArtAnalysisFlags analysis = ArtAnalysisFlags::legacy);
    std::shared_ptr<const ArtPixels> getOrRequestVariant(
        const std::wstring& key,
        const metadb_handle_ptr& track,
        std::uint32_t targetPhysicalEdge,
        bool exactTarget = false,
        ArtRequestPriority priority = ArtRequestPriority::visibleRow,
        ArtAnalysisFlags analysis = ArtAnalysisFlags::legacy);
    std::shared_ptr<const ArtPixels> getOrRequestFile(
        const std::wstring& key,
        const std::filesystem::path& path,
        std::uint32_t decodeEdge = 640,
        ArtRequestPriority priority = ArtRequestPriority::visibleRow,
        ArtAnalysisFlags analysis = ArtAnalysisFlags::legacy);
    std::shared_ptr<const ArtPixels> getOrRequestFileVariant(
        const std::wstring& key,
        const std::filesystem::path& path,
        std::uint32_t targetPhysicalEdge,
        ArtRequestPriority priority = ArtRequestPriority::visibleRow,
        ArtAnalysisFlags analysis = ArtAnalysisFlags::legacy);

private:
    void recordVisibleRequest(const std::wstring& key, ArtRequestPriority priority);
    std::shared_ptr<State> state_;
    bool collectingVisibleRequests_ = false;
    std::vector<std::wstring> visibleRequests_;
    std::vector<std::wstring> retainedVisibleRequests_;
};

} // namespace nekorox::component
