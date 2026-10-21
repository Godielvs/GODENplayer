#pragma once

#include "ui_form_viewport.h"

#include <array>
#include <string>
#include <windows.h>

namespace nekorox::component {

class OptionsWindow final {
public:
    static OptionsWindow& instance();
    void show(HWND owner = nullptr);
    void close();

private:
    OptionsWindow() = default;
    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void updateLayout();
    void updateFont();
    void refreshAll();
    void refreshPresenceStatus();
    void refreshCoverControls();
    void selectPage(std::size_t index);
    void markPresenceDirty();
    void updateFooter();
    bool confirmDiscard();
    void refreshListenEffectsStatus();
    void refreshAlbumAmbientStatus();
    void updateFallbackLabel();
    void drawButton(const DRAWITEMSTRUCT& item);
    void drawCheckbox(const DRAWITEMSTRUCT& item);
    void drawComboItem(const DRAWITEMSTRUCT& item);
    bool toggleCheckbox(HWND control);
    void paint(HDC external = nullptr);
    void releaseResources();

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    FormViewport viewport_;
    HWND title_ = nullptr;
    std::array<HWND, 5> sectionLabels_{};
    std::array<HWND, 5> navigation_{};
    std::array<int, 5> pageScroll_{};
    std::size_t page_ = 0;
    bool presenceAdvanced_ = false;
    bool presenceDirty_ = false;
    bool refreshing_ = false;
    HWND motionLabel_ = nullptr;
    HWND applicationLabel_ = nullptr;
    HWND presenceHint_ = nullptr;
    HWND providerLabel_ = nullptr;
    HWND playbackHint_ = nullptr;
    HWND lyricsHint_ = nullptr;
    HWND toolsHint_ = nullptr;
    HWND listenEffectsLabel_ = nullptr;
    HWND albumAmbientLabel_ = nullptr;
    HWND advancedPresence_ = nullptr;
    HWND footerStatus_ = nullptr;
    HWND closeButton_ = nullptr;
    HWND identity_ = nullptr;
    HWND fallbackButton_ = nullptr;
    HWND fallbackValue_ = nullptr;
    HWND motion_ = nullptr;
    HWND listenEffects_ = nullptr;
    HWND listenEffectsStatus_ = nullptr;
    HWND albumAmbient_ = nullptr;
    HWND albumAmbientStatus_ = nullptr;
    HWND keepPlaying_ = nullptr;
    HWND keepMode_ = nullptr;
    HWND lyricsOnline_ = nullptr;
    HWND lyricsOffline_ = nullptr;
    HWND presence_ = nullptr;
    HWND presencePaused_ = nullptr;
    HWND presenceCovers_ = nullptr;
    HWND coverProvider_ = nullptr;
    HWND relayEndpoint_ = nullptr;
    HWND relayCredential_ = nullptr;
    HWND coverDisclosure_ = nullptr;
    HWND imgbbKey_ = nullptr;
    HWND autoCatbox_ = nullptr;
    HWND autoUguu_ = nullptr;
    HWND autoImgBB_ = nullptr;
    HWND autoCatalogue_ = nullptr;
    HWND testCover_ = nullptr;
    HWND retryCover_ = nullptr;
    HWND applicationId_ = nullptr;
    HWND applyPresence_ = nullptr;
    HWND presenceStatus_ = nullptr;
    HWND listen_ = nullptr;
    HWND daw_ = nullptr;
    HFONT font_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    std::wstring applicationIdText_;
};

} // namespace nekorox::component
