#pragma once

#include "nekorox/contracts.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox::component {

struct AnalysisWorkerHandles {
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    HANDLE output = nullptr;
    // Owned until closeAnalysisWorker(), after the reader has joined.
    HANDLE startThread = nullptr;
};

bool launchAnalysisWorker(
    std::wstring_view command,
    std::uint64_t requestId,
    const std::filesystem::path& localTrack,
    SIZE_T processMemoryLimit,
    AnalysisWorkerHandles& handles,
    std::wstring& error,
    bool deferStart = false);

bool readAnalysisWorkerFrame(
    HANDLE output,
    std::uint64_t expectedRequestId,
    AnalysisFrameHeaderV1& header,
    std::vector<std::byte>& payload,
    std::string& error);

bool cancelAnalysisWorker(
    AnalysisWorkerHandles& handles,
    DWORD waitMilliseconds = 2000) noexcept;
void closeAnalysisWorker(AnalysisWorkerHandles& handles) noexcept;

} // namespace nekorox::component
