#pragma once

#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>

namespace nekorox::component {

// A loader failure can precede main(), so it cannot use the worker protocol.
// Keep the status code and actionable causes without guessing which DLL was
// missing, exposing paths or recommending a security/containment bypass.
[[nodiscard]] inline std::wstring workerExitDiagnostic(
    std::wstring_view task, std::uint32_t exitCode)
{
    wchar_t code[24]{};
    std::swprintf(code, 24, L" (exit 0x%08X). ", static_cast<unsigned>(exitCode));
    std::wstring message(task);
    message += L": ";
    switch (exitCode) {
    case 0xC0000135U: // STATUS_DLL_NOT_FOUND
        message += L"Required Windows DLL missing";
        message += code;
        message += L"On Windows N, "
            L"install the Media Feature Pack for analysis; also check the "
            L"security product's quarantine and repair the installation.";
        break;
    case 0xC000007BU: // STATUS_INVALID_IMAGE_FORMAT
        message += L"Wrong DLL architecture or damaged image";
        message += code;
        message += L"Repair using the matching x64 installer.";
        break;
    case 0xC000001DU: // STATUS_ILLEGAL_INSTRUCTION
        message += L"Unsupported CPU instruction";
        message += code;
        message += L"Report the CPU model and this code.";
        break;
    case 0xC0000142U: // STATUS_DLL_INIT_FAILED
        message += L"Windows DLL initialization failed";
        message += code;
        message += L"Check Windows "
            L"components and security product diagnostics, then retry.";
        break;
    default:
        message += L"Worker ended before producing a result";
        message += code;
        message += L"Retry starts a fresh worker. Report this code if it recurs.";
        break;
    }
    return message;
}

} // namespace nekorox::component
