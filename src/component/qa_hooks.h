#pragma once

#include <windows.h>

// Shipping builds have no active mutation or fault-injection control surface.
// Disposable QA builds must explicitly opt in at build time AND at startup.
#ifndef GODEN_ENABLE_QA_HOOKS
#define GODEN_ENABLE_QA_HOOKS 0
#endif

namespace nekorox::component::qa {

inline constexpr bool compiled = GODEN_ENABLE_QA_HOOKS != 0;

[[nodiscard]] inline bool profileAllowed() noexcept
{
    wchar_t hooks[8]{}, profile[8]{};
    return GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks, 8) == 1 &&
        hooks[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile, 8) == 1 &&
        profile[0] == L'1';
}

[[nodiscard]] inline bool hooksAllowed() noexcept
{
    if constexpr (!compiled) return false;
    return profileAllowed();
}

} // namespace nekorox::component::qa
