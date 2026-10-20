#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <oleacc.h>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")

#include "nekorox/qa_contracts.h"
#include "workpack_driver.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ChildContext {
    const wchar_t* className = nullptr;
    HWND result = nullptr;
};

struct ControlIdContext {
    int id = 0;
    HWND result = nullptr;
};

struct LargestChildContext {
    const wchar_t* className = nullptr;
    HWND result = nullptr;
    std::int64_t bestArea = -1;
    unsigned matches = 0;
    unsigned visibleMatches = 0;
};

struct PopupContext {
    DWORD processId = 0;
    HWND result = nullptr;
};

std::wstring processExecutable(DWORD processId);

BOOL CALLBACK findPopupMenu(HWND window, LPARAM value)
{
    auto& context = *reinterpret_cast<PopupContext*>(value);
    if (!IsWindowVisible(window)) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != context.processId) return TRUE;
    wchar_t className[32]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"#32768") == 0) {
        context.result = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK findLargestVisibleChild(HWND window, LPARAM value)
{
    auto& context = *reinterpret_cast<LargestChildContext*>(value);
    wchar_t className[256]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, context.className) != 0) return TRUE;
    ++context.matches;
    if (!IsWindowVisible(window)) return TRUE;
    ++context.visibleMatches;
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const auto width = (std::max)(0L, bounds.right - bounds.left);
    const auto height = (std::max)(0L, bounds.bottom - bounds.top);
    const auto area = static_cast<std::int64_t>(width) * height;
    if (context.result == nullptr || area > context.bestArea) {
        context.result = window;
        context.bestArea = area;
    }
    return TRUE;
}

BOOL CALLBACK findChild(HWND window, LPARAM value)
{
    auto& context = *reinterpret_cast<ChildContext*>(value);
    wchar_t className[256]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, context.className) == 0) {
        context.result = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK findControlId(HWND window, LPARAM value)
{
    auto& context = *reinterpret_cast<ControlIdContext*>(value);
    if (GetDlgCtrlID(window) == context.id) {
        context.result = window;
        return FALSE;
    }
    return TRUE;
}

HWND descendantControl(HWND root, int id)
{
    if (root == nullptr) return nullptr;
    if (const auto direct = GetDlgItem(root, id); direct != nullptr)
        return direct;
    ControlIdContext context{id};
    EnumChildWindows(root, findControlId,
        reinterpret_cast<LPARAM>(&context));
    return context.result;
}

HWND descendantClass(HWND root, const wchar_t* className)
{
    if (root == nullptr || className == nullptr) return nullptr;
    ChildContext context{className};
    EnumChildWindows(root, findChild, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

bool exerciseFormViewport(HWND root, bool leaveAtBottom)
{
    const auto viewport = descendantClass(root, L"GodenFormViewport");
    if (viewport == nullptr) return false;
    SCROLLINFO before{sizeof(before), SIF_ALL};
    if (!GetScrollInfo(viewport, SB_VERT, &before)) return false;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        SendMessageW(viewport, WM_VSCROLL, SB_BOTTOM, 0);
        SendMessageW(viewport, WM_VSCROLL, SB_TOP, 0);
    }
    SendMessageW(viewport, WM_VSCROLL,
        leaveAtBottom ? SB_BOTTOM : SB_TOP, 0);
    SCROLLINFO after{sizeof(after), SIF_ALL};
    if (!GetScrollInfo(viewport, SB_VERT, &after)) return false;
    const auto maximum = (std::max)(after.nMin,
        after.nMax - static_cast<int>(after.nPage) + 1);
    const auto expected = leaveAtBottom ? maximum : after.nMin;
    return after.nMax >= before.nMin && after.nPos == expected;
}

bool exerciseFormResize(HWND root)
{
    if (root == nullptr) return false;
    RECT original{};
    if (!GetWindowRect(root, &original)) return false;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        const auto width = cycle % 2 == 0 ? 970 : 967;
        const auto height = cycle % 2 == 0 ? 653 : 664;
        if (!SetWindowPos(root, nullptr, original.left, original.top,
                width, height, SWP_NOACTIVATE | SWP_NOZORDER)) return false;
    }
    return SetWindowPos(root, nullptr, original.left, original.top,
        original.right - original.left, original.bottom - original.top,
        SWP_NOACTIVATE | SWP_NOZORDER) != FALSE &&
        descendantClass(root, L"GodenFormViewport") != nullptr;
}

bool exerciseFormFocus(HWND root, int finalControlId)
{
    const auto viewport = descendantClass(root, L"GodenFormViewport");
    const auto target = descendantControl(root, finalControlId);
    if (viewport == nullptr || target == nullptr) return false;
    SendMessageW(viewport, WM_VSCROLL, SB_TOP, 0);
    const auto targetThread = GetWindowThreadProcessId(root, nullptr);
    const auto currentThread = GetCurrentThreadId();
    const auto attached = targetThread == currentThread ||
        AttachThreadInput(currentThread, targetThread, TRUE) != FALSE;
    if (!attached) return false;
    SetFocus(target);
    SCROLLINFO scrolled{sizeof(scrolled), SIF_POS};
    const auto revealed = GetScrollInfo(viewport, SB_VERT, &scrolled) &&
        scrolled.nPos > 0;
    GUITHREADINFO before{sizeof(before)};
    GetGUIThreadInfo(targetThread, &before);
    SendMessageW(target, WM_KEYDOWN, VK_TAB, 0);
    GUITHREADINFO after{sizeof(after)};
    GetGUIThreadInfo(targetThread, &after);
    if (targetThread != currentThread)
        AttachThreadInput(currentThread, targetThread, FALSE);
    SendMessageW(viewport, WM_VSCROLL, SB_TOP, 0);
    return revealed && before.hwndFocus == target &&
        after.hwndFocus != nullptr && after.hwndFocus != target;
}

BOOL CALLBACK printChildWindow(HWND window, LPARAM)
{
    wchar_t className[256]{};
    wchar_t title[256]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    RECT bounds{};
    GetWindowRect(window, &bounds);
    std::wcout
        << L"hwnd=" << window
        << L" parent=" << GetParent(window)
        << L" visible=" << (IsWindowVisible(window) ? L"true" : L"false")
        << L" rect=" << bounds.left << L"," << bounds.top
        << L" " << bounds.right - bounds.left << L"x"
        << bounds.bottom - bounds.top
        << L" style=" << std::hex << std::showbase
        << static_cast<std::uintptr_t>(GetWindowLongPtrW(window, GWL_STYLE))
        << L" ex="
        << static_cast<std::uintptr_t>(GetWindowLongPtrW(window, GWL_EXSTYLE))
        << std::dec
        << L" class=[" << className << L"]"
        << L" title=[" << title << L"]\n";
    return TRUE;
}

struct ZOrderContext {
    HWND target = nullptr;
};

BOOL CALLBACK printLargeZOrderWindow(HWND window, LPARAM value)
{
    auto& context = *reinterpret_cast<ZOrderContext*>(value);
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const auto width = (std::max)(0L, bounds.right - bounds.left);
    const auto height = (std::max)(0L, bounds.bottom - bounds.top);
    if (static_cast<std::int64_t>(width) * height >= 500000) {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        wchar_t className[256]{};
        wchar_t title[256]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        std::wcout
            << L"hwnd=" << window
            << L" pid=" << processId
            << L" visible=" << (IsWindowVisible(window) ? L"true" : L"false")
            << L" rect=" << bounds.left << L"," << bounds.top
            << L" " << width << L"x" << height
            << L" ex=" << std::hex << std::showbase
            << static_cast<std::uintptr_t>(GetWindowLongPtrW(window, GWL_EXSTYLE))
            << std::dec
            << L" class=[" << className << L"]"
            << L" title=[" << title << L"]"
            << L" exe=[" << processExecutable(processId) << L"]\n";
    }
    return window == context.target ? FALSE : TRUE;
}

struct SearchContext {
    std::wstring executable;
    const wchar_t* className = nullptr;
    bool allowOwned = false;
    bool allowHidden = false;
    HWND result = nullptr;
    std::int64_t bestArea = 0;
    const wchar_t* requiredDescendantClass = nullptr;
};

std::wstring processExecutable(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return {};
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) path.clear();
    else path.resize(size);
    CloseHandle(process);
    return path;
}

BOOL CALLBACK findTopLevel(HWND window, LPARAM value)
{
    auto& context = *reinterpret_cast<SearchContext*>(value);
    if (!context.allowHidden && !IsWindowVisible(window)) return TRUE;
    if (!context.allowOwned && GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    if (context.className != nullptr) {
        wchar_t className[256]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, context.className) != 0) return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    const auto path = processExecutable(processId);
    if (!path.empty() && _wcsicmp(path.c_str(), context.executable.c_str()) == 0) {
        if (context.requiredDescendantClass != nullptr) {
            ChildContext descendant{context.requiredDescendantClass};
            EnumChildWindows(
                window, findChild, reinterpret_cast<LPARAM>(&descendant));
            if (descendant.result == nullptr) return TRUE;
        }
        RECT bounds{};
        GetWindowRect(window, &bounds);
        const auto width = (std::max)(0L, bounds.right - bounds.left);
        const auto height = (std::max)(0L, bounds.bottom - bounds.top);
        const auto area = static_cast<std::int64_t>(width) * height;
        if (context.result == nullptr || area > context.bestArea) {
            context.result = window;
            context.bestArea = area;
        }
    }
    return TRUE;
}

bool sendCopyDataText(HWND window, std::uintptr_t tag, std::wstring_view value)
{
    const std::wstring owned(value);
    COPYDATASTRUCT payload{};
    payload.dwData = tag;
    payload.cbData = static_cast<DWORD>(
        (owned.size() + 1U) * sizeof(wchar_t));
    payload.lpData = const_cast<wchar_t*>(owned.c_str());
    DWORD_PTR result = FALSE;
    return SendMessageTimeoutW(
               window,
               WM_COPYDATA,
               0,
               reinterpret_cast<LPARAM>(&payload),
               SMTO_ABORTIFHUNG | SMTO_BLOCK,
               5000,
               &result) != 0 &&
        result != FALSE;
}

bool sendFileDrop(HWND window, const std::filesystem::path& path)
{
    return sendCopyDataText(window, nekorox::kFileDropCopyDataTagV1,
        path.wstring());
}

bool activateInputCandidate(HWND window)
{
    SendMessageTimeoutW(window,WM_CANCELMODE,0,0,SMTO_ABORTIFHUNG,2000,nullptr);
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 40, 40, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    const auto current=GetCurrentThreadId();
    const auto foreground=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    const bool attached=foreground && foreground!=current && AttachThreadInput(current,foreground,TRUE);
    SetForegroundWindow(window);
    if(attached)AttachThreadInput(current,foreground,FALSE);
    for(unsigned attempt=0;attempt<10 && GetForegroundWindow()!=window;++attempt){Sleep(20);SetForegroundWindow(window);}
    return GetForegroundWindow()==window;
}

bool clickDip(HWND window, float x, float y)
{
    const auto dpi = static_cast<float>(GetDpiForWindow(window));
    const auto pixelX = static_cast<int>(std::lround(x * dpi / 96.0f));
    const auto pixelY = static_cast<int>(std::lround(y * dpi / 96.0f));
    const auto point = MAKELPARAM(pixelX, pixelY);
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(window, WM_LBUTTONDOWN, MK_LBUTTON, point,
            SMTO_ABORTIFHUNG, 2000, &ignored)) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    return SendMessageTimeoutW(window, WM_LBUTTONUP, 0, point,
        SMTO_ABORTIFHUNG, 2000, &ignored) != 0 || GetLastError() == ERROR_SUCCESS;
}

bool postClickDip(HWND window, float x, float y)
{
    const auto dpi = static_cast<float>(GetDpiForWindow(window));
    const auto pixelX = static_cast<int>(std::lround(x * dpi / 96.0f));
    const auto pixelY = static_cast<int>(std::lround(y * dpi / 96.0f));
    const auto point = MAKELPARAM(pixelX, pixelY);
    return PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point) != FALSE &&
        PostMessageW(window, WM_LBUTTONUP, 0, point) != FALSE;
}

bool doubleClickDip(HWND window, float x, float y)
{
    if (!clickDip(window, x, y)) return false;
    const auto dpi = static_cast<float>(GetDpiForWindow(window));
    const auto pixelX = static_cast<int>(std::lround(x * dpi / 96.0f));
    const auto pixelY = static_cast<int>(std::lround(y * dpi / 96.0f));
    const auto point = MAKELPARAM(pixelX, pixelY);
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, point,
            SMTO_ABORTIFHUNG, 2000, &ignored)) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    return SendMessageTimeoutW(window, WM_LBUTTONUP, 0, point,
        SMTO_ABORTIFHUNG, 2000, &ignored) != 0 || GetLastError() == ERROR_SUCCESS;
}

bool physicalDoubleClickDip(HWND window, float x, float y)
{
    const auto dpi = static_cast<float>(std::max<UINT>(1U, GetDpiForWindow(window)));
    POINT point{
        static_cast<LONG>(std::lround(x * dpi / 96.0f)),
        static_cast<LONG>(std::lround(y * dpi / 96.0f))};
    if (!ClientToScreen(window, &point)) return false;

    POINT previous{};
    const auto restoreCursor = GetCursorPos(&previous) != FALSE;
    if (!SetCursorPos(point.x, point.y)) return false;
    const auto click = [] {
        std::array<INPUT, 2> inputs{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        return SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
            sizeof(INPUT)) == inputs.size();
    };
    const auto first = click();
    Sleep(60);
    const auto second = click();
    if (restoreCursor) SetCursorPos(previous.x, previous.y);
    return first && second;
}

bool waitForWorkspacePresentation(HWND window, UINT message)
{
    if (window == nullptr || message == 0) return false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        DWORD_PTR desired = 0;
        DWORD_PTR accepted = 0;
        if (SendMessageTimeoutW(window, message, 54, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &desired) &&
            SendMessageTimeoutW(window, message, 55, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &accepted) &&
            accepted >= desired) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

bool dragDip(
    HWND window,
    float startX,
    float startY,
    float endX,
    float endY)
{
    const auto dpi = static_cast<float>(GetDpiForWindow(window));
    const auto dipPoint = [dpi](float x, float y) {
        return MAKELPARAM(
            static_cast<int>(std::lround(x * dpi / 96.0f)),
            static_cast<int>(std::lround(y * dpi / 96.0f)));
    };
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(window, WM_LBUTTONDOWN, MK_LBUTTON,
            dipPoint(startX, startY), SMTO_ABORTIFHUNG, 2000, &ignored)) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    return SendMessageTimeoutW(window, WM_LBUTTONUP, 0,
        dipPoint(endX, endY), SMTO_ABORTIFHUNG, 2000, &ignored) != 0 ||
        GetLastError() == ERROR_SUCCESS;
}

bool wheelDip(HWND window, float x, float y, int delta)
{
    const auto dpi = static_cast<float>(GetDpiForWindow(window));
    POINT point{
        static_cast<LONG>(std::lround(x * dpi / 96.0f)),
        static_cast<LONG>(std::lround(y * dpi / 96.0f))};
    if (!ClientToScreen(window, &point)) return false;
    DWORD_PTR ignored = 0;
    return SendMessageTimeoutW(
        window,
        WM_MOUSEWHEEL,
        MAKEWPARAM(0, static_cast<WORD>(delta)),
        MAKELPARAM(point.x, point.y),
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        2000,
        &ignored) != 0;
}

float workspaceLeftWidth(float width)
{
    return std::clamp(width * 0.44f, 300.0f, std::max(300.0f, width - 380.0f));
}

bool setControlText(HWND control, const std::wstring& value)
{
    if (control == nullptr) return false;
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(control, WM_SETTEXT, 0,
            reinterpret_cast<LPARAM>(value.c_str()),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
        return false;
    }
    if (!SendMessageTimeoutW(control, WM_GETTEXTLENGTH, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
        return value.empty();
    }
    std::wstring verification(static_cast<std::size_t>(result) + 1U, L'\0');
    if (!SendMessageTimeoutW(control, WM_GETTEXT,
            static_cast<WPARAM>(verification.size()),
            reinterpret_cast<LPARAM>(verification.data()),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
        return false;
    }
    verification.resize(static_cast<std::size_t>(result));
    return verification == value;
}

std::wstring controlText(HWND control)
{
    if (control == nullptr) return {};
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXTLENGTH, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &length)) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXT,
            static_cast<WPARAM>(value.size()),
            reinterpret_cast<LPARAM>(value.data()),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &copied)) {
        return {};
    }
    value.resize(static_cast<std::size_t>(copied));
    return value;
}

std::vector<std::wstring> splitFields(const std::wstring& value)
{
    std::vector<std::wstring> fields;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(L'|', begin);
        fields.push_back(value.substr(begin,
            end == std::wstring::npos ? std::wstring::npos : end - begin));
        if (end == std::wstring::npos) break;
        begin = end + 1U;
    }
    return fields;
}

HWND findOwnedWindow(const std::wstring& executable, const wchar_t* className)
{
    // QA-secondary windows may intentionally stay hidden so background runs
    // never steal focus or flash on the user's desktop.
    SearchContext context{executable, className, true, true};
    EnumWindows(findTopLevel, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

void sendToBackNoActivate(HWND window)
{
    if (window == nullptr) return;
    SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool showNoActivatePreservingForeground(HWND window)
{
    if (window == nullptr) return false;
    const auto previousForeground = GetForegroundWindow();

    WINDOWPLACEMENT placement{sizeof(placement)};
    if (GetWindowPlacement(window, &placement)) {
        placement.flags &= ~WPF_RESTORETOMAXIMIZED;
        placement.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(window, &placement);
    }
    // SW_SHOWNOACTIVATE alone leaves an iconic top-level window minimized on
    // some Windows builds. Restore it first, then return focus to the prior
    // owner below if Windows activates it as part of the restore.
    ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOWNOACTIVATE);
    sendToBackNoActivate(window);

    if (GetForegroundWindow() == window &&
            previousForeground != nullptr &&
            previousForeground != window &&
            IsWindow(previousForeground)) {
        ShowWindow(window, SW_HIDE);
        const auto foregroundThread = GetWindowThreadProcessId(
            previousForeground, nullptr);
        const auto currentThread = GetCurrentThreadId();
        const auto attached = foregroundThread != 0 &&
            foregroundThread != currentThread &&
            AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
        SetForegroundWindow(previousForeground);
        if (attached) AttachThreadInput(currentThread, foregroundThread, FALSE);

        if (GetWindowPlacement(window, &placement)) {
            placement.flags &= ~WPF_RESTORETOMAXIMIZED;
            placement.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(window, &placement);
        }
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOWNOACTIVATE);
        sendToBackNoActivate(window);
    }

    return IsWindowVisible(window) != FALSE &&
        IsIconic(window) == FALSE &&
        GetForegroundWindow() != window;
}

bool showOffscreenNoActivatePreservingForeground(HWND window)
{
    if (window == nullptr) return false;
    const auto previousForeground = GetForegroundWindow();
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return false;
    const auto width = std::max(
        960L,
        placement.rcNormalPosition.right - placement.rcNormalPosition.left);
    const auto height = std::max(
        640L,
        placement.rcNormalPosition.bottom - placement.rcNormalPosition.top);
    placement.flags &= ~WPF_RESTORETOMAXIMIZED;
    placement.showCmd = SW_SHOWNOACTIVATE;
    placement.rcNormalPosition = {
        -32000, -32000, -32000 + width, -32000 + height};
    SetWindowPlacement(window, &placement);
    SetWindowPos(
        window,
        HWND_BOTTOM,
        placement.rcNormalPosition.left,
        placement.rcNormalPosition.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    if (GetForegroundWindow() == window &&
            previousForeground != nullptr &&
            previousForeground != window &&
            IsWindow(previousForeground)) {
        SetForegroundWindow(previousForeground);
        sendToBackNoActivate(window);
    }
    return IsWindowVisible(window) != FALSE && IsIconic(window) == FALSE;
}

bool captureWindowBmp(HWND window, const std::filesystem::path& output,
    bool composed = false)
{
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    if (composed) {
        if (!IsWindowVisible(window) || IsIconic(window) ||
            !GetClientRect(window,&bounds)) return false;
        MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&bounds),2);
        struct Visibility {HWND target;RECT area;bool found=false,valid=true;} visible{window,bounds};
        EnumWindows([](HWND other,LPARAM data)->BOOL{
            auto& state=*reinterpret_cast<Visibility*>(data);
            if(other==state.target){state.found=true;return FALSE;}
            if(!IsWindowVisible(other)||IsIconic(other))return TRUE;
            RECT rectangle{},overlap{};DWORD owner=0,targetOwner=0;
            GetWindowThreadProcessId(other,&owner);GetWindowThreadProcessId(state.target,&targetOwner);
            if(owner!=targetOwner&&GetWindowRect(other,&rectangle)&&
                IntersectRect(&overlap,&rectangle,&state.area)){
                state.valid=false;return FALSE;
            }
            return TRUE;
        },reinterpret_cast<LPARAM>(&visible));
        if(!visible.found||!visible.valid)return false;
    }
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;

    const auto screen = GetDC(nullptr);
    const auto memory = screen != nullptr ? CreateCompatibleDC(screen) : nullptr;
    const auto bitmap = memory != nullptr
        ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (screen == nullptr || memory == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory != nullptr) DeleteDC(memory);
        if (screen != nullptr) ReleaseDC(nullptr, screen);
        return false;
    }
    const auto previous = SelectObject(memory, bitmap);
    PatBlt(memory, 0, 0, width, height, BLACKNESS);
    // QA screenshots describe the visible viewport. PW_RENDERFULLCONTENT asks
    // scrollable child windows to paint off-viewport content into the same
    // target and can manufacture duplicate native controls that were never on
    // screen, defeating the clipping checks these captures are used for.
    const auto printed = composed
        ? BitBlt(memory,0,0,width,height,screen,bounds.left,bounds.top,SRCCOPY)!=FALSE
        : PrintWindow(window, memory, 0) != FALSE;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto stride = static_cast<std::size_t>(width) * 4U;
    std::vector<std::uint8_t> pixels(stride * static_cast<std::size_t>(height));
    // Never fall back to a desktop BitBlt here. That captures whichever window
    // happens to cover the target and can interrupt or leak the user's work.
    const auto converted = printed && GetDIBits(memory, bitmap, 0,
        static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS) != 0;

    bool hasVisiblePixels = false;
    if (converted) {
        const auto pixelCount = pixels.size() / 4U;
        const auto required = std::max<std::size_t>(16U, pixelCount / 1000U);
        std::size_t visible = 0;
        for (std::size_t index = 0; index < pixels.size(); index += 4U) {
            if (pixels[index] > 8U || pixels[index + 1U] > 8U ||
                    pixels[index + 2U] > 8U) {
                if (++visible >= required) {
                    hasVisiblePixels = true;
                    break;
                }
            }
        }
    }

    bool written = false;
    if (converted && hasVisiblePixels) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (stream) {
            stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
            stream.write(reinterpret_cast<const char*>(&info.bmiHeader),
                sizeof(info.bmiHeader));
            stream.write(reinterpret_cast<const char*>(pixels.data()),
                static_cast<std::streamsize>(pixels.size()));
            written = stream.good();
        }
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return written;
}

std::wstring symbolizeAddress(HANDLE process, DWORD64 address)
{
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    std::wstring moduleName = L"unknown";
    if (SymGetModuleInfo64(process, address, &module) && module.ModuleName[0]) {
        moduleName.assign(module.ModuleName,
            module.ModuleName + std::strlen(module.ModuleName));
    }

    std::array<std::byte, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    std::wostringstream output;
    output << moduleName << L'!';
    if (SymFromAddr(process, address, &displacement, symbol)) {
        output << std::wstring(symbol->Name, symbol->Name + symbol->NameLen);
        if (displacement != 0) output << L"+0x" << std::hex << displacement;
    } else {
        output << L"0x" << std::hex << address;
    }
    return output.str();
}

int sampleUiThread(HWND window, int seconds)
{
    DWORD processId = 0;
    const auto threadId = GetWindowThreadProcessId(window, &processId);
    if (threadId == 0 || processId == 0) return 8;

    const auto process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId);
    const auto thread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
        FALSE,
        threadId);
    if (process == nullptr || thread == nullptr) {
        if (thread != nullptr) CloseHandle(thread);
        if (process != nullptr) CloseHandle(process);
        return 8;
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    const auto appDirectory=std::filesystem::path(processExecutable(processId)).parent_path();
    const auto symbols=(appDirectory / L"profile/user-components-x64/foo_nekorox").wstring()+L";"+appDirectory.wstring();
    if (!SymInitializeW(process, symbols.c_str(), TRUE)) {
        CloseHandle(thread);
        CloseHandle(process);
        return 8;
    }

    std::map<std::wstring, std::uint64_t> leaves;
    std::map<std::wstring, std::uint64_t> stacks;
    std::uint64_t captured = 0;
    std::uint64_t failed = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<DWORD64> addresses;
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            ++failed;
            break;
        }

        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            addresses.push_back(context.Rip);
            for (int depth = 1; depth < 64; ++depth) {
                if (!StackWalk64(
                        IMAGE_FILE_MACHINE_AMD64,
                        process,
                        thread,
                        &frame,
                        &context,
                        nullptr,
                        SymFunctionTableAccess64,
                        SymGetModuleBase64,
                        nullptr) ||
                        frame.AddrPC.Offset == 0) {
                    break;
                }
                if (frame.AddrPC.Offset != addresses.back()) {
                    addresses.push_back(frame.AddrPC.Offset);
                }
            }
        } else {
            ++failed;
        }
        ResumeThread(thread);

        if (!addresses.empty()) {
            ++captured;
            std::vector<std::wstring> symbols;
            symbols.reserve(addresses.size());
            for (const auto address : addresses) {
                symbols.push_back(symbolizeAddress(process, address));
            }
            ++leaves[symbols.front()];
            std::wstring stack;
            // Long graphics-driver stalls frequently need more than twelve
            // frames before the owning Goden/D2D call becomes visible.
            const auto count = std::min<std::size_t>(symbols.size(), 64U);
            for (std::size_t index = 0; index < count; ++index) {
                if (index != 0) stack += L" <- ";
                stack += symbols[index];
            }
            ++stacks[stack];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto printTop = [](const auto& counts, const wchar_t* prefix) {
        std::vector<std::pair<std::wstring, std::uint64_t>> rows(
            counts.begin(), counts.end());
        std::sort(rows.begin(), rows.end(), [](const auto& left,
            const auto& right) { return left.second > right.second; });
        const auto limit = std::min<std::size_t>(rows.size(), 20U);
        for (std::size_t index = 0; index < limit; ++index) {
            std::wcout << prefix << L" count=" << rows[index].second
                << L" " << rows[index].first << L'\n';
        }
    };
    std::wcout << L"UI_STACK_SAMPLES seconds=" << seconds
        << L" captured=" << captured << L" failed=" << failed << L'\n';
    printTop(leaves, L"LEAF");
    printTop(stacks, L"STACK");

    SymCleanup(process);
    CloseHandle(thread);
    CloseHandle(process);
    return captured != 0 ? 0 : 8;
}

