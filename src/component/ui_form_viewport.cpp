#include "ui_form_viewport.h"

#include <algorithm>
#include <commctrl.h>

namespace nekorox::component {
namespace {

constexpr wchar_t kViewportClass[] = L"GodenFormViewport";
constexpr wchar_t kContentClass[] = L"GodenFormContent";
constexpr UINT_PTR kControlSubclassId = 0x47565054U;

void registerClasses(HINSTANCE instance)
{
    WNDCLASSEXW viewport{sizeof(viewport)};
    viewport.lpfnWndProc = &FormViewport::viewportProc;
    viewport.hInstance = instance;
    viewport.hCursor = LoadCursor(nullptr, IDC_ARROW);
    viewport.lpszClassName = kViewportClass;
    RegisterClassExW(&viewport);

    WNDCLASSEXW content{sizeof(content)};
    content.lpfnWndProc = &FormViewport::contentProc;
    content.hInstance = instance;
    content.hCursor = LoadCursor(nullptr, IDC_ARROW);
    content.lpszClassName = kContentClass;
    RegisterClassExW(&content);
}

FormViewport* instance(HWND window) noexcept
{
    return reinterpret_cast<FormViewport*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

bool forwardedMessage(UINT message) noexcept
{
    switch (message) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        return true;
    default:
        return false;
    }
}

LRESULT eraseFormBackground(HWND owner, HWND window, HDC dc)
{
    RECT client{};
    GetClientRect(window, &client);
    auto brush = owner != nullptr
        ? reinterpret_cast<HBRUSH>(SendMessageW(owner,
            WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
            reinterpret_cast<LPARAM>(window)))
        : nullptr;
    if (brush == nullptr) brush = static_cast<HBRUSH>(
        GetStockObject(BLACK_BRUSH));
    FillRect(dc, &client, brush);
    return TRUE;
}

} // namespace

bool FormViewport::create(HWND owner, int controlId)
{
    if (owner == nullptr) return false;
    destroy();
    owner_ = owner;
    const auto module = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    registerClasses(module);
    viewport_ = CreateWindowExW(WS_EX_CONTROLPARENT,
        kViewportClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
        0, 0, 1, 1, owner,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), module, this);
    if (viewport_ == nullptr) {
        owner_ = nullptr;
        return false;
    }
    content_ = CreateWindowExW(WS_EX_CONTROLPARENT, kContentClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1, viewport_, nullptr, module, this);
    if (content_ == nullptr) {
        DestroyWindow(viewport_);
        viewport_ = nullptr;
        owner_ = nullptr;
        return false;
    }
    updateScrollInfo();
    return true;
}

void FormViewport::destroy() noexcept
{
    if (viewport_ != nullptr && IsWindow(viewport_)) DestroyWindow(viewport_);
    viewport_ = nullptr;
    content_ = nullptr;
    owner_ = nullptr;
    scrollOffset_ = 0;
    contentHeight_ = 1;
    wheelRemainder_ = 0;
}

