// C9 QA reader: pull the shell's opt-in C9 diagnostics array.
//
// The candidate publishes its wheel-transport counters and light state through
// the registered window message "GodenC9DiagnosticsV1", which the component
// guards with NEKOROX_QA_HOOKS=1 + NEKOROX_QA_PROFILE=1 in the TARGET process.
// This reader only queries; it never injects input and never writes anything.
//
// Usage: c9_qa_read <foobar2000.exe> <selector 0|100> [instance-index]
//   0   -> protocol revision
//   100 -> snapshot the QA mirror into the component, then return the revision
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr UINT kDiagnosticsMessageName = 0; // resolved via RegisterWindowMessageW

struct Found {
    HWND shell = nullptr;
    HWND top = nullptr;
};

std::wstring className(HWND window) {
    wchar_t name[256]{};
    if (window) GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return name;
}

BOOL CALLBACK findShell(HWND window, LPARAM parameter) {
    auto* found = reinterpret_cast<Found*>(parameter);
    if (className(window) == L"GodenPlayerShellV2") {
        found->shell = window;
        found->top = GetAncestor(window, GA_ROOT);
        return FALSE;
    }
    return TRUE;
}

// The top level window belongs to the target process; match it by class family
// rather than by title so several candidates can coexist.
struct TopSearch {
    DWORD processId = 0;
    HWND top = nullptr;
    HWND shell = nullptr;
};

BOOL CALLBACK findTop(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<TopSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search->processId) return TRUE;
    Found found{};
    EnumChildWindows(window, findShell, reinterpret_cast<LPARAM>(&found));
    if (found.shell) {
        search->top = window;
        search->shell = found.shell;
        return FALSE;
    }
    return TRUE;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::wprintf(L"usage: c9_qa_read <foobar2000.exe> <selector> [index]\n");
        return 2;
    }
    const int selector = _wtoi(argv[2]);
    const int index = argc > 3 ? _wtoi(argv[3]) : 0;

    // Resolve the owning process by image path, so a stale reader cannot bind to
    // an unrelated foobar2000 instance.
    std::vector<DWORD> candidates;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 3;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const std::wstring wanted = argv[1];
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"foobar2000.exe") != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length) &&
                _wcsicmp(path, wanted.c_str()) == 0) {
                candidates.push_back(entry.th32ProcessID);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (candidates.empty()) {
        std::wprintf(L"no foobar2000 process for %ls\n", wanted.c_str());
        return 4;
    }
    const DWORD processId = candidates[0];

    TopSearch search{};
    search.processId = processId;
    EnumWindows(findTop, reinterpret_cast<LPARAM>(&search));
    if (!search.shell) {
        std::wprintf(L"no GodenPlayerShellV2 window for pid %lu\n", processId);
        return 5;
    }

    const UINT message = RegisterWindowMessageW(L"GodenC9DiagnosticsV1");
    if (!message) return 6;

    // The shell UI thread can be busy absorbing an input burst, and a low-level
    // wheel hook plus a 900 ms burst can leave it briefly unresponsive. Retry the
    // latch instead of reporting a false failure; a genuinely hung shell still
    // fails after the retries and is then a real finding.
    DWORD_PTR result = 0;
    bool latched = false;
    int attemptsUsed = 0;
    for (int attempt = 0; attempt < 12 && !latched; ++attempt) {
        result = 0;
        if (SendMessageTimeoutW(search.shell, message, static_cast<WPARAM>(selector), 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 15000, &result)) {
            ++attemptsUsed;
            if (selector == 0) {
                std::wprintf(L"C9_DIAG rev=%llu\n", static_cast<unsigned long long>(result));
                return 0;
            }
            latched = result != 0 && result != 2 && result != 9;
        }
        if (!latched) Sleep(250);
    }
    if (!latched) {
        std::wprintf(L"diagnostics query failed (selector=%d, attempts=%d)\n", selector,
                     attemptsUsed);
        return 7;
    }
    // Selector 100 latches the mirror and returns the bumped QA revision, not a
    // constant, so any other value means the snapshot is armed.

    // The component latched an 80-slot mirror on selector 100. Read it back one
    // slot at a time so the printed evidence is self-describing (selector 101
    // releases the latch).
    std::wprintf(L"C9_DIAG pid=%lu shell=%p snapshot_revision=%llu\n", processId, search.shell,
                 static_cast<unsigned long long>(result));
    for (int slot = 0; slot < 80; ++slot) {
        DWORD_PTR value = static_cast<DWORD_PTR>(-2);
        bool ok = false;
        for (int attempt = 0; attempt < 6 && !ok; ++attempt) {
            ok = SendMessageTimeoutW(search.shell, message, static_cast<WPARAM>(slot), 0,
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 15000, &value) != 0;
            if (!ok) Sleep(150);
        }
        if (!ok) {
            std::wprintf(L"slot %d: query failed\n", slot);
            continue;
        }
        std::wprintf(L"slot %d=%lld\n", slot, static_cast<long long>(value));
    }
    DWORD_PTR released = 0;
    SendMessageTimeoutW(search.shell, message, 101, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000,
                        &released);
    return 0;
}
