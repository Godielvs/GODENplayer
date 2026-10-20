#include "note_editor.h"

#include "database_service.h"
#include "frameless_window.h"
#include "text_util.h"

#include <algorithm>
#include <vector>

namespace nekorox::component {
namespace {

constexpr wchar_t kWindowClass[] = L"NekoRoXNoteEditorV3";
constexpr int kEditId = 100;
constexpr int kSaveId = 101;
constexpr int kCancelId = 102;
constexpr COLORREF kBackground = RGB(10, 11, 11);
constexpr COLORREF kPanel = RGB(18, 20, 20);
constexpr COLORREF kBorder = RGB(53, 58, 59);
constexpr COLORREF kText = RGB(210, 216, 216);
constexpr COLORREF kMuted = RGB(116, 127, 129);
constexpr COLORREF kAccent = RGB(0, 216, 208);

int titleHeight(HWND window) noexcept
{
    return MulDiv(30, (std::max)(96U, GetDpiForWindow(window)), 96);
}

RECT centeredBounds(HWND owner, int width, int height)
{
    RECT ownerBounds{};
    GetWindowRect(owner, &ownerBounds);
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const auto left = std::clamp(
        ownerBounds.left + (ownerBounds.right - ownerBounds.left - width) / 2,
        info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - width));
    const auto top = std::clamp(
        ownerBounds.top + (ownerBounds.bottom - ownerBounds.top - height) / 2,
        info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - height));
    return RECT{left, top, left + width, top + height};
}

} // namespace

NoteEditorWindow& NoteEditorWindow::instance()
{
    static NoteEditorWindow editor;
    return editor;
}

void NoteEditorWindow::show(HWND owner)
{
    const auto state = DatabaseService::instance().userStateSnapshot();
    if (!state->available || state->trackId.empty()) return;
    if (window_ != nullptr) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        return;
    }

    owner_ = GetAncestor(owner, GA_ROOT);
    if (owner_ == nullptr) owner_ = owner;
    trackId_ = state->trackId;
    initialNote_ = utf8ToWide(state->note);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &NoteEditorWindow::windowProc;
    windowClass.hInstance = core_api::get_my_instance();
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    const auto dpi = std::max<UINT>(96, GetDpiForWindow(owner_));
    const auto width = MulDiv(560, dpi, 96);
    const auto height = MulDiv(360, dpi, 96);
    const auto bounds = centeredBounds(owner_, width, height);
    window_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
        kWindowClass,
        L"GODENplayer Note",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        owner_,
        nullptr,
        core_api::get_my_instance(),
        this);
    if (window_ == nullptr) {
        owner_ = nullptr;
        trackId_.clear();
        initialNote_.clear();
        return;
    }
    EnableWindow(owner_, FALSE);
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    SetFocus(edit_);
}

void NoteEditorWindow::close()
{
    if (window_ != nullptr) DestroyWindow(window_);
}