void FormViewport::setBounds(
    int x, int y, int width, int height, int contentHeight)
{
    if (viewport_ == nullptr || content_ == nullptr) return;
    width = (std::max)(1, width);
    height = (std::max)(1, height);
    contentHeight_ = (std::max)(1, contentHeight);
    SetWindowPos(viewport_, nullptr, x, y, width, height,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    updateScrollInfo();
    positionContent(false);
    RedrawWindow(viewport_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void FormViewport::trackControl(HWND control)
{
    if (control != nullptr) SetWindowSubclass(control, controlProc,
        kControlSubclassId, reinterpret_cast<DWORD_PTR>(this));
}

void FormViewport::ensureVisible(HWND control)
{
    if (control == nullptr || content_ == nullptr || viewport_ == nullptr ||
        !IsChild(content_, control)) return;
    RECT controlRect{};
    GetWindowRect(control, &controlRect);
    MapWindowPoints(nullptr, content_,
        reinterpret_cast<POINT*>(&controlRect), 2);
    RECT client{};
    GetClientRect(viewport_, &client);
    const auto margin = (std::max)(4, MulDiv(8,
        static_cast<int>((std::max)(96U, GetDpiForWindow(viewport_))), 96));
    auto next = scrollOffset_;
    if (controlRect.top < margin)
        next += controlRect.top - margin;
    else if (controlRect.bottom > client.bottom - margin)
        next += controlRect.bottom - client.bottom + margin;
    scrollTo(next);
}

void FormViewport::scrollTo(int offset)
{
    const auto next = (std::clamp)(offset, 0, maximumOffset());
    if (next == scrollOffset_) return;
    scrollOffset_ = next;
    updateScrollInfo();
    positionContent(false);
    RedrawWindow(viewport_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void FormViewport::scrollBy(int delta)
{
    scrollTo(scrollOffset_ + delta);
}

LRESULT CALLBACK FormViewport::viewportProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    FormViewport* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<FormViewport*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = instance(window);
    }
    return self != nullptr
        ? self->handleViewportMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FormViewport::contentProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    FormViewport* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<FormViewport*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = instance(window);
    }
    return self != nullptr
        ? self->handleContentMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FormViewport::controlProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data)
{
    auto* self = reinterpret_cast<FormViewport*>(data);
    if (message == WM_SETFOCUS && self != nullptr)
        self->ensureVisible(window);
    if (message == WM_KEYDOWN && wParam == VK_TAB && self != nullptr &&
            self->owner_ != nullptr) {
        const auto previous = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (const auto next = GetNextDlgTabItem(
                self->owner_, window, previous); next != nullptr) {
            SetFocus(next);
            self->ensureVisible(next);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, controlProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT FormViewport::handleViewportMessage(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE:
        updateScrollInfo();
        positionContent(false);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window, SB_VERT, &info);
        auto next = info.nPos;
        const auto line = MulDiv(30,
            static_cast<int>((std::max)(96U, GetDpiForWindow(window))), 96);
        switch (LOWORD(wParam)) {
        case SB_LINEUP: next -= line; break;
        case SB_LINEDOWN: next += line; break;
        case SB_PAGEUP: next -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: next += static_cast<int>(info.nPage); break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: next = info.nTrackPos; break;
        case SB_TOP: next = info.nMin; break;
        case SB_BOTTOM: next = info.nMax; break;
        default: return 0;
        }
        scrollTo(next);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
        const auto notches = wheelRemainder_ / WHEEL_DELTA;
        wheelRemainder_ %= WHEEL_DELTA;
        if (notches != 0) {
            const auto line = MulDiv(72,
                static_cast<int>((std::max)(96U, GetDpiForWindow(window))), 96);
            scrollBy(-notches * line);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return eraseFormBackground(owner_, window,
            reinterpret_cast<HDC>(wParam));
    case WM_NCDESTROY:
        if (window == viewport_) viewport_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT FormViewport::handleContentMessage(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (forwardedMessage(message) && owner_ != nullptr)
        return SendMessageW(owner_, message, wParam, lParam);
    switch (message) {
    case WM_MOUSEWHEEL:
        return viewport_ != nullptr
            ? SendMessageW(viewport_, message, wParam, lParam) : 0;
    case WM_ERASEBKGND:
        return eraseFormBackground(owner_, window,
            reinterpret_cast<HDC>(wParam));
    case WM_NCDESTROY:
        if (window == content_) content_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void FormViewport::updateScrollInfo()
{
    if (viewport_ == nullptr) return;
    RECT client{};
    GetClientRect(viewport_, &client);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = (std::max)(0, contentHeight_ - 1);
    info.nPage = static_cast<UINT>((std::max)(1L, client.bottom));
    scrollOffset_ = (std::clamp)(scrollOffset_, 0, maximumOffset());
    info.nPos = scrollOffset_;
    SetScrollInfo(viewport_, SB_VERT, &info, TRUE);
}

void FormViewport::positionContent(bool repaint)
{
    if (viewport_ == nullptr || content_ == nullptr) return;
    RECT client{};
    GetClientRect(viewport_, &client);
    SetWindowPos(content_, nullptr, 0, -scrollOffset_,
        (std::max)(1L, client.right), contentHeight_,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
            SWP_NOCOPYBITS);
    if (repaint) {
        RedrawWindow(viewport_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

int FormViewport::maximumOffset() const noexcept
{
    if (viewport_ == nullptr) return 0;
    RECT client{};
    GetClientRect(viewport_, &client);
    return (std::max)(0, contentHeight_ -
        static_cast<int>((std::max)(1L, client.bottom)));
}

} // namespace nekorox::component
