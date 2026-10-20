#pragma once

#include <windows.h>

#include <string>

namespace nekorox::component {

class NoteEditorWindow final {
public:
    static NoteEditorWindow& instance();

    void show(HWND owner);
    void close();

private:
    NoteEditorWindow() = default;
    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void updateLayout();
    void updateFont();
    void paint();
    void drawButton(const DRAWITEMSTRUCT& item);
    void save();
    void releaseResources();

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HWND save_ = nullptr;
    HWND cancel_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    RECT closeRect_{};
    bool closePressed_ = false;
    std::string trackId_;
    std::wstring initialNote_;
};

} // namespace nekorox::component