LRESULT CALLBACK NoteEditorWindow::windowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    NoteEditorWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<NoteEditorWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<NoteEditorWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self != nullptr
        ? self->handleMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT NoteEditorWindow::handleMessage(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        window_ = window;
        enableFramelessWindow(window);
        SetWindowLongPtrW(window, GWL_STYLE,
            GetWindowLongPtrW(window, GWL_STYLE) &
                ~static_cast<LONG_PTR>(WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE | SWP_NOZORDER);
        createControls();
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const auto resize = framelessResizeHitTest(window, lParam);
        if (resize != HTCLIENT) return resize;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        if (point.y >= 0 && point.y < titleHeight(window) &&
                !PtInRect(&closeRect_, point)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (PtInRect(&closeRect_, point)) {
            closePressed_ = true;
            SetCapture(window);
            InvalidateRect(window, &closeRect_, FALSE);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_LBUTTONUP: {
        if (!closePressed_) return DefWindowProcW(window, message, wParam, lParam);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const auto close = PtInRect(&closeRect_, point) != FALSE;
        closePressed_ = false;
        if (GetCapture() == window) ReleaseCapture();
        if (close) DestroyWindow(window);
        else InvalidateRect(window, &closeRect_, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (closePressed_) {
            closePressed_ = false;
            InvalidateRect(window, &closeRect_, FALSE);
        }
        return 0;
    case WM_CONTEXTMENU: {
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (screenPoint.x == -1 && screenPoint.y == -1) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            screenPoint = POINT{bounds.left + 12, bounds.top + titleHeight(window)};
        } else {
            auto clientPoint = screenPoint;
            ScreenToClient(window, &clientPoint);
            if (clientPoint.y < 0 || clientPoint.y >= titleHeight(window)) {
                return DefWindowProcW(window, message, wParam, lParam);
            }
        }
        showFramelessSystemMenu(window, screenPoint);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wParam == VK_SPACE) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            showFramelessSystemMenu(window,
                POINT{bounds.left + 12, bounds.top + titleHeight(window)});
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_SIZE:
        updateLayout();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const auto* bounds = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        updateFont();
        updateLayout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        applyFramelessMinMax(window, lParam);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        applyDpiAwareMinimum(window, limits, 420, 260);
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        paint();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_DRAWITEM:
        drawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == kSaveId) save();
        else if (LOWORD(wParam) == kCancelId) DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        const auto owner = owner_;
        releaseResources();
        window_ = nullptr;
        owner_ = nullptr;
        edit_ = nullptr;
        save_ = nullptr;
        cancel_ = nullptr;
        closeRect_ = {};
        closePressed_ = false;
        trackId_.clear();
        initialNote_.clear();
        if (owner != nullptr && IsWindow(owner)) {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void NoteEditorWindow::createControls()
{
    backgroundBrush_ = CreateSolidBrush(kBackground);
    editBrush_ = CreateSolidBrush(kPanel);
    edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        initialNote_.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
        core_api::get_my_instance(), nullptr);
    SendMessageW(edit_, EM_SETLIMITTEXT, 64U * 1024U, 0);
    save_ = CreateWindowExW(0, L"BUTTON", L"SAVE",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveId)),
        core_api::get_my_instance(), nullptr);
    cancel_ = CreateWindowExW(0, L"BUTTON", L"CANCEL",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)),
        core_api::get_my_instance(), nullptr);
    updateFont();
    updateLayout();
}

