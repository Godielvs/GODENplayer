#include "persona_editor.h"

#include "database_service.h"
#include "frameless_window.h"
#include "text_util.h"

#include <algorithm>
#include <set>
#include <vector>

namespace nekorox::component {
namespace {

constexpr wchar_t kWindowClass[] = L"NekoRoXPersonaEditorV3";
constexpr int kDisplayNameId = 200;
constexpr int kProfileId = 201;
constexpr int kAliasesId = 202;
constexpr int kRelationshipListId = 203;
constexpr int kTargetId = 204;
constexpr int kRelationId = 205;
constexpr int kRelationshipNoteId = 206;
constexpr int kApplyRelationshipId = 207;
constexpr int kRemoveRelationshipId = 208;
constexpr int kSaveId = 209;
constexpr int kCancelId = 210;
constexpr COLORREF kBackground = RGB(10, 11, 11);
constexpr COLORREF kPanel = RGB(18, 20, 20);
constexpr COLORREF kSelected = RGB(27, 31, 31);
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
    const auto monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const auto left = std::clamp(
        ownerBounds.left + (ownerBounds.right - ownerBounds.left - width) / 2,
        info.rcWork.left, (std::max)(info.rcWork.left, info.rcWork.right - width));
    const auto top = std::clamp(
        ownerBounds.top + (ownerBounds.bottom - ownerBounds.top - height) / 2,
        info.rcWork.top, (std::max)(info.rcWork.top, info.rcWork.bottom - height));
    return RECT{left, top, left + width, top + height};
}

HMENU controlId(int value) noexcept
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(value));
}

std::wstring controlText(HWND control)
{
    if (control == nullptr) return {};
    const auto length = static_cast<std::size_t>(
        SendMessageW(control, WM_GETTEXTLENGTH, 0, 0));
    std::vector<wchar_t> buffer(length + 1U, L'\0');
    const auto copied = static_cast<std::size_t>(SendMessageW(
        control, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
        reinterpret_cast<LPARAM>(buffer.data())));
    return std::wstring(buffer.data(), (std::min)(copied, length));
}

std::vector<std::wstring> textLines(std::wstring text)
{
    std::vector<std::wstring> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find_first_of(L"\r\n", begin);
        auto line = trimText(text.substr(begin,
            end == std::wstring::npos ? std::wstring::npos : end - begin));
        if (!line.empty()) result.push_back(std::move(line));
        if (end == std::wstring::npos) break;
        begin = end + 1;
        if (text[end] == L'\r' && begin < text.size() && text[begin] == L'\n') {
            ++begin;
        }
    }
    return result;
}

} // namespace

PersonaEditorWindow& PersonaEditorWindow::instance()
{
    static PersonaEditorWindow editor;
    return editor;
}

void PersonaEditorWindow::show(HWND owner, std::wstring fallbackProfile)
{
    const auto snapshot = DatabaseService::instance().personaSnapshot();
    if (!snapshot->available || snapshot->aliasKey.empty()) return;
    if (window_ != nullptr) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        return;
    }

    owner_ = GetAncestor(owner, GA_ROOT);
    if (owner_ == nullptr) owner_ = owner;
    expectedAliasKey_ = snapshot->aliasKey;
    persona_ = snapshot->persona;
    if (!persona_.found) {
        persona_ = {};
        persona_.displayName = snapshot->artistAlias;
        persona_.aliases.push_back({snapshot->artistAlias, snapshot->aliasKey});
    }
    if (persona_.profile.empty() && !fallbackProfile.empty()) {
        persona_.profile = wideToUtf8(trimText(std::move(fallbackProfile)));
    }
    relationships_ = persona_.relationships;
    status_.clear();
    page_ = Page::profile;
    selectedRelationship_ = -1;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &PersonaEditorWindow::windowProc;
    windowClass.hInstance = core_api::get_my_instance();
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    const auto dpi = (std::max)(96U, GetDpiForWindow(owner_));
    const auto width = MulDiv(720, dpi, 96);
    const auto height = MulDiv(540, dpi, 96);
    const auto bounds = centeredBounds(owner_, width, height);
    window_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
        kWindowClass, L"GODENplayer Persona",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        owner_, nullptr, core_api::get_my_instance(), this);
    if (window_ == nullptr) {
        owner_ = nullptr;
        expectedAliasKey_.clear();
        persona_ = {};
        relationships_.clear();
        return;
    }
    EnableWindow(owner_, FALSE);
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    SetFocus(profile_);
}

