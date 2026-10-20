#pragma once

#include <windows.h>

namespace nekorox::component {

// Owns a native clipped viewport and one vertically translated content HWND.
// Child controls keep their native implementations and original control IDs.
class FormViewport final {
public:
    FormViewport() = default;
    FormViewport(const FormViewport&) = delete;
    FormViewport& operator=(const FormViewport&) = delete;

    bool create(HWND owner, int controlId);
    void destroy() noexcept;
    void setBounds(int x, int y, int width, int height, int contentHeight);
    void trackControl(HWND control);
    void ensureVisible(HWND control);
    void scrollTo(int offset);
    void scrollBy(int delta);

    [[nodiscard]] HWND viewport() const noexcept { return viewport_; }
    [[nodiscard]] HWND content() const noexcept { return content_; }
    [[nodiscard]] int scrollOffset() const noexcept { return scrollOffset_; }
    [[nodiscard]] int contentHeight() const noexcept { return contentHeight_; }

    // Window procedures are public only so RegisterClassEx can bind them;
    // all stateful work remains private and instance-routed.
    static LRESULT CALLBACK viewportProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK contentProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK controlProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data);

private:

    LRESULT handleViewportMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleContentMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void updateScrollInfo();
    void positionContent(bool repaint);
    [[nodiscard]] int maximumOffset() const noexcept;

    HWND owner_ = nullptr;
    HWND viewport_ = nullptr;
    HWND content_ = nullptr;
    int scrollOffset_ = 0;
    int contentHeight_ = 1;
    int wheelRemainder_ = 0;
};

} // namespace nekorox::component