void NoteEditorWindow::updateLayout()
{
    if (window_ == nullptr || edit_ == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    const auto dpi = std::max<UINT>(96, GetDpiForWindow(window_));
    const auto chromeHeight = titleHeight(window_);
    const auto padding = MulDiv(16, dpi, 96);
    const auto labelHeight = MulDiv(28, dpi, 96);
    const auto buttonWidth = MulDiv(88, dpi, 96);
    const auto buttonHeight = MulDiv(30, dpi, 96);
    const auto gap = MulDiv(8, dpi, 96);
    const auto buttonTop = client.bottom - padding - buttonHeight;
    closeRect_ = RECT{client.right - chromeHeight, 0, client.right, chromeHeight};
    const auto editWidth = (std::max)(1,
        static_cast<int>(client.right) - 2 * padding);
    const auto editHeight = (std::max)(1,
        static_cast<int>(buttonTop) - gap - padding - labelHeight - chromeHeight);
    MoveWindow(edit_, padding, chromeHeight + padding + labelHeight,
        editWidth, editHeight, TRUE);
    MoveWindow(cancel_, client.right - padding - buttonWidth,
        buttonTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(save_, client.right - padding - 2 * buttonWidth - gap,
        buttonTop, buttonWidth, buttonHeight, TRUE);
}

void NoteEditorWindow::updateFont()
{
    if (font_ != nullptr) DeleteObject(font_);
    const auto dpi = window_ != nullptr
        ? std::max<UINT>(96, GetDpiForWindow(window_)) : 96U;
    font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (const auto control : {edit_, save_, cancel_}) {
        if (control != nullptr) SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void NoteEditorWindow::paint()
{
    PAINTSTRUCT state{};
    const auto dc = BeginPaint(window_, &state);
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, backgroundBrush_);
    const auto dpi = std::max<UINT>(96, GetDpiForWindow(window_));
    const auto chromeHeight = titleHeight(window_);
    RECT chrome{0, 0, client.right, chromeHeight};
    FillRect(dc, &chrome, editBrush_);
    HPEN divider = CreatePen(PS_SOLID, 1, kBorder);
    const auto oldPen = SelectObject(dc, divider);
    MoveToEx(dc, 0, chromeHeight - 1, nullptr);
    LineTo(dc, client.right, chromeHeight - 1);
    SelectObject(dc, oldPen);
    DeleteObject(divider);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    RECT title{MulDiv(12, dpi, 96), 0,
        closeRect_.left - MulDiv(8, dpi, 96), chromeHeight};
    SetTextColor(dc, kText);
    DrawTextW(dc, L"GODENplayer Note", -1, &title,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (closePressed_) FillRect(dc, &closeRect_, backgroundBrush_);
    HPEN closePen = CreatePen(PS_SOLID, 1, closePressed_ ? kAccent : kMuted);
    const auto previousPen = SelectObject(dc, closePen);
    const auto inset = MulDiv(10, dpi, 96);
    MoveToEx(dc, closeRect_.left + inset, closeRect_.top + inset, nullptr);
    LineTo(dc, closeRect_.right - inset, closeRect_.bottom - inset);
    MoveToEx(dc, closeRect_.right - inset, closeRect_.top + inset, nullptr);
    LineTo(dc, closeRect_.left + inset, closeRect_.bottom - inset);
    SelectObject(dc, previousPen);
    DeleteObject(closePen);

    RECT label{MulDiv(16, dpi, 96), chromeHeight + MulDiv(11, dpi, 96),
        client.right - MulDiv(16, dpi, 96),
        chromeHeight + MulDiv(38, dpi, 96)};
    SetTextColor(dc, kMuted);
    DrawTextW(dc, L"PERSONAL NOTE", -1, &label,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    HPEN framePen = CreatePen(PS_SOLID, 1, kBorder);
    const auto frameOldPen = SelectObject(dc, framePen);
    const auto frameOldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, client.right, client.bottom);
    SelectObject(dc, frameOldBrush);
    SelectObject(dc, frameOldPen);
    DeleteObject(framePen);
    EndPaint(window_, &state);
}

void NoteEditorWindow::drawButton(const DRAWITEMSTRUCT& item)
{
    const auto selected = item.itemID == kSaveId;
    const auto pressed = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH fillBrush = CreateSolidBrush(pressed ? RGB(34, 38, 38) : kPanel);
    FillRect(item.hDC, &item.rcItem, fillBrush);
    DeleteObject(fillBrush);
    HPEN pen = CreatePen(PS_SOLID, 1, selected ? kAccent : kBorder);
    const auto oldPen = SelectObject(item.hDC, pen);
    const auto oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top,
        item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(pen);
    wchar_t label[32]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, selected ? kAccent : kText);
    SelectObject(item.hDC, font_);
    auto textRect = item.rcItem;
    DrawTextW(item.hDC, label, -1, &textRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void NoteEditorWindow::save()
{
    const auto edit = GetDlgItem(window_, kEditId);
    if (edit == nullptr) {
        MessageBeep(MB_ICONERROR);
        return;
    }
    const auto length = static_cast<std::size_t>(
        SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0));
    std::vector<wchar_t> buffer(length + 1U, L'\0');
    const auto copied = static_cast<std::size_t>(SendMessageW(
        edit, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
        reinterpret_cast<LPARAM>(buffer.data())));
    if (copied > length) {
        MessageBeep(MB_ICONERROR);
        return;
    }
    std::wstring note(buffer.data(), copied);
    if (!DatabaseService::instance().setCurrentNote(
            wideToUtf8(note), trackId_)) {
        MessageBeep(MB_ICONWARNING);
        SetWindowTextW(window_, L"GODENplayer Note - track changed");
        return;
    }
    DestroyWindow(window_);
}

void NoteEditorWindow::releaseResources()
{
    if (font_ != nullptr) DeleteObject(font_);
    if (backgroundBrush_ != nullptr) DeleteObject(backgroundBrush_);
    if (editBrush_ != nullptr) DeleteObject(editBrush_);
    font_ = nullptr;
    backgroundBrush_ = nullptr;
    editBrush_ = nullptr;
}

} // namespace nekorox::component