void PersonaEditorWindow::close()
{
    if (window_ != nullptr) DestroyWindow(window_);
}

LRESULT CALLBACK PersonaEditorWindow::windowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    PersonaEditorWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PersonaEditorWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PersonaEditorWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self != nullptr
        ? self->handleMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT PersonaEditorWindow::handleMessage(
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
                !PtInRect(&closeRect_, point)) return HTCAPTION;
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
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (closePressed_) {
            const auto closeWindow = PtInRect(&closeRect_, point) != FALSE;
            closePressed_ = false;
            if (GetCapture() == window) ReleaseCapture();
            if (closeWindow) DestroyWindow(window);
            else InvalidateRect(window, &closeRect_, FALSE);
            return 0;
        }
        for (unsigned index = 0; index < tabRects_.size(); ++index) {
            if (PtInRect(&tabRects_[index], point)) {
                selectPage(static_cast<Page>(index));
                return 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_CAPTURECHANGED:
        closePressed_ = false;
        InvalidateRect(window, &closeRect_, FALSE);
        return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            point = POINT{bounds.left + 12, bounds.top + titleHeight(window)};
        }
        showFramelessSystemMenu(window, point);
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
        applyDpiAwareMinimum(window, limits, 600, 440);
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        paint();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item->CtlID == kRelationshipListId) {
            item->itemHeight = MulDiv(25,
                (std::max)(96U, GetDpiForWindow(window)), 96);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM:
        drawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kSaveId: save(); break;
        case kCancelId: DestroyWindow(window); break;
        case kApplyRelationshipId: applyRelationship(); break;
        case kRemoveRelationshipId: removeRelationship(); break;
        case kRelationshipListId:
            if (HIWORD(wParam) == LBN_SELCHANGE) loadRelationshipSelection();
            break;
        default: break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        const auto owner = owner_;
        releaseResources();
        window_ = nullptr;
        owner_ = nullptr;
        displayName_ = nullptr;
        profile_ = nullptr;
        aliases_ = nullptr;
        relationshipList_ = nullptr;
        target_ = nullptr;
        relation_ = nullptr;
        relationshipNote_ = nullptr;
        applyRelationship_ = nullptr;
        removeRelationship_ = nullptr;
        save_ = nullptr;
        cancel_ = nullptr;
        closeRect_ = {};
        tabRects_ = {};
        closePressed_ = false;
        expectedAliasKey_.clear();
        persona_ = {};
        relationships_.clear();
        status_.clear();
        if (owner != nullptr && IsWindow(owner)) {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PersonaEditorWindow::createControls()
{
    backgroundBrush_ = CreateSolidBrush(kBackground);
    panelBrush_ = CreateSolidBrush(kPanel);
    const auto instance = core_api::get_my_instance();
    const auto editStyle = WS_CHILD | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL;
    const auto multiStyle = WS_CHILD | WS_TABSTOP | WS_VSCROLL |
        ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    displayName_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle,
        0, 0, 0, 0, window_, controlId(kDisplayNameId), instance, nullptr);
    profile_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", multiStyle,
        0, 0, 0, 0, window_, controlId(kProfileId), instance, nullptr);
    aliases_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", multiStyle,
        0, 0, 0, 0, window_, controlId(kAliasesId), instance, nullptr);
    relationshipList_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, window_, controlId(kRelationshipListId), instance, nullptr);
    target_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle,
        0, 0, 0, 0, window_, controlId(kTargetId), instance, nullptr);
    relation_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle,
        0, 0, 0, 0, window_, controlId(kRelationId), instance, nullptr);
    relationshipNote_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", multiStyle,
        0, 0, 0, 0, window_, controlId(kRelationshipNoteId), instance, nullptr);
    const auto buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    applyRelationship_ = CreateWindowExW(0, L"BUTTON", L"APPLY", buttonStyle,
        0, 0, 0, 0, window_, controlId(kApplyRelationshipId), instance, nullptr);
    removeRelationship_ = CreateWindowExW(0, L"BUTTON", L"REMOVE", buttonStyle,
        0, 0, 0, 0, window_, controlId(kRemoveRelationshipId), instance, nullptr);
    save_ = CreateWindowExW(0, L"BUTTON", L"SAVE", buttonStyle | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, window_, controlId(kSaveId), instance, nullptr);
    cancel_ = CreateWindowExW(0, L"BUTTON", L"CANCEL", buttonStyle,
        0, 0, 0, 0, window_, controlId(kCancelId), instance, nullptr);

    SendMessageW(profile_, EM_SETLIMITTEXT, 512U * 1024U, 0);
    SendMessageW(aliases_, EM_SETLIMITTEXT, 64U * 1024U, 0);
    SendMessageW(relationshipNote_, EM_SETLIMITTEXT, 64U * 1024U, 0);
    SetWindowTextW(displayName_, utf8ToWide(persona_.displayName).c_str());
    SetWindowTextW(profile_, utf8ToWide(persona_.profile).c_str());
    std::wstring aliases;
    for (const auto& alias : persona_.aliases) {
        if (!aliases.empty()) aliases += L"\r\n";
        aliases += utf8ToWide(alias.display);
    }
    SetWindowTextW(aliases_, aliases.c_str());
    updateFont();
    rebuildRelationshipList();
    selectPage(Page::profile);
}

