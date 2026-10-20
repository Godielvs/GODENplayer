#pragma once

#include "analysis_controller.h"
#include "library_model.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace nekorox::component {

struct ListenSectionV3 {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float energy = 0.5f;
};

struct ListenAlbumRowV3 {
    int disc = 0;
    int track = 0;
    bool current = false;
    std::wstring title;
};

// Immutable, paint-ready data. JSON parsing, string formatting and library
// scans happen when a semantic/analysis revision arrives, never from WM_PAINT.
struct ListenPresentationModelV3 {
    std::uint64_t libraryGeneration = 0;
    std::uint64_t analysisRequestId = 0;
    DeepAnalysisStatus analysisStatus = DeepAnalysisStatus::idle;
    AnalysisStage analysisStage = AnalysisStage::queued;
    float analysisProgress = 0.0f;
    bool analyzed = false;
    std::wstring trackPath;
    std::wstring formatLine;
    std::wstring analysisStatusText;
    std::array<std::wstring, 12> dossierValues{};
    std::array<float, 3> overviewValues{};
    std::array<bool, 3> overviewValid{};
    std::array<std::wstring, 3> overviewValueText{};
    std::array<float, 8> portraitValues{};
    std::array<bool, 8> portraitValid{};
    std::array<std::wstring, 8> portraitValueText{};
    std::vector<ListenSectionV3> sections;
    std::vector<double> chordStarts;
    std::vector<ListenAlbumRowV3> albumRows;
};

[[nodiscard]] std::shared_ptr<const ListenPresentationModelV3>
buildListenPresentationModelV3(
    const LibraryTrack* track,
    const DeepAnalysisSnapshot& analysis,
    const std::vector<LibraryTrack>& library,
    std::uint64_t libraryGeneration);

} // namespace nekorox::component
