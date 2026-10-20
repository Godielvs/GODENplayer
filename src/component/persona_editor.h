#pragma once

#include "nekorox/database.h"

#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace nekorox::component {

class PersonaEditorWindow final {
public:
    static PersonaEditorWindow& instance();

    void show(HWND owner, std::wstring fallbackProfile = {});
    void close();

private:
    enum class Page : unsigned { profile, aliases, relations };

    PersonaEditorWindow() = default;
    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void updateLayout();
    void updateFont();
    void paint();
    void drawItem(const DRAWITEMSTRUCT& item);
    void selectPage(Page page);
    void rebuildRelationshipList();
    void loadRelationshipSelection();
    void applyRelationship();
    void removeRelationship();
    void save();
    void releaseResources();

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND displayName_ = nullptr;
    HWND profile_ = nullptr;
    HWND aliases_ = nullptr;
    HWND relationshipList_ = nullptr;
    HWND target_ = nullptr;
    HWND relation_ = nullptr;
    HWND relationshipNote_ = nullptr;
    HWND applyRelationship_ = nullptr;
    HWND removeRelationship_ = nullptr;
    HWND save_ = nullptr;
    HWND cancel_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    RECT closeRect_{};
    std::array<RECT, 3> tabRects_{};
    bool closePressed_ = false;
    Page page_ = Page::profile;
    int selectedRelationship_ = -1;
    std::string expectedAliasKey_;
    PersistedPersona persona_;
    std::vector<PersistedPersonaRelationship> relationships_;
    std::wstring status_;
};

} // namespace nekorox::component