void PersonaEditorWindow::updateLayout()
{
    if (window_ == nullptr || save_ == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    const auto dpi = (std::max)(96U, GetDpiForWindow(window_));
    const auto chrome = titleHeight(window_);
    const auto tabs = MulDiv(28, dpi, 96);
    const auto padding = MulDiv(16, dpi, 96);
    const auto gap = MulDiv(8, dpi, 96);
    const auto fieldHeight = MulDiv(28, dpi, 96);
    const auto buttonWidth = MulDiv(88, dpi, 96);
    const auto buttonHeight = MulDiv(30, dpi, 96);
    const auto labelHeight = MulDiv(20, dpi, 96);
    closeRect_ = RECT{client.right - chrome, 0, client.right, chrome};
    const auto tabWidth = client.right / 3;
    for (int index = 0; index < 3; ++index) {
        tabRects_[index] = RECT{
            index * tabWidth, chrome,
            index == 2 ? client.right : (index + 1) * tabWidth,
            chrome + tabs};
    }
    const auto contentTop = chrome + tabs;
    const auto buttonTop = static_cast<int>(client.bottom) - padding - buttonHeight;
    MoveWindow(cancel_, client.right - padding - buttonWidth,
        buttonTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(save_, client.right - padding - 2 * buttonWidth - gap,
        buttonTop, buttonWidth, buttonHeight, TRUE);

    const auto contentLeft = padding;
    const auto contentRight = static_cast<int>(client.right) - padding;
    const auto contentBottom = buttonTop - padding;
    MoveWindow(displayName_, contentLeft, contentTop + padding + labelHeight,
        contentRight - contentLeft, fieldHeight, TRUE);
    const auto profileTop = contentTop + padding + labelHeight + fieldHeight +
        gap + labelHeight;
    MoveWindow(profile_, contentLeft, profileTop,
        contentRight - contentLeft, (std::max)(1, contentBottom - profileTop), TRUE);
    MoveWindow(aliases_, contentLeft, contentTop + padding + labelHeight,
        contentRight - contentLeft,
        (std::max)(1, contentBottom - contentTop - padding - labelHeight), TRUE);

    const auto split = contentLeft + (contentRight - contentLeft) * 43 / 100;
    MoveWindow(relationshipList_, contentLeft, contentTop + padding + labelHeight,
        split - contentLeft - gap,
        (std::max)(1, contentBottom - contentTop - padding - labelHeight), TRUE);
    const auto rightLeft = split + gap;
    auto y = contentTop + padding + labelHeight;
    MoveWindow(target_, rightLeft, y, contentRight - rightLeft, fieldHeight, TRUE);
    y += fieldHeight + gap + labelHeight;
    MoveWindow(relation_, rightLeft, y, contentRight - rightLeft, fieldHeight, TRUE);
    y += fieldHeight + gap + labelHeight;
    const auto relationButtonsTop = contentBottom - buttonHeight;
    MoveWindow(relationshipNote_, rightLeft, y, contentRight - rightLeft,
        (std::max)(1, relationButtonsTop - gap - y), TRUE);
    MoveWindow(removeRelationship_, contentRight - buttonWidth,
        relationButtonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(applyRelationship_, contentRight - 2 * buttonWidth - gap,
        relationButtonsTop, buttonWidth, buttonHeight, TRUE);

    selectPage(page_);
}

void PersonaEditorWindow::updateFont()
{
    if (font_ != nullptr) DeleteObject(font_);
    const auto dpi = window_ != nullptr
        ? (std::max)(96U, GetDpiForWindow(window_)) : 96U;
    font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (const auto control : {displayName_, profile_, aliases_, relationshipList_,
             target_, relation_, relationshipNote_, applyRelationship_,
             removeRelationship_, save_, cancel_}) {
        if (control != nullptr) SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void PersonaEditorWindow::paint()
{
    PAINTSTRUCT state{};
    const auto dc = BeginPaint(window_, &state);
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, backgroundBrush_);
    const auto dpi = (std::max)(96U, GetDpiForWindow(window_));
    const auto chrome = titleHeight(window_);
    RECT chromeRect{0, 0, client.right, chrome};
    FillRect(dc, &chromeRect, panelBrush_);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    SetTextColor(dc, kText);
    RECT title{MulDiv(12, dpi, 96), 0,
        closeRect_.left - MulDiv(8, dpi, 96), chrome};
    DrawTextW(dc, L"GODENplayer Persona", -1, &title,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (closePressed_) FillRect(dc, &closeRect_, backgroundBrush_);
    HPEN closePen = CreatePen(PS_SOLID, 1, closePressed_ ? kAccent : kMuted);
    auto oldPen = SelectObject(dc, closePen);
    const auto closeInset = MulDiv(10, dpi, 96);
    MoveToEx(dc, closeRect_.left + closeInset, closeRect_.top + closeInset, nullptr);
    LineTo(dc, closeRect_.right - closeInset, closeRect_.bottom - closeInset);
    MoveToEx(dc, closeRect_.right - closeInset, closeRect_.top + closeInset, nullptr);
    LineTo(dc, closeRect_.left + closeInset, closeRect_.bottom - closeInset);
    SelectObject(dc, oldPen);
    DeleteObject(closePen);

    constexpr std::array labels{L"PROFILE", L"ALIASES", L"RELATIONS"};
    for (unsigned index = 0; index < labels.size(); ++index) {
        const auto selected = static_cast<unsigned>(page_) == index;
        SetTextColor(dc, selected ? kAccent : kMuted);
        auto rectangle = tabRects_[index];
        DrawTextW(dc, labels[index], -1, &rectangle,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (selected) {
            HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
            oldPen = SelectObject(dc, pen);
            MoveToEx(dc, rectangle.left, rectangle.bottom - 1, nullptr);
            LineTo(dc, rectangle.right, rectangle.bottom - 1);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
    }

    const auto contentTop = tabRects_[0].bottom;
    const auto padding = MulDiv(16, dpi, 96);
    const auto labelHeight = MulDiv(20, dpi, 96);
    SetTextColor(dc, kMuted);
    if (page_ == Page::profile) {
        RECT displayLabel{padding, contentTop + padding,
            client.right - padding, contentTop + padding + labelHeight};
        DrawTextW(dc, L"DISPLAY NAME", -1, &displayLabel,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT profileLabel{padding,
            contentTop + padding + labelHeight + MulDiv(28, dpi, 96) +
                MulDiv(8, dpi, 96),
            client.right - padding, client.bottom};
        DrawTextW(dc, L"PROFILE", -1, &profileLabel,
            DT_LEFT | DT_TOP | DT_SINGLELINE);
    } else if (page_ == Page::aliases) {
        RECT label{padding, contentTop + padding,
            client.right - padding, contentTop + padding + labelHeight};
        DrawTextW(dc, L"ALIASES", -1, &label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        const auto contentRight = client.right - padding;
        const auto split = padding + (contentRight - padding) * 43 / 100;
        const auto gap = MulDiv(8, dpi, 96);
        const auto fieldHeight = MulDiv(28, dpi, 96);
        RECT listLabel{padding, contentTop + padding,
            split - gap, contentTop + padding + labelHeight};
        DrawTextW(dc, L"RELATIONS", -1, &listLabel,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        auto y = contentTop + padding;
        RECT targetLabel{split + gap, y, contentRight, y + labelHeight};
        DrawTextW(dc, L"TARGET", -1, &targetLabel,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += labelHeight + fieldHeight + MulDiv(8, dpi, 96);
        RECT relationLabel{split + gap, y, contentRight, y + labelHeight};
        DrawTextW(dc, L"RELATION", -1, &relationLabel,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += labelHeight + fieldHeight + MulDiv(8, dpi, 96);
        RECT noteLabel{split + gap, y, contentRight, y + labelHeight};
        DrawTextW(dc, L"NOTE", -1, &noteLabel,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (!status_.empty()) {
        SetTextColor(dc, kAccent);
        RECT status{padding, client.bottom - padding - MulDiv(28, dpi, 96),
            client.right - MulDiv(220, dpi, 96), client.bottom - padding};
        DrawTextW(dc, status_.c_str(), -1, &status,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    HPEN framePen = CreatePen(PS_SOLID, 1, kBorder);
    oldPen = SelectObject(dc, framePen);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, client.right, client.bottom);
    MoveToEx(dc, 0, chrome - 1, nullptr);
    LineTo(dc, client.right, chrome - 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(framePen);
    EndPaint(window_, &state);
}

void PersonaEditorWindow::drawItem(const DRAWITEMSTRUCT& item)
{
    if (item.CtlID == kRelationshipListId) {
        if (item.itemID == static_cast<UINT>(-1)) return;
        const auto selected = (item.itemState & ODS_SELECTED) != 0;
        HBRUSH brush = CreateSolidBrush(selected ? kSelected : kPanel);
        FillRect(item.hDC, &item.rcItem, brush);
        DeleteObject(brush);
        std::wstring label;
        if (item.itemID < relationships_.size()) {
            const auto& relationship = relationships_[item.itemID];
            label = utf8ToWide(relationship.relation) + L"  " +
                utf8ToWide(relationship.targetDisplayName);
        }
        auto rectangle = item.rcItem;
        rectangle.left += 8;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, selected ? kAccent : kText);
        SelectObject(item.hDC, font_);
        DrawTextW(item.hDC, label.c_str(), -1, &rectangle,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    const auto accent = item.CtlID == kSaveId ||
        item.CtlID == kApplyRelationshipId;
    const auto pressed = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH brush = CreateSolidBrush(pressed ? kSelected : kPanel);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, accent ? kAccent : kBorder);
    const auto oldPen = SelectObject(item.hDC, pen);
    const auto oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top,
        item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(pen);
    wchar_t label[32]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    auto rectangle = item.rcItem;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, accent ? kAccent : kText);
    SelectObject(item.hDC, font_);
    DrawTextW(item.hDC, label, -1, &rectangle,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PersonaEditorWindow::selectPage(Page page)
{
    page_ = page;
    const auto profileVisible = page == Page::profile ? SW_SHOW : SW_HIDE;
    const auto aliasesVisible = page == Page::aliases ? SW_SHOW : SW_HIDE;
    const auto relationsVisible = page == Page::relations ? SW_SHOW : SW_HIDE;
    ShowWindow(displayName_, profileVisible);
    ShowWindow(profile_, profileVisible);
    ShowWindow(aliases_, aliasesVisible);
    for (const auto control : {relationshipList_, target_, relation_,
             relationshipNote_, applyRelationship_, removeRelationship_}) {
        ShowWindow(control, relationsVisible);
    }
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

void PersonaEditorWindow::rebuildRelationshipList()
{
    SendMessageW(relationshipList_, LB_RESETCONTENT, 0, 0);
    for (const auto& relationship : relationships_) {
        const auto label = utf8ToWide(relationship.relation) + L"  " +
            utf8ToWide(relationship.targetDisplayName);
        SendMessageW(relationshipList_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (selectedRelationship_ >= 0 &&
        selectedRelationship_ < static_cast<int>(relationships_.size())) {
        SendMessageW(relationshipList_, LB_SETCURSEL,
            static_cast<WPARAM>(selectedRelationship_), 0);
    }
}

void PersonaEditorWindow::loadRelationshipSelection()
{
    selectedRelationship_ = static_cast<int>(
        SendMessageW(relationshipList_, LB_GETCURSEL, 0, 0));
    if (selectedRelationship_ < 0 ||
        selectedRelationship_ >= static_cast<int>(relationships_.size())) return;
    const auto& relationship = relationships_[selectedRelationship_];
    SetWindowTextW(target_, utf8ToWide(relationship.targetDisplayName).c_str());
    SetWindowTextW(relation_, utf8ToWide(relationship.relation).c_str());
    SetWindowTextW(relationshipNote_, utf8ToWide(relationship.note).c_str());
}

void PersonaEditorWindow::applyRelationship()
{
    const auto target = trimText(controlText(target_));
    const auto relation = trimText(controlText(relation_));
    if (target.empty() || relation.empty()) {
        status_ = L"Target and relation are required";
        MessageBeep(MB_ICONWARNING);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    PersistedPersonaRelationship next;
    if (selectedRelationship_ >= 0 &&
        selectedRelationship_ < static_cast<int>(relationships_.size())) {
        next = relationships_[selectedRelationship_];
    }
    next.targetDisplayName = wideToUtf8(target);
    next.targetAliasKey = personaAliasKey(target);
    next.relation = wideToUtf8(relation);
    next.note = wideToUtf8(trimText(controlText(relationshipNote_)));
    if (selectedRelationship_ < 0) {
        const auto match = std::find_if(relationships_.begin(), relationships_.end(),
            [&](const auto& candidate) {
                return candidate.targetAliasKey == next.targetAliasKey &&
                    candidate.relation == next.relation;
            });
        if (match == relationships_.end()) {
            relationships_.push_back(std::move(next));
            selectedRelationship_ = static_cast<int>(relationships_.size() - 1U);
        } else {
            *match = std::move(next);
            selectedRelationship_ = static_cast<int>(
                std::distance(relationships_.begin(), match));
        }
    } else {
        relationships_[selectedRelationship_] = std::move(next);
    }
    status_.clear();
    rebuildRelationshipList();
    InvalidateRect(window_, nullptr, FALSE);
}

void PersonaEditorWindow::removeRelationship()
{
    if (selectedRelationship_ < 0 ||
        selectedRelationship_ >= static_cast<int>(relationships_.size())) return;
    relationships_.erase(relationships_.begin() + selectedRelationship_);
    selectedRelationship_ = -1;
    SetWindowTextW(target_, L"");
    SetWindowTextW(relation_, L"");
    SetWindowTextW(relationshipNote_, L"");
    rebuildRelationshipList();
}

void PersonaEditorWindow::save()
{
    const auto displayName = trimText(controlText(displayName_));
    if (displayName.empty()) {
        status_ = L"Display name is required";
        selectPage(Page::profile);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    std::vector<PersistedPersonaAlias> aliases;
    std::set<std::string> keys;
    for (const auto& line : textLines(controlText(aliases_))) {
        const auto key = personaAliasKey(line);
        if (!key.empty() && keys.insert(key).second) {
            aliases.push_back({wideToUtf8(line), key});
        }
    }
    const auto snapshot = DatabaseService::instance().personaSnapshot();
    if (keys.insert(expectedAliasKey_).second) {
        aliases.push_back({snapshot->artistAlias, expectedAliasKey_});
    }
    if (aliases.empty()) {
        status_ = L"At least one alias is required";
        selectPage(Page::aliases);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    persona_.displayName = wideToUtf8(displayName);
    persona_.profile = wideToUtf8(trimText(controlText(profile_)));
    persona_.aliases = std::move(aliases);
    persona_.relationships = relationships_;
    if (!DatabaseService::instance().saveCurrentPersona(
            persona_, expectedAliasKey_)) {
        status_ = L"Track changed before the persona could be saved";
        MessageBeep(MB_ICONWARNING);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    DestroyWindow(window_);
}

void PersonaEditorWindow::releaseResources()
{
    if (font_ != nullptr) DeleteObject(font_);
    if (backgroundBrush_ != nullptr) DeleteObject(backgroundBrush_);
    if (panelBrush_ != nullptr) DeleteObject(panelBrush_);
    font_ = nullptr;
    backgroundBrush_ = nullptr;
    panelBrush_ = nullptr;
}

} // namespace nekorox::component
