#pragma once

#include <cstddef>
#include <string_view>
#include <windows.h>

// Runs a bounded, persistent wheel schedule against one already-validated
// GODENplayer child window. The encoded form is:
//   handler-message|total_us|loop_us|t_us:delta,t_us:delta,...
// or send-input with the same remaining fields.
int runWorkpackWheelDriver(
    HWND target,
    HWND topLevel,
    DWORD expectedProcessId,
    std::wstring_view encodedSchedule);

int dumpPerformanceEvents(DWORD expectedProcessId, std::size_t maximumRecords);