int watchUiStalls(HWND window, int seconds)
{
    DWORD processId = 0;
    const auto threadId = GetWindowThreadProcessId(window, &processId);
    if (threadId == 0 || processId == 0) return 8;
    const auto mappingName = std::wstring(
        nekorox::kPerformanceEventMappingPrefixV2) +
        std::to_wstring(processId);
    const auto mapping = OpenFileMappingW(
        FILE_MAP_READ, FALSE, mappingName.c_str());
    if (mapping == nullptr) return 8;
    const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0,
        sizeof(nekorox::PerformanceEventRingHeaderV2));
    if (view == nullptr) {
        CloseHandle(mapping);
        return 8;
    }
    const auto* header = static_cast<
        const nekorox::PerformanceEventRingHeaderV2*>(view);
    if (header->magic != nekorox::kPerformanceEventMagicV2 ||
        header->ownerPid != processId || header->qpcFrequency == 0) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 8;
    }
    const auto process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    const auto thread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
            THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (process == nullptr || thread == nullptr) {
        if (thread != nullptr) CloseHandle(thread);
        if (process != nullptr) CloseHandle(process);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 8;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    const auto appDirectory=std::filesystem::path(processExecutable(processId)).parent_path();
    const auto symbols=(appDirectory / L"profile/user-components-x64/foo_nekorox").wstring()+L";"+appDirectory.wstring();
    if (!SymInitializeW(process, symbols.c_str(), TRUE)) {
        CloseHandle(thread);
        CloseHandle(process);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 8;
    }

    unsigned captured = 0;
    std::uint64_t lastHeartbeat = 0;
    bool capturedForHeartbeat = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE)
            break;
        const auto heartbeat = *reinterpret_cast<volatile const std::uint64_t*>(
            &header->heartbeatQpc);
        if (heartbeat != lastHeartbeat) {
            lastHeartbeat = heartbeat;
            capturedForHeartbeat = false;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (heartbeat != 0 && now.QuadPart > 0 &&
                static_cast<std::uint64_t>(now.QuadPart) > heartbeat &&
                !capturedForHeartbeat) {
            const auto gapMs = static_cast<double>(
                static_cast<std::uint64_t>(now.QuadPart) - heartbeat) *
                1000.0 / static_cast<double>(header->qpcFrequency);
            if (gapMs >= 150.0) {
                capturedForHeartbeat = true;
                std::vector<DWORD64> addresses;
                if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &context)) {
                        STACKFRAME64 frame{};
                        frame.AddrPC.Offset = context.Rip;
                        frame.AddrPC.Mode = AddrModeFlat;
                        frame.AddrFrame.Offset = context.Rbp;
                        frame.AddrFrame.Mode = AddrModeFlat;
                        frame.AddrStack.Offset = context.Rsp;
                        frame.AddrStack.Mode = AddrModeFlat;
                        addresses.push_back(context.Rip);
                        for (int depth = 1; depth < 64; ++depth) {
                            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                                    process, thread, &frame, &context,
                                    nullptr, SymFunctionTableAccess64,
                                    SymGetModuleBase64, nullptr) ||
                                    frame.AddrPC.Offset == 0) break;
                            if (frame.AddrPC.Offset != addresses.back())
                                addresses.push_back(frame.AddrPC.Offset);
                        }
                    }
                    ResumeThread(thread);
                }
                std::wcout << L"UI_STALL gap_ms=" << std::fixed <<
                    std::setprecision(2) << gapMs << L" sequence=" <<
                    header->latestSequence << L" stack=";
                for (std::size_t index = 0; index < addresses.size(); ++index) {
                    if (index != 0) std::wcout << L" <- ";
                    std::wcout << symbolizeAddress(process, addresses[index]);
                }
                std::wcout << std::endl;
                ++captured;
            }
        }
        Sleep(5);
    }
    std::wcout << L"UI_STALL_WATCH seconds=" << seconds <<
        L" captured=" << captured << std::endl;
    SymCleanup(process);
    CloseHandle(thread);
    CloseHandle(process);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Coordinate injection must use the same physical/DIP transform as foobar.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const bool background = argc >= 4 &&
        _wcsicmp(argv[argc - 1], L"--background") == 0;
    const int actionArgc = argc - (background ? 1 : 0);
    if (actionArgc != 3 && actionArgc != 4) {
        std::wcerr << L"Usage: nekorox_ui_probe <foobar2000.exe> "
            L"<np|listen|play|stop|previous|next|history|filter|inf|bio|lyr|art|lib|"
            L"play-first-album|play-first-library-after-temporary|"
            L"favorite|note-open|note-set|note-clear|note-capture|"
            L"tag-open|tag-state|tag-close|tag-capture|tag-scroll-top|tag-scroll-bottom|tag-scroll-cycle|tag-resize-cycle|tag-keyboard-focus|tag-roots-state|tag-drop|tag-set-title|tag-set-destination|tag-set-artwork-front|tag-artwork-front-mode|tag-mode|tag-collision|tag-preview-copy|tag-execute-copy|tag-apply|tag-undo|"
            L"options-open|options-capture|options-scroll-top|options-scroll-bottom|options-scroll-cycle|options-resize-cycle|options-keyboard-focus|options-affordance-capture|options-state|options-close|motion-mode|listen-effects-mode|ambient-mode|presence-enable|presence-disable|paused-show|paused-hide|covers-enable|covers-disable|daw-open|daw-capture|daw-drop|daw-state|daw-storage-state|daw-lifecycle|daw-position|daw-monitor-state|daw-meter-state|daw-play-toggle|daw-stop|daw-seek|daw-remove|daw-split|daw-clone-offset|daw-mute-left|daw-mute-right|daw-close|"
            L"persona-open|persona-profile|persona-aliases|persona-relations|"
            L"persona-set|persona-map|persona-capture|"
            L"app-close|window-maximize|window-native-maximize|header-double-click-maximize|window-minimize|window-hide|"
            L"window-background-show|window-offscreen-show|"
            L"window-offscreen-restore|window-visible-focus|"
            L"window-state|window-dpi-state|window-resize|window-resize-transaction|cover-state|cover-redraw|"
            L"shell-deck-resize|shell-library|shell-album|shell-album-scroll-bound|benchmark-album-scroll-input|benchmark-album-lyrics-scroll-input|benchmark-curated-lyrics-scroll-input|shell-album-transition-state|shell-album-ambient-state|shell-album-progress|shell-album-scroll-telemetry|shell-c10-diagnostics|shell-c6-history-state|shell-collection-select|shell-collection-original|shell-collection-music|shell-a4-state|shell-a4-diagnostics|shell-a4-r12-diagnostics|shell-a4-cap-light|shell-a4-cap-field|shell-a4-retry|shell-a4-select|shell-flow-select|shell-a4-v2-refined|shell-a4-v2-hybrid|shell-a4-v2-show|shell-a4-v2-large|shell-a4-grain-off|shell-a4-grain-fine|shell-a4-grain-strong|shell-a4-quality-full|shell-a4-quality-auto|shell-a4-quality-light|shell-a4-quality-field|shell-curated-state|shell-curated-caustics|shell-curated-wave|shell-curated-original|shell-curated-music|shell-curated-c3|shell-curated-c2|shell-curated-speed1|shell-curated-speed15|shell-curated-speed2|shell-curated-palette-original|shell-curated-palette-album|shell-curated-restart|shell-a4-material-legacy|shell-a4-material-clovis|shell-album-booklet-toggle|shell-album-booklet-capture|"
            L"menu-capture|transport-state|playback-state|repeat-toggle|shuffle-toggle|"
            L"mute-toggle|volume-set|library-count|"
            L"file-open-state|workspace-state|workspace-render-state|temporary-workspace-state|"
            L"play-library-outside-temporary|workspace-query|workspace-accessibility|workspace-rating|workspace-rating-toggle|workspace-rating-state|frame-clock-state|"
            L"waveseek-state|benchmark-waveseek-first-pixel|"
            L"waveseek-transition-capture|waveseek-hover-capture|benchmark-waveseek-hover|waveseek-pointer-tracking|waveseek-seek-percent|input-routing-state|"
            L"sample-ui-stacks|watch-ui-stalls|capture-hang-dump|album-accessibility|album-keyboard|album-menu-race|"
            L"window-capture|window-tree|window-zorder|window-redraw|occlusion-input-recovery|"
            L"scroll-results|scroll-artist|scroll-album|verify-facet-scroll-preservation|compact-facet-refresh|"
            L"filter-click-refresh|drag-results-scrollbar|drag-artist-scrollbar|drag-album-scrollbar|"
            L"verify-random-album-now-playing-reveal|"
            L"benchmark-scroll-input|benchmark-scroll|"
            L"workpack-wheel-driver|performance-events|"
            L"drop-file|"
            L"benchmark-np|"
            L"listen-overview|listen-live|listen-map|"
            L"listen-portrait|listen-mode-toggle|listen-scope-state|listen-scope-120|listen-gain-2|listen-focus-scope|listen-focus-crt|listen-focus-spectrum|listen-focus-spectrogram|listen-focus-return|listen-maximize|listen-analyze|"
            L"listen-favorite|listen-close|listen-minimize|listen-hide|"
            L"listen-show-noactivate|listen-analysis-state|"
            L"listen-artwork-state|"
            L"listen-capture|listen-window-capture|listen-window-state|listen-place|"
            L"listen-crt-stats|mute|mute-on> [output] [--background]\n";
        return 2;
    }

    wchar_t fullPath[32768]{};
    if (GetFullPathNameW(argv[1], static_cast<DWORD>(std::size(fullPath)),
            fullPath, nullptr) == 0) {
        std::wcerr << L"Could not normalize executable path\n";
        return 3;
    }
    const std::wstring action = argv[2];
    SearchContext topLevel{fullPath, nullptr, false};
    topLevel.allowHidden = background;
    // Minimized main windows have a small rectangle. Without the same
    // descendant filter used by window-state, restore could select a larger
    // hidden SDK/SMTC helper window and report success for the wrong HWND.
    topLevel.requiredDescendantClass = L"NekoRoXHeaderV3";
    EnumWindows(findTopLevel, reinterpret_cast<LPARAM>(&topLevel));
    if (topLevel.result == nullptr) {
        std::wcerr << L"Target foobar2000 window was not found\n";
        return 4;
    }
    if(action==L"benchmark-open-latency"){
        if(actionArgc!=4)return 2;
        ChildContext shell{L"GodenPlayerShellV2"},transport{L"NekoRoXTransportV3"},cover{L"NekoRoXCoverV3"};
        EnumChildWindows(topLevel.result,findChild,reinterpret_cast<LPARAM>(&shell));
        EnumChildWindows(topLevel.result,findChild,reinterpret_cast<LPARAM>(&transport));
        EnumChildWindows(topLevel.result,findChild,reinterpret_cast<LPARAM>(&cover));
        const auto sm=RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        const auto tm=RegisterWindowMessageW(nekorox::kTransportStateMessageV1);
        const auto cm=RegisterWindowMessageW(nekorox::kCoverStateMessageV1);
        const auto query=[](HWND hwnd,UINT message,WPARAM field)->std::optional<DWORD_PTR>{DWORD_PTR value=0;
            if(!hwnd||!SendMessageTimeoutW(hwnd,message,field,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&value)||
                static_cast<LRESULT>(value)==E_ACCESSDENIED)return {};return value;};
        const auto mode=query(shell.result,sm,2),pcm=query(shell.result,sm,35),stream=query(shell.result,sm,39),track=query(transport.result,tm,7);
        if(!mode||!pcm||!stream||!track)return 8;
        const bool album=*mode!=0;const auto art=query(album?shell.result:cover.result,album?sm:cm,album?19:5);
        if(!art)return 8;
        auto command=L"\""+std::wstring(fullPath)+L"\" /nekorox-open \""+std::wstring(argv[3])+L"\"";
        STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION child{};const auto start=std::chrono::steady_clock::now();
        if(!CreateProcessW(fullPath,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child))return 8;
        CloseHandle(child.hThread);
        double pcmMs=-1,trackMs=-1,artMs=-1,dispatchMs=-1;
        const auto elapsed=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
        while(elapsed()<10000){
            if(dispatchMs<0&&WaitForSingleObject(child.hProcess,0)==WAIT_OBJECT_0)dispatchMs=elapsed();
            const auto currentTrack=query(transport.result,tm,7),currentPcm=query(shell.result,sm,35),currentStream=query(shell.result,sm,39);
            if(!currentTrack||!currentPcm||!currentStream){CloseHandle(child.hProcess);return 8;}
            if(trackMs<0&&*currentTrack!=*track&&*currentTrack)trackMs=elapsed();
            if(pcmMs<0&&trackMs>=0&&*currentStream!=*stream&&*currentPcm>*pcm)pcmMs=elapsed();
            const auto currentArt=query(album?shell.result:cover.result,album?sm:cm,album?19:5);
            if(!currentArt){CloseHandle(child.hProcess);return 8;}
            if(artMs<0&&trackMs>=0&&*currentArt>*art)artMs=elapsed();
            if(pcmMs>=0&&artMs>=0&&dispatchMs>=0)break;
            Sleep(5);
        }
        CloseHandle(child.hProcess);
        std::wcout<<L"OPEN_LATENCY mode="<<(album?L"album":L"library")<<L" commandProcessMs="<<dispatchMs
            <<L" newTrackMs="<<trackMs<<L" firstPCMDetectedMs="<<pcmMs<<L" artworkTransitionMs="<<artMs
            <<L" pollMs=5 audibleOutputMeasured=false\n";
        return pcmMs>=0&&artMs>=0?0:8;
    }
    if (action == L"window-composed-capture") {
        if(actionArgc!=4)return 2;
        if(!captureWindowBmp(topLevel.result,std::filesystem::path(argv[3]),true)){
            std::wcerr<<L"Composed capture refused: target not fully visible\n";return 10;
        }
        std::wcout<<L"Captured composed client without repaint or input\n";return 0;
    }
    if (action == L"input-routing-state") {
        DWORD pid = 0;
        const auto thread = GetWindowThreadProcessId(topLevel.result, &pid);
        GUITHREADINFO gui{sizeof(gui)};
        if (!GetGUIThreadInfo(thread, &gui)) return 8;
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return 8;
        const auto hit = WindowFromPoint(cursor);
        wchar_t hitClass[128]{}, captureClass[128]{};
        GetClassNameW(hit, hitClass, static_cast<int>(std::size(hitClass)));
        GetClassNameW(gui.hwndCapture, captureClass,
            static_cast<int>(std::size(captureClass)));
        std::wcout << L"INPUT_ROUTING pid=" << pid
            << L" enabled=" << IsWindowEnabled(topLevel.result)
            << L" foreground=" << GetForegroundWindow()
            << L" active=" << gui.hwndActive << L" focus=" << gui.hwndFocus
            << L" capture=" << gui.hwndCapture << L" captureClass=" << captureClass
            << L" menuOwner=" << gui.hwndMenuOwner << L" moveSize=" << gui.hwndMoveSize
            << L" flags=" << gui.flags << L" hit=" << hit
            << L" hitClass=" << hitClass << L" cursor=" << cursor.x << L"," << cursor.y << L'\n';
        return 0;
    }
    if (action == L"shell-frame-delivery-state") {
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&shell));
        const auto message = RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        DWORD_PTR state = 0, generation = 0;
        if (!shell.result || !SendMessageTimeoutW(shell.result, message, 239, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &state) ||
                !SendMessageTimeoutW(shell.result, message, 240, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &generation)) return 8;
        std::wcout << L"FRAME_DELIVERY flags=" << (state & 0xffffffff) << L" epoch=" << (state >> 32)
            << L" noticeEpoch=" << generation << L'\n'; return 0;
    }
    if (action == L"header-paint-state") {
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&header));
        const auto message = RegisterWindowMessageW(L"Goden.HeaderPaintQa.V1");
        DWORD_PTR counts[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            if (!header.result || !SendMessageTimeoutW(header.result, message, i, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &counts[i]) ||
                static_cast<LRESULT>(counts[i]) == E_ACCESSDENIED) return 8;
        }
        std::wcout << L"HEADER_PAINT full=" << counts[0] << L" partial=" << counts[1]
            << L" ticks=" << counts[2] << L'\n';
        return 0;
    }
    if (action == L"shell-fast-mode-credit") {
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&shell));
        const auto message=RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        const auto read=[&](WPARAM field,DWORD_PTR& value){return shell.result &&
            SendMessageTimeoutW(shell.result,message,field,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&value);};
        DWORD_PTR value=0;
        if(!read(241,value)||value!=TRUE)return 8;
        bool completed=false;
        for(unsigned attempt=0;attempt<200;++attempt){
            if(!read(242,value))return 8;
            if(static_cast<LRESULT>(value)!=-1){completed=true;break;}
            Sleep(10);
        }
        const bool pass=completed&&value==TRUE;
        std::wcout<<L"FAST_MODE_HELD_CREDIT "<<(pass?L"PASS":L"FAIL")<<L" sameUiDispatch=true\n";
        return pass?0:8;
    }
    if (action == L"shell-collection-render-state") {
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&shell));
        const auto message=RegisterWindowMessageW(L"Goden.CollectionQa.V1");
        DWORD_PTR values[7]{};
        constexpr WPARAM fields[]{3,4,5,10,11,13,15};
        bool consistent=false;
        for(unsigned attempt=0;attempt<8&&!consistent;++attempt){
            DWORD_PTR before=0,after=0;
            if(!shell.result || !SendMessageTimeoutW(shell.result,message,3,0,
                SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&before))return 8;
            for(unsigned i=0;i<7;++i) if(!SendMessageTimeoutW(shell.result,message,fields[i],0,
                SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&values[i]) || static_cast<LRESULT>(values[i])==E_ACCESSDENIED) return 8;
            if(!SendMessageTimeoutW(shell.result,message,3,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&after))return 8;
            consistent=before==after&&values[0]==after;
        }
        if(!consistent){std::wcerr<<L"No consistent render sample\n";return 8;}
        std::wcout<<L"COLLECTION_RENDER produced="<<values[0]<<L" drawn="<<values[1]<<L" accepted="<<values[2]
            <<L" active="<<values[3]<<L" budget="<<values[4]<<L" hr="<<static_cast<LRESULT>(values[5])
            <<L" gpuP95Us="<<double(values[6])/1000.0<<L'\n';return 0;
    }
    if (action == L"observe-album-pixels" || action == L"observe-deck-coverage" ||
            action == L"record-deck-reference") {
        // Read already composed pixels, without PrintWindow/WM_PRINT/redraw.
        // Abort before capture when another app covers any of the sampled
        // viewport. This probe must never turn a stale target into a paint.
        const bool recordDeck = action == L"record-deck-reference";
        const bool deckCoverage = action != L"observe-album-pixels";
        if (deckCoverage && actionArgc != 4) return 2;
        ChildContext shell{L"GodenPlayerShellV2"}, transport{L"NekoRoXTransportV3"}, wave{L"NekoRoXWaveSeekV3"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&shell));
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&transport));
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&wave));
        if (!shell.result || !transport.result) return 5;
        RECT bounds{}, deck{}; GetWindowRect(shell.result, &bounds); GetWindowRect(transport.result, &deck);
        bounds.top += MulDiv(38, GetDpiForWindow(shell.result), 96);
        bounds.bottom = deck.top - 2;
        if (deckCoverage) {if(!wave.result)return 5;GetWindowRect(wave.result,&bounds);}
        const auto width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) return 8;
        const auto screen = GetDC(nullptr);
        const auto memory = screen ? CreateCompatibleDC(screen) : nullptr;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const auto bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
        if (!bitmap) {if(memory)DeleteDC(memory);if(screen)ReleaseDC(nullptr,screen);return 8;}
        const auto previous = SelectObject(memory, bitmap);
        const auto cleanup = [&] {SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);};
        std::vector<std::uint8_t> last(static_cast<std::size_t>(width) * height * 4);
        std::vector<std::uint8_t> reference(last.size());
        if (deckCoverage && !recordDeck) {
            std::ifstream input(std::filesystem::path(argv[3]), std::ios::binary);
            std::array<std::int32_t,2> dimensions{};
            input.read(reinterpret_cast<char*>(dimensions.data()), sizeof(dimensions));
            input.read(reinterpret_cast<char*>(reference.data()), reference.size());
            if (!input || dimensions[0]!=width || dimensions[1]!=height) {cleanup();return 8;}
        }
        const auto seconds = deckCoverage ? 8 :
            std::clamp(actionArgc == 4 ? _wtoi(argv[3]) : 8, 2, 120);
        unsigned samples = 0, moving = 0;
        auto lastChange = GetTickCount64(), start = lastChange;
        ULONGLONG largestGap = 0;
        unsigned covered = 0;
        while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000) {
            struct Visibility {HWND target;RECT rect;bool visible=true;bool found=false;HWND obscurer=nullptr;} visibility{topLevel.result,bounds};
            EnumWindows([](HWND window,LPARAM parameter)->BOOL {
                auto& state=*reinterpret_cast<Visibility*>(parameter);
                if(window==state.target){state.found=true;return FALSE;}
                wchar_t className[128]{};GetClassNameW(window,className,128);
                const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
                if(std::wstring_view(className)==L"CodexComputerUseCursorOverlay" &&
                    (style&WS_EX_LAYERED) && (style&WS_EX_TRANSPARENT)) return TRUE;
                RECT rectangle{},overlap{};
                if(IsWindowVisible(window)&&!IsIconic(window)&&GetWindowRect(window,&rectangle)&&
                    IntersectRect(&overlap,&state.rect,&rectangle)) {state.visible=false;state.obscurer=window;return FALSE;}
                return TRUE;
            },reinterpret_cast<LPARAM>(&visibility));
            const bool visible = visibility.visible && visibility.found;
            if (!visible) {
                wchar_t name[128]{};GetClassNameW(visibility.obscurer,name,128);
                RECT obscurerBounds{};GetWindowRect(visibility.obscurer,&obscurerBounds);
                DWORD obscurerPid=0;GetWindowThreadProcessId(visibility.obscurer,&obscurerPid);
                cleanup();std::wcerr<<L"PIXEL_OBSERVATION obscured class="<<name<<L" foreground="<<GetForegroundWindow()
                    <<L" candidate="<<topLevel.result<<L" pid="<<obscurerPid
                    <<L" rect="<<obscurerBounds.left<<L","<<obscurerBounds.top<<L","<<obscurerBounds.right<<L","<<obscurerBounds.bottom
                    <<L" exe="<<processExecutable(obscurerPid)<<L"; no external pixels captured\n";return 10;
            }
            if (!BitBlt(memory,0,0,width,height,screen,bounds.left,bounds.top,SRCCOPY) ||
                    !GdiFlush()) {cleanup();return 8;}
            const auto data = static_cast<const std::uint8_t*>(pixels);
            if(deckCoverage){
                // Compare the entire rail interior to an accepted, paused
                // reference. Borders alone cannot qualify a black rectangle.
                const int rail=std::min<int>(width/3,MulDiv(220,GetDpiForWindow(wave.result),96));
                std::size_t lit=0, mismatched=0, total=0;
                // Exclude the cost ribbon: this proof requires waveform
                // body pixels below it, rather than a lit upper border.
                const int waveformTop=std::min<int>(height-3,MulDiv(12,GetDpiForWindow(wave.result),96));
                for(int y=waveformTop;y<height-2;++y) for(int x=width-rail;x<width-2;++x){
                    const auto i=(static_cast<std::size_t>(y)*width+x)*4;
                    if(data[i]+data[i+1]+data[i+2]>36)++lit;
                    if(std::abs(int(data[i])-int(reference[i]))>3 ||
                        std::abs(int(data[i+1])-int(reference[i+1]))>3 ||
                        std::abs(int(data[i+2])-int(reference[i+2]))>3)++mismatched;
                    ++total;
                }
                if(recordDeck){
                    if(!total || lit<total/10){cleanup();std::wcerr<<L"Reference lacks interior waveform\n";return 8;}
                    std::ofstream output(std::filesystem::path(argv[3]),std::ios::binary);
                    const std::array<std::int32_t,2> dimensions{width,height};
                    output.write(reinterpret_cast<const char*>(dimensions.data()),sizeof(dimensions));
                    output.write(reinterpret_cast<const char*>(data),last.size());
                    const bool saved=bool(output);cleanup();
                    std::wcout<<L"DECK_REFERENCE "<<(saved?L"PASS":L"FAIL")<<L" interiorLit="<<lit<<L" pixels="<<total<<L'\n';
                    return saved?0:8;
                }
                if(total && mismatched<=total/1000)++covered;
            }
            std::size_t changed = 0;
            if (samples) for (std::size_t i=0;i<last.size();i+=4) {
                if (std::abs(int(data[i])-int(last[i]))>1 || std::abs(int(data[i+1])-int(last[i+1]))>1 ||
                    std::abs(int(data[i+2])-int(last[i+2]))>1) ++changed;
            }
            const auto now = GetTickCount64();
            if (changed >= 32) {++moving;largestGap=std::max(largestGap,now-lastChange);lastChange=now;}
            std::copy(data,data+last.size(),last.begin());
            ++samples; Sleep(100);
        }
        largestGap=std::max(largestGap,GetTickCount64()-lastChange);
        cleanup();
        const bool pass = deckCoverage ? covered>=samples*9/10 : moving >= samples/2 && largestGap < 1500;
        std::wcout<<(deckCoverage?L"DECK_COMPOSED_COVERAGE ":L"ALBUM_COMPOSED_PIXELS ")<<(pass?L"PASS":L"FAIL")<<L" seconds="<<seconds
            <<L" samples="<<samples<<L" moving="<<moving<<L" maxUnchangedMs="<<largestGap
            <<L" railCovered="<<covered
            <<L" noTargetPaintOrInput=true\n";
        return pass ? 0 : 8;
    }
    if (action == L"album-transition-input-recovery") {
        if (!activateInputCandidate(topLevel.result)) return 8;
        ChildContext header{L"NekoRoXHeaderV3"}, transport{L"NekoRoXTransportV3"}, shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&header));
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&transport));
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&shell));
        if (!header.result || !transport.result || !shell.result) return 5;
        const auto transportMessage = RegisterWindowMessageW(nekorox::kTransportStateMessageV1);
        const auto shellMessage = RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        const auto query = [&](HWND target, UINT message, WPARAM field) -> std::optional<LRESULT> {
            DWORD_PTR value = 0;
            if (!SendMessageTimeoutW(target, message, field, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &value)) return {};
            return static_cast<LRESULT>(value);
        };
        POINT previous{}; const bool restore = GetCursorPos(&previous) != FALSE;
        const auto click = [&](HWND target, float x, float y, bool fromRight) {
            RECT bounds{}; GetClientRect(target, &bounds);
            const auto scale = GetDpiForWindow(target) / 96.f;
            POINT point{fromRight ? bounds.right - LONG(std::lround(x * scale)) : LONG(std::lround(x * scale)),
                y < 0 ? bounds.bottom + LONG(std::lround(y * scale)) : LONG(std::lround(y * scale))};
            if (!ClientToScreen(target, &point) || GetForegroundWindow() != topLevel.result ||
                    !SetCursorPos(point.x, point.y) || WindowFromPoint(point) != target) return false;
            INPUT down{}; down.type = INPUT_MOUSE; down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            auto up = down; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            INPUT inputs[]{down, up}; return SendInput(2, inputs, sizeof(INPUT)) == 2;
        };
        const auto wait = [&](auto predicate, double& elapsed) {
            const auto start = std::chrono::steady_clock::now();
            bool ready = false;
            do {if (predicate()) {ready = true; break;} Sleep(2);}
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2000));
            elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return ready;
        };
        const auto count = std::clamp(actionArgc == 4 ? _wtoi(argv[3]) : 12, 1, 40);
        bool valid = true; int completed = 0; double maximumMs = 0;
        const wchar_t* failure = L"none";
        for (int round = 0; round < count && valid; ++round) {
            double elapsed = 0;
            if (!click(shell.result, 54, 18, false) || !wait([&] {return query(shell.result, shellMessage, 2).value_or(-1) == 0;}, elapsed)) {
                failure = L"library-navigation"; valid = false; break;
            }
            const auto oldTrack = query(transport.result, transportMessage, 7);
            Sleep(GetDoubleClickTime() + 20);
            // Real next-song input immediately followed by real Album input.
            if (!oldTrack || !click(header.result, 17.5f, -14, true)) {
                failure = L"next-input-boundary"; valid = false; break;
            }
            Sleep(15);
            if (!click(shell.result, 162, 18, false) || !wait([&] {
                return query(shell.result, shellMessage, 2).value_or(-1) == 1 &&
                    query(transport.result, transportMessage, 7).value_or(*oldTrack) != *oldTrack &&
                    query(transport.result, transportMessage, 5).value_or(0) == 1;
            }, elapsed)) {failure = L"next-and-album"; valid = false; break;}
            maximumMs = std::max(maximumMs, elapsed);
            Sleep(150);
            for (int toggle = 0; toggle < 2 && valid; ++toggle) {
                const auto oldState = query(transport.result, transportMessage, 10);
                const auto injected = oldState && click(header.result, 45.5f, -14, true);
                if (!injected || !wait([&] {
                    return query(transport.result, transportMessage, 10).value_or(*oldState) != *oldState;
                }, elapsed)) {
                    failure = injected ? L"pause-resume-command" : L"pause-resume-input-boundary";
                    valid = false;
                    POINT cursor{}; GetCursorPos(&cursor);
                    std::wcout << L"PAUSE_FAILURE round=" << round << L" toggle=" << toggle << L" injected=" << injected
                        << L" old=" << oldState.value_or(-1) << L" now=" << query(transport.result, transportMessage, 10).value_or(-1)
                        << L" elapsedMs=" << elapsed << L" foreground=" << GetForegroundWindow() << L" candidate=" << topLevel.result
                        << L" hit=" << WindowFromPoint(cursor) << L" header=" << header.result << L'\n';
                    break;
                }
                maximumMs = std::max(maximumMs, elapsed);
                Sleep(GetDoubleClickTime() + 20);
            }
            for (int toggle = 0; toggle < 2 && valid; ++toggle) {
                const auto oldMute = query(transport.result, transportMessage, 0);
                RECT bounds{}; GetClientRect(transport.result, &bounds);
                const auto injected = oldMute && click(transport.result, 17.f, bounds.bottom * 48.f / GetDpiForWindow(transport.result), true);
                if (!injected || !wait([&] {
                    return ((query(transport.result, transportMessage, 0).value_or(*oldMute) ^ *oldMute) & nekorox::kTransportMutedBitV1) != 0;
                }, elapsed)) {
                    failure = injected ? L"mute-command" : L"mute-input-boundary"; valid = false;
                    POINT cursor{}; GetCursorPos(&cursor);
                    std::wcout << L"MUTE_FAILURE round=" << round << L" toggle=" << toggle << L" injected=" << injected
                        << L" old=" << oldMute.value_or(-1) << L" now=" << query(transport.result, transportMessage, 0).value_or(-1)
                        << L" elapsedMs=" << elapsed << L" foreground=" << GetForegroundWindow() << L" candidate=" << topLevel.result
                        << L" hit=" << WindowFromPoint(cursor) << L" transport=" << transport.result << L'\n';
                    break;
                }
                maximumMs = std::max(maximumMs, elapsed);
                Sleep(GetDoubleClickTime() + 20);
            }
            if (valid) ++completed;
        }
        if (restore) SetCursorPos(previous.x, previous.y);
        std::wcout << L"ALBUM_TRANSITION_INPUT " << (valid ? L"PASS" : L"FAIL")
            << L" rounds=" << completed << L" maximumMs=" << maximumMs << L" failure=" << failure << L'\n';
        return valid ? 0 : 8;
    }
    if (action == L"occlusion-input-recovery") {
        if (!activateInputCandidate(topLevel.result)) return 8;
        ChildContext header{L"NekoRoXHeaderV3"}, transport{L"NekoRoXTransportV3"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&header));
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&transport));
        if (!header.result || !transport.result) return 5;
        const auto message = RegisterWindowMessageW(nekorox::kTransportStateMessageV1);
        const auto query = [&](WPARAM field) -> std::optional<LRESULT> {
            DWORD_PTR value = 0;
            if (!SendMessageTimeoutW(transport.result, message, field, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &value)) return {};
            return static_cast<LRESULT>(value);
        };
        if (query(10).value_or(0) == 0) {
            std::wcerr << L"Requires a playing or paused track\n"; return 8;
        }
        const auto overlay = CreateWindowExW(0, L"STATIC", L"Goden overlap test",
            WS_OVERLAPPEDWINDOW, 100, 100, 600, 400, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!overlay) return 8;
        POINT previous{}; const bool restore = GetCursorPos(&previous) != FALSE;
        const auto click = [&](HWND target, float fromRight, bool headerBand) {
            RECT bounds{}; GetClientRect(target, &bounds);
            const auto dpi = static_cast<float>(GetDpiForWindow(target));
            POINT point{bounds.right - static_cast<LONG>(std::lround(fromRight * dpi / 96.f)),
                headerBand ? bounds.bottom - static_cast<LONG>(std::lround(14.f * dpi / 96.f)) : bounds.bottom / 2};
            if (!ClientToScreen(target, &point) || GetForegroundWindow() != topLevel.result ||
                    !SetCursorPos(point.x, point.y) || WindowFromPoint(point) != target) return false;
            INPUT inputs[2]{}; inputs[0].type = inputs[1].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            return SendInput(2, inputs, sizeof(INPUT)) == 2;
        };
        const auto count = std::clamp(actionArgc == 4 ? _wtoi(argv[3]) : 12, 1, 40);
        bool valid = true; int completed = 0; double maximumMs = 0;
        const wchar_t* failure = L"none";
        // A preceding probe may have clicked the same header button too.
        // Start with an independent gesture, not the second half of it.
        Sleep(GetDoubleClickTime() + 20);
        for (int round = 0; round < count && valid; ++round) {
            RECT bounds{}; GetWindowRect(topLevel.result, &bounds);
            ShowWindow(overlay, SW_SHOWNOACTIVATE);
            for (int step = 0; step < 10; ++step) {
                SetWindowPos(overlay, HWND_TOP, bounds.left + step * 40,
                    bounds.top + 50 + step * 10, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                Sleep(8);
            }
            ShowWindow(overlay, SW_HIDE);
            SetForegroundWindow(topLevel.result);
            const auto oldState = query(10), oldMute = query(0);
            if (!oldState || !oldMute || !click(header.result, 45.5f, true)) {
                failure = L"pause-input-boundary"; valid = false; break;
            }
            const auto start = std::chrono::steady_clock::now();
            bool paused = false;
            do {
                if (query(10).value_or(*oldState) != *oldState) { paused = true; break; }
                Sleep(2);
            } while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
            maximumMs = std::max(maximumMs, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count());
            if (!paused || !click(transport.result, 17.f, false)) {
                failure = paused ? L"mute-input-boundary" : L"pause-command";
                valid = false; break;
            }
            bool muted = false;
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto now = query(0);
                if (now && ((*now ^ *oldMute) & nekorox::kTransportMutedBitV1)) { muted = true; break; }
                Sleep(2);
            }
            valid = muted; if (valid) ++completed;
            if (!valid) failure = L"mute-command";
            // Header deliberately executes only the first click of a Windows
            // double-click. Separate independent pause toggles accordingly.
            if (round + 1 < count) Sleep(GetDoubleClickTime() + 20);
        }
        DestroyWindow(overlay);
        if (restore) SetCursorPos(previous.x, previous.y);
        std::wcout << L"OCCLUSION_INPUT_RECOVERY " << (valid ? L"PASS" : L"FAIL")
            << L" rounds=" << completed << L" pauseMaxMs=" << maximumMs
            << L" noScroll=true failure=" << failure
            << L" foreground=" << GetForegroundWindow() << L" candidate=" << topLevel.result << L'\n';
        return valid ? 0 : 8;
    }

    if (action == L"window-background-show") {
        if (!showNoActivatePreservingForeground(topLevel.result)) {
            std::wcerr << L"Main window could not be shown without activation\n";
            return 7;
        }
        std::wcout << L"Background-shown main window on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-offscreen-show") {
        if (!showOffscreenNoActivatePreservingForeground(topLevel.result)) {
            std::wcerr << L"Main window could not be shown offscreen\n";
            return 7;
        }
        std::wcout << L"Offscreen-shown main window on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-visible-focus") {
        // Present the window on a real desktop position and make it the
        // foreground window. Injected wheel input and DXGI frame latency both
        // depend on this: an offscreen or unoccluded-free window never
        // reproduces the presentation pacing a user actually sees.
        const auto previousForeground = GetForegroundWindow();
        ShowWindow(topLevel.result, SW_RESTORE);
        RECT bounds{};
        if (!GetWindowRect(topLevel.result, &bounds)) {
            std::wcerr << L"Main window bounds could not be read\n";
            return 7;
        }
        const auto width = std::max<LONG>(960, bounds.right - bounds.left);
        const auto height = std::max<LONG>(640, bounds.bottom - bounds.top);
        SetWindowPos(topLevel.result, HWND_TOP, 40, 40,
            width, height, SWP_SHOWWINDOW);
        SetForegroundWindow(topLevel.result);
        SetActiveWindow(topLevel.result);
        for (unsigned attempt = 0;
                attempt < 50 && GetForegroundWindow() != topLevel.result;
                ++attempt) {
            Sleep(20);
            SetForegroundWindow(topLevel.result);
        }
        const auto focused = GetForegroundWindow() == topLevel.result;
        // WM_MOUSEWHEEL follows the keyboard focus and DefWindowProc only
        // propagates it upwards, so a shell that does not own focus never sees
        // the wheel. A user clicking in Album mode gives it focus explicitly;
        // injected input has to do the same or the burst is silently dropped.
        auto focusNote = std::wstring(L"shellFocus=absent");
        const auto skipFocus = actionArgc == 4 &&
            _wcsicmp(argv[3], L"no-focus") == 0;
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&shell));
        if (shell.result != nullptr && !skipFocus) {
            const auto targetThread =
                GetWindowThreadProcessId(shell.result, nullptr);
            const auto currentThread = GetCurrentThreadId();
            auto attached = false;
            if (targetThread != currentThread) {
                attached = AttachThreadInput(
                    currentThread, targetThread, TRUE) != FALSE;
            }
            SetFocus(shell.result);
            const auto owns = GetFocus() == shell.result;
            if (attached) {
                AttachThreadInput(currentThread, targetThread, FALSE);
            }
            focusNote = owns ? L"shellFocus=shell" : L"shellFocus=other";
        }
        std::wcout << L"Visible-focus main window on " << fullPath
            << L" foreground=" << (focused ? L"true" : L"false")
            << L' ' << focusNote
            << L" previousForeground="
            << reinterpret_cast<std::uintptr_t>(previousForeground) << L'\n';
        return focused ? 0 : 8;
    }
    if (action == L"window-offscreen-restore") {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(topLevel.result, &placement)) {
            std::wcerr << L"Main window placement could not be read\n";
            return 7;
        }
        const auto width = std::max(
            960L,
            placement.rcNormalPosition.right -
                placement.rcNormalPosition.left);
        const auto height = std::max(
            640L,
            placement.rcNormalPosition.bottom -
                placement.rcNormalPosition.top);
        placement.rcNormalPosition = {
            -32000, -32000, -32000 + width, -32000 + height};
        if (!SetWindowPlacement(topLevel.result, &placement)) {
            std::wcerr << L"Main window could not be moved offscreen\n";
            return 7;
        }
        ShowWindow(topLevel.result, SW_RESTORE);
        SetWindowPos(topLevel.result, HWND_BOTTOM,
            placement.rcNormalPosition.left,
            placement.rcNormalPosition.top,
            0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        std::wcout << L"Offscreen-restored main window on "
            << fullPath << L'\n';
        return 0;
    }
    if (background && (action == L"window-maximize" ||
            action == L"window-native-maximize" ||
            action == L"listen-maximize")) {
        std::wcerr << L"Maximize actions are disabled in background mode\n";
        return 10;
    }
    if(action==L"capture-hang-dump"){
        if(actionArgc!=4)return 2;
        const auto normalized=std::filesystem::path(fullPath).lexically_normal().wstring();
        if(normalized.find(L"\\artifacts\\")==std::wstring::npos){
            std::wcerr<<L"Hang dumps are restricted to evaluation candidates\n";return 2;
        }
        DWORD pid=0;GetWindowThreadProcessId(topLevel.result,&pid);
        const auto process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
        if(!process)return 8;
        const auto file=CreateFileW(argv[3],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE){CloseHandle(process);return 8;}
        const auto flags=static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules|MiniDumpWithFullMemoryInfo);
        const auto ok=MiniDumpWriteDump(process,pid,file,flags,nullptr,nullptr,nullptr);
        CloseHandle(file);CloseHandle(process);
        std::wcout<<L"Candidate hang dump pid="<<pid<<L" complete="<<(ok?L"true":L"false")<<L'\n';return ok?0:8;
    }
    if (action == L"sample-ui-stacks") {
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 10;
        return sampleUiThread(topLevel.result, std::clamp(requested, 1, 60));
    }
    if (action == L"watch-ui-stalls") {
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 30;
        return watchUiStalls(topLevel.result, std::clamp(requested, 1, 3600));
    }
    if (action == L"drop-file") {
        if (actionArgc != 4) {
            std::wcerr << L"drop-file requires a file or directory path\n";
            return 8;
        }
        ChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findChild,
            reinterpret_cast<LPARAM>(&workspace));
        const auto target = workspace.result != nullptr
            ? workspace.result
            : topLevel.result;
        wchar_t targetClass[256]{};
        GetClassNameW(
            target, targetClass, static_cast<int>(std::size(targetClass)));
        const auto targetStyle = GetWindowLongPtrW(target, GWL_EXSTYLE);
        if (!sendFileDrop(target, std::filesystem::path(argv[3]))) {
            std::wcerr << L"WM_DROPFILES injection failed\n";
            return 8;
        }
        std::wcout
            << L"Dropped " << argv[3] << L" on " << fullPath
            << L" [" << targetClass
            << L", WS_EX_ACCEPTFILES="
            << ((targetStyle & WS_EX_ACCEPTFILES) != 0 ? L"yes" : L"no")
            << L"]\n";
        return 0;
    }
        if (action == L"tag-open" || action == L"tag-state" ||
            action == L"tag-close" || action == L"tag-select-all" ||
            action == L"tag-capture" ||
            action == L"tag-scroll-top" ||
            action == L"tag-scroll-bottom" ||
            action == L"tag-scroll-cycle" ||
            action == L"tag-resize-cycle" ||
            action == L"tag-keyboard-focus" ||
            action == L"tag-roots-state" ||
            action == L"tag-set-destination" ||
            action == L"tag-set-artwork-front" ||
            action == L"tag-artwork-front-mode" ||
            action == L"tag-mode" || action == L"tag-collision" ||
            action == L"tag-preview-copy" || action == L"tag-execute-copy" ||
            action == L"tag-drop" || action == L"tag-set-title" ||
            action == L"tag-apply" || action == L"tag-undo") {
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&header));
        if (action == L"tag-open") {
            const auto message = RegisterWindowMessageW(L"GodenHeaderQaOpenV1");
            DWORD_PTR result = FALSE;
            if (!message || !header.result || !SendMessageTimeoutW(header.result,
                    message, 4, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) ||
                    result == FALSE) {
                std::wcerr << L"TAG window open request failed\n";
                return 7;
            }
            std::wcout << L"Opened TAG window on " << fullPath << L'\n';
            return 0;
        }
        SearchContext tag{fullPath, L"GodenTagEditorV1", false, background};
        EnumWindows(findTopLevel, reinterpret_cast<LPARAM>(&tag));
        if (!tag.result) {
            std::wcerr << L"TAG window was not found\n";
            return 7;
        }
        if (action == L"tag-close") {
            SendMessageW(tag.result, WM_CLOSE, 0, 0);
            std::wcout << L"Closed TAG window\n";
            return 0;
        }
        if (action == L"tag-capture") {
            if (actionArgc != 4 ||
                !captureWindowBmp(tag.result, std::filesystem::path(argv[3]))) {
                std::wcerr << L"TAG window capture failed\n";
                return 8;
            }
            std::wcout << L"Captured TAG window\n";
            return 0;
        }
        if (action == L"tag-scroll-top" ||
                action == L"tag-scroll-bottom" ||
                action == L"tag-scroll-cycle") {
            const auto bottom = action == L"tag-scroll-bottom";
            if (!exerciseFormViewport(tag.result, bottom)) {
                std::wcerr << L"TAG form viewport scroll failed\n";
                return 8;
            }
            std::wcout << L"TAG form viewport "
                << (bottom ? L"bottom" : L"top") << L" PASS\n";
            return 0;
        }
        if (action == L"tag-resize-cycle") {
            if (!exerciseFormResize(tag.result)) {
                std::wcerr << L"TAG form resize cycle failed\n";
                return 8;
            }
            std::wcout << L"TAG form resize 20 cycles PASS\n";
            return 0;
        }
        if (action == L"tag-keyboard-focus") {
            if (!exerciseFormFocus(tag.result, 311)) {
                std::wcerr << L"TAG keyboard focus reveal failed\n";
                return 8;
            }
            std::wcout << L"TAG keyboard focus reveal and Tab PASS\n";
            return 0;
        }
        if (action == L"tag-drop") {
            if (actionArgc != 4 ||
                    !sendFileDrop(tag.result, std::filesystem::path(argv[3]))) {
                std::wcerr << L"TAG drop injection failed\n";
                return 8;
            }
            std::wcout << L"Dropped fixture into TAG\n";
            return 0;
        }
        if (action == L"tag-set-title") {
            if (actionArgc != 4) return 8;
            if (!sendCopyDataText(tag.result,
                    nekorox::kTagSetTitleCopyDataTagV1, argv[3])) {
                std::wcerr << L"TAG title injection failed\n";
                return 8;
            }
            std::wcout << L"Set TAG title mutation\n";
            return 0;
        }
        if (action == L"tag-select-all") {
            const auto list = descendantControl(tag.result, 100);
            if (!list || SendMessageW(list, LB_SETSEL, TRUE, -1) == LB_ERR) return 8;
            SendMessageW(tag.result, WM_COMMAND, MAKEWPARAM(100, LBN_SELCHANGE),
                reinterpret_cast<LPARAM>(list));
            const auto count = SendMessageW(list, LB_GETSELCOUNT, 0, 0);
            std::wcout << L"TAG selected=" << count << L'\n';
            return count > 0 ? 0 : 8;
        }
        if (action == L"tag-set-destination") {
            if (actionArgc != 4 || !sendCopyDataText(tag.result,
                    nekorox::kTagSetDestinationCopyDataTagV1, argv[3])) {
                std::wcerr << L"TAG destination injection failed\n";
                return 8;
            }
            std::wcout << L"Set TAG verified destination\n";
            return 0;
        }
        if (action == L"tag-set-artwork-front") {
            if (actionArgc != 4 || !sendCopyDataText(tag.result,
                    nekorox::kTagSetFrontArtworkCopyDataTagV1, argv[3])) {
                std::wcerr << L"TAG front artwork injection failed\n";
                return 8;
            }
            std::wcout << L"Set TAG front artwork mutation\n";
            return 0;
        }
        if (action == L"tag-artwork-front-mode") {
            if (actionArgc != 4) return 8;
            const auto control = descendantControl(tag.result, 500);
            const auto selection = _wtoi(argv[3]);
            if (!control || SendMessageW(control, CB_SETCURSEL,
                    selection, 0) == CB_ERR) return 8;
            SendMessageW(tag.result, WM_COMMAND,
                MAKEWPARAM(500, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(control));
            std::wcout << L"Set TAG front artwork mode " << selection << L'\n';
            return 0;
        }
        if (action == L"tag-mode" || action == L"tag-collision") {
            if (actionArgc != 4) return 8;
            const auto control = descendantControl(tag.result,
                action == L"tag-mode" ? 115 : 116);
            const auto selection = _wtoi(argv[3]);
            if (!control || SendMessageW(control, CB_SETCURSEL, selection, 0) == CB_ERR) {
                std::wcerr << L"TAG organization option injection failed\n";
                return 8;
            }
            SendMessageW(tag.result, WM_COMMAND,
                MAKEWPARAM(action == L"tag-mode" ? 115 : 116, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(control));
            std::wcout << L"Set TAG organization option " << selection << L'\n';
            return 0;
        }
        if (action == L"tag-preview-copy") {
            SendMessageW(tag.result, WM_COMMAND, MAKEWPARAM(113, BN_CLICKED),
                reinterpret_cast<LPARAM>(descendantControl(tag.result, 113)));
            std::wcout << L"Requested TAG Copy preview\n";
            return 0;
        }
        if (action == L"tag-execute-copy") {
            SendMessageW(tag.result, WM_COMMAND, MAKEWPARAM(114, BN_CLICKED),
                reinterpret_cast<LPARAM>(descendantControl(tag.result, 114)));
            std::wcout << L"Requested TAG Copy execution\n";
            return 0;
        }
        if (action == L"tag-apply") {
            SendMessageW(tag.result, WM_COMMAND, MAKEWPARAM(102, BN_CLICKED),
                reinterpret_cast<LPARAM>(descendantControl(tag.result, 102)));
            std::wcout << L"Requested TAG Apply\n";
            return 0;
        }
        if (action == L"tag-undo") {
            SendMessageW(tag.result, WM_COMMAND, MAKEWPARAM(104, BN_CLICKED),
                reinterpret_cast<LPARAM>(descendantControl(tag.result, 104)));
            std::wcout << L"Requested TAG Undo\n";
            return 0;
        }
        if (action == L"tag-roots-state") {
            wchar_t label[512]{};
            GetWindowTextW(descendantControl(tag.result, 410), label,
                static_cast<int>(std::size(label)));
            const auto roots = descendantControl(tag.result, 110);
            const auto count = SendMessageW(roots, LB_GETCOUNT, 0, 0);
            std::wcout << L"TAG rootsLabel=[" << label << L"] roots=" << count;
            for (LRESULT index = 0; index < count; ++index) {
                const auto length = SendMessageW(roots, LB_GETTEXTLEN, index, 0);
                if (length == LB_ERR) continue;
                std::wstring item(static_cast<std::size_t>(length) + 1U, L'\0');
                if (SendMessageW(roots, LB_GETTEXT, index,
                        reinterpret_cast<LPARAM>(item.data())) != LB_ERR) {
                    item.resize(static_cast<std::size_t>(length));
                    std::wcout << L" item" << index << L"=[" << item << L"]";
                }
            }
            std::wcout << L'\n';
            return 0;
        }
        wchar_t status[1024]{};
        GetWindowTextW(descendantControl(tag.result, 101), status,
            static_cast<int>(std::size(status)));
        const auto count = SendMessageW(descendantControl(tag.result, 100),
            LB_GETCOUNT, 0, 0);
        wchar_t titleValue[1024]{};
        DWORD_PTR copied = 0;
        SendMessageTimeoutW(descendantControl(tag.result, 300), WM_GETTEXT,
            std::size(titleValue), reinterpret_cast<LPARAM>(titleValue),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &copied);
        const auto titleMode = SendMessageW(descendantControl(tag.result, 200),
            CB_GETCURSEL, 0, 0);
        RECT bounds{};
        GetWindowRect(tag.result, &bounds);
        std::wcout << L"TAG visible=" << (IsWindowVisible(tag.result) ? L"true" : L"false")
            << L" tracks=" << count
            << L" applyEnabled=" << (IsWindowEnabled(descendantControl(tag.result, 102)) ? L"true" : L"false")
            << L" undoEnabled=" << (IsWindowEnabled(descendantControl(tag.result, 104)) ? L"true" : L"false")
            << L" previewCopyEnabled=" << (IsWindowEnabled(descendantControl(tag.result, 113)) ? L"true" : L"false")
            << L" executeCopyEnabled=" << (IsWindowEnabled(descendantControl(tag.result, 114)) ? L"true" : L"false")
            << L" titleMode=" << titleMode
            << L" title=[" << titleValue << L"]"
            << L" bounds=" << bounds.right - bounds.left << L"x" << bounds.bottom - bounds.top
            << L" status=[" << status << L"]\n";
        return 0;
    }
    if (action == L"window-native-maximize") {
        ShowWindow(topLevel.result, SW_RESTORE);
        ShowWindow(topLevel.result, SW_MAXIMIZE);
        SetForegroundWindow(topLevel.result);
        std::wcout << L"Native-maximized main window on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"header-double-click-maximize") {
        ShowWindow(topLevel.result, SW_RESTORE);
        SetForegroundWindow(topLevel.result);
        Sleep(120);
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&header));
        if (header.result == nullptr) {
            std::wcerr << L"Header panel was not found\n";
            return 5;
        }
        const auto classStyle = static_cast<UINT>(
            GetClassLongPtrW(header.result, GCL_STYLE));
        if ((classStyle & CS_DBLCLKS) == 0U) {
            std::wcerr << L"Header class does not request double-click synthesis\n";
            return 8;
        }
        if (!physicalDoubleClickDip(header.result, 80.0f, 18.0f)) {
            std::wcerr << L"Could not inject a physical header double click\n";
            return 7;
        }
        for (unsigned attempt = 0; attempt < 80 && !IsZoomed(topLevel.result);
                ++attempt) {
            Sleep(25);
        }
        const auto maximized = IsZoomed(topLevel.result) != FALSE;
        std::wcout << L"HeaderDoubleClick classStyle=" << std::hex
            << std::showbase << classStyle << std::dec
            << L" maximized=" << (maximized ? L"true" : L"false") << L'\n';
        return maximized ? 0 : 8;
    }
    if (action == L"app-close") {
        PostMessageW(topLevel.result, WM_CLOSE, 0, 0);
        std::wcout << L"Requested graceful close on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-resize") {
        if (actionArgc != 4) {
            std::wcerr << L"window-resize requires WIDTHxHEIGHT\n";
            return 8;
        }
        int width = 0;
        int height = 0;
        if (swscanf_s(argv[3], L"%dx%d", &width, &height) != 2 ||
                width < 640 || height < 480) {
            std::wcerr << L"window-resize dimensions are invalid\n";
            return 8;
        }
        ShowWindow(topLevel.result, SW_RESTORE);
        if (!SetWindowPos(
                topLevel.result,
                nullptr,
                0,
                0,
                width,
                height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            std::wcerr << L"Main window resize failed\n";
            return 8;
        }
        RECT bounds{};
        GetWindowRect(topLevel.result, &bounds);
        std::wcout << L"Resized main window to "
            << bounds.right - bounds.left << L"x"
            << bounds.bottom - bounds.top << L" on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-resize-slow") {
        // Exercise the real Windows sizing loop with mouse input. A sequence
        // of SetWindowPos calls does not cover readiness credits during drag.
        if (actionArgc != 4 || background) {
            std::wcerr << L"window-resize-slow requires target width and foreground access\n";
            return 8;
        }
        wchar_t* end = nullptr;
        const long width = std::wcstol(argv[3], &end, 10);
        if (end == argv[3] || *end || width < 640 || width > 3840) return 8;
        ShowWindow(topLevel.result, SW_RESTORE);
        SetForegroundWindow(topLevel.result);
        RECT before{}; POINT cursor{};
        if (!GetWindowRect(topLevel.result, &before) || !GetCursorPos(&cursor)) return 8;
        const LONG x = before.right - 2, y = (before.top + before.bottom) / 2;
        const LONG delta = width - (before.right - before.left);
        SetCursorPos(x, y);
        INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        if (SendInput(1, &input, sizeof(input)) != 1) return 8;
        for (int step = 1; step <= 60; ++step) {
            SetCursorPos(x + delta * step / 60, y);
            Sleep(65);
        }
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        const bool released = SendInput(1, &input, sizeof(input)) == 1;
        Sleep(300);
        SetCursorPos(cursor.x, cursor.y);
        RECT after{};
        if (!released || !GetWindowRect(topLevel.result, &after)) return 8;
        std::wcout << L"Slow horizontal mouse resize " << before.right-before.left
            << L" -> " << after.right-after.left << L" target=" << width << L'\n';
        return std::abs((after.right-after.left)-width) <= 4 ? 0 : 8;
    }
    if (action == L"window-resize-transaction") {
        if (actionArgc != 4) {
            std::wcerr << L"window-resize-transaction requires WIDTHxHEIGHT\n";
            return 8;
        }
        int width = 0;
        int height = 0;
        if (swscanf_s(argv[3], L"%dx%d", &width, &height) != 2 ||
                width < 640 || height < 480) {
            std::wcerr << L"window-resize-transaction dimensions are invalid\n";
            return 8;
        }
        ShowWindow(topLevel.result, SW_RESTORE);
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(topLevel.result, WM_ENTERSIZEMOVE, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) ||
                !SetWindowPos(topLevel.result, nullptr, 0, 0, width, height,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
                !SendMessageTimeoutW(topLevel.result, WM_EXITSIZEMOVE, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored)) {
            std::wcerr << L"Interactive resize transaction failed\n";
            return 8;
        }
        Sleep(250);
        RECT bounds{};
        if (!GetWindowRect(topLevel.result, &bounds)) {
            std::wcerr << L"Interactive resize bounds could not be read\n";
            return 8;
        }
        std::wcout << L"Interactive-resized main window to "
            << bounds.right - bounds.left << L"x"
            << bounds.bottom - bounds.top << L" on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-minimize") {
        if (background) {
            WINDOWPLACEMENT placement{sizeof(placement)};
            if (GetWindowPlacement(topLevel.result, &placement)) {
                placement.flags &= ~WPF_RESTORETOMAXIMIZED;
                placement.showCmd = SW_SHOWMINNOACTIVE;
                SetWindowPlacement(topLevel.result, &placement);
            }
            ShowWindow(topLevel.result, SW_SHOWMINNOACTIVE);
            sendToBackNoActivate(topLevel.result);
        } else {
            ShowWindow(topLevel.result, SW_MINIMIZE);
        }
        std::wcout << L"Minimized main window on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-hide") {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(topLevel.result, &placement)) {
            placement.flags &= ~WPF_RESTORETOMAXIMIZED;
            placement.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(topLevel.result, &placement);
        }
        ShowWindow(topLevel.result, SW_SHOWNOACTIVATE);
        sendToBackNoActivate(topLevel.result);
        ShowWindow(topLevel.result, SW_HIDE);
        std::wcout << L"Hid main window on " << fullPath << L'\n';
        return 0;
    }
    if (action == L"window-state") {
        wchar_t title[1024]{};
        GetWindowTextW(
            topLevel.result, title, static_cast<int>(std::size(title)));
        std::wcout
            << L"Main visible="
            << (IsWindowVisible(topLevel.result) ? L"true" : L"false")
            << L" iconic=" << (IsIconic(topLevel.result) ? L"true" : L"false")
            << L" zoomed=" << (IsZoomed(topLevel.result) ? L"true" : L"false")
            << L" foreground="
            << (GetForegroundWindow() == topLevel.result ? L"true" : L"false")
            << L" title=[" << title << L"]"
            << L'\n';
        return 0;
    }
    if (action == L"window-dpi-state") {
        RECT bounds{};
        GetWindowRect(topLevel.result, &bounds);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const auto monitor = MonitorFromWindow(
            topLevel.result, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(monitor, &monitorInfo);
        const auto context = GetWindowDpiAwarenessContext(topLevel.result);
        const auto awareness = GetAwarenessFromDpiAwarenessContext(context);
        std::wcout
            << L"Main dpi=" << GetDpiForWindow(topLevel.result)
            << L" pmv2=" << AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
            << L" awareness=" << static_cast<int>(awareness)
            << L" physical=" << bounds.right - bounds.left << L"x"
            << bounds.bottom - bounds.top
            << L" work=" << monitorInfo.rcWork.right - monitorInfo.rcWork.left
            << L"x" << monitorInfo.rcWork.bottom - monitorInfo.rcWork.top
            << L'\n';
        return AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ? 0 : 9;
    }
    if (action == L"cover-state" || action == L"cover-redraw") {
        LargestChildContext cover{L"NekoRoXCoverV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&cover));
        if (cover.result == nullptr) {
            std::wcerr << L"NekoRoX cover panel was not found\n";
            return 5;
        }
        if (action == L"cover-redraw") {
            RedrawWindow(
                cover.result,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            Sleep(100);
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kCoverStateMessageV1);
        DWORD_PTR state = 0;
        const auto query = [&](WPARAM selector, DWORD_PTR& output) {
            return message != 0 && SendMessageTimeoutW(
                cover.result, message, selector, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &output) != 0;
        };
        DWORD_PTR presentedFrames = 0;
        DWORD_PTR reconstructionFrames = 0;
        DWORD_PTR fadeFallbackFrames = 0;
        DWORD_PTR maximumFrameMicros = 0;
        DWORD_PTR transitionGeneration = 0;
        DWORD_PTR pixelWidth = 0;
        DWORD_PTR pixelHeight = 0;
        DWORD_PTR targetPhysicalEdge = 0;
        DWORD_PTR artLoadRequests = 0;
        DWORD_PTR artLocalPaths = 0;
        DWORD_PTR artSidecars = 0;
        DWORD_PTR artFileReads = 0;
        DWORD_PTR artDecodeAttempts = 0;
        DWORD_PTR artDecodeSuccesses = 0;
        DWORD_PTR artReadyBytes = 0;
        DWORD_PTR artPeakTemporaryBytes = 0;
        DWORD_PTR artBusyRetryRequeues = 0, artFailedNotices = 0;
        DWORD_PTR artResampleAttempts = 0, artResampleMemoHits = 0;
        if (!query(0, state) || !query(1, presentedFrames) ||
            !query(2, reconstructionFrames) ||
            !query(3, fadeFallbackFrames) ||
            !query(4, maximumFrameMicros) ||
            !query(5, transitionGeneration) ||
            !query(6, pixelWidth) ||
            !query(7, pixelHeight) ||
            !query(8, targetPhysicalEdge) ||
            !query(20, artLoadRequests) ||
            !query(21, artLocalPaths) ||
            !query(22, artSidecars) ||
            !query(23, artFileReads) ||
            !query(24, artDecodeAttempts) ||
            !query(25, artDecodeSuccesses) ||
            !query(26, artReadyBytes) ||
            !query(27, artPeakTemporaryBytes) ||
            !query(28, artBusyRetryRequeues) || !query(29, artFailedNotices)) {
            std::wcerr << L"NekoRoX cover state query failed\n";
            return 7;
        }
        // Optional diagnostics keep this probe compatible with the baseline.
        (void)query(30, artResampleAttempts);
        (void)query(31, artResampleMemoHits);
        std::wcout
            << L"Cover track="
            << ((state & nekorox::kCoverTrackValidBitV1) ? L"true" : L"false")
            << L" pixels="
            << ((state & nekorox::kCoverPixelsReadyBitV1) ? L"true" : L"false")
            << L" bitmap="
            << ((state & nekorox::kCoverBitmapReadyBitV1) ? L"true" : L"false")
            << L" bitmapFailed="
            << ((state & nekorox::kCoverBitmapFailedBitV1) ? L"true" : L"false")
            << L" cacheLoading="
            << ((state & nekorox::kCoverCacheLoadingBitV1) ? L"true" : L"false")
            << L" cacheMissing="
            << ((state & nekorox::kCoverCacheMissingBitV1) ? L"true" : L"false")
            << L" cacheReady="
            << ((state & nekorox::kCoverCacheReadyBitV1) ? L"true" : L"false")
            << L" key="
            << ((state & nekorox::kCoverKeyValidBitV1) ? L"true" : L"false")
            << L" painted="
            << ((state & nekorox::kCoverPaintObservedBitV1) ? L"true" : L"false")
            << L" render="
            << ((state & nekorox::kCoverRenderReadyBitV1) ? L"true" : L"false")
            << L" transition="
            << ((state & nekorox::kCoverTransitionActiveBitV1) ? L"true" : L"false")
            << L" reconstruction="
            << ((state & nekorox::kCoverReconstructionUsedBitV1) ? L"true" : L"false")
            << L" fadeFallback="
            << ((state & nekorox::kCoverFadeFallbackUsedBitV1) ? L"true" : L"false")
            << L" reconstructionPrepared="
            << ((state & nekorox::kCoverReconstructionPreparedBitV1) ? L"true" : L"false")
            << L" presentedFrames=" << presentedFrames
            << L" reconstructionFrames=" << reconstructionFrames
            << L" fadeFallbackFrames=" << fadeFallbackFrames
            << L" maxFrameMs=" << std::fixed << std::setprecision(3)
            << static_cast<double>(maximumFrameMicros) / 1000.0
            << L" generation=" << transitionGeneration
            << L" raster=" << pixelWidth << L"x" << pixelHeight
            << L" targetPx=" << targetPhysicalEdge
            << L" artLoads=" << artLoadRequests
            << L" localPaths=" << artLocalPaths
            << L" sidecars=" << artSidecars
            << L" fileReads=" << artFileReads
            << L" decodeAttempts=" << artDecodeAttempts
            << L" decodeSuccesses=" << artDecodeSuccesses
            << L" resampleAttempts=" << artResampleAttempts
            << L" resampleMemoHits=" << artResampleMemoHits
            << L" busyRetries=" << artBusyRetryRequeues
            << L" failedNotices=" << artFailedNotices
            << L" readyBytes=" << artReadyBytes
            << L" peakTemporaryBytes=" << artPeakTemporaryBytes
            << L" matches=" << cover.matches
            << L" visibleMatches=" << cover.visibleMatches
            << L" area=" << cover.bestArea
            << L'\n';
        return 0;
    }
    if (action == L"menu-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"menu-capture requires a BMP path\n";
            return 8;
        }
        LargestChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&header));
        if (header.result == nullptr) {
            std::wcerr << L"NekoRoX header was not found\n";
            return 5;
        }
        SetForegroundWindow(topLevel.result);
        if (!postClickDip(header.result, 20.0f, 10.0f)) {
            std::wcerr << L"Menu button did not accept the click\n";
            return 7;
        }
        DWORD processId = 0;
        const auto ownerThread =
            GetWindowThreadProcessId(topLevel.result, &processId);
        PopupContext popup{processId};
        for (unsigned attempt = 0; attempt < 40 && popup.result == nullptr;
                ++attempt) {
            EnumWindows(findPopupMenu, reinterpret_cast<LPARAM>(&popup));
            if (popup.result == nullptr) Sleep(25);
        }
        bool captured = false;
        for (unsigned attempt = 0; popup.result != nullptr && attempt < 12;
                ++attempt) {
            RedrawWindow(
                popup.result,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            Sleep(30);
            if (captureWindowBmp(popup.result, argv[3])) {
                captured = true;
                break;
            }
        }
        if (popup.result == nullptr || !captured) {
            if (popup.result != nullptr && IsWindow(popup.result)) {
                PostThreadMessageW(ownerThread, WM_KEYDOWN, VK_ESCAPE, 0);
                PostThreadMessageW(ownerThread, WM_KEYUP, VK_ESCAPE, 0);
                for (unsigned attempt = 0;
                        attempt < 40 && IsWindow(popup.result); ++attempt) {
                    Sleep(25);
                }
            }
            std::wcerr << L"NekoRoX menu capture failed\n";
            return 8;
        }
        PostThreadMessageW(ownerThread, WM_KEYDOWN, VK_ESCAPE, 0);
        PostThreadMessageW(ownerThread, WM_KEYUP, VK_ESCAPE, 0);
        for (unsigned attempt = 0; attempt < 40 && IsWindow(popup.result);
                ++attempt) {
            Sleep(25);
        }
        if (IsWindow(popup.result)) {
            std::wcerr << L"NekoRoX menu did not leave modal state\n";
            return 9;
        }
        std::wcout << L"Captured native menu to " << argv[3] << L'\n';
        return 0;
    }
    if (action == L"transport-state") {
        LargestChildContext transport{L"NekoRoXTransportV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&transport));
        if (transport.result == nullptr) {
            std::wcerr << L"NekoRoX transport panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kTransportStateMessageV1);
        DWORD_PTR state = 0;
        if (message == 0 || !SendMessageTimeoutW(
                transport.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &state)) {
            std::wcerr << L"NekoRoX transport state query failed\n";
            return 7;
        }
        const auto volume = static_cast<unsigned>((state >>
            nekorox::kTransportVolumeShiftV1) &
            nekorox::kTransportVolumeMaskV1);
        const auto order = static_cast<unsigned>((state >>
            nekorox::kTransportOrderShiftV1) &
            nekorox::kTransportOrderMaskV1);
        std::wcout
            << L"Transport playing="
            << ((state & nekorox::kTransportPlayingBitV1) ? L"true" : L"false")
            << L" muted="
            << ((state & nekorox::kTransportMutedBitV1) ? L"true" : L"false")
            << L" repeat="
            << ((state & nekorox::kTransportRepeatBitV1) ? L"true" : L"false")
            << L" shuffle="
            << ((state & nekorox::kTransportShuffleBitV1) ? L"true" : L"false")
            << L" costMap="
            << ((state & nekorox::kTransportCostMapReadyBitV1) ? L"true" : L"false")
            << L" volume=" << volume
            << L" order=" << order << L'\n';
        return 0;
    }
    if (action == L"playback-state") {
        LargestChildContext transport{L"NekoRoXTransportV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&transport));
        if (transport.result == nullptr) {
            std::wcerr << L"NekoRoX transport panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kTransportStateMessageV1);
        auto query = [&](WPARAM selector, DWORD_PTR& value) {
            value = 0;
            return message != 0 && SendMessageTimeoutW(
                transport.result, message, selector, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &value) != 0;
        };
        DWORD_PTR state = 0;
        DWORD_PTR generation = 0;
        DWORD_PTR track = 0;
        DWORD_PTR positionMs = 0;
        DWORD_PTR durationMs = 0;
        if (!query(5U, state) || !query(6U, generation) ||
            !query(7U, track) || !query(8U, positionMs) ||
            !query(9U, durationMs)) {
            std::wcerr << L"Goden playback state query failed\n";
            return 7;
        }
        const auto* stateText = state == 1U ? L"playing" :
            state == 2U ? L"paused" : L"stopped";
        std::wcout << L"Playback state=" << stateText
            << L" generation=" << generation
            << L" track=0x" << std::hex << track << std::dec
            << L" positionMs=" << positionMs
            << L" durationMs=" << durationMs << L'\n';
        return 0;
    }
    if (action == L"library-count") {
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"NekoRoX workspace panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kLibraryCountMessageV1);
        DWORD_PTR count = 0;
        if (message == 0 || !SendMessageTimeoutW(
                workspace.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &count)) {
            std::wcerr << L"NekoRoX library count query failed\n";
            return 7;
        }
        std::wcout << L"Library tracks=" << count << L'\n';
        return 0;
    }

    if (action == L"file-open-state") {
        ChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"Workspace was not found\n";
            return 8;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kFileOpenStateMessageV1);
        DWORD_PTR packed = 0;
        if (!SendMessageTimeoutW(
                workspace.result,
                message,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                2000,
                &packed)) {
            std::wcerr << L"File-open state query failed\n";
            return 8;
        }
        const auto state = static_cast<std::uint32_t>(packed);
        std::wcout
            << L"FileOpen dropObserved="
            << ((state & nekorox::kFileOpenDropObservedBitV1) ? L"true" : L"false")
            << L" dropExtracted="
            << ((state & nekorox::kFileOpenDropExtractedBitV1) ? L"true" : L"false")
            << L" request="
            << ((state & nekorox::kFileOpenRequestStartedBitV1) ? L"true" : L"false")
            << L" callback="
            << ((state & nekorox::kFileOpenCallbackBitV1) ? L"true" : L"false")
            << L" items="
            << ((state & nekorox::kFileOpenItemsResolvedBitV1) ? L"true" : L"false")
            << L" playback="
            << ((state & nekorox::kFileOpenPlaybackStartedBitV1) ? L"true" : L"false")
            << L" temporaryAlbum="
            << ((state & nekorox::kFileOpenTemporaryAlbumActiveBitV1) ? L"true" : L"false")
            << L" dismissed="
            << ((state & nekorox::kFileOpenTemporaryAlbumDismissedBitV1) ? L"true" : L"false")
            << L" temporaryItems="
            << ((state >> nekorox::kFileOpenTemporaryAlbumCountShiftV1) &
                nekorox::kFileOpenTemporaryAlbumCountMaskV1)
            << L'\n';
        return 0;
    }
    if (action == L"workspace-state") {
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"NekoRoX workspace panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceStateMessageV1);
        DWORD_PTR packed = 0;
        if (message == 0 || !SendMessageTimeoutW(
                workspace.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &packed)) {
            std::wcerr << L"NekoRoX workspace state query failed\n";
            return 7;
        }
        const auto state = static_cast<std::uint32_t>(packed);
        std::wcout << L"Workspace tab="
            << (state & nekorox::kWorkspaceTabMaskV1)
            << L" history="
            << ((state & nekorox::kWorkspaceHistoryBitV1) ? L"true" : L"false")
            << L'\n';
        return 0;
    }
    if (action == L"text-surface-state") {
        ChildContext workspace{L"NekoRoXWorkspaceV3"};
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&workspace));
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&shell));
        auto query = [&](HWND window, UINT message, unsigned first, unsigned last, const wchar_t* prefix) {
            for (unsigned key = first; key <= last; ++key) {
                DWORD_PTR value = 0;
                if (!window || !SendMessageTimeoutW(window, message, key, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &value)) return false;
                std::wcout << prefix << key << L"=" << static_cast<LRESULT>(value) << L" ";
            }
            return true;
        };
        if (!query(workspace.result, RegisterWindowMessageW(nekorox::kWorkspaceRenderStateMessageV1), 80, 89, L"w") ||
            !query(shell.result, RegisterWindowMessageW(L"Goden.CollectionQa.V1"), 2400, 2420, L"a")) return 8;
        std::wcout << L'\n';
        return 0;
    }
    if (action == L"workspace-text-open" || action == L"workspace-text-cold-wheel") {
        ChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&workspace));
        DWORD_PTR value = 0;
        const auto tab = actionArgc == 4 ? std::clamp(_wtoi(argv[3]), 0, 4) : 1;
        if (!workspace.result || !SendMessageTimeoutW(workspace.result,
            RegisterWindowMessageW(nekorox::kWorkspaceRenderStateMessageV1),
            action == L"workspace-text-open" ? 90 : 91, tab, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &value)) return 8;
        std::wcout << L"TEXT_QA result=" << value << L'\n';
        return value == static_cast<DWORD_PTR>(tab) ? 0 : 8;
    }
    if (action == L"workspace-render-state") {
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"NekoRoX workspace panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        std::array<DWORD_PTR, 15> values{};
        for (WPARAM query = 0; query < values.size(); ++query) {
            if (message == 0 || !SendMessageTimeoutW(
                    workspace.result, message, query, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &values[query])) {
                std::wcerr << L"NekoRoX workspace render query failed\n";
                return 7;
            }
        }
        std::array<DWORD_PTR, 3> presentation{};
        for (WPARAM index = 0; index < presentation.size(); ++index) {
            if (!SendMessageTimeoutW(workspace.result, message, 54U + index, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &presentation[index])) {
                std::wcerr << L"Workspace presentation query failed\n";
                return 7;
            }
        }
        RECT update{};
        const auto paintPending = GetUpdateRect(workspace.result, &update, FALSE);
        std::wcout << std::hex << std::showbase
            << L"WorkspaceRender d2d=" << values[0]
            << L" dwrite=" << values[1]
            << L" target=" << values[2]
            << L" end=" << values[3]
            << std::dec
            << L" ready=" << values[4]
            << L" normal=" << values[5]
            << L" tab=" << values[6]
            << L" textBrush=" << values[7]
            << L" attached=" << values[8]
            << L" size=" << (values[9] >> 32U)
            << L"x" << (values[9] & 0xffffffffU)
            << L" windowState=" << values[10]
            << L" pixels=" << (values[11] >> 32U)
            << L"x" << (values[11] & 0xffffffffU)
            << L" themeFrames=" << values[12]
            << L" lastThemeFrames=" << values[13]
            << L" themeActive=" << values[14]
            << L" desired=" << presentation[0]
            << L" accepted=" << presentation[1]
            << L" staticCache=" << presentation[2]
            << L" paintPending=" << (paintPending ? L"true" : L"false")
            ;
        constexpr std::array<const wchar_t*,6> labels{L"tileBuilds",L"tileHits",L"directFrames",L"facetHits",L"facetBuilds",L"maxTileBuildUs"};
        for(WPARAM index=0;index<labels.size();++index){
            DWORD_PTR value=0;
            if(SendMessageTimeoutW(workspace.result,message,63+index,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&value) &&
                static_cast<LRESULT>(value)>=0) std::wcout<<L" "<<labels[index]<<L"="<<value;
        }
        constexpr std::array<const wchar_t*,4> occurrenceLabels{L"selectedOccurrence",L"playingOccurrence",L"selectedRows",L"playingRows"};
        for (WPARAM index=0;index<occurrenceLabels.size();++index) {
            DWORD_PTR value=0;
            if (SendMessageTimeoutW(workspace.result,message,69+index,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&value) &&
                    static_cast<LRESULT>(value)!=E_INVALIDARG) {
                std::wcout<<L" "<<occurrenceLabels[index]<<L"="<<static_cast<LRESULT>(value);
            }
        }
        std::wcout<<L'\n';
        return 0;
    }
    if (action == L"temporary-workspace-state") {
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"NekoRoX workspace panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        constexpr std::array<WPARAM, 5> selectors{42, 43, 44, 46, 47};
        std::array<DWORD_PTR, selectors.size()> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (message == 0 || !SendMessageTimeoutW(
                    workspace.result,
                    message,
                    selectors[index],
                    0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    2000,
                    &values[index])) {
                std::wcerr << L"Temporary workspace state query failed\n";
                return 7;
            }
        }
        std::wcout
            << L"TemporaryWorkspace active="
            << (values[0] ? L"true" : L"false")
            << L" tracks=" << values[1]
            << L" groups=" << values[2]
            << L" libraryTracks=" << values[3]
            << L" totalGroups=" << values[4]
            << L'\n';
        return 0;
    }
    if (action == L"play-library-outside-temporary") {
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"NekoRoX workspace panel was not found\n";
            return 5;
        }
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        DWORD_PTR played = FALSE;
        if (message == 0 || !SendMessageTimeoutW(
                workspace.result,
                message,
                45,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                2000,
                &played) || played == FALSE) {
            std::wcerr << L"Could not play a library album outside the temporary set\n";
            return 7;
        }
        std::wcout << L"External library playback: PASS\n";
        return 0;
    }
    if (action == L"workspace-query") {
        if (actionArgc != 4) {
            std::wcerr << L"workspace-query requires query text\n";
            return 8;
        }
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(
            topLevel.result,
            findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (workspace.result == nullptr) {
            std::wcerr << L"NekoRoX workspace panel was not found\n";
            return 5;
        }
        SendMessageW(workspace.result, WM_KEYDOWN, VK_ESCAPE, 0);
        SendMessageW(workspace.result, WM_KEYUP, VK_ESCAPE, 0);
        for (const auto character : std::wstring_view(argv[3])) {
            SendMessageW(workspace.result, WM_CHAR, character, 0);
        }
        RedrawWindow(
            workspace.result,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        std::wcout << L"Workspace query applied\n";
        return 0;
    }
    if(action==L"album-menu-race"){
        ChildContext shell{L"GodenPlayerShellV2"},header{L"NekoRoXHeaderV3"};
        EnumChildWindows(topLevel.result,findChild,reinterpret_cast<LPARAM>(&shell));
        EnumChildWindows(topLevel.result,findChild,reinterpret_cast<LPARAM>(&header));
        if(!shell.result||!header.result)return 5;
        const auto message=RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        const auto query=[&](unsigned selector){DWORD_PTR value=0;return message && SendMessageTimeoutW(shell.result,message,selector,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&value)?std::optional<std::uint64_t>(value):std::nullopt;};
        for(unsigned i=0;i<40 && !query(189).value_or(0);++i)Sleep(50);
        const auto before=query(188);if(!before||!query(189).value_or(0)){
            std::wcerr<<L"Album menu target is not available\n";return 8;}
        DWORD pid=0;GetWindowThreadProcessId(topLevel.result,&pid);
        if(!activateInputCandidate(topLevel.result)){std::wcerr<<L"Album menu race requires foreground candidate\n";return 8;}
        if(!PostMessageW(shell.result,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(shell.result),-1))return 8;
        PopupContext popup{pid};for(unsigned i=0;i<40 && !popup.result;++i){EnumWindows(findPopupMenu,reinterpret_cast<LPARAM>(&popup));if(!popup.result)Sleep(25);}
        if(!popup.result)return 8;
        // Drive the normal transport without stealing popup mouse capture.
        const bool next=query(190).value_or(0)!=0;
        Sleep(400);
        if(next && (!IsWindow(popup.result) || !IsWindowVisible(popup.result))){
            std::wcout<<L"Album popup track change: PASS (host canceled popup; stale execution guard not exercised)\n";return 0;
        }
        const auto key=[](WORD k){INPUT down{};down.type=INPUT_KEYBOARD;down.ki.wVk=k;INPUT up=down;up.ki.dwFlags=KEYEVENTF_KEYUP;INPUT keys[]{down,up};return SendInput(2,keys,sizeof(INPUT))==2;};
        const bool moved=key(VK_DOWN);Sleep(120);const bool selected=moved&&key(VK_RETURN);Sleep(300);
        const auto after=query(188);const bool valid=next&&selected&&after&&*after==*before+1;
        if(!valid){(void)query(194);Sleep(100);}
        std::wcout<<L"Album popup track change: "<<(valid?L"PASS (stale action rejected)":L"FAIL")<<L" next="<<next<<L" selected="<<selected<<L" before="<<before.value_or(999)<<L" after="<<after.value_or(999)<<L" command="<<query(192).value_or(999)<<L'\n';return valid?0:8;
    }
    if(action==L"album-accessibility"||action==L"album-keyboard"){
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result,findChild,reinterpret_cast<LPARAM>(&shell));
        if(!shell.result)return 5;
        const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        IAccessible* root=nullptr;const auto hr=AccessibleObjectFromWindow(shell.result,OBJID_CLIENT,IID_IAccessible,reinterpret_cast<void**>(&root));
        if(FAILED(hr)||!root){if(SUCCEEDED(com))CoUninitialize();return 8;}
        long count=0;bool valid=SUCCEEDED(root->get_accChildCount(&count))&&count==15;
        for(long id=1;id<=15&&valid;++id){
            VARIANT child{};child.vt=VT_I4;child.lVal=id;BSTR name=nullptr;VARIANT role{},state{};
            valid=SUCCEEDED(root->get_accName(child,&name))&&name&&SysStringLen(name)>0&&
                SUCCEEDED(root->get_accRole(child,&role))&&role.vt==VT_I4&&SUCCEEDED(root->get_accState(child,&state));
            if(id==12||id==13)valid=valid&&role.lVal==ROLE_SYSTEM_LIST;
            SysFreeString(name);
        }
        if(valid&&action==L"album-keyboard"){
            const bool activated=activateInputCandidate(topLevel.result);Sleep(150);
            const auto qaMessage=RegisterWindowMessageW(L"GodenPlayerShellQaV1");
            DWORD_PTR keysBefore=0;SendMessageTimeoutW(shell.result,qaMessage,191,0,SMTO_ABORTIFHUNG,2000,&keysBefore);
            VARIANT trackList{};trackList.vt=VT_I4;trackList.lVal=12;
            const auto selectionResult=root->accSelect(SELFLAG_TAKEFOCUS,trackList);
            std::wcout<<L"Keyboard activation foreground="<<(GetForegroundWindow()==topLevel.result)<<L" select="<<selectionResult<<L"\n";
            valid=activated&&GetForegroundWindow()==topLevel.result&&SUCCEEDED(selectionResult);
            VARIANT focus{};const auto initialFocusResult=root->get_accFocus(&focus);
            std::wcout<<L"Keyboard initial focus result="<<initialFocusResult<<L" type="<<focus.vt<<L" child="<<focus.lVal<<L"\n";
            valid=valid&&SUCCEEDED(initialFocusResult)&&focus.vt==VT_I4&&focus.lVal==12;
            for(const WORD key:{WORD(VK_DOWN),WORD(VK_UP),WORD(VK_TAB)}){
                INPUT down{};down.type=INPUT_KEYBOARD;down.ki.wVk=key;
                INPUT up=down;up.ki.dwFlags=KEYEVENTF_KEYUP;INPUT input[]{down,up};
                const auto sent=SendInput(2,input,sizeof(INPUT));
                std::wcout<<L"Keyboard key="<<key<<L" sent="<<sent<<L" error="<<GetLastError()<<L" ctrl="<<(GetAsyncKeyState(VK_CONTROL)<0)<<L" alt="<<(GetAsyncKeyState(VK_MENU)<0)<<L"\n";
                valid=valid&&sent==2;Sleep(150);
                std::wcout<<L"Keyboard delivery foreground="<<(GetForegroundWindow()==topLevel.result)<<L"\n";
            }
            focus={};const auto finalFocusResult=root->get_accFocus(&focus);
            std::wcout<<L"Keyboard final focus result="<<finalFocusResult<<L" type="<<focus.vt<<L" child="<<focus.lVal<<L"\n";
            valid=valid&&SUCCEEDED(finalFocusResult)&&focus.vt==VT_I4&&focus.lVal!=12;
            DWORD_PTR keysAfter=0;SendMessageTimeoutW(shell.result,qaMessage,191,0,SMTO_ABORTIFHUNG,2000,&keysAfter);
            std::wcout<<L"Keyboard UI messages="<<(keysAfter-keysBefore)<<L"\n";
        }
        root->Release();if(SUCCEEDED(com))CoUninitialize();
        std::wcout<<L"Album "<<(action==L"album-keyboard"?L"hardware keyboard":L"MSAA controls")<<L": "<<(valid?L"PASS":L"FAIL")<<L'\n';return valid?0:8;
    }
    if (action == L"workspace-accessibility") {
        LargestChildContext workspace{L"NekoRoXWorkspaceV3"};
        EnumChildWindows(topLevel.result, findLargestVisibleChild,
            reinterpret_cast<LPARAM>(&workspace));
        if (!workspace.result) {
            std::wcerr << L"Visible workspace was not found\n";
            return 5;
        }
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IAccessible* root = nullptr;
        const auto result = AccessibleObjectFromWindow(workspace.result,
            OBJID_CLIENT, IID_IAccessible,
            reinterpret_cast<void**>(&root));
        if (FAILED(result) || !root) {
            if (SUCCEEDED(com)) CoUninitialize();
            std::wcerr << L"Workspace accessibility provider is missing\n";
            return 8;
        }
        long count = 0;
        bool valid = SUCCEEDED(root->get_accChildCount(&count)) && count == 5;
        VARIANT search{};
        search.vt = VT_I4;
        search.lVal = 1;
        IDispatch* editDispatch = nullptr;
        valid = valid && SUCCEEDED(root->get_accChild(search, &editDispatch)) &&
            editDispatch != nullptr;
        IAccessible* edit = nullptr;
        if (editDispatch) {
            valid = valid && SUCCEEDED(editDispatch->QueryInterface(
                IID_IAccessible, reinterpret_cast<void**>(&edit))) && edit;
            editDispatch->Release();
        }
        if (edit) {
            VARIANT self{};
            self.vt = VT_I4;
            self.lVal = CHILDID_SELF;
            VARIANT role{};
            valid = valid && SUCCEEDED(edit->get_accRole(self, &role)) &&
                role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_TEXT;
            edit->Release();
        }
        for (long child = 2; child <= 5; ++child) {
            VARIANT index{};
            index.vt = VT_I4;
            index.lVal = child;
            VARIANT role{};
            BSTR name = nullptr;
            valid = valid && SUCCEEDED(root->get_accRole(index, &role)) &&
                role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_LIST &&
                SUCCEEDED(root->get_accName(index, &name)) &&
                name != nullptr && SysStringLen(name) > 0;
            SysFreeString(name);
        }
        root->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        if (!valid) {
            std::wcerr << L"Workspace search/facet/results roles are incomplete\n";
            return 8;
        }
        std::wcout << L"Workspace native edit and accessible pane roles: PASS\n";
        return 0;
    }
    if (action == L"window-tree") {
        RECT bounds{};
        GetWindowRect(topLevel.result, &bounds);
        std::wcout
            << L"top=" << topLevel.result
            << L" rect=" << bounds.left << L"," << bounds.top
            << L" " << bounds.right - bounds.left << L"x"
            << bounds.bottom - bounds.top << L'\n';
        printChildWindow(topLevel.result, 0);
        EnumChildWindows(topLevel.result, printChildWindow, 0);
        return 0;
    }
    if (action == L"window-zorder") {
        ZOrderContext context{topLevel.result};
        EnumWindows(printLargeZOrderWindow, reinterpret_cast<LPARAM>(&context));
        return 0;
    }
    if (action == L"window-redraw") {
        if (!RedrawWindow(
                topLevel.result,
                nullptr,
                nullptr,
                RDW_ERASE | RDW_FRAME | RDW_INVALIDATE |
                    RDW_ALLCHILDREN | RDW_UPDATENOW)) {
            std::wcerr << L"Main window redraw failed\n";
            return 8;
        }
        std::wcout << L"Redrew main window and descendants\n";
        return 0;
    }
    if (action == L"shell-library" || action == L"shell-album") {
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&shell));
        if (shell.result == nullptr) {
            std::wcerr << L"Player shell was not found\n";
            return 5;
        }
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(shell.result));
        const auto xDip = action == L"shell-library" ? 54U : 162U;
        SendMessageW(shell.result, WM_LBUTTONUP, 0, MAKELPARAM(
            static_cast<int>((xDip * dpi + 48U) / 96U),
            static_cast<int>((18U * dpi + 48U) / 96U)));
        // Production navigation owns damage and presentation demand. Do not
        // repair a missing first frame with a synthetic recursive redraw.
        std::wcout << L"Selected " << action << L'\n';
        return 0;
    }
    if (action == L"shell-deck-resize") {
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&shell));
        ChildContext transport{L"NekoRoXTransportV3"};
        if (shell.result != nullptr) {
            EnumChildWindows(shell.result, findChild,
                reinterpret_cast<LPARAM>(&transport));
        }
        if (shell.result == nullptr || transport.result == nullptr) {
            std::wcerr << L"Player shell or transport child was not found\n";
            return 5;
        }
        RECT before{};
        GetWindowRect(transport.result, &before);
        MapWindowPoints(HWND_DESKTOP, shell.result,
            reinterpret_cast<POINT*>(&before), 2);
        const auto x = 24;
        const auto qaMessage = RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        const auto reportedTransportY = static_cast<LONG>(SendMessageW(
            shell.result, qaMessage, 1, 0));
        // Exercise the user-visible separator immediately above WaveSeek,
        // not the otherwise invisible top edge of Transport.
        const auto startY = before.bottom;
        const auto dragDown = actionArgc == 4 &&
            _wcsicmp(argv[3], L"down") == 0;
        const auto endY = dragDown
            ? startY + 80L : std::max(0L, startY - 80L);
        SendMessageW(shell.result, WM_PARENTNOTIFY,
            MAKEWPARAM(WM_LBUTTONDOWN, 0), MAKELPARAM(x, startY));
        SendMessageW(shell.result, WM_MOUSEMOVE, MK_LBUTTON,
            MAKELPARAM(x, endY));
        SendMessageW(shell.result, WM_LBUTTONUP, 0,
            MAKELPARAM(x, endY));
        RECT after{};
        GetWindowRect(transport.result, &after);
        MapWindowPoints(HWND_DESKTOP, shell.result,
            reinterpret_cast<POINT*>(&after), 2);
        const auto moved = before.top - after.top;
        const auto fixedHeight = (before.bottom - before.top) ==
            (after.bottom - after.top);
        std::wcout << L"ShellDeck beforeY=" << before.top
            << L" reportedY=" << reportedTransportY
            << L" grabY=" << startY
            << L" afterY=" << after.top << L" moved=" << moved
            << L" fixedTransportHeight=" << (fixedHeight ? L"true" : L"false")
            << L'\n';
        return (dragDown ? moved < 0 : moved > 0) && fixedHeight ? 0 : 8;
    }
    if (action == L"shell-album-scroll-bound" ||
            action == L"shell-album-theater-state" ||
            action == L"shell-visual-source-stall" ||
            action == L"shell-visual-source-state" ||
            action == L"shell-album-theater-on" ||
            action == L"shell-album-theater-off" ||
            action == L"benchmark-album-scroll-input" ||
            action == L"benchmark-album-lyrics-scroll-input" ||
            action == L"benchmark-curated-lyrics-scroll-input" ||
            action == L"shell-album-transition-state" ||
            action == L"shell-album-ambient-state" ||
            action == L"shell-album-progress" ||
            action == L"shell-album-scroll-telemetry" ||
            action == L"shell-c10-diagnostics" ||
            action == L"shell-c6-history-state" ||
            action == L"shell-collection-select" ||
            action == L"shell-collection-state" ||
            action == L"shell-collection-original" ||
            action == L"shell-collection-music" ||
            action == L"shell-a4-state" ||
            action == L"shell-a4-diagnostics" ||
            action == L"shell-a4-r12-diagnostics" ||
            action == L"shell-a4-cap-light" ||
            action == L"shell-a4-cap-field" ||
            action == L"shell-a4-retry" ||
            action == L"shell-a4-select" ||
            action == L"shell-flow-select" ||
                 action == L"shell-a4-v2-refined" ||
                 action == L"shell-a4-v2-hybrid" ||
                 action == L"shell-a4-v2-show" ||
                 action == L"shell-a4-v2-large" ||
            action == L"shell-a4-grain-off" ||
            action == L"shell-a4-grain-fine" ||
            action == L"shell-a4-grain-strong" ||
            action == L"shell-a4-quality-full" ||
            action == L"shell-a4-quality-auto" ||
            action == L"shell-a4-quality-light" ||
            action == L"shell-a4-quality-field" ||
            action == L"shell-curated-state" ||
            action == L"shell-curated-caustics" ||
            action == L"shell-curated-wave" ||
            action == L"shell-curated-original" ||
            action == L"shell-curated-music" ||
            action == L"shell-curated-c3" ||
            action == L"shell-curated-c2" ||
            action == L"shell-curated-speed1" ||
            action == L"shell-curated-speed15" ||
            action == L"shell-curated-speed2" ||
            action == L"shell-curated-palette-original" ||
            action == L"shell-curated-palette-album" ||
            action == L"shell-curated-restart" ||
            action == L"shell-a4-material-legacy" ||
            action == L"shell-a4-material-clovis" ||
            action == L"shell-album-booklet-toggle" ||
            action == L"shell-album-booklet-capture") {
        ChildContext shell{L"GodenPlayerShellV2"};
        EnumChildWindows(topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&shell));
        if (shell.result == nullptr) {
            std::wcerr << L"Player shell was not found\n";
            return 5;
        }
        const auto qaMessage = RegisterWindowMessageW(L"GodenPlayerShellQaV1");
        const auto query = [&](WPARAM selector) -> std::optional<LRESULT> {
            DWORD_PTR value = 0;
            if (qaMessage == 0 || !SendMessageTimeoutW(
                    shell.result, qaMessage, selector, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &value)) {
                return std::nullopt;
            }
            return static_cast<LRESULT>(value);
        };
        const bool diagnosticInLibrary = action == L"shell-collection-state" ||
            action == L"shell-album-ambient-state" || action == L"shell-visual-source-state" ||
            action == L"shell-album-theater-state";
        if (!diagnosticInLibrary && query(2).value_or(0) != 1) {
            std::wcerr << L"Player shell is not in ALBUM mode\n";
            return 8;
        }
        const auto coordinate = [&](WPARAM selector) -> std::optional<float> {
            const auto value = query(selector);
            if (!value) return std::nullopt;
            return static_cast<float>(*value) / 1000.0f;
        };
        if (action == L"shell-c10-diagnostics") {
            std::array<std::optional<LRESULT>, 17> values{};
            for (std::size_t index = 0; index < values.size(); ++index)
                values[index] = query(160U + index);
            if (std::any_of(values.begin(), values.end(),
                    [](const auto& value) { return !value.has_value(); })) return 8;
            std::wcout << L"C10 queryDisplayConfigCalls=" << *values[0]
                << L" libraryFullRebuilds=" << *values[1]
                << L" libraryDeltaRebuilds=" << *values[2]
                << L" libraryDescribedTracks=" << *values[3]
                << L" libraryDeepCopyBytes=" << *values[4]
                << L" nowPlayingIndexHits=" << *values[5]
                << L" nowPlayingFallbackScans=" << *values[6]
                << L" clockTransientFailures=" << *values[7]
                << L" authoritativeDiscontinuities=" << *values[8]
                << L" publishRejected=" << *values[9]
                << L" discontinuityCommitted=" << *values[10]
                << L" analysisDroppedPublications=" << *values[11]
                << L" pendingReason=" << *values[12]
                << L" dynamicsPartialBins=" << *values[13]
                << L" dynamicsUnknownUs=" << *values[14]
                << L" dynamicsSanitizedSamples=" << *values[15]
                << L" discontinuityRejectedPublications=" << *values[16]
                << L'\n';
            return 0;
        }
        if(action==L"shell-c6-history-state"){
            const auto message=RegisterWindowMessageW(L"Goden.CollectionQa.V1");
            const auto collectionQuery=[&](WPARAM selector)->std::optional<LRESULT>{DWORD_PTR value=0;
                if(message==0||!SendMessageTimeoutW(shell.result,message,selector,0,
                    SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&value))return std::nullopt;
                return static_cast<LRESULT>(value);};
            const auto rows=collectionQuery(18),skipped=collectionQuery(24),fence=collectionQuery(25),
                reads=collectionQuery(26),pending=collectionQuery(27),applied=collectionQuery(28),
                planned=collectionQuery(29),generation=collectionQuery(30),resourceEpoch=collectionQuery(31),
                staged=collectionQuery(32),selected=collectionQuery(0),preset=collectionQuery(1),
                acquired=collectionQuery(2000),begun=collectionQuery(2001),retired=collectionQuery(2002),
                ready=collectionQuery(9),active=collectionQuery(10),budget=collectionQuery(11),
                hr=collectionQuery(13),width=collectionQuery(16),height=collectionQuery(17);
            if(!rows||!skipped||!fence||!reads||!pending||!applied||!planned||
                !generation||!resourceEpoch||!staged||!selected||!preset||!ready||!active||
                !budget||!hr||!width||!height)return 8;
            std::wcout<<L"C6History selected="<<(*selected?L"true":L"false")
                <<L" preset="<<*preset<<L" ready="<<(*ready?L"true":L"false")
                <<L" active="<<(*active?L"true":L"false")<<L" budget="<<(*budget?L"true":L"false")
                <<L" hr="<<*hr<<L" size="<<*width<<L"x"<<*height
                <<L" rows="<<*rows<<L" skipped="<<*skipped
                <<L" restoreFence="<<*fence<<L" readCalls="<<*reads
                <<L" restorePending="<<(*pending?L"true":L"false")
                <<L" applied="<<*applied<<L" planned="<<*planned
                <<L" generation="<<*generation<<L" resourceEpoch="<<*resourceEpoch
                <<L" staged="<<(*staged?L"true":L"false")
                <<L" acquired="<<(acquired?*acquired:-1)<<L" begun="<<(begun?*begun:-1)
                <<L" retired="<<(retired?*retired:-1)<<L'\n';
            return 0;
        }
        if(action==L"shell-collection-state"){
            const auto message=RegisterWindowMessageW(L"Goden.CollectionQa.V1");
            const auto read=[&](WPARAM field)->std::optional<LRESULT>{
                DWORD_PTR value=0;
                if(!message||!SendMessageTimeoutW(shell.result,message,field,0,
                    SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&value))return {};
                return static_cast<LRESULT>(value);
            };
            const auto selected=read(0),preset=read(1),style=query(65);
            if(!selected||!preset||!style)return 8;
            std::wcout<<L"CollectionSelection selected="<<*selected
                <<L" preset="<<*preset<<L" style="<<*style<<L'\n';return 0;
        }
        if(action==L"shell-collection-select"){
            if(actionArgc!=4){std::wcerr<<L"shell-collection-select requires a preset index\n";return 2;}
            wchar_t*end=nullptr;const auto preset=std::wcstoll(argv[3],&end,10);
            if(end==argv[3]||*end!=L'\0'||preset<0){std::wcerr<<L"Invalid collection preset index\n";return 2;}
            const auto message=RegisterWindowMessageW(L"Goden.CollectionQa.V1");DWORD_PTR result=0;
            if(message==0||!SendMessageTimeoutW(shell.result,message,100,static_cast<LPARAM>(preset),
                SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&result)||result!=TRUE)return 8;
            std::wcout<<L"Selected collection preset="<<preset<<L'\n';return 0;
        }
        if(action==L"shell-collection-original"||action==L"shell-collection-music"){
            const auto message=RegisterWindowMessageW(L"Goden.CollectionQa.V1");DWORD_PTR result=0;
            const auto music=action==L"shell-collection-music"?1:0;
            if(message==0||!SendMessageTimeoutW(shell.result,message,101,music,
                SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&result)||result!=TRUE)return 8;
            std::wcout<<L"Selected collection mode="<<(music?L"music":L"original")<<L'\n';return 0;
        }
        if (action == L"shell-curated-state" ||
                action == L"shell-curated-caustics" ||
                action == L"shell-curated-wave" ||
                action == L"shell-curated-original" ||
                action == L"shell-curated-music" ||
                action == L"shell-curated-c3" ||
                action == L"shell-curated-c2" ||
                action == L"shell-curated-speed1" ||
                action == L"shell-curated-speed15" ||
                action == L"shell-curated-speed2" ||
                action == L"shell-curated-palette-original" ||
                action == L"shell-curated-palette-album" ||
                action == L"shell-curated-restart" ||
                action == L"shell-a4-material-legacy" ||
                action == L"shell-a4-material-clovis") {
            const auto command = [&](WPARAM selector, LPARAM value) {
                DWORD_PTR result = 0;
                return qaMessage != 0 && SendMessageTimeoutW(shell.result,
                    qaMessage, selector, value, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    5000, &result) != 0 && result == TRUE;
            };
            if (action == L"shell-curated-caustics" && !command(145, 2)) return 8;
            if (action == L"shell-curated-wave" && !command(145, 3)) return 8;
            if (action == L"shell-curated-original" && !command(142, 0)) return 8;
            if (action == L"shell-curated-music" && !command(142, 1)) return 8;
            if (action == L"shell-curated-c3" && !command(154, 0)) return 8;
            if (action == L"shell-curated-c2" && !command(154, 1)) return 8;
            if (action == L"shell-curated-speed1" && !command(155, 0)) return 8;
            if (action == L"shell-curated-speed15" && !command(155, 1)) return 8;
            if (action == L"shell-curated-speed2" && !command(155, 2)) return 8;
            if (action == L"shell-curated-palette-original" && !command(143, 0)) return 8;
            if (action == L"shell-curated-palette-album" && !command(143, 1)) return 8;
            if (action == L"shell-curated-restart" && !command(144, 0)) return 8;
            if (action == L"shell-a4-material-legacy" && !command(156, 0)) return 8;
            if (action == L"shell-a4-material-clovis" && !command(156, 1)) return 8;
            const auto snapshotVersion = query(152);
            if (!snapshotVersion) return 8;
            std::array<std::optional<LRESULT>, 12> values{};
            for (std::size_t index = 0; index < values.size(); ++index)
                values[index] = query(130U + index);
            const auto style = query(65);
            const auto mode = query(146);
            const auto palette = query(147);
            const auto response = query(157);
            const auto speed = query(158);
            const auto material = query(159);
            const auto drawnRevision = query(148);
            const auto prepareLastUs = query(149);
            const auto discardedCatchupMs = query(150);
            const auto gpuTimingAvailable = query(151);
            const auto released = query(153);
            if (!style || !mode || !palette || !response || !speed || !material || !drawnRevision || !prepareLastUs ||
                    !discardedCatchupMs || !gpuTimingAvailable || !released ||
                    std::any_of(values.begin(), values.end(),
                    [](const auto& value) { return !value.has_value(); })) return 8;
            std::wcout << L"Curated snapshotVersion=" << *snapshotVersion
                << L" style=" << *style
                << L" mode=" << *mode
                << L" palette=" << *palette
                << L" response=" << *response
                << L" speed=" << *speed
                << L" material=" << *material
                << L" referenceFrames=" << *values[0]
                << L" renderedFrames=" << *values[1]
                << L" outputRevision=" << *values[2]
                << L" drawnRevision=" << *drawnRevision
                << L" acceptedRevision=" << *values[3]
                << L" resetCount=" << *values[4]
                << L" bytes=" << *values[5]
                << L" gpuP95Us=" << *values[6]
                << L" prepareLastUs=" << *prepareLastUs
                << L" discardedCatchupMs=" << *discardedCatchupMs
                << L" gpuTimingAvailable=" << (*gpuTimingAvailable ? L"true" : L"false")
                << L" hr=" << *values[7]
                << L" ready=" << (*values[8] ? L"true" : L"false")
                << L" budgetPaused=" << (*values[9] ? L"true" : L"false")
                << L" dispatches=" << *values[10]
                << L" active=" << (*values[11] ? L"true" : L"false") << L'\n';
            return 0;
        }
        if (action == L"shell-visual-source-stall" || action == L"shell-visual-source-state") {
            if (action == L"shell-visual-source-stall") {
                const auto accepted=query(2202);
                if(!accepted || *accepted!=TRUE)return 8;
            }
            const auto count=query(2203);
            if(!count)return 8;
            std::wcout<<L"VisualSource recoveries="<<*count<<L'\n';
            return 0;
        }
        if (action == L"shell-album-theater-state" ||
                action == L"shell-album-theater-on" || action == L"shell-album-theater-off") {
            if (action != L"shell-album-theater-state") {
                DWORD_PTR result=0;
                if (!SendMessageTimeoutW(shell.result,qaMessage,2201,
                        action==L"shell-album-theater-on",SMTO_ABORTIFHUNG|SMTO_BLOCK,
                        5000,&result) || result!=TRUE)return 8;
            }
            const auto enabled=query(2200);
            if(!enabled)return 8;
            std::wcout<<L"AlbumTheater enabled="<<(*enabled?L"true":L"false")<<L'\n';
            return 0;
        }
        if (action == L"shell-a4-state" || action == L"shell-a4-select" ||
                action == L"shell-flow-select" ||
                action == L"shell-a4-v2-refined" ||
                action == L"shell-a4-v2-hybrid" ||
                action == L"shell-a4-v2-show" ||
                action == L"shell-a4-v2-large" ||
                action == L"shell-a4-cap-light" ||
                action == L"shell-a4-cap-field" ||
                action == L"shell-a4-retry" ||
                action == L"shell-a4-grain-off" ||
                action == L"shell-a4-grain-fine" ||
                action == L"shell-a4-grain-strong" ||
                action == L"shell-a4-quality-full" ||
                action == L"shell-a4-quality-auto" ||
                action == L"shell-a4-quality-light" ||
                action == L"shell-a4-quality-field") {
            const auto command = [&](WPARAM selector, LPARAM value) {
                DWORD_PTR result = 0;
                return qaMessage != 0 && SendMessageTimeoutW(shell.result,
                    qaMessage, selector, value, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    5000, &result) != 0 && result == TRUE;
            };
            if (action == L"shell-a4-select" && !command(67, 1)) return 8;
            if (action == L"shell-flow-select" && !command(67, 0)) return 8;
            if (action == L"shell-a4-v2-refined" &&
                    (!command(156, 2) || !command(177, 0))) return 8;
            if (action == L"shell-a4-v2-hybrid" &&
                    (!command(156, 2) || !command(177, 1))) return 8;
            if (action == L"shell-a4-v2-show" &&
                    (!command(156, 2) || !command(177, 2))) return 8;
            if (action == L"shell-a4-v2-large" &&
                    (!command(156, 2) || !command(177, 3))) return 8;
            if (action == L"shell-a4-cap-light" && !command(98, 1)) return 8;
            if (action == L"shell-a4-cap-field" && !command(98, 2)) return 8;
            if (action == L"shell-a4-retry" && !command(99, 0)) return 8;
            if (action == L"shell-a4-grain-off" && !command(68, 0)) return 8;
            if (action == L"shell-a4-grain-fine" && !command(68, 1)) return 8;
            if (action == L"shell-a4-grain-strong" && !command(68, 2)) return 8;
            if (action == L"shell-a4-quality-full" && !command(117, 0)) return 8;
            if (action == L"shell-a4-quality-auto" && !command(117, 1)) return 8;
            if (action == L"shell-a4-quality-light" && !command(117, 2)) return 8;
            if (action == L"shell-a4-quality-field" && !command(117, 3)) return 8;
            const auto style = query(65);
            const auto grain = query(66);
            const auto quality = query(116);
            const auto v2MainBands = query(178);
            if (!style || !grain || !quality || !v2MainBands) return 8;
            std::wcout << L"AlbumVisual style=" << *style
                << L" grain=" << *grain << L" quality=" << *quality
                << L" v2MainBands=" << *v2MainBands << L'\n';
            return 0;
        }
        if (action == L"shell-a4-r12-diagnostics") {
            std::array<std::optional<LRESULT>, 16> values{};
            for (std::size_t index = 0; index < values.size(); ++index)
                values[index] = query(82U + index);
            if (std::any_of(values.begin(), values.end(),
                    [](const auto& value) { return !value.has_value(); })) return 8;
            std::wcout << L"AlloyR12 tier=" << *values[0]
                << L" gpuDecorativeP95Us=" << *values[1]
                << L" gpuBlurP95Us=" << *values[2]
                << L" gpuSceneP95Us=" << *values[3]
                << L" gpuFinishP95Us=" << *values[4]
                << L" budgetEpoch=" << *values[5]
                << L" budgetSamples=" << *values[6]
                << L" budgetWindows=" << *values[7]
                << L" filmUpdates=" << *values[8]
                << L" finishedReuses=" << *values[9]
                << L" queryDrops=" << *values[10]
                << L" queryDisjoint=" << *values[11]
                << L" measured=" << (*values[12] ? L"true" : L"false")
                << L" cpuPrepareUs=" << *values[13]
                << L" cpuStageSumUs=" << *values[14]
                << L" capability=" << *values[15] << L'\n';
            return *values[15] == 12 ? 0 : 8;
        }
        if (action == L"shell-album-scroll-telemetry") {
            // One coherent sample of both Album lists. Geometry selectors use
            // the same milli-DIP encoding as the wheel driver, so a caller can
            // aim a schedule at TRACKS or LYRICS without duplicating layout.
            const auto tracksLeft = coordinate(11);
            const auto tracksTop = coordinate(12);
            const auto tracksRight = coordinate(13);
            const auto tracksBottom = coordinate(14);
            const auto lyricsLeft = coordinate(112);
            const auto lyricsTop = coordinate(113);
            const auto lyricsRight = coordinate(114);
            const auto lyricsBottom = coordinate(115);
            const auto trackPosition = coordinate(118);
            const auto trackTarget = coordinate(119);
            const auto trackVelocity = coordinate(120);
            const auto trackActive = query(121);
            const auto lyricPosition = coordinate(122);
            const auto lyricTarget = coordinate(123);
            const auto lyricVelocity = coordinate(124);
            const auto lyricActive = query(125);
            const auto lyricMaximum = coordinate(126);
            const auto trackMaximum = coordinate(4);
            const auto trackCount = query(10);
            const auto pulses = query(23);
            const auto album = query(2);
            if (!tracksLeft || !tracksTop || !tracksRight || !tracksBottom ||
                    !lyricsLeft || !lyricsTop || !lyricsRight || !lyricsBottom ||
                    !trackPosition || !trackTarget || !trackVelocity ||
                    !trackActive || !lyricPosition || !lyricTarget ||
                    !lyricVelocity || !lyricActive || !lyricMaximum ||
                    !trackMaximum || !trackCount || !pulses || !album) {
                std::wcerr << L"Could not query ALBUM scroll telemetry\n";
                return 8;
            }
            const auto reset = actionArgc == 4 && _wcsicmp(argv[3], L"reset") == 0;
            if (reset) {
                query(127);
                Sleep(120);
            }
            std::wcout << std::fixed << std::setprecision(3)
                << L"AlbumScrollTelemetry schema=1"
                << L" albumMode=" << (*album ? 1 : 0)
                << L" tracks=" << *tracksLeft << L"," << *tracksTop << L","
                << *tracksRight << L"," << *tracksBottom
                << L" lyrics=" << *lyricsLeft << L"," << *lyricsTop << L","
                << *lyricsRight << L"," << *lyricsBottom
                << L" trackPosition=" << *trackPosition
                << L" trackTarget=" << *trackTarget
                << L" trackVelocity=" << *trackVelocity
                << L" trackActive=" << (*trackActive ? 1 : 0)
                << L" lyricPosition=" << *lyricPosition
                << L" lyricTarget=" << *lyricTarget
                << L" lyricVelocity=" << *lyricVelocity
                << L" lyricActive=" << (*lyricActive ? 1 : 0)
                << L" lyricMaximum=" << *lyricMaximum
                << L" trackMaximum=" << *trackMaximum
                << L" trackCount=" << *trackCount
                << L" framePulses=" << *pulses
                << L" reset=" << (reset ? 1 : 0) << L'\n';
            return 0;
        }
        if (action == L"shell-album-progress") {
            std::array<std::optional<LRESULT>, 12> values{};
            for (std::size_t index = 0; index < values.size(); ++index)
                values[index] = query(100U + index);
            if (std::any_of(values.begin(), values.end(),
                    [](const auto& value) { return !value.has_value(); })) return 8;
            std::wcout << L"AlbumProgress schema=1"
                << L" desiredRevision=" << *values[0]
                << L" producedRevision=" << *values[1]
                << L" acceptedRevision=" << *values[2]
                << L" nextTextureQpc=" << *values[3]
                << L" lastTextureProductionQpc=" << *values[4]
                << L" attempted=" << *values[5]
                << L" accepted=" << *values[6]
                << L" rejected=" << *values[7]
                << L" deferTransitions=" << *values[8]
                << L" deferReasonMask=" << *values[9]
                << L" deferred=" << (*values[10] ? L"true" : L"false")
                << L" lastRealInputQpc=" << *values[11] << L'\n';
            return 0;
        }
        if (action == L"shell-a4-diagnostics") {
            std::array<std::optional<LRESULT>, 13> values{};
            for (std::size_t index = 0; index < values.size(); ++index)
                values[index] = query(69U + index);
            if (std::any_of(values.begin(), values.end(),
                    [](const auto& value) { return !value.has_value(); })) return 8;
            std::wcout << L"Alloy fieldUpdates=" << *values[0]
                << L" sceneCompositions=" << *values[1]
                << L" resourceBytes=" << *values[2]
                << L" lastHr=" << *values[3]
                << L" ready=" << (*values[4] ? L"true" : L"false")
                << L" low=" << (*values[5] ? L"true" : L"false")
                << L" fieldProduced=" << (*values[6] ? L"true" : L"false")
                << L" latched=" << (*values[7] ? L"true" : L"false")
                << L" gpuFieldP95Us=" << *values[8]
                << L" gpuInclusiveP95Us=" << *values[9]
                << L" reusedScenes=" << *values[10]
                << L" fieldRevision=" << *values[11]
                << L" sceneReady=" << (*values[12] ? L"true" : L"false")
                << L'\n';
            return 0;
        }
        if (action == L"shell-album-transition-state") {
            const auto active = query(15);
            const auto presented = query(16);
            const auto glyphFrames = query(17);
            const auto maximumFrameMicros = query(18);
            const auto generation = query(19);
            if (!active || !presented || !glyphFrames ||
                    !maximumFrameMicros || !generation) {
                std::wcerr << L"Could not query ALBUM artwork transition\n";
                return 8;
            }
            std::wcout << L"AlbumArtworkTransition active="
                << (*active ? L"true" : L"false")
                << L" presentedFrames=" << *presented
                << L" glyphFrames=" << *glyphFrames
                << L" maxFrameMs=" << std::fixed << std::setprecision(3)
                << static_cast<double>(*maximumFrameMicros) / 1000.0
                << L" generation=" << *generation
                << L" acceptedFrames=" << query(180).value_or(0)
                << L" rejectedFrames=" << query(181).value_or(0)
                << L" fallbackFrames=" << query(182).value_or(0)
                << L" maxIntervalMs=" << static_cast<double>(query(183).value_or(0))/1000.0
                << L" p95IntervalMs=" << static_cast<double>(query(184).value_or(0))/1000.0
                << L" prepared=" << query(185).value_or(0) << L'\n';
            return 0;
        }
        if (action == L"shell-album-ambient-state") {
            const auto mode = query(25);
            const auto fallback = query(26);
            const auto updates = query(27);
            const auto publications = query(28);
            const auto bytes = query(29);
            const auto gpu = query(30);
            const auto p95 = query(31);
            const auto flags = query(32);
            const auto featureMask = query(33);
            const auto windows = query(34);
            const auto latestSequence = query(35);
            const auto snapshotRevision = query(36);
            const auto snapshotStream = query(37);
            const auto snapshotSequence = query(38);
            const auto runtimeStream = query(39);
            const auto controllerState = query(40);
            const auto wantsPump = query(41);
            const auto textureDue = query(42);
            const auto visible = query(43);
            const auto playing = query(44);
            const auto muted = query(45);
            const auto hardware = query(46);
            const auto artwork = query(47);
            const auto booklet = query(48);
            const auto daw = query(49);
            const auto outside = query(50);
            const auto reduced = query(51);
            const auto albumMode = query(52);
            const auto visualRevision = query(53);
            const auto presentedRevision = query(54);
            const auto physicalMotion = query(55);
            const auto ingestAccepted = query(56);
            const auto playbackSerial = query(57);
            const auto bassPowerDb100 = query(58);
            const auto midPowerDb100 = query(59);
            const auto treblePowerDb100 = query(60);
            const auto runtimeSerial = query(61);
            const auto runtimeIdentity = query(62);
            const auto semanticIdentity = query(63);
            const auto backendTrack = query(64);
            if (!mode || !fallback || !updates || !publications || !bytes ||
                    !gpu || !p95 || !flags || !featureMask || !windows ||
                    !latestSequence || !snapshotRevision || !snapshotStream ||
                    !snapshotSequence || !runtimeStream || !controllerState ||
                    !wantsPump || !textureDue || !visible || !playing ||
                    !muted || !hardware || !artwork || !booklet || !daw ||
                    !outside || !reduced || !albumMode || !visualRevision ||
                    !presentedRevision || !physicalMotion || !ingestAccepted ||
                    !playbackSerial || !bassPowerDb100 || !midPowerDb100 ||
                    !treblePowerDb100 || !runtimeSerial || !runtimeIdentity ||
                    !semanticIdentity || !backendTrack) {
                std::wcerr << L"Could not query ALBUM ambient diagnostics\n";
                return 8;
            }
            std::wcout << L"AlbumAmbient mode=" << *mode
                << L" fallback=" << *fallback
                << L" textureUpdates=" << *updates
                << L" featurePublications=" << *publications
                << L" resourceBytes=" << *bytes
                << L" gpuUs=" << *gpu
                << L" gpuP95Us=" << *p95
                << L" gpuMeasured=" << ((*flags & 1) ? L"true" : L"false")
                << L" degraded=" << ((*flags & 2) ? L"true" : L"false")
                << L" featureMask=" << *featureMask
                << L" processedWindows=" << *windows
                << L" latestSequence=" << *latestSequence
                << L" snapshotRevision=" << *snapshotRevision
                << L" snapshotStream=" << *snapshotStream
                << L" snapshotSequence=" << *snapshotSequence
                << L" runtimeStream=" << *runtimeStream
                << L" controllerState=" << *controllerState
                << L" wantsPump=" << (*wantsPump ? L"true" : L"false")
                << L" textureDue=" << (*textureDue ? L"true" : L"false")
                << L" visible=" << (*visible ? L"true" : L"false")
                << L" playing=" << (*playing ? L"true" : L"false")
                << L" muted=" << (*muted ? L"true" : L"false")
                << L" hardware=" << (*hardware ? L"true" : L"false")
                << L" artwork=" << (*artwork ? L"true" : L"false")
                << L" booklet=" << (*booklet ? L"true" : L"false")
                << L" daw=" << (*daw ? L"true" : L"false")
                << L" outside=" << (*outside ? L"true" : L"false")
                << L" reduced=" << (*reduced ? L"true" : L"false")
                << L" albumMode=" << (*albumMode ? L"true" : L"false")
                << L" visualRevision=" << *visualRevision
                << L" presentedRevision=" << *presentedRevision
                << L" physicalMotion=" << (*physicalMotion ? L"true" : L"false")
                << L" ingestAccepted=" << (*ingestAccepted ? L"true" : L"false")
                << L" playbackSerial=" << *playbackSerial
                << L" powerDb=" << static_cast<double>(*bassPowerDb100) / 100.0
                << L',' << static_cast<double>(*midPowerDb100) / 100.0
                << L',' << static_cast<double>(*treblePowerDb100) / 100.0
                << L" runtimeSerial=" << *runtimeSerial
                << L" runtimeIdentity=" << (*runtimeIdentity ? L"true" : L"false")
                << L" semanticIdentity=" << (*semanticIdentity ? L"true" : L"false")
                << L" backendTrack=" << (*backendTrack ? L"true" : L"false")
                << L'\n';
            return 0;
        }
        if (action == L"benchmark-album-scroll-input") {
            const auto left = coordinate(11);
            const auto top = coordinate(12);
            const auto right = coordinate(13);
            const auto bottom = coordinate(14);
            if (!left || !top || !right || !bottom || *right <= *left ||
                    *bottom <= *top) {
                std::wcerr << L"ALBUM tracklist is not visible\n";
                return 8;
            }
            const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 80;
            const auto count = std::clamp(requested, 1, 500);
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
                std::wcerr << L"High-resolution clock is unavailable\n";
                return 8;
            }
            std::vector<double> elapsed;
            elapsed.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index) {
                LARGE_INTEGER started{};
                LARGE_INTEGER finished{};
                QueryPerformanceCounter(&started);
                if (!wheelDip(shell.result, (*left + *right) * 0.5f,
                        (*top + *bottom) * 0.5f,
                        index % 2 == 0 ? -WHEEL_DELTA : WHEEL_DELTA)) {
                    std::wcerr << L"ALBUM scroll input benchmark failed\n";
                    return 7;
                }
                QueryPerformanceCounter(&finished);
                elapsed.push_back(
                    static_cast<double>(finished.QuadPart - started.QuadPart) *
                    1000.0 / static_cast<double>(frequency.QuadPart));
                Sleep(6);
            }
            std::sort(elapsed.begin(), elapsed.end());
            const auto percentile = [&](double value) {
                const auto rank = static_cast<std::size_t>(std::ceil(
                    value * static_cast<double>(elapsed.size())));
                return elapsed[
                    std::clamp<std::size_t>(rank, 1U, elapsed.size()) - 1U];
            };
            std::wcout << std::fixed << std::setprecision(3)
                << L"ALBUM_SCROLL_INPUT_BENCHMARK count=" << elapsed.size()
                << L" median_ms=" << percentile(0.5)
                << L" p95_ms=" << percentile(0.95)
                << L" max_ms=" << elapsed.back() << L'\n';
            return 0;
        }
        if (action == L"benchmark-album-lyrics-scroll-input") {
            const auto left = coordinate(112);
            const auto top = coordinate(113);
            const auto right = coordinate(114);
            const auto bottom = coordinate(115);
            const auto beforePulses = query(23);
            const auto beforeFilm = query(90);
            const auto collectionMessage =
                RegisterWindowMessageW(L"Goden.CollectionQa.V1");
            const auto queryCollection =
                [&](WPARAM selector) -> std::optional<LRESULT> {
                DWORD_PTR value = 0;
                if (collectionMessage == 0 || !SendMessageTimeoutW(
                        shell.result, collectionMessage, selector, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &value)) {
                    return std::nullopt;
                }
                return static_cast<LRESULT>(value);
            };
            const auto beforeCollectionSelected = queryCollection(0);
            const auto beforeCollectionProduced = queryCollection(3);
            if (!left || !top || !right || !bottom || !beforePulses ||
                    !beforeFilm || *right <= *left || *bottom <= *top) {
                std::wcerr << L"ALBUM lyrics surface is not visible\n";
                return 8;
            }
            const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 80;
            const auto count = std::clamp(requested, 1, 500);
            for (int index = 0; index < count; ++index) {
                if (!wheelDip(shell.result, (*left + *right) * 0.5f,
                        (*top + *bottom) * 0.5f,
                        index % 2 == 0 ? -WHEEL_DELTA : WHEEL_DELTA)) {
                    std::wcerr << L"ALBUM lyrics scroll input failed\n";
                    return 7;
                }
                Sleep(6);
            }
            Sleep(100);
            const auto afterPulses = query(23);
            const auto afterFilm = query(90);
            const auto afterCollectionProduced = queryCollection(3);
            if (!afterPulses || !afterFilm || !beforeCollectionSelected ||
                    !beforeCollectionProduced || !afterCollectionProduced) {
                return 8;
            }
            const auto collectionUpdates = *afterCollectionProduced -
                *beforeCollectionProduced;
            std::wcout << L"ALBUM_LYRICS_SCROLL_INPUT count=" << count
                << L" framePulses=" << (*afterPulses - *beforePulses)
                << L" filmUpdates=" << (*afterFilm - *beforeFilm)
                << L" collectionUpdates=" << collectionUpdates << L'\n';
            const auto visualAdvanced = *beforeCollectionSelected != 0
                ? collectionUpdates > 0 : *afterFilm > *beforeFilm;
            return *afterPulses > *beforePulses && visualAdvanced ? 0 : 8;
        }
        if (action == L"benchmark-curated-lyrics-scroll-input") {
            const auto left = coordinate(112);
            const auto top = coordinate(113);
            const auto right = coordinate(114);
            const auto bottom = coordinate(115);
            const auto beforePulses = query(23);
            const auto beforeReference = query(130);
            const auto beforeRendered = query(131);
            if (!left || !top || !right || !bottom || !beforePulses ||
                    !beforeReference || !beforeRendered || *right <= *left ||
                    *bottom <= *top) {
                std::wcerr << L"Curated lyrics surface is not visible\n";
                return 8;
            }
            const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 80;
            const auto count = std::clamp(requested, 1, 500);
            for (int index = 0; index < count; ++index) {
                if (!wheelDip(shell.result, (*left + *right) * 0.5f,
                        (*top + *bottom) * 0.5f,
                        index % 2 == 0 ? -WHEEL_DELTA : WHEEL_DELTA)) {
                    std::wcerr << L"Curated lyrics scroll input failed\n";
                    return 7;
                }
                Sleep(6);
            }
            Sleep(100);
            const auto afterPulses = query(23);
            const auto afterReference = query(130);
            const auto afterRendered = query(131);
            if (!afterPulses || !afterReference || !afterRendered) return 8;
            std::wcout << L"CURATED_LYRICS_SCROLL_INPUT count=" << count
                << L" framePulses=" << (*afterPulses - *beforePulses)
                << L" referenceFrames=" << (*afterReference - *beforeReference)
                << L" renderedFrames=" << (*afterRendered - *beforeRendered) << L'\n';
            return *afterPulses > *beforePulses &&
                (*afterReference > *beforeReference ||
                 *afterRendered > *beforeRendered) ? 0 : 8;
        }
        if (action == L"shell-album-scroll-bound") {
            const auto left = coordinate(11);
            const auto top = coordinate(12);
            const auto right = coordinate(13);
            const auto bottom = coordinate(14);
            if (!left || !top || !right || !bottom || *right <= *left ||
                    *bottom <= *top) {
                std::wcerr << L"ALBUM tracklist is not visible\n";
                return 8;
            }
            for (unsigned index = 0; index < 80; ++index) {
                if (!wheelDip(shell.result, (*left + *right) * 0.5f,
                        (*top + *bottom) * 0.5f, -WHEEL_DELTA)) {
                    std::wcerr << L"ALBUM tracklist did not accept wheel input\n";
                    return 7;
                }
            }
            Sleep(120);
            const auto position = query(3);
            const auto maximum = query(4);
            const auto count = query(10);
            const auto passed = position && maximum && count &&
                *position >= 0 && *position <= *maximum &&
                *position == *maximum;
            std::wcout << L"AlbumTrackScroll tracks="
                << (count ? *count : -1) << L" positionMilliDip="
                << (position ? *position : -1) << L" maximumMilliDip="
                << (maximum ? *maximum : -1) << L'\n';
            return passed ? 0 : 8;
        }

        if (action == L"shell-album-booklet-toggle") {
            const auto left = coordinate(5);
            const auto top = coordinate(6);
            const auto right = coordinate(7);
            const auto bottom = coordinate(8);
            const auto before = query(9);
            if(before && *before==0 && left && right && top && bottom &&
                (*right<=*left || *bottom<=*top)){
                std::wcout<<L"Album booklet unavailable: artwork is hidden\n";return 10;
            }
            const auto accepted = before && *before > 0
                ? SendMessageW(shell.result, WM_KEYDOWN, VK_ESCAPE, 0) == 0
                : left && top && right && bottom && *right > *left &&
                    *bottom > *top && clickDip(shell.result,
                        (*left + *right) * 0.5f, (*top + *bottom) * 0.5f);
            if (!accepted) {
                std::wcerr << L"Could not toggle ALBUM booklet\n";
                return 7;
            }
            Sleep(500);
            const auto progress = query(9);
            std::wcout << L"AlbumBookletToggle progressMilli="
                << (progress ? *progress : -1) << L'\n';
            return progress ? 0 : 8;
        }

        if (actionArgc != 4) {
            std::wcerr << L"shell-album-booklet-capture requires a directory\n";
            return 8;
        }
        const auto left = coordinate(5);
        const auto top = coordinate(6);
        const auto right = coordinate(7);
        const auto bottom = coordinate(8);
        if (!left || !top || !right || !bottom || *right <= *left ||
                *bottom <= *top) {
            std::wcerr << L"ALBUM artwork is not visible\n";
            return 8;
        }
        const std::filesystem::path directory(argv[3]);
        std::error_code createError;
        std::filesystem::create_directories(directory, createError);
        if (createError || !clickDip(shell.result,
                (*left + *right) * 0.5f, (*top + *bottom) * 0.5f)) {
            std::wcerr << L"Could not start ALBUM booklet animation\n";
            return 7;
        }
        bool captured = true;
        for (unsigned frame = 0; frame < 12; ++frame) {
            Sleep(35);
            const auto path = directory /
                (L"booklet-" + std::to_wstring(frame) + L".bmp");
            captured = captureWindowBmp(topLevel.result, path.c_str()) && captured;
        }
        const auto progress = query(9);
        std::wcout << L"AlbumBookletCapture frames=12 progressMilli="
            << (progress ? *progress : -1) << L" directory="
            << directory.c_str() << L'\n';
        return captured && progress && *progress > 0 ? 0 : 8;
    }
    if (action == L"window-capture") {
        if (actionArgc != 4 || !captureWindowBmp(topLevel.result, argv[3])) {
            std::wcerr << L"Main window capture failed\n";
            return 8;
        }
        std::wcout << L"Captured main window to " << argv[3] << L'\n';
        return 0;
    }
    if (action == L"options-capture" || action == L"daw-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"Secondary window capture requires a BMP path\n";
            return 8;
        }
        const auto className = action == L"options-capture"
            ? L"GodenOptionsV1" : L"GodenDawModeV1";
        const auto secondary = findOwnedWindow(fullPath, className);
        if (secondary == nullptr || !captureWindowBmp(secondary, argv[3])) {
            std::wcerr << L"Goden secondary window capture failed\n";
            return 8;
        }
        std::wcout << L"Captured " << className << L" to " << argv[3] << L'\n';
        return 0;
    }
    if (action == L"options-affordance-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"Options affordance capture requires state|path\n";
            return 8;
        }
        const auto fields = splitFields(argv[3]);
        if (fields.size() != 2U ||
                (fields[0] != L"normal" && fields[0] != L"hover" &&
                 fields[0] != L"pressed" && fields[0] != L"disabled")) {
            std::wcerr << L"Unknown Options affordance state\n";
            return 8;
        }
        const auto options = findOwnedWindow(fullPath, L"GodenOptionsV1");
        if (options == nullptr) {
            std::wcerr << L"Goden Options window was not found\n";
            return 8;
        }
        const auto target = descendantControl(
            options, fields[0] == L"disabled" ? 1023 : 1030);
        if (target == nullptr ||
                (fields[0] == L"disabled" && IsWindowEnabled(target))) {
            std::wcerr << L"Options affordance target has the wrong state\n";
            return 8;
        }
        SendMessageW(target, BM_SETSTATE, FALSE, 0);
        SendMessageW(target, WM_MOUSELEAVE, 0, 0);
        if (fields[0] == L"hover" || fields[0] == L"pressed") {
            RECT targetBounds{};
            GetClientRect(target, &targetBounds);
            SendMessageW(target, WM_MOUSEMOVE, 0, MAKELPARAM(
                (targetBounds.right - targetBounds.left) / 2,
                (targetBounds.bottom - targetBounds.top) / 2));
        }
        if (fields[0] == L"pressed") {
            SendMessageW(target, BM_SETSTATE, TRUE, 0);
        }
        InvalidateRect(target, nullptr, FALSE);
        UpdateWindow(target);
        UpdateWindow(options);
        const auto captured = captureWindowBmp(options, fields[1]);
        SendMessageW(target, BM_SETSTATE, FALSE, 0);
        SendMessageW(target, WM_MOUSELEAVE, 0, 0);
        InvalidateRect(target, nullptr, FALSE);
        if (!captured) {
            std::wcerr << L"Options affordance capture failed\n";
            return 8;
        }
        std::wcout << L"Captured Options " << fields[0]
            << L" state to " << fields[1] << L'\n';
        return 0;
    }
    if (action == L"daw-drop" || action == L"daw-state" ||
            action == L"daw-storage-state" ||
            action == L"daw-lifecycle" ||
            action == L"daw-monitor-state" ||
            action == L"daw-meter-state" || action == L"daw-position" ||
            action == L"daw-play-toggle" || action == L"daw-stop" ||
            action == L"daw-mute-left" || action == L"daw-mute-right" ||
            action == L"daw-seek" || action == L"daw-remove" ||
            action == L"daw-split" || action == L"daw-clone-offset" ||
            action == L"daw-close") {
        const auto daw = findOwnedWindow(fullPath, L"GodenDawModeV1");
        if (daw == nullptr) {
            std::wcerr << L"Goden DAW Mode window was not found\n";
            return 8;
        }
        if (action == L"daw-close") {
            DWORD_PTR ignored = 0;
            if (!SendMessageTimeoutW(daw, WM_CLOSE, 0, 0,
                    SMTO_ABORTIFHUNG, 5000, &ignored)) {
                std::wcerr << L"Goden DAW Mode did not close\n";
                return 8;
            }
            std::wcout << L"Closed Goden DAW Mode\n";
            return 0;
        }
        if (action == L"daw-drop") {
            if (actionArgc != 4 || !sendFileDrop(daw, argv[3])) {
                std::wcerr << L"Goden DAW test drop failed\n";
                return 8;
            }
            std::wcout << L"Dropped test file into Goden DAW Mode\n";
            return 0;
        }
        if (action == L"daw-play-toggle" || action == L"daw-stop" ||
                action == L"daw-mute-left" || action == L"daw-mute-right") {
            const auto commandMessage = RegisterWindowMessageW(
                L"GodenDawQaCommandV1");
            DWORD_PTR accepted = 0;
            const auto command = action == L"daw-play-toggle" ? 1U :
                action == L"daw-stop" ? 2U :
                action == L"daw-mute-left" ? 4U : 6U;
            if (commandMessage == 0 || !SendMessageTimeoutW(daw,
                    commandMessage, command, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &accepted) ||
                accepted == 0) {
                std::wcerr << L"Goden DAW command failed\n";
                return 8;
            }
            std::wcout << L"Sent " << action << L" to Goden DAW Mode\n";
            return 0;
        }
        if (action == L"daw-seek") {
            if (actionArgc != 4) {
                std::wcerr << L"daw-seek requires milliseconds\n";
                return 2;
            }
            wchar_t* end = nullptr;
            const auto milliseconds = std::wcstoll(argv[3], &end, 10);
            if (end == argv[3] || *end != L'\0' || milliseconds < 0) {
                std::wcerr << L"Invalid daw-seek milliseconds\n";
                return 2;
            }
            const auto commandMessage = RegisterWindowMessageW(
                L"GodenDawQaCommandV1");
            DWORD_PTR accepted = 0;
            if (commandMessage == 0 || !SendMessageTimeoutW(daw,
                    commandMessage, 7U, static_cast<LPARAM>(milliseconds),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &accepted) ||
                    accepted == 0) {
                std::wcerr << L"GODENplayer DAW seek failed\n";
                return 8;
            }
            std::wcout << L"DAW_SEEK ms=" << milliseconds << L'\n';
            return 0;
        }
        if (action == L"daw-remove" || action == L"daw-split" ||
                action == L"daw-clone-offset") {
            if (actionArgc != 4) {
                std::wcerr << action << L" requires an index or milliseconds\n";
                return 2;
            }
            wchar_t* end = nullptr;
            const auto value = std::wcstoll(argv[3], &end, 10);
            if (end == argv[3] || *end != L'\0' || value < 0) {
                std::wcerr << L"Invalid DAW clip-edit value\n";
                return 2;
            }
            const auto commandMessage = RegisterWindowMessageW(
                L"GodenDawQaCommandV1");
            DWORD_PTR accepted = 0;
            const auto command = action == L"daw-remove" ? 8U :
                action == L"daw-split" ? 9U : 10U;
            if (commandMessage == 0 || !SendMessageTimeoutW(daw,
                    commandMessage, command, static_cast<LPARAM>(value),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &accepted) ||
                    accepted == 0) {
                std::wcerr << L"GODENplayer DAW clip edit failed\n";
                return 8;
            }
            std::wcout << (action == L"daw-remove"
                ? L"DAW_REMOVE index=" : action == L"daw-split"
                ? L"DAW_SPLIT ms=" : L"DAW_CLONE_OFFSET ms=")
                << value << L'\n';
            return 0;
        }
        if (action == L"daw-monitor-state") {
            const auto stateMessage = RegisterWindowMessageW(
                L"GodenDawQaMonitorStateV1");
            DWORD_PTR state = 0;
            if (stateMessage == 0 || !SendMessageTimeoutW(daw, stateMessage,
                    0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &state)) {
                std::wcerr << L"Goden DAW monitor state query failed\n";
                return 8;
            }
            std::wcout << L"DAW_MONITOR muteLeft=" << ((state & 1U) != 0)
                << L" muteRight=" << ((state & 2U) != 0) << L'\n';
            return 0;
        }
        if (action == L"daw-meter-state") {
            const auto stateMessage = RegisterWindowMessageW(
                L"GodenDawQaMonitorStateV1");
            auto query = [&](WPARAM selector, DWORD_PTR& output) {
                output = 0;
                return stateMessage != 0 && SendMessageTimeoutW(
                    daw, stateMessage, selector, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &output) != 0;
            };
            DWORD_PTR gain = 0;
            DWORD_PTR muted = 0;
            DWORD_PTR packedPeaks = 0;
            if (!query(1U, gain) || !query(2U, muted) ||
                    !query(3U, packedPeaks)) {
                std::wcerr << L"Goden DAW meter state query failed\n";
                return 8;
            }
            const auto left = static_cast<unsigned>(packedPeaks & 0xffffU);
            const auto right = static_cast<unsigned>(
                (packedPeaks >> 16U) & 0xffffU);
            std::wcout << L"DAW_METER gain="
                << static_cast<double>(gain) / 1000000.0
                << L" masterMuted=" << (muted != 0)
                << L" peakLeft=" << static_cast<double>(left) / 65535.0
                << L" peakRight=" << static_cast<double>(right) / 65535.0
                << L'\n';
            return 0;
        }
        if (action == L"daw-position") {
            const auto positionMessage = RegisterWindowMessageW(
                L"GodenDawQaPositionV1");
            DWORD_PTR position = 0;
            if (positionMessage == 0 || !SendMessageTimeoutW(daw,
                    positionMessage, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &position)) {
                std::wcerr << L"Goden DAW position query failed\n";
                return 8;
            }
            std::wcout << L"DAW_POSITION ms=" << position << L'\n';
            return 0;
        }
        if (action == L"daw-lifecycle") {
            const auto lifecycleMessage = RegisterWindowMessageW(
                L"GodenDawQaLifecycleV2");
            auto query = [&](WPARAM selector, DWORD_PTR& output) {
                output = 0;
                return lifecycleMessage != 0 && SendMessageTimeoutW(
                    daw, lifecycleMessage, selector, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &output) != 0;
            };
            DWORD_PTR state = 0;
            DWORD_PTR result = 0;
            DWORD_PTR generation = 0;
            if (!query(0, state) || !query(1, result) ||
                    !query(2, generation)) {
                std::wcerr << L"Goden DAW lifecycle query failed\n";
                return 8;
            }
            constexpr std::array<const wchar_t*, 7> names{
                L"idle", L"starting", L"playing", L"stopping",
                L"paused", L"eof", L"error"};
            const auto stateName = state < names.size()
                ? names[static_cast<std::size_t>(state)] : L"unknown";
            std::wcout << L"DAW_LIFECYCLE state=" << stateName
                << L" result=" << result
                << L" generation=" << generation << L'\n';
            return 0;
        }
        const auto stateMessage = RegisterWindowMessageW(L"GodenDawQaStateV1");
        if (action == L"daw-storage-state") {
            auto query = [&](WPARAM selector, DWORD_PTR& output) {
                output = 0;
                return stateMessage != 0 && SendMessageTimeoutW(
                    daw, stateMessage, selector, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &output) != 0;
            };
            DWORD_PTR spill = 0, unique = 0, shared = 0;
            if (!query(1U, spill) || !query(2U, unique) ||
                    !query(3U, shared)) {
                std::wcerr << L"Goden DAW storage state query failed\n";
                return 8;
            }
            std::wcout << L"DAW_STORAGE spillTracks=" << spill
                << L" uniquePcm=" << unique
                << L" firstPcmReferences=" << shared << L'\n';
            return 0;
        }
        DWORD_PTR state = 0;
        if (stateMessage == 0 || !SendMessageTimeoutW(daw, stateMessage, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &state)) {
            std::wcerr << L"Goden DAW state query failed\n";
            return 8;
        }
        std::wcout << L"DAW_STATE tracks=" << (state & 0xffffU)
            << L" ready=" << ((state >> 16U) & 0xffffU)
            << L" playing=" << ((state >> 32U) & 1U)
            << L" audio=" << ((state >> 33U) & 0x7fffU)
            << L" failed=" << ((state >> 48U) & 0x7fffU)
            << L" bridge=" << ((state >> 63U) & 1U) << L'\n';
        return 0;
    }
    if (action == L"options-state" || action == L"options-scroll-top" ||
            action == L"options-scroll-bottom" ||
            action == L"options-scroll-cycle" ||
            action == L"options-resize-cycle" ||
            action == L"options-keyboard-focus" ||
            action == L"motion-mode" ||
            action == L"listen-effects-mode" || action == L"ambient-mode" ||
            action == L"presence-enable" ||
            action == L"presence-disable" || action == L"covers-enable" ||
            action == L"covers-disable" || action == L"paused-show" ||
            action == L"paused-hide" || action == L"options-close") {
        const auto options = findOwnedWindow(fullPath, L"GodenOptionsV1");
        if (options == nullptr) {
            std::wcerr << L"Goden Options window was not found\n";
            return 8;
        }
        if (action == L"options-state") {
            // Production intentionally suspends the 1 Hz status timer while
            // Options is hidden. QA still needs to observe the rendered
            // asynchronous status, so expose the disposable window offscreen
            // without activation before reading its native controls.
            if (background && !IsWindowVisible(options)) {
                if (!showOffscreenNoActivatePreservingForeground(options)) {
                    std::wcerr << L"Goden Options could not be exposed offscreen\n";
                    return 8;
                }
                Sleep(1050);
            }
            const auto appId = controlText(descendantControl(options, 1022));
            const auto status = controlText(descendantControl(options, 1024));
            const auto ambientStatus = controlText(
                descendantControl(options, 1008));
            const auto checked = [&](int id) {
                return SendMessageW(descendantControl(options, id),
                    BM_GETCHECK, 0, 0) ==
                    BST_CHECKED;
            };
            std::wcout << L"OPTIONS_STATE identity=" << checked(1001)
                << L" motion=" << SendMessageW(
                    descendantControl(options, 1004), CB_GETCURSEL, 0, 0)
                << L" listen_effects=" << SendMessageW(
                    descendantControl(options, 1005), CB_GETCURSEL, 0, 0)
                << L" keep=" << checked(1010)
                << L" keep_mode=" << SendMessageW(
                    descendantControl(options, 1011), CB_GETCURSEL, 0, 0)
                << L" presence=" << checked(1020)
                << L" paused=" << checked(1021)
                << L" covers=" << checked(1025)
                << L" ambient_mode=" << SendMessageW(
                    descendantControl(options, 1007), CB_GETCURSEL, 0, 0)
                << L" ambient_status=[" << ambientStatus << L"]"
                << L" app_id=" << appId
                << L" status=[" << status << L"]\n";
            return 0;
        }
        if (action == L"motion-mode" || action == L"listen-effects-mode") {
            if (actionArgc != 4) {
                std::wcerr << (action == L"motion-mode"
                    ? L"motion-mode requires 0 (Normal), 1 (Reduced), or 2 (Off)\n"
                    : L"listen-effects-mode requires 0 (Effects) or 1 (No effects)\n");
                return 8;
            }
            const auto value = _wtoi(argv[3]);
            const auto id = action == L"motion-mode" ? 1004 : 1005;
            const auto maximum = action == L"motion-mode" ? 2 : 1;
            const auto combo = descendantControl(options, id);
            if (combo == nullptr || value < 0 || value > maximum ||
                    SendMessageW(combo, CB_SETCURSEL, value, 0) == CB_ERR) {
                std::wcerr << L"Options combo selection failed\n";
                return 8;
            }
            SendMessageW(options, WM_COMMAND,
                MAKEWPARAM(id, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(combo));
            std::wcout << (action == L"motion-mode"
                ? L"Motion mode=" : L"Listen effects mode=") << value << L'\n';
            return 0;
        }
        if (action == L"options-scroll-top" ||
                action == L"options-scroll-bottom" ||
                action == L"options-scroll-cycle") {
            const auto bottom = action == L"options-scroll-bottom";
            if (!exerciseFormViewport(options, bottom)) {
                std::wcerr << L"Options form viewport scroll failed\n";
                return 8;
            }
            std::wcout << L"Options form viewport "
                << (bottom ? L"bottom" : L"top") << L" PASS\n";
            return 0;
        }
        if (action == L"options-resize-cycle") {
            if (!exerciseFormResize(options)) {
                std::wcerr << L"Options form resize cycle failed\n";
                return 8;
            }
            std::wcout << L"Options form resize 20 cycles PASS\n";
            return 0;
        }
        if (action == L"options-keyboard-focus") {
            if (!exerciseFormFocus(options, 1030)) {
                std::wcerr << L"Options keyboard focus reveal failed\n";
                return 8;
            }
            std::wcout << L"Options keyboard focus reveal and Tab PASS\n";
            return 0;
        }
        if (action == L"ambient-mode") {
            if (actionArgc != 4) {
                std::wcerr << L"ambient-mode requires 0 (Off), 1 (Subtle), or 2 (Reactive)\n";
                return 8;
            }
            const auto mode = _wtoi(argv[3]);
            const auto combo = descendantControl(options, 1007);
            if (combo == nullptr || mode < 0 || mode > 2 ||
                    SendMessageW(combo, CB_SETCURSEL, mode, 0) == CB_ERR) {
                std::wcerr << L"Album ambient mode selection failed\n";
                return 8;
            }
            SendMessageW(options, WM_COMMAND,
                MAKEWPARAM(1007, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(combo));
            std::wcout << L"Album ambient mode=" << mode << L'\n';
            return 0;
        }
        if (action == L"presence-enable" || action == L"presence-disable") {
            const auto enable = action == L"presence-enable";
            const auto check = descendantControl(options, 1020);
            if (check == nullptr) {
                std::wcerr << L"Presence checkbox was not found\n";
                return 8;
            }
            const auto current = SendMessageW(
                check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (current != enable) {
                SendMessageW(options, WM_COMMAND,
                    MAKEWPARAM(1020, BN_CLICKED),
                    reinterpret_cast<LPARAM>(check));
            }
            std::wcout << (enable ? L"Enabled" : L"Disabled")
                << L" native Discord Presence\n";
            return 0;
        }
        if (action == L"paused-show" || action == L"paused-hide") {
            const auto show = action == L"paused-show";
            const auto check = descendantControl(options, 1021);
            if (check == nullptr) {
                std::wcerr << L"Paused Presence checkbox was not found\n";
                return 8;
            }
            const auto current = SendMessageW(
                check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (current != show) {
                SendMessageW(options, WM_COMMAND,
                    MAKEWPARAM(1021, BN_CLICKED),
                    reinterpret_cast<LPARAM>(check));
            }
            std::wcout << (show ? L"Showing" : L"Hiding")
                << L" paused Discord Presence\n";
            return 0;
        }
        if (action == L"covers-enable" || action == L"covers-disable") {
            const auto enable = action == L"covers-enable";
            const auto check = descendantControl(options, 1025);
            if (check == nullptr) {
                std::wcerr << L"Presence cover checkbox was not found\n";
                return 8;
            }
            const auto current = SendMessageW(
                check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (current != enable) {
                SendMessageW(options, WM_COMMAND,
                    MAKEWPARAM(1025, BN_CLICKED),
                    reinterpret_cast<LPARAM>(check));
            }
            std::wcout << (enable ? L"Enabled" : L"Disabled")
                << L" temporary Discord covers\n";
            return 0;
        }
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(options, WM_CLOSE, 0, 0,
                SMTO_ABORTIFHUNG, 5000, &ignored)) {
            std::wcerr << L"Goden Options did not close\n";
            return 8;
        }
        std::wcout << L"Closed Goden Options\n";
        return 0;
    }
    if (action == L"note-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"Note editor capture requires a BMP path\n";
            return 8;
        }
        SearchContext editor{fullPath, L"NekoRoXNoteEditorV3", true};
        EnumWindows(findTopLevel, reinterpret_cast<LPARAM>(&editor));
        if (editor.result == nullptr ||
                !captureWindowBmp(editor.result, argv[3])) {
            std::wcerr << L"NekoRoX note editor capture failed\n";
            return 8;
        }
        std::wcout << L"Captured note editor to " << argv[3] << L'\n';
        return 0;
    }
    if (action == L"persona-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"Persona editor capture requires a BMP path\n";
            return 8;
        }
        const auto editor = findOwnedWindow(fullPath, L"NekoRoXPersonaEditorV3");
        if (editor == nullptr || !captureWindowBmp(editor, argv[3])) {
            std::wcerr << L"NekoRoX persona editor capture failed\n";
            return 8;
        }
        std::wcout << L"Captured persona editor to " << argv[3] << L'\n';
        return 0;
    }
    if (action.starts_with(L"listen-") && action != L"listen") {
        SearchContext listen{
            fullPath, L"NekoRoXListenWindowV3", true, true};
        EnumWindows(findTopLevel, reinterpret_cast<LPARAM>(&listen));
        if (listen.result == nullptr) {
            std::wcerr << L"NekoRoX Listen window was not found\n";
            return 5;
        }
        if (action == L"listen-window-state") {
            const auto owner = GetWindow(listen.result, GW_OWNER);
            const auto extendedStyle = static_cast<DWORD_PTR>(
                GetWindowLongPtrW(listen.result, GWL_EXSTYLE));
            const auto topmost = (extendedStyle & WS_EX_TOPMOST) != 0;
            RECT bounds{};
            GetWindowRect(listen.result, &bounds);
            std::wcout
                << L"Listen owner=" << reinterpret_cast<std::uintptr_t>(owner)
                << L" topmost=" << (topmost ? L"true" : L"false")
                << L" visible="
                << (IsWindowVisible(listen.result) ? L"true" : L"false")
                << L" iconic=" << (IsIconic(listen.result) ? L"true" : L"false")
                << L" zoomed=" << (IsZoomed(listen.result) ? L"true" : L"false")
                << L" foreground="
                << (GetForegroundWindow() == listen.result ? L"true" : L"false")
                << L" bounds=" << bounds.left << L',' << bounds.top
                << L',' << bounds.right << L',' << bounds.bottom
                << L'\n';
            return owner == nullptr && !topmost ? 0 : 9;
        }
        if (action == L"listen-place") {
            long x = 0;
            long y = 0;
            if (actionArgc != 4 ||
                    swscanf_s(argv[3], L"%ld,%ld", &x, &y) != 2 ||
                    !SetWindowPos(listen.result, HWND_TOP, x, y, 0, 0,
                        SWP_NOSIZE | SWP_NOACTIVATE)) {
                std::wcerr << L"listen-place requires x,y coordinates\n";
                return 8;
            }
            std::wcout << L"Moved Listen to " << x << L',' << y << L'\n';
            return 0;
        }
        if (action == L"listen-analysis-state") {
            const auto message = RegisterWindowMessageW(
                nekorox::kListenAnalysisStateMessageV1);
            DWORD_PTR packed = 0;
            const auto sent = message != 0 && SendMessageTimeoutW(
                listen.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &packed) != 0;
            if (!sent) {
                std::wcerr << L"Listen analysis state query failed\n";
                return 8;
            }
            const auto state = static_cast<std::uint32_t>(packed);
            const auto status = (state >>
                nekorox::kListenAnalysisStatusShiftV1) &
                nekorox::kListenAnalysisStatusMaskV1;
            constexpr const wchar_t* labels[] = {
                L"idle", L"running", L"complete", L"cancelled", L"error"};
            const auto* label = status < std::size(labels)
                ? labels[status] : L"unknown";
            std::wcout
                << L"Listen lifecycleVisible="
                << ((state & nekorox::kListenAnalysisVisibleBitV1) != 0
                    ? L"true" : L"false")
                << L" pending="
                << ((state & nekorox::kListenAnalysisPendingBitV1) != 0
                    ? L"true" : L"false")
                << L" status=" << label << L'\n';
            return 0;
        }
        if (action == L"listen-artwork-state") {
            const auto message = RegisterWindowMessageW(
                nekorox::kListenArtworkStateMessageV1);
            DWORD_PTR packed = 0;
            const auto sent = message != 0 && SendMessageTimeoutW(
                listen.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &packed) != 0;
            if (!sent) {
                std::wcerr << L"Listen artwork state query failed\n";
                return 8;
            }
            const auto state = static_cast<std::uint32_t>(packed);
            const auto flag = [&](std::uint32_t bit) {
                return (state & bit) != 0 ? L"true" : L"false";
            };
            const auto mode = (state >>
                nekorox::kListenArtworkMotionModeShiftV1) &
                nekorox::kListenArtworkMotionModeMaskV1;
            constexpr const wchar_t* modes[] = {
                L"normal", L"reduced", L"off"};
            std::wcout
                << L"Listen artwork track="
                << flag(nekorox::kListenArtworkTrackValidBitV1)
                << L" info=" << flag(nekorox::kListenArtworkTrackInfoBitV1)
                << L" pixels=" << flag(nekorox::kListenArtworkPixelsReadyBitV1)
                << L" render=" << flag(nekorox::kListenArtworkRenderReadyBitV1)
                << L" transition="
                << flag(nekorox::kListenArtworkTransitionActiveBitV1)
                << L" cache-loading="
                << flag(nekorox::kListenArtworkCacheLoadingBitV1)
                << L" cache-missing="
                << flag(nekorox::kListenArtworkCacheMissingBitV1)
                << L" cache-ready="
                << flag(nekorox::kListenArtworkCacheReadyBitV1)
                << L" motion="
                << (mode < std::size(modes) ? modes[mode] : L"unknown")
                << L'\n';
            return 0;
        }
        if (action == L"listen-minimize") {
            ShowWindow(listen.result,
                background ? SW_SHOWMINNOACTIVE : SW_MINIMIZE);
            if (background) sendToBackNoActivate(listen.result);
            std::wcout << L"Minimized Listen on " << fullPath << L'\n';
            return 0;
        }
        if (action == L"listen-hide") {
            ShowWindow(listen.result, SW_HIDE);
            std::wcout << L"Hid Listen on " << fullPath << L'\n';
            return 0;
        }
        if (action == L"listen-show-noactivate") {
            if (!showNoActivatePreservingForeground(listen.result)) {
                std::wcerr << L"Listen did not restore without activation\n";
                return 7;
            }
            std::wcout << L"Restored Listen without activation on "
                << fullPath << L'\n';
            return 0;
        }
        if (!background) {
            ShowWindow(listen.result, SW_RESTORE);
            SetWindowPos(listen.result, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(listen.result, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            BringWindowToTop(listen.result);
            SetForegroundWindow(listen.result);
            Sleep(80);
        } else if (action != L"listen-close") {
            if (!IsWindowVisible(listen.result) || IsIconic(listen.result)) {
                std::wcerr << L"Background Listen action requires a visible, "
                    L"non-minimized window\n";
                return 7;
            }
            sendToBackNoActivate(listen.result);
        }
        RECT client{};
        GetClientRect(listen.result, &client);
        const auto dpi = static_cast<float>(GetDpiForWindow(listen.result));
        const auto width = static_cast<float>(client.right - client.left) * 96.0f /
            std::max(1.0f, dpi);
        if (action == L"listen-capture") {
            if (actionArgc != 4) {
                std::wcerr << L"Listen window capture requires a BMP path\n";
                return 8;
            }
            const auto message = RegisterWindowMessageW(
                nekorox::kListenCaptureMessageV1);
            DWORD_PTR captured = FALSE;
            const auto sent = message != 0 && SendMessageTimeoutW(
                listen.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &captured) != 0;
            const auto snapshot = std::filesystem::path(fullPath).parent_path() /
                L"profile" / nekorox::kListenCaptureRelativePathV1;
            const auto copied = sent && captured != FALSE &&
                CopyFileW(snapshot.c_str(), argv[3], FALSE) != FALSE;
            if (copied) DeleteFileW(snapshot.c_str());
            if (!copied) {
                std::wcerr << L"Listen window capture failed\n";
                return 8;
            }
            std::wcout << L"Captured Listen to " << argv[3] << L'\n';
            return 0;
        }
        if (action == L"listen-crt-stats") {
            if (actionArgc != 4) {
                std::wcerr << L"Listen CRT stats requires a JSON path\n";
                return 8;
            }
            const auto message = RegisterWindowMessageW(
                nekorox::kListenCrtStatsMessageV1);
            DWORD_PTR captured = FALSE;
            const auto sent = message != 0 && SendMessageTimeoutW(
                listen.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &captured) != 0;
            const auto snapshot = std::filesystem::path(fullPath).parent_path() /
                L"profile" / nekorox::kListenCrtStatsRelativePathV1;
            const auto copied = sent && captured != FALSE &&
                CopyFileW(snapshot.c_str(), argv[3], FALSE) != FALSE;
            if (copied) DeleteFileW(snapshot.c_str());
            if (!copied) {
                std::wcerr << L"Listen CRT stats capture failed\n";
                return 8;
            }
            std::wcout << L"Captured Listen CRT stats to "
                << argv[3] << L'\n';
            return 0;
        }
        if (action == L"listen-window-capture") {
            if (!background) {
                SetWindowPos(listen.result, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            RedrawWindow(
                listen.result,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            Sleep(240);
            const auto captured =
                actionArgc == 4 && captureWindowBmp(listen.result, argv[3]);
            if (!background) {
                SetWindowPos(listen.result, HWND_NOTOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            if (!captured) {
                std::wcerr << L"Composed Listen window capture failed\n";
                return 8;
            }
            std::wcout << L"Captured composed Listen to "
                << argv[3] << L'\n';
            return 0;
        }
        if (action == L"listen-maximize") {
            if (!clickDip(listen.result, width - 45.0f, 15.0f)) {
                std::wcerr << L"Listen maximize button did not accept the click\n";
                return 7;
            }
            std::wcout << L"Clicked " << action << L" on " << fullPath << L'\n';
            return 0;
        }
        if (action == L"listen-analyze") {
            if (!clickDip(listen.result, width - 70.0f, 247.0f)) {
                std::wcerr << L"Listen Analyze button did not accept the click\n";
                return 7;
            }
            std::wcout << L"Clicked " << action << L" on " << fullPath << L'\n';
            return 0;
        }
        if (action == L"listen-favorite") {
            if (!clickDip(listen.result, width - 149.0f, 247.0f)) {
                std::wcerr << L"Listen favorite button did not accept the click\n";
                return 7;
            }
            std::wcout << L"Clicked " << action << L" on " << fullPath << L'\n';
            return 0;
        }
        if (action == L"listen-close") {
            DWORD_PTR ignored = 0;
            if (!SendMessageTimeoutW(listen.result, WM_CLOSE, 0, 0,
                    SMTO_ABORTIFHUNG, 2000, &ignored)) {
                std::wcerr << L"Listen window did not accept WM_CLOSE\n";
                return 7;
            }
            std::wcout << L"Closed Listen on " << fullPath << L'\n';
            return 0;
        }
        if (action == L"listen-mode-toggle") {
            DWORD_PTR ignored = 0;
            if (!SendMessageTimeoutW(
                    listen.result,
                    WM_KEYDOWN,
                    'X',
                    0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    2000,
                    &ignored)) {
                std::wcerr << L"Listen mode toggle did not accept the key\n";
                return 7;
            }
            std::wcout << L"Toggled Listen live mode on " << fullPath << L'\n';
            return 0;
        }
        const auto height = static_cast<float>(client.bottom - client.top) *
            96.0f / std::max(1.0f, dpi);
        const auto contentHeight = std::max(0.0f, height - 127.0f);
        const auto scopeBottom = 113.0f + (contentHeight - 12.0f) * 0.5f;
        if (action == L"listen-scope-state") {
            DWORD_PTR state = 0;
            const auto message = RegisterWindowMessageW(L"GodenListenScopeStateR2");
            if (!message || !SendMessageTimeoutW(listen.result, message, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &state)) return 7;
            std::wcout << L"Listen scope windowMs=" << (state & 0xffffU)
                << L" gainIndex=" << ((state >> 16U) & 0x3U)
                << L" mode=" << ((state >> 18U) & 0x7U)
                << L" focus=" << ((state >> 21U) & 0x7U) << L'\n';
            return 0;
        }
        if (action == L"listen-scope-120" || action == L"listen-gain-2") {
            const auto x = action == L"listen-scope-120" ? 144.0f : 249.0f;
            if (!clickDip(listen.result, x, scopeBottom - 16.0f)) return 7;
            std::wcout << L"Clicked effective Listen control " << action << L'\n';
            return 0;
        }
        if (action == L"listen-focus-scope" || action == L"listen-focus-crt" ||
            action == L"listen-focus-spectrum" || action == L"listen-focus-spectrogram") {
            const auto paneWidth = (width - 28.0f - 12.0f) * 0.5f;
            const auto paneHeight = (contentHeight - 12.0f) * 0.5f;
            const auto right = action == L"listen-focus-crt" || action == L"listen-focus-spectrogram";
            const auto bottom = action == L"listen-focus-spectrum" || action == L"listen-focus-spectrogram";
            const auto x = 14.0f + (right ? paneWidth + 12.0f : 0.0f) + paneWidth * 0.5f;
            const auto y = 113.0f + (bottom ? paneHeight + 12.0f : 0.0f) + paneHeight * 0.5f;
            if (!doubleClickDip(listen.result, x, y)) return 7;
            std::wcout << L"Focused Listen pane by double click\n";
            return 0;
        }
        if (action == L"listen-focus-return") {
            SendMessageW(listen.result, WM_KEYDOWN, VK_ESCAPE, 0);
            std::wcout << L"Returned from Listen focus with Escape\n";
            return 0;
        }
        constexpr std::array actions{L"listen-overview", L"listen-live"};
        const auto iterator = std::find(actions.begin(), actions.end(), action);
        if (iterator == actions.end()) {
            std::wcerr << L"Unknown Listen action\n";
            return 6;
        }
        const auto index = static_cast<float>(std::distance(actions.begin(), iterator));
        if (!clickDip(listen.result, width * (index + 0.5f) / 2.0f, 50.0f)) {
            std::wcerr << L"Listen window did not accept the click\n";
            return 7;
        }
        wchar_t title[1024]{};
        GetWindowTextW(listen.result, title, static_cast<int>(std::size(title)));
        RECT bounds{};
        GetWindowRect(listen.result, &bounds);
        std::wcout << L"Clicked " << action << L" on " << fullPath
            << L"; Listen title: [" << title << L"]"
            << L"; bounds: " << bounds.left << L"," << bounds.top << L","
            << bounds.right << L"," << bounds.bottom << L'\n';
        return 0;
    }

    if (action == L"dsp-open") {
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&header));
        if (!header.result) return 7;
        RECT bounds{}; GetClientRect(header.result, &bounds);
        const float scale = 96.0f / GetDpiForWindow(header.result);
        const float width = bounds.right * scale, height = bounds.bottom * scale;
        const float menuHeight = std::clamp(height - 24.0f, 28.0f, 38.0f);
        const float monitorWidth = std::max(20.0f, menuHeight - 6.0f) * 4.7f;
        // HeaderPanel::headerLayout, full utility row. Refuse compact layouts.
        if (width - 387.0f < monitorWidth + 200.0f) return 8;
        if (!clickDip(header.result, width - 222.0f - monitorWidth -
                5.0f - 62.0f - 5.0f - 42.0f - 5.0f - 20.0f, menuHeight * .5f)) return 7;
        for (unsigned i = 0; i < 80; ++i) {
            if (findOwnedWindow(fullPath, L"GODENplayer.MasterDSP.V1")) return 0;
            Sleep(25);
        }
        return 7;
    }
    if (action == L"dsp-capture" || action == L"dsp-click" || action == L"dsp-add" ||
            action == L"dsp-close" || action == L"dsp-minimize" || action == L"dsp-restore") {
        const auto dsp = findOwnedWindow(fullPath, L"GODENplayer.MasterDSP.V1");
        if (!dsp) return 7;
        if (action == L"dsp-close") { SendMessageW(dsp, WM_CLOSE, 0, 0); return 0; }
        if (action == L"dsp-minimize") { ShowWindow(dsp, SW_MINIMIZE); return 0; }
        if (action == L"dsp-restore") { ShowWindow(dsp, SW_SHOWNOACTIVATE); return 0; }
        if (actionArgc != 4) return 8;
        if (action == L"dsp-capture") return captureWindowBmp(dsp, argv[3]) ? 0 : 7;
        if (action == L"dsp-add") {
            // Exercise the existing add-browser selection and its commit event.
            ChildContext list{L"ListBox"};
            EnumChildWindows(dsp, findChild, reinterpret_cast<LPARAM>(&list));
            if (!list.result || !IsWindowVisible(list.result)) return 7;
            const auto index = SendMessageW(list.result, LB_FINDSTRINGEXACT,
                static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(argv[3]));
            if (index == LB_ERR) return 8;
            SendMessageW(list.result, LB_SETCURSEL, index, 0);
            SendMessageW(dsp, WM_COMMAND,
                MAKEWPARAM(GetDlgCtrlID(list.result), LBN_DBLCLK),
                reinterpret_cast<LPARAM>(list.result));
            return 0;
        }
        float x = 0, y = 0;
        if (swscanf_s(argv[3], L"%f,%f", &x, &y) != 2) return 8;
        return clickDip(dsp, x, y) ? 0 : 7;
    }

    ChildContext child{
        action == L"waveseek-state" ||
            action == L"benchmark-waveseek-first-pixel" ||
            action == L"waveseek-transition-capture" ||
            action == L"waveseek-hover-capture" ||
            action == L"benchmark-waveseek-hover" ||
            action == L"waveseek-pointer-tracking" ||
            action == L"waveseek-seek-percent"
            ? L"NekoRoXWaveSeekV3"
            : action == L"mute" || action == L"mute-on" ||
            action == L"repeat-toggle" || action == L"shuffle-toggle" ||
            action == L"mute-toggle" || action == L"volume-set"
            ? L"NekoRoXTransportV3"
            : action == L"np" || action == L"benchmark-np" ||
                action == L"listen" || action == L"play" ||
                action == L"options-open" || action == L"daw-open" ||
                action == L"stop" || action == L"previous" ||
                action == L"next" || action == L"frame-clock-state" ||
                action == L"window-maximize"
            ? L"NekoRoXHeaderV3"
            : L"NekoRoXWorkspaceV3"};
    EnumChildWindows(topLevel.result, findChild, reinterpret_cast<LPARAM>(&child));
    if (child.result == nullptr) {
        std::wcerr << L"Target NekoRoX panel was not found\n";
        return 5;
    }

    RECT client{};
    GetClientRect(child.result, &client);
    const auto dpi = static_cast<float>(GetDpiForWindow(child.result));
    const auto width = static_cast<float>(client.right - client.left) * 96.0f /
        std::max(1.0f, dpi);
    const auto height = static_cast<float>(client.bottom - client.top) * 96.0f /
        std::max(1.0f, dpi);
    const auto transportY = std::max(10.0f, height - 14.0f);
    bool clicked = false;
    if (action == L"workpack-wheel-driver") {
        if (actionArgc != 4) {
            std::wcerr << L"workpack-wheel-driver requires an encoded schedule\n";
            return 8;
        }
        std::wstring schedule = argv[3];
        // The default wheel target is the Library workspace child. Album
        // TRACKS/LYRICS are drawn by the player shell, so an `album-` prefixed
        // schedule is retargeted there without changing any existing schedule.
        if (schedule.rfind(L"album-", 0U) == 0U) {
            schedule.erase(0U, 6U);
            ChildContext shell{L"GodenPlayerShellV2"};
            EnumChildWindows(topLevel.result, findChild,
                reinterpret_cast<LPARAM>(&shell));
            if (shell.result == nullptr) {
                std::wcerr << L"Player shell was not found\n";
                return 5;
            }
            child.result = shell.result;
        }
        DWORD processId = 0;
        GetWindowThreadProcessId(topLevel.result, &processId);
        return runWorkpackWheelDriver(
            child.result, topLevel.result, processId, schedule);
    }
    if (action == L"performance-events") {
        DWORD processId = 0;
        GetWindowThreadProcessId(topLevel.result, &processId);
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 256;
        return dumpPerformanceEvents(processId,
            static_cast<std::size_t>(std::clamp(requested, 1, 8192)));
    }
    if (action == L"workspace-rating" ||
        action == L"workspace-rating-toggle" ||
        action == L"workspace-rating-state") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto query = [&](WPARAM selector, LPARAM parameter,
                               DWORD_PTR& result) {
            result = 0;
            return message != 0 && SendMessageTimeoutW(
                child.result,
                message,
                selector,
                parameter,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                5000,
                &result) != 0;
        };
        DWORD_PTR effective = 0;
        DWORD_PTR persisted = 0;
        if (action == L"workspace-rating" ||
            action == L"workspace-rating-toggle") {
            if (actionArgc != 4) {
                std::wcerr << action
                    << L" requires a value from 1 to 5\n";
                return 8;
            }
            const auto requested = std::clamp(_wtoi(argv[3]), 1, 5);
            if (!query(49, 0, effective) || !query(50, 0, persisted)) {
                std::wcerr << L"Workspace rating state query failed\n";
                return 8;
            }
            DWORD_PTR packedPoint = 0;
            if (!query(51, requested, packedPoint) ||
                static_cast<LRESULT>(packedPoint) == -1) {
                std::wcerr << L"Workspace rating target was not available\n";
                return 8;
            }
            const auto x = static_cast<float>(LOWORD(packedPoint));
            const auto y = static_cast<float>(HIWORD(packedPoint));
            const auto toggling = action == L"workspace-rating-toggle";
            if (!toggling && static_cast<int>(effective) == requested &&
                static_cast<int>(persisted) == requested) {
                std::wcout << L"Workspace rating effective=" << effective
                    << L" stored=" << persisted << L'\n';
                return 0;
            }
            if (toggling && static_cast<int>(effective) != requested) {
                if (!clickDip(child.result, x, y)) {
                    std::wcerr << L"Workspace rating did not accept setup click\n";
                    return 7;
                }
                for (unsigned attempt = 0; attempt < 160; ++attempt) {
                    if (!query(49, 0, effective) ||
                        !query(50, 0, persisted)) {
                        break;
                    }
                    if (static_cast<int>(persisted) == requested) break;
                    Sleep(25);
                }
            }
            if (!clickDip(child.result, x, y)) {
                std::wcerr << L"Workspace rating did not accept the click\n";
                return 7;
            }
            const auto expected = toggling ? 0 : requested;
            for (unsigned attempt = 0; attempt < 160; ++attempt) {
                if (!query(49, 0, effective) || !query(50, 0, persisted)) {
                    break;
                }
                if (static_cast<int>(persisted) == expected) break;
                Sleep(25);
            }
            if (static_cast<int>(effective) != expected ||
                static_cast<int>(persisted) != expected) {
                std::wcerr << L"Workspace rating did not persist: effective="
                    << effective << L" stored=" << persisted << L'\n';
                return 8;
            }
        } else if (!query(49, 0, effective) || !query(50, 0, persisted)) {
            std::wcerr << L"Workspace rating state query failed\n";
            return 8;
        }
        std::wcout << L"Workspace rating effective=" << effective
            << L" stored=" << persisted << L'\n';
        return 0;
    } else if (action == L"waveseek-seek-percent") {
        if (actionArgc != 4) {
            std::wcerr << L"waveseek-seek-percent requires a percentage\n";
            return 8;
        }
        const auto percent = std::clamp(_wtof(argv[3]), 0.0, 100.0);
        clicked = clickDip(
            child.result,
            width * static_cast<float>(percent / 100.0),
            height * 0.5f);
    } else if (action == L"waveseek-pointer-tracking") {
        if (!activateInputCandidate(topLevel.result)) return 8;
        const auto message = RegisterWindowMessageW(nekorox::kWaveSeekStateMessageV1);
        const auto query = [&](WPARAM field) -> std::optional<LRESULT> {
            DWORD_PTR value = 0;
            if (!message || !SendMessageTimeoutW(child.result, message, field, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &value)) return {};
            return static_cast<LRESULT>(value);
        };
        const auto sampleCount = 160;
        double maxSettleMs = 0;
        POINT previous{};
        const auto restoreCursor = GetCursorPos(&previous) != FALSE;
        const auto moveTo = [&](int x, int y) {
            POINT point{x, y};
            return ClientToScreen(child.result, &point) &&
                GetForegroundWindow() == topLevel.result &&
                SetCursorPos(point.x, point.y) &&
                WindowFromPoint(point) == child.result;
        };
        bool valid = true;
        for (int index = 0; index < sampleCount && valid; ++index) {
            const auto x = 5 + (client.right - 10) *
                ((index * 37) % sampleCount) / (sampleCount - 1);
            const auto expected = static_cast<LRESULT>(std::lround(x * 96.0 / dpi * 1000.0));
            if (!moveTo(x, client.bottom / 2)) { valid = false; break; }
            // Most moves form a burst. Only sample boundaries wait for the
            // final accepted marker, without injecting mouse-leave or paint.
            if (index % 16 != 15) { Sleep(2); continue; }
            const auto start = std::chrono::steady_clock::now();
            bool settled = false;
            do {
                const auto target = query(8), shown = query(9), pending = query(10);
                if (target && shown && pending && std::abs(*target - expected) <= 1000 &&
                        std::abs(*shown - expected) <= 1000 && !*pending) {
                    settled = true; break;
                }
                Sleep(2);
            } while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150));
            maxSettleMs = std::max(maxSettleMs,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            valid = settled;
            if (!valid) {
                POINT actual{}; GetCursorPos(&actual); ScreenToClient(child.result, &actual);
                std::wcerr << L"expected=" << expected
                    << L" target=" << query(8).value_or(-999)
                    << L" shown=" << query(9).value_or(-999)
                    << L" pending=" << query(10).value_or(-999)
                    << L" cursorClient=" << actual.x << L"," << actual.y
                    << L" foreground=" << GetForegroundWindow()
                    << L" candidate=" << topLevel.result << L'\n';
            }
        }
        if (restoreCursor) SetCursorPos(previous.x, previous.y);
        std::wcout << L"WAVESEEK_POINTER_TRACKING " << (valid ? L"PASS" : L"FAIL")
            << L" moves=" << sampleCount << L" maxFinalMarkerMs=" << maxSettleMs << L'\n';
        return valid ? 0 : 8;
    } else if (action == L"benchmark-waveseek-hover") {
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 80;
        const auto count = std::clamp(requested, 10, 500);
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            const auto x = 1 + (client.right - client.left - 2) *
                ((index * 37) % count) / std::max(1, count - 1);
            const auto y = std::max<LONG>(
                1L, (client.bottom - client.top) / 2);
            const auto started = std::chrono::steady_clock::now();
            SendMessageW(child.result, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            samples.push_back(elapsed);
        }
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](double fraction) {
            const auto index = static_cast<std::size_t>(std::clamp(
                std::llround(fraction * static_cast<double>(samples.size() - 1)),
                0LL,
                static_cast<long long>(samples.size() - 1)));
            return samples[index];
        };
        const auto median = percentile(0.50);
        const auto p95 = percentile(0.95);
        const auto maximum = samples.back();
        std::wcout << L"WAVESEEK_HOVER_BENCHMARK count=" << count
            << L" median_ms=" << std::fixed << std::setprecision(3)
            << median << L" p95_ms=" << p95
            << L" max_ms=" << maximum << L'\n';
        return p95 <= 8.0 ? 0 : 8;
    } else if (action == L"waveseek-hover-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"WaveSeek hover capture requires a BMP path\n";
            return 8;
        }
        SendMessageW(child.result, WM_MOUSEMOVE, 0, MAKELPARAM(
            (client.right - client.left) * 35 / 100,
            (client.bottom - client.top) / 2));
        RedrawWindow(child.result, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        if (!captureWindowBmp(child.result, argv[3])) {
            std::wcerr << L"WaveSeek hover capture failed\n";
            return 8;
        }
        std::wcout << L"Captured WaveSeek hover to " << argv[3] << L'\n';
        return 0;
    } else if (action == L"waveseek-transition-capture") {
        if (actionArgc != 4) {
            std::wcerr << L"WaveSeek transition capture requires an output directory\n";
            return 8;
        }
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(
            topLevel.result, findChild,
            reinterpret_cast<LPARAM>(&header));
        if (header.result == nullptr) {
            std::wcerr << L"WaveSeek capture header was not found\n";
            return 5;
        }
        RECT headerClient{};
        GetClientRect(header.result, &headerClient);
        const auto headerDpi = static_cast<float>(
            GetDpiForWindow(header.result));
        const auto headerWidth = static_cast<float>(headerClient.right) *
            96.0f / std::max(1.0f, headerDpi);
        const auto headerHeight = static_cast<float>(headerClient.bottom) *
            96.0f / std::max(1.0f, headerDpi);
        std::error_code directoryError;
        const std::filesystem::path directory(argv[3]);
        std::filesystem::create_directories(directory, directoryError);
        if (directoryError || !clickDip(
                header.result,
                headerWidth - 18.5f,
                std::max(10.0f, headerHeight - 14.0f))) {
            std::wcerr << L"WaveSeek transition capture could not start\n";
            return 7;
        }
        constexpr std::array delaysMs{
            0, 60, 120, 180, 240, 280, 320, 360, 420, 520};
        const auto started = std::chrono::steady_clock::now();
        for (const auto delay : delaysMs) {
            const auto deadline = started + std::chrono::milliseconds(delay);
            std::this_thread::sleep_until(deadline);
            RedrawWindow(
                topLevel.result, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            std::wstringstream name;
            name << L"waveseek-" << std::setw(3) << std::setfill(L'0')
                 << delay << L"ms.bmp";
            if (!captureWindowBmp(topLevel.result, directory / name.str())) {
                std::wcerr << L"WaveSeek transition frame capture failed\n";
                return 8;
            }
        }
        std::wcout << L"Captured WaveSeek transition to "
            << directory.wstring() << L'\n';
        return 0;
    } else if (action == L"waveseek-state" ||
        action == L"benchmark-waveseek-first-pixel") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWaveSeekStateMessageV1);
        const auto query = [&](WPARAM selector, DWORD_PTR& result) {
            result = 0;
            return message != 0 && SendMessageTimeoutW(
                child.result, message, selector, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) != 0;
        };
        DWORD_PTR beforeGeneration = 0;
        if (!query(3, beforeGeneration)) {
            std::wcerr << L"WaveSeek state query failed\n";
            return 8;
        }
        if (action == L"benchmark-waveseek-first-pixel") {
            ChildContext header{L"NekoRoXHeaderV3"};
            EnumChildWindows(
                topLevel.result, findChild,
                reinterpret_cast<LPARAM>(&header));
            if (header.result == nullptr) {
                std::wcerr << L"WaveSeek benchmark header was not found\n";
                return 5;
            }
            RECT headerClient{};
            GetClientRect(header.result, &headerClient);
            const auto headerDpi = static_cast<float>(
                GetDpiForWindow(header.result));
            const auto headerWidth =
                static_cast<float>(headerClient.right) * 96.0f /
                std::max(1.0f, headerDpi);
            const auto headerHeight =
                static_cast<float>(headerClient.bottom) * 96.0f /
                std::max(1.0f, headerDpi);
            if (!clickDip(
                    header.result,
                    headerWidth - 18.5f,
                    std::max(10.0f, headerHeight - 14.0f))) {
                std::wcerr << L"WaveSeek benchmark next click failed\n";
                return 7;
            }

            DWORD_PTR generation = beforeGeneration;
            DWORD_PTR latencyMicros = 0;
            for (unsigned attempt = 0; attempt < 750; ++attempt) {
                RedrawWindow(
                    child.result, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_UPDATENOW);
                if (!query(3, generation) || !query(1, latencyMicros)) {
                    break;
                }
                if (generation != beforeGeneration && latencyMicros != 0) {
                    break;
                }
                Sleep(2);
            }
            if (generation == beforeGeneration || latencyMicros == 0) {
                std::wcerr << L"WaveSeek first visual was not observed\n";
                return 8;
            }
            DWORD_PTR coverage = 0;
            DWORD_PTR state = 0;
            query(2, coverage);
            query(0, state);
            std::wcout << std::fixed << std::setprecision(3)
                << L"WAVESEEK_FIRST_PIXEL latency_ms="
                << static_cast<double>(latencyMicros) / 1000.0
                << L" coverage="
                << static_cast<double>(coverage) / 1000000.0
                << L" state=" << state << L'\n';
            return 0;
        }

        DWORD_PTR state = 0;
        DWORD_PTR latencyMicros = 0;
        DWORD_PTR coverage = 0;
        DWORD_PTR paints = 0, pulses = 0, skipped = 0, refresh = 0;
        if (!query(0, state) || !query(1, latencyMicros) ||
            !query(2, coverage) || !query(4, paints) || !query(5, pulses) ||
            !query(6, skipped) || !query(7, refresh)) {
            std::wcerr << L"WaveSeek state query failed\n";
            return 8;
        }
        std::wcout << std::fixed << std::setprecision(3)
            << L"WAVESEEK state=" << state
            << L" first_visual_ms="
            << static_cast<double>(latencyMicros) / 1000.0
            << L" coverage="
            << static_cast<double>(coverage) / 1000000.0
            << L" generation=" << beforeGeneration
            << L" playbackPaints=" << paints << L" pulses=" << pulses
            << L" skipped=" << skipped << L" refreshMilliHz=" << refresh << L'\n';
        return 0;
    } else if (action == L"frame-clock-state") {
        const auto message = RegisterWindowMessageW(
            nekorox::kFrameClockStateMessageV1);
        DWORD_PTR count = 0;
        if (message == 0 || !SendMessageTimeoutW(
                child.result, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &count)) {
            std::wcerr << L"Frame-clock state query failed\n";
            return 8;
        }
        std::wcout << L"Frame clocks active=" << count << L'\n';
        return 0;
    } else if (action == L"benchmark-np") {
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 100;
        const auto count = std::clamp(requested, 1, 1000);
        std::vector<double> elapsed;
        elapsed.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            const auto started = std::chrono::steady_clock::now();
            if (!clickDip(child.result, width - 108.0f, 10.0f)) {
                std::wcerr << L"NP benchmark click failed at iteration "
                    << index << L'\n';
                return 7;
            }
            elapsed.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count());
        }
        std::sort(elapsed.begin(), elapsed.end());
        const auto percentile = [&](double value) {
            const auto rank = static_cast<std::size_t>(std::ceil(
                value * static_cast<double>(elapsed.size())));
            return elapsed[std::clamp<std::size_t>(rank, 1U, elapsed.size()) - 1U];
        };
        std::wcout << std::fixed << std::setprecision(3)
            << L"NP_BENCHMARK count=" << elapsed.size()
            << L" median_ms=" << percentile(0.5)
            << L" p95_ms=" << percentile(0.95)
            << L" max_ms=" << elapsed.back() << L'\n';
        return 0;
    } else if (action == L"benchmark-scroll-input") {
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 80;
        const auto count = std::clamp(requested, 1, 500);
        const auto leftWidth = workspaceLeftWidth(width);
        const auto x = leftWidth + (width - leftWidth) * 0.5f;
        const auto y = std::clamp(height * 0.45f, 110.0f, height - 40.0f);
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            std::wcerr << L"High-resolution clock is unavailable\n";
            return 8;
        }
        std::vector<double> elapsed;
        elapsed.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            LARGE_INTEGER started{};
            LARGE_INTEGER finished{};
            QueryPerformanceCounter(&started);
            if (!wheelDip(child.result, x, y, -WHEEL_DELTA)) {
                std::wcerr << L"Scroll input benchmark failed at iteration "
                    << index << L'\n';
                return 7;
            }
            QueryPerformanceCounter(&finished);
            elapsed.push_back(
                static_cast<double>(finished.QuadPart - started.QuadPart) *
                1000.0 / static_cast<double>(frequency.QuadPart));
            Sleep(6);
        }
        std::sort(elapsed.begin(), elapsed.end());
        const auto percentile = [&](double value) {
            const auto rank = static_cast<std::size_t>(std::ceil(
                value * static_cast<double>(elapsed.size())));
            return elapsed[
                std::clamp<std::size_t>(rank, 1U, elapsed.size()) - 1U];
        };
        std::wcout << std::fixed << std::setprecision(3)
            << L"SCROLL_INPUT_BENCHMARK count=" << elapsed.size()
            << L" median_ms=" << percentile(0.5)
            << L" p95_ms=" << percentile(0.95)
            << L" max_ms=" << elapsed.back() << L'\n';
        return 0;
    } else if (action == L"benchmark-scroll") {
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 80;
        const auto count = std::clamp(requested, 1, 500);
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto leftWidth = workspaceLeftWidth(width);
        const auto x = leftWidth + (width - leftWidth) * 0.5f;
        const auto y = std::clamp(height * 0.45f, 110.0f, height - 40.0f);
        std::vector<double> elapsed;
        std::vector<double> dwmElapsed;
        elapsed.reserve(static_cast<std::size_t>(count));
        dwmElapsed.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            DWORD_PTR armed = FALSE;
            if (message == 0 || !SendMessageTimeoutW(
                    child.result, message, 20, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &armed) ||
                armed == FALSE ||
                !wheelDip(child.result, x, y, -WHEEL_DELTA)) {
                std::wcerr << L"Scroll benchmark input failed at iteration "
                    << index << L'\n';
                return 7;
            }
            RedrawWindow(
                child.result,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW);
            DWORD_PTR latencyMicros = 0;
            for (unsigned wait = 0; wait < 50 && latencyMicros == 0; ++wait) {
                if (!SendMessageTimeoutW(
                        child.result, message, 21, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000,
                        &latencyMicros)) {
                    break;
                }
                if (latencyMicros == 0) Sleep(1);
            }
            if (latencyMicros == 0) {
                std::wcerr << L"Scroll benchmark present was not observed at iteration "
                    << index << L'\n';
                return 8;
            }
            elapsed.push_back(static_cast<double>(latencyMicros) / 1000.0);
            DWORD_PTR dwmLatencyMicros = 0;
            SendMessageTimeoutW(
                child.result, message, 22, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000,
                &dwmLatencyMicros);
            if (dwmLatencyMicros != 0) {
                dwmElapsed.push_back(
                    static_cast<double>(dwmLatencyMicros) / 1000.0);
            }
            Sleep(6);
        }
        std::sort(elapsed.begin(), elapsed.end());
        std::sort(dwmElapsed.begin(), dwmElapsed.end());
        const auto percentile = [](const std::vector<double>& values, double value) {
            const auto rank = static_cast<std::size_t>(std::ceil(
                value * static_cast<double>(values.size())));
            return values[
                std::clamp<std::size_t>(rank, 1U, values.size()) - 1U];
        };
        std::wcout << std::fixed << std::setprecision(3)
            << L"SCROLL_BENCHMARK count=" << elapsed.size()
            << L" median_ms=" << percentile(elapsed, 0.5)
            << L" p95_ms=" << percentile(elapsed, 0.95)
            << L" max_ms=" << elapsed.back();
        if (!dwmElapsed.empty()) {
            std::wcout
                << L" dwm_median_ms=" << percentile(dwmElapsed, 0.5)
                << L" dwm_p95_ms=" << percentile(dwmElapsed, 0.95)
                << L" dwm_max_ms=" << dwmElapsed.back();
        }
        std::wcout << L'\n';
        return 0;
    } else if (action == L"np") {
        clicked = clickDip(child.result, width - 108.0f, 10.0f);
    } else if (action == L"options-open" || action == L"daw-open") {
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(
            topLevel.result,
            findChild,
            reinterpret_cast<LPARAM>(&header));
        const auto openMessage = RegisterWindowMessageW(L"GodenHeaderQaOpenV1");
        DWORD_PTR accepted = FALSE;
        clicked = header.result != nullptr && openMessage != 0 &&
            SendMessageTimeoutW(
                header.result,
                openMessage,
                action == L"daw-open" ? 1U : 2U,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                5000,
                &accepted) && accepted != FALSE;
        const auto className = action == L"options-open"
            ? L"GodenOptionsV1" : L"GodenDawModeV1";
        HWND secondary = nullptr;
        for (unsigned attempt = 0; clicked && secondary == nullptr && attempt < 80;
                ++attempt) {
            secondary = findOwnedWindow(fullPath, className);
            if (secondary == nullptr) Sleep(25);
        }
        if (secondary == nullptr) {
            std::wcerr << L"Goden secondary window did not open\n";
            return 7;
        }
    } else if (action == L"listen") {
        const auto previousForeground = background
            ? GetForegroundWindow()
            : nullptr;
        ChildContext header{L"NekoRoXHeaderV3"};
        EnumChildWindows(
            topLevel.result,
            findChild,
            reinterpret_cast<LPARAM>(&header));
        const auto openMessage = RegisterWindowMessageW(L"GodenHeaderQaOpenV1");
        DWORD_PTR accepted = FALSE;
        clicked = header.result != nullptr && openMessage != 0 &&
            SendMessageTimeoutW(
                header.result,
                openMessage,
                3U,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                5000,
                &accepted) && accepted != FALSE;
        if (clicked && background) {
            HWND listen = nullptr;
            for (unsigned attempt = 0; attempt < 40 && listen == nullptr;
                    ++attempt) {
                listen = findOwnedWindow(fullPath, L"NekoRoXListenWindowV3");
                if (listen == nullptr) Sleep(25);
            }
            if (listen == nullptr) {
                std::wcerr << L"Background Listen did not open\n";
                return 7;
            }
            if (listen != nullptr && GetForegroundWindow() == listen &&
                    previousForeground != nullptr &&
                    previousForeground != listen &&
                    IsWindow(previousForeground)) {
                ShowWindow(listen, SW_HIDE);
                SetForegroundWindow(previousForeground);
                ShowWindow(listen, SW_SHOWNOACTIVATE);
            }
            sendToBackNoActivate(listen);
            if (GetForegroundWindow() == listen) {
                std::wcerr << L"Background Listen retained foreground focus\n";
                return 10;
            }
        }
    } else if (action == L"play") {
        clicked = clickDip(child.result, width - 45.5f, transportY);
    } else if (action == L"next") {
        clicked = clickDip(child.result, width - 18.5f, transportY);
    } else if (action == L"previous") {
        clicked = clickDip(child.result, width - 72.5f, transportY);
    } else if (action == L"stop") {
        clicked = clickDip(child.result, width - 99.5f, transportY);
    } else if (action == L"mute" || action == L"mute-on") {
        // The standalone probe cannot query foobar2000's SDK mute bit. Setting
        // its native transport volume to the mute floor is idempotent and is
        // effective before playback creates an operating-system audio session.
        clicked = dragDip(child.result,
            width - 80.0f, height * 0.5f, 0.0f, height * 0.5f);
    } else if (action == L"repeat-toggle") {
        clicked = clickDip(child.result, width - 177.0f, height * 0.5f);
    } else if (action == L"shuffle-toggle") {
        clicked = clickDip(child.result, width - 145.0f, height * 0.5f);
    } else if (action == L"mute-toggle") {
        clicked = clickDip(child.result, width - 17.0f, height * 0.5f);
    } else if (action == L"volume-set") {
        if (actionArgc != 4) {
            std::wcerr << L"volume-set requires a percentage\n";
            return 8;
        }
        const auto percent = std::clamp(_wtoi(argv[3]), 0, 100);
        const auto target = width - 126.0f + 92.0f *
            static_cast<float>(percent) / 100.0f;
        clicked = dragDip(
            child.result,
            target,
            height * 0.5f,
            target,
            height * 0.5f);
    } else if (action == L"window-maximize") {
        clicked = clickDip(child.result, width - 44.0f, 10.0f);
    } else if (action == L"history" || action == L"filter") {
        const auto leftWidth = workspaceLeftWidth(width);
        clicked = clickDip(child.result,
            leftWidth * (action == L"history" ? 0.75f : 0.25f), 39.0f);
    } else if (action == L"play-first-album") {
        const auto leftWidth = workspaceLeftWidth(width);
        clicked = doubleClickDip(child.result, leftWidth + 80.0f, 78.0f);
    } else if (action == L"play-first-library-after-temporary") {
        const auto leftWidth = workspaceLeftWidth(width);
        // Full album header (80 DIP) + two fixture rows (2 x 21 DIP),
        // then the centre of the first normal library album header.
        clicked = doubleClickDip(
            child.result,
            leftWidth + 80.0f,
            48.0f + 80.0f + 42.0f + 40.0f);
    } else if (action == L"favorite") {
        clicked = clickDip(child.result, width * 0.1f, 14.0f);
        if (clicked) {
            Sleep(40);
            clicked = clickDip(child.result, width - 32.0f, 58.0f);
        }
    } else if (action == L"note-open" || action == L"note-set" ||
               action == L"note-clear") {
        if (action == L"note-set" && actionArgc != 4) {
            std::wcerr << L"note-set requires note text\n";
            return 8;
        }
        auto editor = findOwnedWindow(fullPath, L"NekoRoXNoteEditorV3");
        clicked = editor != nullptr;
        for (unsigned openAttempt = 0;
                editor == nullptr && openAttempt < 3;
                ++openAttempt) {
            clicked = clickDip(child.result, width * 0.3f, 14.0f);
            if (!clicked) break;
            Sleep(100);
            clicked = clickDip(child.result, width - 61.0f, 58.0f);
            for (unsigned waitAttempt = 0;
                    clicked && editor == nullptr && waitAttempt < 80;
                    ++waitAttempt) {
                editor = findOwnedWindow(fullPath, L"NekoRoXNoteEditorV3");
                if (editor == nullptr) Sleep(25);
            }
        }
        if (editor == nullptr) {
            std::wcerr << L"NekoRoX note editor was not found\n";
            return 8;
        }
        if (action == L"note-set" || action == L"note-clear") {
            const auto edit = GetDlgItem(editor, 100);
            const auto save = GetDlgItem(editor, 101);
            const auto* input = action == L"note-set" ? argv[3] : L"";
            DWORD_PTR messageResult = 0;
            if (edit == nullptr || save == nullptr ||
                    !SendMessageTimeoutW(edit, WM_SETTEXT, 0,
                        reinterpret_cast<LPARAM>(input),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &messageResult)) {
                std::wcerr << L"NekoRoX note controls were not available\n";
                return 8;
            }
            if (!SendMessageTimeoutW(edit, WM_GETTEXTLENGTH, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &messageResult)) {
                std::wcerr << L"NekoRoX note length could not be read\n";
                return 8;
            }
            std::wstring verification(static_cast<std::size_t>(messageResult) + 1U,
                L'\0');
            if (!SendMessageTimeoutW(edit, WM_GETTEXT,
                    static_cast<WPARAM>(verification.size()),
                    reinterpret_cast<LPARAM>(verification.data()),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &messageResult)) {
                std::wcerr << L"NekoRoX note text could not be read\n";
                return 8;
            }
            verification.resize(static_cast<std::size_t>(messageResult));
            if (verification != input) {
                std::wcerr << L"NekoRoX note text did not round-trip through the editor\n";
                return 8;
            }
            Sleep(40);
            DWORD_PTR ignored = 0;
            if (!SendMessageTimeoutW(editor, WM_COMMAND,
                    MAKEWPARAM(101, BN_CLICKED), reinterpret_cast<LPARAM>(save),
                    SMTO_ABORTIFHUNG, 2000, &ignored)) {
                std::wcerr << L"NekoRoX note save did not accept the click\n";
                return 8;
            }
            Sleep(150);
        }
    } else if (action == L"persona-open" || action == L"persona-profile" ||
               action == L"persona-aliases" ||
               action == L"persona-relations" || action == L"persona-set") {
        if (action == L"persona-set" && actionArgc != 4) {
            std::wcerr << L"persona-set requires display|profile|aliases"
                L"[|target|relation|note]\n";
            return 8;
        }
        auto editor = findOwnedWindow(fullPath, L"NekoRoXPersonaEditorV3");
        clicked = editor != nullptr;
        if (!clicked) {
            clicked = clickDip(child.result, width * 0.3f, 14.0f);
            if (clicked) {
                Sleep(60);
                clicked = clickDip(child.result, width - 159.0f, 58.0f);
            }
        }
        for (unsigned attempt = 0; clicked && editor == nullptr && attempt < 40;
                ++attempt) {
            editor = findOwnedWindow(fullPath, L"NekoRoXPersonaEditorV3");
            if (editor == nullptr) Sleep(25);
        }
        if (editor == nullptr) {
            std::wcerr << L"NekoRoX persona editor was not found\n";
            return 8;
        }
        if (background) {
            ShowWindow(editor, SW_SHOWNOACTIVATE);
            sendToBackNoActivate(editor);
        } else {
            ShowWindow(editor, SW_RESTORE);
            BringWindowToTop(editor);
            SetForegroundWindow(editor);
        }
        RECT editorClient{};
        GetClientRect(editor, &editorClient);
        const auto editorDpi = static_cast<float>(GetDpiForWindow(editor));
        const auto editorWidth = static_cast<float>(
            editorClient.right - editorClient.left) * 96.0f /
            std::max(1.0f, editorDpi);
        if (action == L"persona-profile" || action == L"persona-aliases" ||
                action == L"persona-relations") {
            const auto index = action == L"persona-profile" ? 0.0f :
                action == L"persona-aliases" ? 1.0f : 2.0f;
            clicked = clickDip(editor, editorWidth * (index + 0.5f) / 3.0f, 44.0f);
        } else if (action == L"persona-set") {
            auto fields = splitFields(argv[3]);
            if (fields.size() != 3U && fields.size() != 6U) {
                std::wcerr << L"persona-set requires 3 or 6 pipe-separated fields\n";
                return 8;
            }
            std::replace(fields[2].begin(), fields[2].end(), L';', L'\n');
            if (!setControlText(GetDlgItem(editor, 200), fields[0]) ||
                    !setControlText(GetDlgItem(editor, 201), fields[1]) ||
                    !setControlText(GetDlgItem(editor, 202), fields[2])) {
                std::wcerr << L"Persona profile controls did not round-trip\n";
                return 8;
            }
            if (fields.size() == 6U) {
                if (!clickDip(editor, editorWidth * 5.0f / 6.0f, 44.0f) ||
                        !setControlText(GetDlgItem(editor, 204), fields[3]) ||
                        !setControlText(GetDlgItem(editor, 205), fields[4]) ||
                        !setControlText(GetDlgItem(editor, 206), fields[5])) {
                    std::wcerr << L"Persona relationship controls did not round-trip\n";
                    return 8;
                }
                DWORD_PTR ignored = 0;
                const auto apply = GetDlgItem(editor, 207);
                if (apply == nullptr || !SendMessageTimeoutW(editor, WM_COMMAND,
                        MAKEWPARAM(207, BN_CLICKED), reinterpret_cast<LPARAM>(apply),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored)) {
                    std::wcerr << L"Persona relationship did not apply\n";
                    return 8;
                }
            }
            DWORD_PTR ignored = 0;
            const auto save = GetDlgItem(editor, 209);
            if (save == nullptr || !SendMessageTimeoutW(editor, WM_COMMAND,
                    MAKEWPARAM(209, BN_CLICKED), reinterpret_cast<LPARAM>(save),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored)) {
                std::wcerr << L"Persona save did not accept the command\n";
                return 8;
            }
            Sleep(180);
            clicked = true;
        }
    } else if (action == L"persona-map") {
        clicked = clickDip(child.result, width * 0.3f, 14.0f);
        if (clicked) {
            Sleep(60);
            clicked = clickDip(child.result, width - 235.0f, 58.0f);
        }
    } else if (action == L"inf" || action == L"bio" ||
               action == L"lyr" || action == L"art" || action == L"lib") {
        const std::array names{L"inf", L"bio", L"lyr", L"art", L"lib"};
        const auto iterator = std::find(names.begin(), names.end(), action);
        const auto index = static_cast<float>(std::distance(names.begin(), iterator));
        const auto x = width * (index + 0.5f) / 5.0f;
        const auto stateMessage = RegisterWindowMessageW(
            nekorox::kWorkspaceStateMessageV1);
        for (unsigned attempt = 0; attempt < 3 && !clicked; ++attempt) {
            if (!clickDip(child.result, x, 14.0f)) break;
            for (unsigned wait = 0; wait < 10; ++wait) {
                DWORD_PTR state = 0;
                if (SendMessageTimeoutW(
                        child.result, stateMessage, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &state) &&
                        (state & nekorox::kWorkspaceTabMaskV1) ==
                            static_cast<DWORD_PTR>(index)) {
                    clicked = true;
                    break;
                }
                Sleep(20);
            }
        }
    } else if (action == L"verify-facet-scroll-preservation") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto queryScroll = [&](WPARAM query)
            -> std::optional<double> {
            DWORD_PTR packed = 0;
            if (message == 0 || !SendMessageTimeoutW(
                    child.result,
                    message,
                    query,
                    0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    2000,
                    &packed)) {
                return std::nullopt;
            }
            return static_cast<double>(
                static_cast<LRESULT>(packed)) / 1000.0;
        };
        const auto leftWidth = workspaceLeftWidth(width);
        if (!clickDip(child.result, leftWidth * 0.25f, 39.0f)) {
            std::wcerr << L"Could not enter library filter mode\n";
            return 7;
        }
        Sleep(80);
        const auto beforeMaximum = queryScroll(25);
        if (!beforeMaximum || *beforeMaximum < 1.0) {
            std::wcerr << L"Result list has no scrollable library content"
                << L" max=" << (beforeMaximum ? *beforeMaximum : -1.0)
                << L'\n';
            return 7;
        }
        constexpr LPARAM kTestOffsetMilliDip = 320000;
        DWORD_PTR primed = FALSE;
        if (!SendMessageTimeoutW(
                child.result,
                message,
                26,
                kTestOffsetMilliDip,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                2000,
                &primed) || primed == FALSE) {
            std::wcerr << L"Could not prime result scroll\n";
            return 7;
        }
        Sleep(30);
        const auto beforePosition = queryScroll(23);
        const auto beforeTarget = queryScroll(24);
        if (!beforePosition || !beforeTarget || *beforePosition < 1.0) {
            std::wcerr << L"Result list did not acquire a testable offset"
                << L" position="
                << (beforePosition ? *beforePosition : -1.0)
                << L" target=" << (beforeTarget ? *beforeTarget : -1.0)
                << L" max=" << *beforeMaximum << L'\n';
            return 7;
        }

        DWORD_PTR selected = FALSE;
        if (!SendMessageTimeoutW(
                child.result,
                message,
                27,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                2000,
                &selected) || selected == FALSE) {
            std::wcerr << L"Could not select the first genre facet\n";
            return 7;
        }
        Sleep(120);
        const auto afterPosition = queryScroll(23);
        const auto afterTarget = queryScroll(24);
        const auto afterMaximum = queryScroll(25);
        if (!afterPosition || !afterTarget || !afterMaximum) {
            std::wcerr << L"Could not query result scroll after facet selection\n";
            return 7;
        }
        const auto expectedPosition = std::min(
            *beforePosition,
            *afterMaximum);
        const auto expectedTarget = std::min(
            *beforeTarget,
            *afterMaximum);
        if (std::abs(*afterPosition - expectedPosition) > 1.0 ||
            std::abs(*afterTarget - expectedTarget) > 1.0) {
            std::wcerr << L"Facet selection moved result viewport: before="
                << *beforePosition << L"/" << *beforeTarget
                << L" after=" << *afterPosition << L"/" << *afterTarget
                << L" max=" << *afterMaximum << L'\n';
            return 8;
        }
        std::wcout << L"Facet scroll preservation: PASS position="
            << *afterPosition << L" target=" << *afterTarget
            << L" max=" << *afterMaximum << L'\n';
        return 0;
    } else if (action == L"verify-random-album-now-playing-reveal") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto query = [&](WPARAM selector)
            -> std::optional<LRESULT> {
            DWORD_PTR packed = 0;
            if (message == 0 || !SendMessageTimeoutW(
                    child.result,
                    message,
                    selector,
                    0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    2000,
                    &packed)) {
                return std::nullopt;
            }
            return static_cast<LRESULT>(packed);
        };
        const auto queryScroll = [&](WPARAM selector)
            -> std::optional<double> {
            const auto value = query(selector);
            if (!value) return std::nullopt;
            return static_cast<double>(*value) / 1000.0;
        };

        const auto leftWidth = workspaceLeftWidth(width);
        if (!clickDip(child.result, leftWidth * 0.25f, 39.0f)) {
            std::wcerr << L"Could not enter library filter mode\n";
            return 7;
        }
        Sleep(80);
        const auto beforeMaximum = queryScroll(25);
        if (!beforeMaximum || *beforeMaximum < 1.0) {
            std::wcerr << L"Result list has no scrollable library content\n";
            return 7;
        }

        constexpr LPARAM kTestOffsetMilliDip = 320000;
        DWORD_PTR primed = FALSE;
        if (!SendMessageTimeoutW(
                child.result,
                message,
                26,
                kTestOffsetMilliDip,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                2000,
                &primed) || primed == FALSE) {
            std::wcerr << L"Could not prime result scroll\n";
            return 7;
        }
        Sleep(30);

        const auto beforePosition = queryScroll(23);
        const auto beforeTarget = queryScroll(24);
        const auto primedActivePlaylist = query(18);
        const auto beforeActivePlaylist = query(15);
        const auto beforeRevealSuccesses = query(30);
        const auto beforeRandomRequests = query(37);
        const auto interactionFacetTop = queryScroll(32);
        const auto interactionLeftWidth = queryScroll(33);
        if (!beforePosition || !beforeTarget || !primedActivePlaylist ||
            !beforeActivePlaylist || !beforeRevealSuccesses ||
            !beforeRandomRequests || !interactionFacetTop ||
            !interactionLeftWidth || *interactionLeftWidth < 1.0 ||
            *primedActivePlaylist < 0 ||
            *beforeActivePlaylist != *primedActivePlaylist) {
            std::wcerr << L"Could not capture pre-playback navigation state\n";
            return 7;
        }

        constexpr float kFacetRowHeightDip = 20.0f;
        const auto firstGenreRowCenter = static_cast<float>(
            *interactionFacetTop + kFacetRowHeightDip * 1.5f);
        if (!doubleClickDip(
                child.result,
                static_cast<float>(*interactionLeftWidth / 6.0),
                firstGenreRowCenter)) {
            std::wcerr << L"Random-album double click was not accepted\n";
            return 7;
        }
        bool revealCompleted = false;
        for (unsigned attempt = 0; attempt < 150; ++attempt) {
            const auto successes = query(30);
            if (successes && *successes > *beforeRevealSuccesses) {
                revealCompleted = true;
                break;
            }
            Sleep(20);
        }
        if (!revealCompleted) {
            std::wcerr << L"Random album did not complete its NP reveal\n";
            return 8;
        }

        const auto afterPosition = queryScroll(23);
        const auto afterTarget = queryScroll(24);
        const auto afterMaximum = queryScroll(25);
        const auto afterActivePlaylist = query(15);
        const auto playingPlaylist = query(16);
        const auto selectionPlaylist = query(17);
        const auto pendingReveal = query(28);
        const auto revealAttempts = query(29);
        const auto revealSuccesses = query(30);
        const auto selectedNowPlaying = query(31);
        const auto facetTop = queryScroll(32);
        const auto actualLeftWidth = queryScroll(33);
        const auto resultsTop = queryScroll(34);
        const auto historyMode = query(35);
        const auto activeTab = query(36);
        const auto randomRequests = query(37);
        const auto playGroupRequests = query(38);
        const auto doubleClickRegion = query(40);
        const auto centerError = queryScroll(41);
        if (!afterPosition || !afterTarget || !afterMaximum ||
            !afterActivePlaylist || !playingPlaylist ||
            !selectionPlaylist || !pendingReveal || !revealAttempts ||
            !revealSuccesses || !selectedNowPlaying || !facetTop ||
            !actualLeftWidth ||
            !resultsTop || !historyMode || !activeTab || !randomRequests ||
            !playGroupRequests || !doubleClickRegion || !centerError) {
            std::wcerr << L"Could not capture post-playback navigation state\n";
            return 7;
        }

        if (*selectionPlaylist < 0 ||
            *afterActivePlaylist != *selectionPlaylist ||
            *playingPlaylist != *selectionPlaylist ||
            *pendingReveal != 0 || *selectedNowPlaying == 0 ||
            *randomRequests <= *beforeRandomRequests ||
            *revealSuccesses <= *beforeRevealSuccesses ||
            *revealAttempts < *revealSuccesses || *centerError > 1.0 ||
            *activeTab != 4 || *historyMode != 0 ||
            *doubleClickRegion != 5) {
            std::wcerr << L"Random album did not reproduce NP navigation: active="
                << *beforeActivePlaylist << L"->" << *afterActivePlaylist
                << L" scroll=" << *beforePosition << L"/" << *beforeTarget
                << L"->" << *afterPosition << L"/" << *afterTarget
                << L" max=" << *afterMaximum
                << L" playlists=" << *playingPlaylist
                << L"/" << *selectionPlaylist
                << L" pending=" << *pendingReveal
                << L" reveals=" << *revealAttempts
                << L"/" << *revealSuccesses
                << L" selected=" << *selectedNowPlaying
                << L" centerError=" << *centerError
                << L" geometry=" << *actualLeftWidth
                << L"/" << *facetTop << L"/" << *resultsTop
                << L" mode=" << *activeTab << L"/" << *historyMode
                << L" routes=" << *randomRequests
                << L"/" << *playGroupRequests
                << L"/" << *doubleClickRegion << L'\n';
            return 8;
        }

        std::wcout << L"Random album NP reveal: PASS active="
            << *afterActivePlaylist << L" playing=" << *playingPlaylist
            << L" position=" << *afterPosition
            << L" target=" << *afterTarget
            << L" centerError=" << *centerError << L'\n';
        return 0;
    } else if (action == L"compact-facet-refresh") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto query = [&](WPARAM selector) -> std::optional<LRESULT> {
            DWORD_PTR value = 0;
            if (message == 0 || !SendMessageTimeoutW(
                    child.result, message, selector, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &value)) {
                return std::nullopt;
            }
            return static_cast<LRESULT>(value);
        };
        const auto compact = query(53);
        const auto beforeFacet = query(52);
        const auto leftWidthMilli = query(33);
        const auto facetTopMilli = query(32);
        if (!compact || *compact == 0 || !beforeFacet || !leftWidthMilli ||
                !facetTopMilli) {
            std::wcerr << L"Workspace is not in compact facet mode\n";
            return 8;
        }
        const auto nextFacet = (*beforeFacet + 1) % 3;
        const auto leftWidth = static_cast<float>(*leftWidthMilli) / 1000.0f;
        const auto facetTop = static_cast<float>(*facetTopMilli) / 1000.0f;
        if (!clickDip(child.result,
                leftWidth * (static_cast<float>(nextFacet) + 0.5f) / 3.0f,
                facetTop - 10.0f)) {
            std::wcerr << L"Compact facet selector did not accept the click\n";
            return 7;
        }

        std::optional<LRESULT> selected;
        std::optional<LRESULT> desired;
        std::optional<LRESULT> accepted;
        std::optional<LRESULT> cacheValid;
        std::optional<LRESULT> cachedFacet;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            selected = query(52);
            desired = query(54);
            accepted = query(55);
            cacheValid = query(56);
            cachedFacet = query(57);
            if (selected && desired && accepted && cacheValid && cachedFacet &&
                    *selected == nextFacet && *accepted >= *desired &&
                    *cacheValid != 0 && *cachedFacet == nextFacet) {
                break;
            }
            Sleep(25);
        }
        const auto passed = selected && desired && accepted && cacheValid &&
            cachedFacet && *selected == nextFacet && *accepted >= *desired &&
            *cacheValid != 0 && *cachedFacet == nextFacet;
        std::wcout << L"CompactFacet " << *beforeFacet << L"->"
            << (selected ? *selected : -1) << L" desired="
            << (desired ? *desired : -1) << L" accepted="
            << (accepted ? *accepted : -1) << L" cache="
            << (cacheValid ? *cacheValid : -1) << L" cachedFacet="
            << (cachedFacet ? *cachedFacet : -1) << L'\n';
        return passed ? 0 : 8;
    } else if (action == L"filter-click-refresh") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        SendMessageW(child.result, message, 61, 0);
        RedrawWindow(child.result, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        if (!waitForWorkspacePresentation(child.result, message)) {
            std::wcerr << L"Workspace did not present cleared filter state\n";
            return 8;
        }
        const auto compact = SendMessageW(child.result, message, 53, 0) != 0;
        if (compact) {
            std::wcerr << L"Filter click refresh requires wide facet mode\n";
            return 8;
        }
        const auto facetTop = static_cast<float>(SendMessageW(
            child.result, message, 32, 0)) / 1000.0f;
        const auto leftWidth = static_cast<float>(SendMessageW(
            child.result, message, 33, 0)) / 1000.0f;
        constexpr float kFacetRowHeightDip = 20.0f;
        if (!clickDip(child.result, leftWidth * 0.5f,
                facetTop + kFacetRowHeightDip * 1.5f)) {
            std::wcerr << L"Artist filter did not accept the click\n";
            return 7;
        }
        LRESULT desired = 0;
        LRESULT accepted = 0;
        LRESULT selected = 0;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            desired = static_cast<LRESULT>(SendMessageW(
                child.result, message, 54, 0));
            accepted = static_cast<LRESULT>(SendMessageW(
                child.result, message, 55, 0));
            selected = static_cast<LRESULT>(SendMessageW(
                child.result, message, 62, 0));
            if (selected != 0 && accepted >= desired) break;
            Sleep(25);
        }
        std::wcout << L"FilterClick selected=" << selected
            << L" desired=" << desired << L" accepted=" << accepted << L'\n';
        return selected != 0 && accepted >= desired ? 0 : 8;
    } else if (action == L"drag-results-scrollbar") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        SendMessageW(child.result, message, 61, 0);
        RedrawWindow(child.result, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        if (!waitForWorkspacePresentation(child.result, message)) {
            std::wcerr << L"Workspace did not present result scrollbar geometry\n";
            return 8;
        }
        SendMessageW(child.result, message, 26, 0);
        RedrawWindow(child.result, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        const auto maximum = static_cast<double>(SendMessageW(
            child.result, message, 25, 0)) / 1000.0;
        const auto resultsTop = static_cast<float>(SendMessageW(
            child.result, message, 34, 0)) / 1000.0f;
        const auto viewportHeight = std::max(1.0f, height - resultsTop);
        const auto contentHeight = maximum + viewportHeight;
        const auto thumbHeight = static_cast<float>(std::max(
            28.0, viewportHeight * viewportHeight / contentHeight));
        const auto toPixels = [dpi](float value) {
            return static_cast<int>(std::lround(
                value * std::max(1.0f, dpi) / 96.0f));
        };
        const auto x = toPixels(width - 2.0f);
        const auto fromY = toPixels(resultsTop + thumbHeight * 0.5f);
        const auto toY = toPixels(resultsTop + viewportHeight * 0.70f);
        SendMessageW(child.result, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(x, fromY));
        SendMessageW(child.result, WM_MOUSEMOVE, MK_LBUTTON,
            MAKELPARAM(x, toY));
        SendMessageW(child.result, WM_LBUTTONUP, 0,
            MAKELPARAM(x, toY));
        const auto target = static_cast<LRESULT>(SendMessageW(
            child.result, message, 24, 0));
        std::wcout << L"ResultScrollbar edgeX=" << x
            << L" target=" << target << L" maximum="
            << static_cast<LRESULT>(maximum * 1000.0) << L'\n';
        return maximum > 0.0 && target > 0 ? 0 : 8;
    } else if (action == L"drag-artist-scrollbar" ||
               action == L"drag-album-scrollbar") {
        const auto message = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto facetIndex = action == L"drag-artist-scrollbar" ? 1 : 2;
        SendMessageW(child.result, message, 61, 0);
        RedrawWindow(child.result, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        if (!waitForWorkspacePresentation(child.result, message)) {
            std::wcerr << L"Workspace did not present facet scrollbar geometry\n";
            return 8;
        }
        const auto before = static_cast<double>(SendMessageW(
            child.result, message, 58, facetIndex)) / 1000.0;
        const auto maximum = static_cast<double>(SendMessageW(
            child.result, message, 60, facetIndex)) / 1000.0;
        const auto facetTop = static_cast<float>(SendMessageW(
            child.result, message, 32, 0)) / 1000.0f;
        const auto leftWidth = static_cast<float>(SendMessageW(
            child.result, message, 33, 0)) / 1000.0f;
        const auto compact = SendMessageW(child.result, message, 53, 0) != 0;
        if (compact) {
            std::wcerr << L"Facet scrollbar drag requires wide facet mode\n";
            return 8;
        }
        const auto viewportHeight = std::max(1.0f, height - facetTop);
        const auto contentHeight = maximum + viewportHeight;
        const auto thumbHeight = static_cast<float>(std::max(
            28.0, viewportHeight * viewportHeight / contentHeight));
        const auto facetWidth = leftWidth / 3.0f;
        const auto toPixels = [dpi](float value) {
            return static_cast<int>(std::lround(
                value * std::max(1.0f, dpi) / 96.0f));
        };
        const auto x = toPixels(facetWidth * (facetIndex + 1.0f) - 1.0f);
        const auto travel = std::max(1.0f, viewportHeight - thumbHeight);
        const auto thumbTop = facetTop + travel * static_cast<float>(
            maximum > 0.0 ? std::clamp(before / maximum, 0.0, 1.0) : 0.0);
        const auto fromY = toPixels(thumbTop + thumbHeight * 0.5f);
        const auto destinationFraction = before < maximum * 0.55 ? 0.75f : 0.25f;
        const auto toY = toPixels(
            facetTop + travel * destinationFraction + thumbHeight * 0.5f);
        SendMessageW(child.result, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(x, fromY));
        SendMessageW(child.result, WM_MOUSEMOVE, MK_LBUTTON,
            MAKELPARAM(x, toY));
        SendMessageW(child.result, WM_LBUTTONUP, 0,
            MAKELPARAM(x, toY));
        const auto target = static_cast<LRESULT>(SendMessageW(
            child.result, message, 59, facetIndex));
        std::wcout << L"FacetScrollbar index=" << facetIndex
            << L" edgeX=" << x << L" position="
            << static_cast<LRESULT>(before * 1000.0) << L" target=" << target
            << L" maximum="
            << static_cast<LRESULT>(maximum * 1000.0) << L'\n';
        return maximum > 0.0 &&
            std::abs(static_cast<double>(target) - before * 1000.0) > 1000.0
            ? 0 : 8;
    } else if (action == L"scroll-results" ||
               action == L"scroll-artist" ||
               action == L"scroll-album") {
        const auto renderStateMessage = RegisterWindowMessageW(
            nekorox::kWorkspaceRenderStateMessageV1);
        const auto beforeResultTarget = action == L"scroll-results"
            ? static_cast<LRESULT>(SendMessageW(
                child.result, renderStateMessage, 24, 0)) : 0;
        const auto resultMaximum = action == L"scroll-results"
            ? static_cast<LRESULT>(SendMessageW(
                child.result, renderStateMessage, 25, 0)) : 0;
        const auto requested = actionArgc == 4 ? _wtoi(argv[3]) : 8;
        const auto count = std::clamp(std::abs(requested), 1, 100);
        const auto delta = requested < 0 ? WHEEL_DELTA : -WHEEL_DELTA;
        const auto leftWidth = workspaceLeftWidth(width);
        const auto x = action == L"scroll-results"
            ? leftWidth + (width - leftWidth) * 0.5f
            : action == L"scroll-artist"
                ? leftWidth * 0.5f
                : leftWidth * 5.0f / 6.0f;
        const auto y = std::clamp(height * 0.45f, 110.0f, height - 40.0f);
        clicked = true;
        for (int index = 0; index < count; ++index) {
            if (!wheelDip(child.result, x, y, delta)) {
                clicked = false;
                break;
            }
            Sleep(6);
        }
        if (clicked) {
            Sleep(180);
            if (action == L"scroll-results" && resultMaximum > 0) {
                const auto afterResultTarget = static_cast<LRESULT>(
                    SendMessageW(child.result, renderStateMessage, 24, 0));
                std::wcout << L"ResultScroll target=" << beforeResultTarget
                    << L"->" << afterResultTarget
                    << L" maximum=" << resultMaximum << L'\n';
                if (afterResultTarget == beforeResultTarget) clicked = false;
            }
        }
    } else {
        std::wcerr << L"Unknown action\n";
        return 6;
    }
    if (!clicked) {
        std::wcerr << L"Panel did not accept the click\n";
        return 7;
    }
    std::wcout << L"Clicked " << action << L" on " << fullPath << L'\n';
    return 0;
}
