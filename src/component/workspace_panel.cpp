#include "qa_hooks.h"
#include "workspace_panel.h"
#include "device_failure.h"

#include "art_cache.h"
#include "artwork_bitmap_key.h"
#include "album_session_service.h"
#include "artwork_transition_renderer.h"
#include "chrome.h"
#include "coverflow_model.h"
#include "database_service.h"
#include "display_environment.h"
#include "file_open_service.h"
#include "frame_clock.h"
#include "glyph_flow_renderer.h"
#include "glyph_flow_service.h"
#include "interaction_latency_guard.h"
#include "keep_playing_service.h"
#include "library_model.h"
#include "listen_window.h"
#include "local_content_cache.h"
#include "lyrics_service.h"
#include "motion_coordinator.h"
#include "note_editor.h"
#include "persona_editor.h"
#include "presentation_revision.h"
#include "player_shell_panel.h"
#include "pixel_ui/pixel_icon_painter.hpp"
#include "render_device_hub.h"
#include "rollout_flags.h"
#include "runtime.h"
#include "text_util.h"
#include "text_layout_cache.h"
#include "async_text_view.h"
#include "theme_service.h"
#include "ui_design_system.h"
#include "ui_invalidation_bus.h"
#include "ui_preferences.h"
#include "workspace_notifications.h"
#include "workspace_retained_scene.h"
#include "workspace_view_model.h"

#include "nekorox/lyrics_timeline.h"
#include "nekorox/qa_contracts.h"
#include "nekorox/scroll_controller.h"
#include "nekorox/track_identity.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dwrite.h>
#include "monitor_text_rendering.h"
#include <dxgi1_2.h>
#include <windowsx.h>
#include <commctrl.h>
#include <oleacc.h>
#include <wrl/client.h>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;

std::atomic<std::uint64_t> gNextWorkspaceSurfaceLifetime{1};

std::wstring lyricsPresenterText(
    const std::shared_ptr<const LyricsSnapshot>& snapshot)
{
    if (!snapshot || !snapshot->document) return {};
    const auto& document = *snapshot->document;
    if ((document.timing == TimingStatus::catalogMatched ||
            document.timing == TimingStatus::audioVerified) &&
        !document.cues.empty()) {
        std::wstring result;
        wchar_t timestamp[32]{};
        for (const auto& cue : document.cues) {
            const auto total = std::max<std::int64_t>(0, cue.startMs);
            const auto minutes = total / 60000;
            const auto seconds = (total / 1000) % 60;
            const auto milliseconds = total % 1000;
            swprintf_s(timestamp, L"[%lld:%02lld.%03lld]",
                minutes, seconds, milliseconds);
            for (const auto& line : cue.simultaneousLines) {
                result += timestamp;
                result += utf8ToWide(line);
                result.push_back(L'\n');
            }
        }
        return result;
    }
    return utf8ToWide(document.plainTextUtf8);
}

UINT libraryCountMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kLibraryCountMessageV1);
    return message;
}

UINT fileOpenStateMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kFileOpenStateMessageV1);
    return message;
}

UINT workspaceStateMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kWorkspaceStateMessageV1);
    return message;
}

UINT workspaceRenderStateMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kWorkspaceRenderStateMessageV1);
    return message;
}

UINT displayEnvironmentChangedMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"Goden.DisplayEnvironmentChanged.V1");
    return message;
}

D2D1_COLOR_F d2dColor(UiColor color) noexcept
{
    return D2D1::ColorF(
        color.red,
        color.green,
        color.blue,
        color.alpha);
}

constexpr UINT kModelChangedMessage = WM_APP + 0x4e0;
constexpr UINT kPlaybackChangedMessage = WM_APP + 0x4e1;
constexpr UINT kRevealNowPlayingMessage = WM_APP + 0x4e2;
constexpr UINT kArtworkReadyMessage = WM_APP + 0x4e3;
constexpr UINT kToggleQueueMessage = WM_APP + 0x4e4;
constexpr UINT kQueueChangedMessage = WM_APP + 0x4e5;
constexpr UINT kOpenPaletteMessage = WM_APP + 0x4e6;
constexpr UINT kHistoryChangedMessage = WM_APP + 0x4e7;
constexpr UINT kShowHistoryMessage = WM_APP + 0x4e8;
constexpr UINT kLocalContentReadyMessage = WM_APP + 0x4e9;
constexpr UINT kWorkspaceFrameMessage = WM_APP + 0x4ea;
constexpr UINT kGlyphFlowChangedMessage = WM_APP + 0x4eb;
constexpr UINT kArtworkInvalidatedMessage = WM_APP + 0x4ec;
constexpr UINT kThemeTransitionMessage = WM_APP + 0x4ed;
constexpr MotionKey kWorkspaceTabMotion = 0x200U;
constexpr MotionKey kWorkspaceQueueMotion = 0x201U;
constexpr UINT_PTR kOverlayScrollbarCollapseTimer = 0x4e52U;
constexpr UINT_PTR kSearchEditSubclass = 0x4e53U;
constexpr int kSearchEditId = 0x4e54;
const float kFacetRowHeight = UiDesignSystemV1::metrics().rowDip;
constexpr float kQueueHeaderHeight = 36.0f;
constexpr float kQueueRowHeight = 40.0f;
constexpr float kHistoryRowHeight = 34.0f;
constexpr float kRatingCellWidth = 14.0f;
constexpr int kRatingCellCount = 5;
constexpr float kRatingWidth = kRatingCellWidth * kRatingCellCount;
constexpr std::size_t kArtworkGpuCacheBudgetBytes =
    48U * 1024U * 1024U;
constexpr std::size_t kArtworkGpuCacheMaximumEntries = 192U;

const GUID kWorkspaceGuid = {
    0x6c52b18d, 0x4664, 0x4e8f, {0x8d, 0xcd, 0xce, 0x2b, 0x82, 0xb7, 0x6b, 0x32}};
const GUID kActiveTabConfigGuid = {
    0x9079689e, 0x89d4, 0x46ef, {0x8a, 0xbc, 0x6b, 0x42, 0xa5, 0xe7, 0xce, 0x13}};

cfg_uint g_activeTab(kActiveTabConfigGuid, 4);

std::mutex g_windowsMutex;
std::vector<HWND> g_windows;

bool sameText(const std::wstring& left, const std::wstring& right) noexcept
{
    return compareText(left, right) == 0;
}

bool pointIn(const D2D1_RECT_F& rectangle, float x, float y) noexcept
{
    return x >= rectangle.left && x < rectangle.right &&
        y >= rectangle.top && y < rectangle.bottom;
}

D2D1_RECT_F ratingCellsRect(const D2D1_RECT_F& rectangle) noexcept
{
    const auto availableWidth = rectangle.right - rectangle.left;
    const auto left = rectangle.left +
        std::max(0.0f, (availableWidth - kRatingWidth) * 0.5f);
    return D2D1::RectF(left, rectangle.top, left + kRatingWidth, rectangle.bottom);
}

std::optional<int> ratingValueAt(
    const D2D1_RECT_F& rectangle,
    float x,
    float y) noexcept
{
    const auto cells = ratingCellsRect(rectangle);
    if (!pointIn(cells, x, y)) return std::nullopt;
    return std::clamp(
        static_cast<int>((x - cells.left) / kRatingCellWidth) + 1,
        1,
        kRatingCellCount);
}

bool asciiEqualsNoCase(const char* left, const char* right) noexcept
{
    return left != nullptr && right != nullptr && _stricmp(left, right) == 0;
}

bool asciiContainsRatingPath(const char* text) noexcept
{
    if (text == nullptr) return false;
    std::string folded(text);
    std::transform(folded.begin(), folded.end(), folded.begin(), [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    return folded.find("rating/") != std::string::npos ||
        folded.find("rating\\") != std::string::npos;
}

bool ratingCommandNameMatches(const char* name, int rating) noexcept
{
    if (name == nullptr) return false;
    if (rating == 0) return asciiEqualsNoCase(name, "<not set>");
    if (rating < 1 || rating > kRatingCellCount) return false;
    const char expected[] = {static_cast<char>('0' + rating), '\0'};
    return asciiEqualsNoCase(name, expected);
}

contextmenu_node* findRatingCommand(
    contextmenu_node* node,
    int rating,
    bool insideRating = false) noexcept
{
    if (node == nullptr) return nullptr;
    const auto* name = node->get_name();
    pfc::string8 fullName;
    const auto hasFullName = node->get_type() == contextmenu_item_node::TYPE_COMMAND &&
        node->get_full_name(fullName);
    const auto ratingScope = insideRating || asciiEqualsNoCase(name, "Rating") ||
        (hasFullName && asciiContainsRatingPath(fullName.c_str()));
    if (node->get_type() == contextmenu_item_node::TYPE_COMMAND) {
        return ratingScope && ratingCommandNameMatches(name, rating)
            ? node
            : nullptr;
    }
    if (node->get_type() != contextmenu_item_node::TYPE_POPUP) return nullptr;
    for (t_size index = 0; index < node->get_num_children(); ++index) {
        if (auto* match = findRatingCommand(
                node->get_child(index), rating, ratingScope)) {
            return match;
        }
    }
    return nullptr;
}

bool setPlaybackStatisticsRating(
    const metadb_handle_ptr& handle,
    int rating) noexcept
{
    if (!handle.is_valid() || rating < 0 || rating > kRatingCellCount) {
        return false;
    }
    try {
        metadb_handle_list items;
        items.add_item(handle);
        auto manager = contextmenu_manager::g_create();
        manager->init_context_ex(
            items,
            contextmenu_manager::flag_view_full,
            contextmenu_item::caller_media_library_viewer);
        auto* command = findRatingCommand(manager->get_root(), rating);
        if (command == nullptr ||
            (command->get_display_flags() &
                contextmenu_item_node::FLAG_DISABLED_GRAYED) != 0) {
            return false;
        }
        return manager->execute_by_id(command->get_id());
    } catch (...) {
        return false;
    }
}

std::wstring foldedText(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool containsFolded(const std::wstring& value, const std::wstring& foldedQuery)
{
    return foldedQuery.empty() || foldedText(value).find(foldedQuery) != std::wstring::npos;
}

double qpcSeconds()
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(counter.QuadPart) /
        static_cast<double>(frequency.QuadPart);
}

std::wstring durationText(double seconds, bool forceHours = false)
{
    const auto total = static_cast<long long>(std::max(0.0, std::round(seconds)));
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto remaining = total % 60;
    wchar_t buffer[40]{};
    if (forceHours && hours >= 24) {
        const auto days = hours / 24;
        std::swprintf(buffer, std::size(buffer), L"%lldd %lld:%02lld:%02lld",
            days, hours % 24, minutes, remaining);
    } else if (forceHours || hours > 0) {
        std::swprintf(buffer, std::size(buffer), L"%lld:%02lld:%02lld", hours, minutes, remaining);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%lld:%02lld", total / 60, remaining);
    }
    return buffer;
}

std::wstring sampleRateText(int sampleRate)
{
    if (sampleRate <= 0) return {};
    wchar_t buffer[24]{};
    const auto kilohertz = static_cast<double>(sampleRate) / 1000.0;
    if (sampleRate % 1000 == 0) {
        std::swprintf(buffer, std::size(buffer), L"%.0f kHz", kilohertz);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%.1f kHz", kilohertz);
    }
    return buffer;
}

std::wstring displaySourcePath(const std::wstring& path)
{
    constexpr std::wstring_view prefix = L"file://";
    if (path.size() >= prefix.size() &&
        CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()),
            prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
        return path.substr(prefix.size());
    }
    return path;
}

std::wstring localTimeText(std::int64_t utcMs)
{
    if (utcMs <= 0) return L"--:--";
    constexpr std::uint64_t kUnixEpochFileTime = 116444736000000000ULL;
    const auto ticks = kUnixEpochFileTime +
        static_cast<std::uint64_t>(utcMs) * 10000ULL;
    FILETIME utc{
        static_cast<DWORD>(ticks & 0xffffffffULL),
        static_cast<DWORD>(ticks >> 32U)};
    FILETIME local{};
    SYSTEMTIME system{};
    if (!FileTimeToLocalFileTime(&utc, &local) ||
        !FileTimeToSystemTime(&local, &system)) {
        return L"--:--";
    }
    wchar_t buffer[16]{};
    std::swprintf(buffer, std::size(buffer), L"%02u:%02u",
        system.wHour, system.wMinute);
    return buffer;
}

void registerWorkspaceWindow(HWND window)
{
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    g_windows.push_back(window);
    UiInvalidationBus::instance().registerWindow(
        UiInvalidationSurface::workspace,
        window,
        kPlaybackChangedMessage);
}

void unregisterWorkspaceWindow(HWND window)
{
    UiInvalidationBus::instance().unregisterWindow(
        UiInvalidationSurface::workspace,
        window);
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    std::erase(g_windows, window);
}

std::vector<HWND> workspaceWindows()
{
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    return g_windows;
}

class QueueObserver : public playback_queue_callback {
public:
    void on_changed(t_change_origin) override
    {
        KeepPlayingService::instance().onQueueChanged();
        AlbumSessionService::instance().onQueueChanged();
        Runtime::instance().notifyQueueChanged();
        notifyWorkspaceQueueChanged();
        notifyNativePanelsPlaybackChanged();
    }
};

FB2K_SERVICE_FACTORY(QueueObserver);

struct Geometry {
    float width = 0.0f;
    float height = 0.0f;
    float tabsHeight = 28.0f;
    float modeHeight = 20.0f;
    float contextHeight = 40.0f;
    float columnsHeight = 20.0f;
    float leftWidth = 0.0f;
    float facetWidth = 0.0f;
    float facetSelectorWidth = 0.0f;
    float resultsTop = 50.0f;
    float facetTop = 109.0f;
    bool compactFacets = false;

    D2D1_RECT_F facetViewport(std::size_t index) const
    {
        const auto left = compactFacets
            ? 0.0f
            : facetWidth * static_cast<float>(index);
        return D2D1::RectF(left, facetTop, left + facetWidth, height);
    }

    D2D1_RECT_F resultViewport() const
    {
        return D2D1::RectF(leftWidth, resultsTop, width, height);
    }
};

Geometry calculateGeometry(float width, float height)
{
    Geometry result;
    result.width = width;
    result.height = height;
    result.leftWidth = std::clamp(width * 0.44f, 300.0f, std::max(300.0f, width - 380.0f));
    result.compactFacets = result.leftWidth < 420.0f;
    result.facetSelectorWidth = result.leftWidth / 3.0f;
    result.facetWidth = result.compactFacets
        ? result.leftWidth
        : result.facetSelectorWidth;
    result.resultsTop = result.tabsHeight + result.modeHeight;
    result.facetTop = result.resultsTop + result.contextHeight + result.columnsHeight;
    return result;
}

enum class FacetKind : std::size_t {
    genre = 0,
    artist = 1,
    album = 2,
};

enum class PostPlaybackAction {
    none,
    revealNowPlaying,
};

} // namespace

class WorkspacePanel::Impl {
public:
    enum class FocusRegion : std::uint8_t {
        none, search, genre, artist, album, results,
    };

    // The edit is a real HWND. These four simple MSAA children describe the
    // remaining panes, whose rows stay in the Direct2D surface.
    class Accessible final : public IAccessible {
    public:
        explicit Accessible(Impl* owner) : owner_(owner) {}
        void detach() noexcept { owner_ = nullptr; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
        {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (id == IID_IUnknown || id == IID_IDispatch || id == IID_IAccessible) {
                *object = static_cast<IAccessible*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto remaining = --references_;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override
        { if (!count) return E_POINTER; *count = 0; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override
        { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID,
            DISPID*) override { return DISP_E_UNKNOWNNAME; }
        HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD,
            DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override
        { return DISP_E_MEMBERNOTFOUND; }
        HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override
        { if (!parent) return E_POINTER; *parent = nullptr; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override
        { if (!count) return E_POINTER; *count = owner_ ? 5 : 0; return S_OK; }
        HRESULT STDMETHODCALLTYPE get_accChild(VARIANT index, IDispatch** child) override
        {
            if (!child) return E_POINTER;
            *child = nullptr;
            if (owner_ && childIndex(index) == 1 && owner_->searchEdit_)
                return AccessibleObjectFromWindow(owner_->searchEdit_,
                    OBJID_CLIENT, IID_IDispatch,
                    reinterpret_cast<void**>(child));
            return S_FALSE;
        }
        HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override
        {
            if (!name) return E_POINTER;
            *name = nullptr;
            const auto index = childIndex(child);
            if (index < 0 || index > 5) return E_INVALIDARG;
            static constexpr const wchar_t* names[]{L"GODENplayer library",
                L"Search library", L"Genre filters", L"Artist filters", L"Album filters",
                L"Library results"};
            const auto label = owner_ && owner_->historyMode_
                ? (index == 1 ? L"Search history"
                    : index == 2 ? L"Period filters"
                    : index == 5 ? L"History results" : names[index])
                : names[index];
            *name = SysAllocString(label);
            return *name ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            const auto index = childIndex(child);
            if (!owner_ || index < 1 || index > 5) return S_FALSE;
            std::wstring summary;
            if (index == 1) {
                summary = owner_->historyMode_ ? owner_->historyQuery_
                    : owner_->query_;
            } else if (index <= 4) {
                const auto& items = owner_->focusedFacetItems(index - 2);
                const auto row = std::min(owner_->focusedFacetRow_[index - 2],
                    items.size());
                summary = row == 0 ? L"All" : items[row - 1].name;
                if (summary.empty()) summary = L"Unknown";
                summary += L", " + std::to_wstring(items.size() + 1U) + L" choices";
            } else {
                const auto count = owner_->historyMode_ ? owner_->historyRows_.size()
                    : owner_->view_.resultTrackCount();
                summary = std::to_wstring(count) + L" results";
                if (owner_->focusRegion_ == FocusRegion::results &&
                        GetFocus() == owner_->window_) {
                    std::wstring focused;
                    if (owner_->historyMode_) {
                        if (owner_->resultFocusRow_ < owner_->historyRows_.size())
                            focused = owner_->historyRows_[owner_->resultFocusRow_].title;
                    } else if (owner_->resultFocusRow_ < owner_->view_.entries().size()) {
                        const auto& entry = owner_->view_.entries()[owner_->resultFocusRow_];
                        if (entry.kind == ResultEntryKind::track) {
                            focused = entry.displayTitle;
                        } else if (entry.groupIndex < owner_->view_.groups().size()) {
                            const auto& group = owner_->view_.groups()[entry.groupIndex];
                            focused = L"Album: " + group.album;
                        }
                    }
                    if (!focused.empty()) summary += L", focused: " + focused;
                }
            }
            *value = SysAllocString(summary.c_str());
            return *value ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child,
            BSTR* description) override
        {
            if (!description) return E_POINTER;
            *description = nullptr;
            const auto index = childIndex(child);
            if (index < 1 || index > 5) return S_FALSE;
            *description = SysAllocString(index == 1
                ? L"Type to filter the library."
                : index == 5
                ? L"Use arrow keys to choose a result; Enter plays it."
                : L"Use arrow keys to choose a filter; Enter applies it.");
            return *description ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override
        {
            if (!role) return E_POINTER;
            const auto index = childIndex(child);
            if (index < 0 || index > 5) return E_INVALIDARG;
            role->vt = VT_I4;
            role->lVal = index == 0 ? ROLE_SYSTEM_PANE
                : index == 1 ? ROLE_SYSTEM_TEXT : ROLE_SYSTEM_LIST;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override
        {
            if (!state) return E_POINTER;
            const auto index = childIndex(child);
            if (index < 0 || index > 5) return E_INVALIDARG;
            state->vt = VT_I4;
            state->lVal = index == 0 ? 0 : STATE_SYSTEM_FOCUSABLE;
            if (owner_ && index > 0 &&
                    (owner_->activeTab_ != 4 ||
                        !IsWindowVisible(owner_->window_)))
                state->lVal |= STATE_SYSTEM_INVISIBLE;
            if (owner_ && index >= 2 && index <= 4 &&
                    owner_->geometry_.compactFacets &&
                    static_cast<int>(owner_->compactFacet_) != index - 2)
                state->lVal |= STATE_SYSTEM_INVISIBLE;
            if (owner_ && index > 0 &&
                    (index == 1 ? owner_->searchEdit_ == GetFocus()
                        : owner_->window_ == GetFocus()) &&
                    owner_->focusRegion_ == static_cast<FocusRegion>(index))
                state->lVal |= STATE_SYSTEM_FOCUSED;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* help) override
        { if (!help) return E_POINTER; *help = nullptr; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT,
            long* topic) override
        { if (file) *file = nullptr; if (topic) *topic = 0; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* shortcut) override
        { if (!shortcut) return E_POINTER; *shortcut = nullptr; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override
        {
            if (!focus) return E_POINTER;
            focus->vt = VT_EMPTY;
            if (!owner_) return S_FALSE;
            if (GetFocus() == owner_->searchEdit_) {
                focus->vt = VT_I4; focus->lVal = 1; return S_OK;
            }
            if (GetFocus() != owner_->window_) return S_FALSE;
            const auto region = static_cast<int>(owner_->focusRegion_);
            if (region < static_cast<int>(FocusRegion::genre)) return S_FALSE;
            focus->vt = VT_I4;
            focus->lVal = region;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override
        { if (!selection) return E_POINTER; selection->vt = VT_EMPTY; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child,
            BSTR* action) override
        {
            if (!action) return E_POINTER;
            *action = nullptr;
            if (childIndex(child) < 1) return S_FALSE;
            *action = SysAllocString(L"Activate selected row");
            return *action ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override
        {
            const auto index = childIndex(child);
            if (!owner_ || owner_->activeTab_ != 4 ||
                    index < 1 || index > 5 ||
                    (flags & SELFLAG_TAKEFOCUS) == 0) return E_INVALIDARG;
            owner_->setFocusRegion(static_cast<FocusRegion>(index));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top,
            long* width, long* height, VARIANT child) override
        {
            if (!left || !top || !width || !height) return E_POINTER;
            const auto index = childIndex(child);
            if (!owner_ || index < 0 || index > 5) return E_INVALIDARG;
            RECT rect{};
            if (index == 0) GetClientRect(owner_->window_, &rect);
            else if (index == 1 && owner_->searchEdit_) {
                GetWindowRect(owner_->searchEdit_, &rect);
                *left = rect.left; *top = rect.top;
                *width = rect.right - rect.left;
                *height = rect.bottom - rect.top;
                return S_OK;
            }
            else {
                const auto dipRect = index == 5
                    ? owner_->geometry_.resultViewport()
                    : owner_->geometry_.facetViewport(index - 2);
                rect.left = static_cast<LONG>(owner_->pixels(dipRect.left));
                rect.top = static_cast<LONG>(owner_->pixels(dipRect.top));
                rect.right = static_cast<LONG>(owner_->pixels(dipRect.right));
                rect.bottom = static_cast<LONG>(owner_->pixels(dipRect.bottom));
            }
            POINT point{rect.left, rect.top};
            ClientToScreen(owner_->window_, &point);
            *left = point.x; *top = point.y;
            *width = rect.right - rect.left;
            *height = rect.bottom - rect.top;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT from,
            VARIANT* target) override
        {
            if (!target) return E_POINTER;
            target->vt = VT_EMPTY;
            auto index = childIndex(from);
            if (index < 0 || index > 5) return E_INVALIDARG;
            if (direction == NAVDIR_FIRSTCHILD && index == 0) index = 1;
            else if (direction == NAVDIR_LASTCHILD && index == 0) index = 5;
            else if (direction == NAVDIR_NEXT && index >= 1 && index < 5) ++index;
            else if (direction == NAVDIR_PREVIOUS && index > 1) --index;
            else return S_FALSE;
            target->vt = VT_I4; target->lVal = index;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accHitTest(long x, long y,
            VARIANT* hit) override
        {
            if (!hit) return E_POINTER;
            hit->vt = VT_EMPTY;
            if (!owner_) return S_FALSE;
            POINT point{x, y};
            ScreenToClient(owner_->window_, &point);
            const auto dx = owner_->dip(static_cast<float>(point.x));
            const auto dy = owner_->dip(static_cast<float>(point.y));
            int index = 0;
            const auto search = owner_->librarySearchRect();
            if (dx < search.right && dy >= search.top && dy < search.bottom)
                index = 1;
            else if (dx >= owner_->geometry_.leftWidth &&
                    dy >= owner_->geometry_.resultsTop) index = 5;
            else if (dx < owner_->geometry_.leftWidth &&
                    dy >= owner_->geometry_.facetTop) index =
                owner_->geometry_.compactFacets
                    ? static_cast<int>(owner_->compactFacet_) + 2
                    : std::min(4, static_cast<int>(dx /
                        owner_->geometry_.facetWidth) + 2);
            hit->vt = VT_I4; hit->lVal = index;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override
        {
            const auto index = childIndex(child);
            if (!owner_ || index < 1 || index > 5) return E_INVALIDARG;
            owner_->setFocusRegion(static_cast<FocusRegion>(index));
            if (index > 1) owner_->handleFocusedLibraryKey(VK_RETURN);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override
        { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override
        { return E_NOTIMPL; }
    private:
        static int childIndex(VARIANT child) noexcept
        { return child.vt == VT_I4 ? static_cast<int>(child.lVal) : -1; }
        std::atomic<ULONG> references_{1};
        Impl* owner_ = nullptr;
    };

    static LRESULT CALLBACK searchEditProc(HWND edit, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_GETDLGCODE)
            return DefSubclassProc(edit, message, wParam, lParam) | DLGC_WANTTAB;
        if (message == WM_KEYDOWN && self) {
            if (wParam == VK_TAB) {
                self->cycleFocus((GetKeyState(VK_SHIFT) & 0x8000) == 0);
                return 0;
            }
            if (wParam == VK_RETURN) {
                if (!self->handleKeyDown(VK_RETURN))
                    self->setFocusRegion(FocusRegion::results);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (GetWindowTextLengthW(edit) != 0) {
                    SetWindowTextW(edit, L"");
                } else {
                    self->setSearchFocused(false);
                }
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                    (wParam == 'K' || wParam == 'Q')) {
                SetFocus(self->window_);
                self->handleKeyDown(wParam);
                return 0;
            }
        }
        if (message == WM_CHAR &&
                (wParam == L'\t' || wParam == L'\r' || wParam == 0x1b ||
                    wParam == 0x0b || wParam == 0x11)) return 0;
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(edit, searchEditProc, subclassId);
        return DefSubclassProc(edit, message, wParam, lParam);
    }

    Impl()
        : artCache_(kArtworkReadyMessage)
        , localContentCache_(kLocalContentReadyMessage)
        , random_(static_cast<unsigned>(
              std::chrono::high_resolution_clock::now().time_since_epoch().count()))
    {
        activeTab_ = std::min<std::uint32_t>(g_activeTab.get_value(), 4);
        motion_.begin(
            kWorkspaceTabMotion,
            MotionToken::tab,
            MotionMode::off,
            static_cast<double>(activeTab_),
            static_cast<double>(activeTab_));
        motion_.begin(
            kWorkspaceQueueMotion,
            MotionToken::drawer,
            MotionMode::off,
            0.0,
            0.0);
    }

    ~Impl()
    {
        detach();
    }

    void attach(HWND window)
    {
        if (window_ == window) return;
        detach();
        window_ = window;
        SetWindowLongPtrW(window_, GWL_EXSTYLE,
            GetWindowLongPtrW(window_, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
        wchar_t parentClass[64]{};
        const auto parent = GetParent(window_);
        if (parent != nullptr) {
            GetClassNameW(parent, parentClass,
                static_cast<int>(std::size(parentClass)));
        }
        libraryOnly_ = std::wcscmp(parentClass, L"GodenPlayerShellV2") == 0;
        if (libraryOnly_) activeTab_ = 4U;
        dpi_ = static_cast<float>(GetDpiForWindow(window_));
        updateGeometryFromClient();
        searchBrush_ = CreateSolidBrush(RGB(12, 13, 13));
        searchEdit_ = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
            core_api::get_my_instance(), nullptr);
        if (searchEdit_) {
            SetWindowSubclass(searchEdit_, searchEditProc,
                kSearchEditSubclass, reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(searchEdit_, EM_LIMITTEXT, 128, 0);
            SendMessageW(searchEdit_, EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM>(L"Search library"));
            updateSearchEdit();
        }
        frameClock_.attach(window_, kWorkspaceFrameMessage);
        frameClock_.setTelemetrySurface(TelemetrySurface::workspace);
        frameClock_.setLatencyCritical(true);
        GlyphFlowService::instance().subscribe(
            window_, kGlyphFlowChangedMessage);
        ThemeService::instance().subscribe(
            window_, kThemeTransitionMessage);
        artCache_.attach(window_);
        localContentCache_.attach(window_);
        registerWorkspaceWindow(window_);
        DragAcceptFiles(window_, TRUE);
        refreshNowPlaying();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void detach()
    {
        resetReaderText();
        if (accessible_) {
            accessible_->detach();
            accessible_->Release();
            accessible_ = nullptr;
        }
        stopAnimation();
        settleScrollControllers();
        lastWheelSeconds_.fill(0.0);
        if (searchEdit_) {
            DestroyWindow(searchEdit_);
            searchEdit_ = nullptr;
        }
        if (searchBrush_) {
            DeleteObject(searchBrush_);
            searchBrush_ = nullptr;
        }
        if (searchFont_) {
            DeleteObject(searchFont_);
            searchFont_ = nullptr;
        }
        if (window_ != nullptr) {
            KillTimer(window_, kOverlayScrollbarCollapseTimer);
            DragAcceptFiles(window_, FALSE);
            GlyphFlowService::instance().unsubscribe(window_);
            ThemeService::instance().unsubscribe(window_);
            unregisterWorkspaceWindow(window_);
        }
        artCache_.detach();
        localContentCache_.detach();
        frameClock_.detach();
        overlayScrollbars_.clear();
        overlayScrollbarDragging_ = false;
        overlayDraggingController_ = nullptr;
        overlayMouseTracking_ = false;
        window_ = nullptr;
        lyricsPresenterSnapshot_.reset();
        lyricsPresenterCache_.clear();
        paletteCacheItems_.clear();
        paletteCacheValid_ = false;
        releaseDeviceResources();
    }

    LRESULT onMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == AsyncTextView::readyMessage()) {
            if (readerDemand_ && DisplayEnvironmentService::instance().presentationVisible(window)) invalidate();
            return 0;
        }
        static const UINT rootVisibility = RegisterWindowMessageW(L"Goden.RootPresentationVisibility.V1");
        if (message == rootVisibility) {
            if (!wParam) suspendReaderText();
            else invalidate();
            return 0;
        }
        if (message == WM_GETOBJECT &&
                static_cast<LONG>(lParam) == OBJID_CLIENT) {
            if (!accessible_) accessible_ = new Accessible(this);
            return LresultFromObject(IID_IAccessible, wParam, accessible_);
        }
        if (message == libraryCountMessage()) {
            return static_cast<LRESULT>(LibraryModel::instance().tracks().size());
        }
        if (message == fileOpenStateMessage()) {
            return static_cast<LRESULT>(fileOpenQaState());
        }
        if (message == workspaceStateMessage()) {
            const auto state = activeTab_ & nekorox::kWorkspaceTabMaskV1;
            return static_cast<LRESULT>(state |
                (historyMode_ ? nekorox::kWorkspaceHistoryBitV1 : 0U));
        }
        if (message == workspaceRenderStateMessage()) {
            if ((wParam == 18 || wParam == 26 || wParam == 27 || wParam == 45 || wParam == 90 || wParam == 91) &&
                    !qa::hooksAllowed()) return E_ACCESSDENIED;
            switch (wParam) {
            case 0: return static_cast<LRESULT>(d2dFactoryResult_);
            case 1: return static_cast<LRESULT>(writeFactoryResult_);
            case 2: return static_cast<LRESULT>(targetResult_);
            case 3: return static_cast<LRESULT>(endDrawResult_);
            case 4: return target_ ? 1 : 0;
            case 5: return normalFormat_ ? 1 : 0;
            case 6: return tabFormat_ ? 1 : 0;
            case 7: return brushes_.text ? 1 : 0;
            case 8: return window_ ? 1 : 0;
            case 9: {
                const auto width = static_cast<std::uint32_t>(
                    std::max(0.0f, geometry_.width));
                const auto height = static_cast<std::uint32_t>(
                    std::max(0.0f, geometry_.height));
                return static_cast<LRESULT>(
                    (static_cast<std::uint64_t>(width) << 32U) | height);
            }
            case 10:
                return target_ ? 0 : -1;
            case 11: {
                const auto size = target_
                    ? target_->GetPixelSize()
                    : D2D1::SizeU(0, 0);
                return static_cast<LRESULT>(
                    (static_cast<std::uint64_t>(size.width) << 32U) |
                    size.height);
            }
            case 12:
                return static_cast<LRESULT>(themeTransitionFrames_);
            case 13:
                return static_cast<LRESULT>(lastThemeTransitionFrames_);
            case 14:
                return themeTransitionCounting_ ? TRUE : FALSE;
            case 15: {
                const auto playlist =
                    playlist_manager::get()->get_active_playlist();
                return playlist == pfc_infinite
                    ? static_cast<LRESULT>(-1)
                    : static_cast<LRESULT>(playlist);
            }
            case 16: {
                t_size playlist = pfc::infinite_size;
                t_size item = pfc::infinite_size;
                return playlist_manager::get()->get_playing_item_location(
                        &playlist, &item)
                    ? static_cast<LRESULT>(playlist)
                    : static_cast<LRESULT>(-1);
            }
            case 17: {
                const auto playlist = playlist_manager::get()->find_playlist(
                    "NekoRoX Selection");
                return playlist == pfc_infinite
                    ? static_cast<LRESULT>(-1)
                    : static_cast<LRESULT>(playlist);
            }
            case 18: {
                auto playlists = playlist_manager::get();
                const auto selection = playlists->find_playlist(
                    "NekoRoX Selection");
                const auto count = playlists->get_playlist_count();
                for (t_size index = 0; index < count; ++index) {
                    if (index == selection) continue;
                    playlists->set_active_playlist(index);
                    return static_cast<LRESULT>(index);
                }
                return static_cast<LRESULT>(-1);
            }
            case 20: {
                LARGE_INTEGER started{};
                if (!QueryPerformanceCounter(&started)) return FALSE;
                scrollQaStartedQpc_ = started.QuadPart;
                scrollQaLatencyMicros_ = 0;
                scrollQaDwmLatencyMicros_ = 0;
                scrollQaArmed_ = true;
                return TRUE;
            }
            case 21:
                return static_cast<LRESULT>(scrollQaLatencyMicros_);
            case 22:
                return static_cast<LRESULT>(scrollQaDwmLatencyMicros_);
            case 23:
                return static_cast<LRESULT>(std::llround(
                    resultScroll_.position() * 1000.0));
            case 24:
                return static_cast<LRESULT>(std::llround(
                    resultScroll_.target() * 1000.0));
            case 25: {
                const auto viewportHeight = std::max(
                    0.0f,
                    geometry_.height - geometry_.resultsTop);
                const auto contentHeight = historyMode_
                    ? historyRows_.size() * kHistoryRowHeight
                    : view_.resultHeight();
                return static_cast<LRESULT>(std::llround(
                    std::max(0.0, contentHeight - viewportHeight) *
                    1000.0));
            }
            case 26: {
                const auto viewportHeight = std::max(
                    0.0f,
                    geometry_.height - geometry_.resultsTop);
                const auto contentHeight = historyMode_
                    ? historyRows_.size() * kHistoryRowHeight
                    : view_.resultHeight();
                resultScroll_.setBounds(
                    0.0,
                    std::max(0.0, contentHeight - viewportHeight));
                resultScroll_.setPosition(
                    static_cast<double>(lParam) / 1000.0);
                invalidate();
                return TRUE;
            }
            case 27:
                ensureView();
                selectFacet(FacetKind::genre, 1U);
                return TRUE;
            case 28:
                return pendingRandomAlbumReveal_.is_valid() ? TRUE : FALSE;
            case 29:
                return static_cast<LRESULT>(randomAlbumRevealAttempts_);
            case 30:
                return static_cast<LRESULT>(randomAlbumRevealSuccesses_);
            case 31:
                return nowPlaying_.is_valid() && selectedTrack_.is_valid() &&
                    nowPlaying_ == selectedTrack_ ? TRUE : FALSE;
            case 32:
                return static_cast<LRESULT>(std::llround(
                    geometry_.facetTop * 1000.0f));
            case 33:
                return static_cast<LRESULT>(std::llround(
                    geometry_.leftWidth * 1000.0f));
            case 34:
                return static_cast<LRESULT>(std::llround(
                    geometry_.resultsTop * 1000.0f));
            case 35:
                return historyMode_ ? TRUE : FALSE;
            case 36:
                return static_cast<LRESULT>(activeTab_);
            case 37:
                return static_cast<LRESULT>(randomAlbumRequests_);
            case 38:
                return static_cast<LRESULT>(playGroupRequests_);
            case 39:
                return static_cast<LRESULT>(randomAlbumRevealSuccesses_);
            case 40:
                return static_cast<LRESULT>(lastDoubleClickRegion_);
            case 41:
                return nowPlayingCenterErrorMilliDip();
            case 42:
                ensureView();
                return temporaryAlbumView_ ? TRUE : FALSE;
            case 43:
                return static_cast<LRESULT>(temporaryTracks_.size());
            case 44:
                return temporaryAlbumView_ && !temporaryTracks_.empty()
                    ? 1
                    : 0;
            case 45:
                return playFirstLibraryAlbumForQa() ? TRUE : FALSE;
            case 46:
                return static_cast<LRESULT>(
                    temporaryAlbumView_
                        ? combinedTracks_.size() - temporaryTracks_.size()
                        : LibraryModel::instance().tracks().size());
            case 47:
                return static_cast<LRESULT>(view_.groups().size());
            case 48:
                return searchFocused_ ? TRUE : FALSE;
            case 49:
            case 50:
            case 51:
                return ratingQaState(wParam, lParam);
            case 52:
                return static_cast<LRESULT>(compactFacet_);
            case 53:
                return geometry_.compactFacets ? TRUE : FALSE;
            case 54:
                return static_cast<LRESULT>(presentationRevision_.desired());
            case 55:
                return static_cast<LRESULT>(presentationRevision_.accepted());
            case 56:
                return libraryStaticCacheValid_ ? TRUE : FALSE;
            case 57:
                return static_cast<LRESULT>(libraryStaticCompactFacet_);
            case 58: {
                const auto index = std::min<std::size_t>(
                    2U, static_cast<std::size_t>(lParam));
                return static_cast<LRESULT>(std::llround(
                    facetScroll_[index].position() * 1000.0));
            }
            case 59: {
                const auto index = std::min<std::size_t>(
                    2U, static_cast<std::size_t>(lParam));
                return static_cast<LRESULT>(std::llround(
                    facetScroll_[index].target() * 1000.0));
            }
            case 60: {
                ensureView();
                const auto index = std::min<std::size_t>(
                    2U, static_cast<std::size_t>(lParam));
                const std::array<std::size_t, 3> counts{
                    view_.genres().size() + 1U,
                    view_.artists().size() + 1U,
                    view_.albums().size() + 1U};
                const auto viewportHeight = std::max(
                    0.0f, geometry_.height - geometry_.facetTop);
                return static_cast<LRESULT>(std::llround(std::max(
                    0.0,
                    static_cast<double>(counts[index]) * kFacetRowHeight -
                        viewportHeight) * 1000.0));
            }
            case 61:
                selectedGenre_.clear();
                selectedArtist_.clear();
                selectedAlbum_.clear();
                query_.clear();
                for (auto& controller : facetScroll_) {
                    controller.setPosition(0.0);
                }
                resultScroll_.setPosition(0.0);
                viewDirty_ = true;
                ensureView();
                invalidate();
                return TRUE;
            case 62:
                return selectedArtist_.empty() ? FALSE : TRUE;
            case 80: return static_cast<LRESULT>(readerText_.stats().status);
            case 81: return static_cast<LRESULT>(readerText_.stats().generation);
            case 82: return static_cast<LRESULT>(readerText_.stats().submissions);
            case 83: return static_cast<LRESULT>(readerText_.stats().publications);
            case 84: return static_cast<LRESULT>(readerText_.stats().layoutsCreated);
            case 85: return readerText_.stats().running ? 1 : 0;
            case 86: return activeTab_ < tabScroll_.size() ? static_cast<LRESULT>(std::llround(tabScroll_[activeTab_].position()*1000)) : 0;
            case 87: return readerFrame_ ? static_cast<LRESULT>(std::llround(readerFrame_->geometry->height*1000)) : 0;
            case 88: return static_cast<LRESULT>(readerBudget_->live.load());
            case 89: return static_cast<LRESULT>(readerBudget_->peak.load());
            case 90:
            case 91: {
                wchar_t enabled[2]{};
                if (GetEnvironmentVariableW(L"GODEN_QA_TEXT_SURFACES", enabled, 2) != 1 || enabled[0] != L'1') return 0;
                libraryOnly_ = false;
                setActiveTab(static_cast<std::uint32_t>(lParam));
                // Deterministic queued-input regression: no WM_PAINT can
                // establish bounds between opening the tab and this wheel.
                if (wParam == 91) handleWheel(MAKEWPARAM(0, -10 * WHEEL_DELTA), 0);
                invalidate();
                return activeTab_;
            }
            case 63: return static_cast<LRESULT>(resultTileBuilds_);
            case 64: return static_cast<LRESULT>(resultTileHits_);
            case 65: return static_cast<LRESULT>(resultDirectFrames_);
            case 66: return static_cast<LRESULT>(facetCacheHits_);
            case 67: return static_cast<LRESULT>(facetCacheBuilds_);
            case 68: return static_cast<LRESULT>(std::llround(maximumTileBuildMs_*1000));
            case 69: return selectedOccurrence_ ? static_cast<LRESULT>(*selectedOccurrence_) : -1;
            case 70: return playingOccurrence_ ? static_cast<LRESULT>(*playingOccurrence_) : -1;
            case 71:
            case 72: {
                ensureView();
                const auto selected = wParam == 71;
                return static_cast<LRESULT>(std::count_if(view_.entries().begin(), view_.entries().end(),
                    [&](const ResultEntry& entry) {
                        return entry.kind == ResultEntryKind::track && (selected
                            ? isSelectedOccurrence(entry.trackIndex) : isPlayingOccurrence(entry.trackIndex));
                    }));
            }
            default: return E_INVALIDARG;
            }
        }
        if (message == displayEnvironmentChangedMessage()) {
            DisplayEnvironmentService::instance().notifyWindowChanged(window);
            adapterCheckRequired_ = true;
            frameClock_.requestImmediate();
            invalidate();
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            attach(window);
            return 0;
        case WM_DROPFILES:
            openDroppedFiles(window, reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_COPYDATA: {
            const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (copy == nullptr ||
                copy->dwData != nekorox::kFileDropCopyDataTagV1 ||
                copy->lpData == nullptr ||
                copy->cbData < sizeof(wchar_t) ||
                copy->cbData % sizeof(wchar_t) != 0) {
                break;
            }
            const auto* text = static_cast<const wchar_t*>(copy->lpData);
            const auto count = copy->cbData / sizeof(wchar_t);
            const auto length = wcsnlen_s(text, count);
            if (length == 0 || length >= count) return FALSE;
            return openDroppedFilePath(
                window, std::wstring_view(text, length)) ? TRUE : FALSE;
        }
        case WM_DESTROY:
            detach();
            return 0;
        case kModelChangedMessage:
            {
                const auto snapshot=LibraryModel::instance().sharedSnapshot();
                const auto temporaryChanged = temporaryAlbumGeneration()!=observedTemporaryAlbumGeneration_;
                if(!snapshot||snapshot->layoutRevision!=observedLayoutRevision_||
                    temporaryChanged)viewDirty_=true;
                metadb_handle_list temporaryItems;
                if (temporaryChanged && temporaryAlbumItems(temporaryItems)) {
                    setActiveTab(4);
                    historyMode_ = false;
                }
            }
            // Presentation-only database notifications do not change library
            // rows. ensureView owns cache invalidation for actual model changes.
            historyDirty_=true;
            invalidate();
            return 0;
        case kPlaybackChangedMessage:
            {
                const auto dirty = UiInvalidationBus::instance().consume(
                    UiInvalidationSurface::workspace,
                    window);
                // A queued bus wake may already have been consumed by the
                // settled notification. Only actual dirty bits need a full
                // metadata refresh. Settled still resolves playlist occurrence
                // and deferred navigation after the new-track callback.
                const auto settledRefresh = wParam == 2 &&
                    (temporaryAlbumView_ || pendingRandomAlbumReveal_.is_valid());
                if (any(dirty & (UiDirty::semantic | UiDirty::artwork)) ||
                        settledRefresh) {
                    refreshNowPlaying();
                }
            }
            if (wParam == 2 && pendingRandomAlbumReveal_.is_valid()) {
                ++randomAlbumRevealAttempts_;
                const auto expected = pendingRandomAlbumReveal_;
                pendingRandomAlbumReveal_.release();
                if (nowPlaying_.is_valid() && nowPlaying_ == expected &&
                    revealNowPlaying(false)) {
                    ++randomAlbumRevealSuccesses_;
                }
            }
            invalidate();
            return 0;
        case kRevealNowPlayingMessage:
            return revealNowPlaying() ? TRUE : FALSE;
        case kArtworkReadyMessage:
            if (activeTab_ == 4 && !historyMode_ && resultScroll_.active()) {
                resultArtworkRefreshPending_ = true;
            } else {
                resultWindowCacheValid_ = false;
                invalidate();
            }
            return 0;
        case kArtworkInvalidatedMessage:
            invalidateCurrentArtwork();
            return 0;
        case kLocalContentReadyMessage:
            invalidate();
            return 0;
        case kToggleQueueMessage:
            setQueueOpen(!queueOpen_);
            return 0;
        case kQueueChangedMessage:
            queueSnapshotDirty_ = true;
            updateScrollBounds();
            invalidate();
            return 0;
        case kOpenPaletteMessage:
            paletteOpen_ = true;
            paletteQuery_.clear();
            paletteSelection_ = 0;
            SetFocus(window_);
            invalidate();
            return 0;
        case kHistoryChangedMessage:
            historyDirty_ = true;
            invalidate();
            return 0;
        case kShowHistoryMessage:
            setActiveTab(4);
            setSearchFocused(false);
            historyMode_ = true;
            syncSearchEdit();
            historyDirty_ = true;
            for (auto& controller : facetScroll_) controller.setPosition(0.0);
            resultScroll_.setPosition(0.0);
            ensureHistoryView();
            invalidate();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SIZE:
            // Geometry participates in every retained layer even when DXGI
            // rounds two adjacent DIP sizes to the same physical buffer.
            // Retire the scene eagerly. The next paint commits the latest
            // pixel size once, outside the synchronous parent SetWindowPos
            // transaction; intermediate WM_SIZE messages can then coalesce.
            libraryStaticCacheValid_ = false;
            resultWindowCacheValid_ = false;
            libraryStaticBitmap_.Reset();
            resultWindowBitmap_.Reset();
            updateGeometryFromClient();
            updateSearchEdit();
            updateScrollBounds();
            invalidate();
            return 0;
        case WM_SHOWWINDOW:
            if (wParam != FALSE) {
                invalidate();
            } else {
                suspendReaderText();
                // Do not retain an armed one-shot readiness epoch for a child
                // that PlayerShell has intentionally hidden. Semantic state
                // remains dirty and the show path above restarts latest-wins.
                frameClock_.stop();
                artCache_.beginVisibleRequests();
                artCache_.endVisibleRequests();
            }
            break;
        case WM_DPICHANGED:
            dpi_ = static_cast<float>(GetDpiForWindow(window));
            releaseDeviceResources();
            updateSearchEdit();
            invalidate();
            return 0;
        case WM_SETFOCUS:
            if (focusRegion_ == FocusRegion::search && searchEdit_)
                SetFocus(searchEdit_);
            return 0;
        case WM_KILLFOCUS:
            invalidate();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kSearchEditId &&
                    reinterpret_cast<HWND>(lParam) == searchEdit_) {
                if (HIWORD(wParam) == EN_CHANGE && !syncingSearchEdit_) {
                    auto& activeQuery = historyMode_ ? historyQuery_ : query_;
                    const auto length = GetWindowTextLengthW(searchEdit_);
                    std::wstring next(static_cast<std::size_t>(length) + 1U, L'\0');
                    GetWindowTextW(searchEdit_, next.data(), length + 1);
                    next.resize(static_cast<std::size_t>(length));
                    if (next.size() > 128) next.resize(128);
                    if (next != activeQuery) {
                        activeQuery = std::move(next);
                        onQueryChanged();
                    }
                } else if (HIWORD(wParam) == EN_SETFOCUS) {
                    focusRegion_ = FocusRegion::search;
                    searchFocused_ = true;
                    libraryStaticCacheValid_ = false;
                    invalidate();
                } else if (HIWORD(wParam) == EN_KILLFOCUS) {
                    searchFocused_ = false;
                    libraryStaticCacheValid_ = false;
                    invalidate();
                }
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lParam) == searchEdit_) {
                SetTextColor(reinterpret_cast<HDC>(wParam), RGB(224, 233, 234));
                SetBkColor(reinterpret_cast<HDC>(wParam), RGB(12, 13, 13));
                return reinterpret_cast<LRESULT>(searchBrush_);
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window_, &point);
                const auto x = dip(static_cast<float>(point.x));
                const auto y = dip(static_cast<float>(point.y));
                if (ratingHitAt(x, y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                if (activeTab_ == 4 && !historyMode_ && pointIn(
                        librarySearchRect(),
                        x,
                        y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                    return TRUE;
                }
            }
            break;
        case WM_PAINT:
            paint();
            return 0;
        case WM_MOUSEWHEEL:
            PerformanceTelemetry::instance().input(
                TelemetrySurface::workspace);
            handleWheel(wParam, lParam);
            return 0;
        case WM_LBUTTONDOWN:
            PerformanceTelemetry::instance().input(
                TelemetrySurface::workspace);
            SetFocus(window_);
            if (beginOverlayScrollbarPointer(lParam)) return 0;
            if (beginCoverflowPointer(lParam)) return 0;
            handleClick(wParam, lParam, false);
            return 0;
        case WM_MOUSEMOVE:
            if (overlayScrollbarDragging_) {
                updateOverlayScrollbarPointer(lParam);
                return 0;
            }
            updateOverlayScrollbarHover(lParam);
            if (coverflowPointerDown_ &&
                (wParam & MK_LBUTTON) != 0) {
                updateCoverflowPointer(lParam);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (overlayScrollbarDragging_) {
                endOverlayScrollbarPointer();
                return 0;
            }
            if (coverflowPointerDown_) {
                endCoverflowPointer(lParam);
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            SetFocus(window_);
            if (handleCoverflowDoubleClick(lParam)) return 0;
            handleClick(wParam, lParam, true);
            return 0;
        case WM_RBUTTONDOWN:
            // Do not leave context-menu routing to DefWindowProc. Workspace is
            // hosted several child windows below the foobar root and Windows
            // is allowed to forward an unhandled WM_CONTEXTMENU to that
            // container instead of back to this panel.
            PerformanceTelemetry::instance().input(
                TelemetrySurface::workspace);
            SetFocus(window_);
            return 0;
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(window_, &point);
            showContextMenuAt(point);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (overlayScrollbarDragging_) cancelOverlayScrollbarPointer();
            if (coverflowPointerDown_) cancelCoverflowPointer();
            break;
        case WM_MOUSELEAVE:
            overlayMouseTracking_ = false;
            leaveOverlayScrollbars();
            return 0;
        case WM_TIMER:
            if (wParam == kOverlayScrollbarCollapseTimer) {
                KillTimer(window_, kOverlayScrollbarCollapseTimer);
                collapseIdleOverlayScrollbars();
                return 0;
            }
            break;
        case WM_CONTEXTMENU: {
            POINT point{};
            if (static_cast<LONG_PTR>(lParam) == -1) {
                GetCursorPos(&point);
            } else {
                point = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            }
            showContextMenuAt(point);
            return 0;
        }
        case WM_KEYDOWN:
            if (handleKeyDown(wParam)) return 0;
            break;
        case WM_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wParam))) return 0;
            break;
        case kWorkspaceFrameMessage:
            if (frameClock_.consume(wParam)) {
                animate();
                // FrameNotification already runs inside WM_PAINT; the normal
                // handler follows this authorized animation update.
                return 0;
            }
            break;
        case kGlyphFlowChangedMessage:
            if (GlyphFlowService::instance().active(motionMode())) {
                startAnimation(qpcSeconds());
            }
            invalidate();
            return 0;
        case kThemeTransitionMessage:
            themeTransitionFrames_ = 0;
            themeTransitionCounting_ = true;
            if (!startAnimation(qpcSeconds())) {
                if (!InteractionLatencyGuard::instance().shouldYieldRender()) {
                    RedrawWindow(
                        window_, nullptr, nullptr,
                        RDW_INVALIDATE | RDW_UPDATENOW);
                } else {
                    invalidate();
                }
            } else {
                invalidate();
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

private:
    struct Brushes {
        ComPtr<ID2D1SolidColorBrush> background;
        ComPtr<ID2D1SolidColorBrush> panel;
        ComPtr<ID2D1SolidColorBrush> row;
        ComPtr<ID2D1SolidColorBrush> alternate;
        ComPtr<ID2D1SolidColorBrush> selected;
        ComPtr<ID2D1SolidColorBrush> border;
        ComPtr<ID2D1SolidColorBrush> text;
        ComPtr<ID2D1SolidColorBrush> muted;
        ComPtr<ID2D1SolidColorBrush> bright;
        ComPtr<ID2D1SolidColorBrush> accent;
        ComPtr<ID2D1SolidColorBrush> secondary;
        ComPtr<ID2D1SolidColorBrush> highlight;
    } brushes_;

    enum class PaletteKind {
        showNowPlaying,
        openListen,
        toggleQueue,
        clearFilters,
        playPause,
        toggleFavorite,
        editNote,
        editPersona,
        track,
    };

    struct PaletteItem {
        PaletteKind kind = PaletteKind::track;
        std::size_t trackIndex = 0;
        std::wstring primary;
        std::wstring secondary;
    };

    struct HistoryRow {
        std::size_t eventIndex = 0;
        std::wstring artist;
        std::wstring album;
        std::wstring title;
    };

    struct ArtBitmapEntry {
        ComPtr<ID2D1Bitmap> bitmap;
        std::size_t bytes = 0;
        std::uint64_t lastUse = 0;
    };

    void invalidate(bool semantic = true)
    {
        presentationRevision_.invalidate();
        if (window_ != nullptr) {
            // A DXGI readiness-backed surface may receive WM_PAINT before a
            // presentation permit is available. If that early paint merely
            // validates the update region, a semantic change can otherwise
            // remain invisible until unrelated animated input wakes the
            // frame clock. Queue the one-shot clock before invalidating so
            // every state mutation owns a later presentation opportunity.
            if (semantic) (void)frameClock_.requestImmediate();
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    float dip(float pixels) const noexcept
    {
        return pixels * 96.0f / std::max(1.0f, dpi_);
    }

    float pixels(float dips) const noexcept
    {
        return dips * std::max(1.0f, dpi_) / 96.0f;
    }

    D2D1_RECT_F librarySearchRect() const noexcept
    {
        const auto contextTop =
            geometry_.tabsHeight + geometry_.modeHeight;
        const auto contextMiddle =
            contextTop + geometry_.contextHeight * 0.5f;
        return D2D1::RectF(
            0.0f,
            contextMiddle,
            geometry_.leftWidth,
            contextTop + geometry_.contextHeight);
    }

    void updateSearchEdit()
    {
        if (!searchEdit_) return;
        if (searchFont_) DeleteObject(searchFont_);
        searchFont_ = CreateFontW(-static_cast<int>(std::lround(pixels(14.0f))),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(searchEdit_, WM_SETFONT,
            reinterpret_cast<WPARAM>(searchFont_), TRUE);
        const auto rectangle = librarySearchRect();
        const auto x = static_cast<int>(std::lround(pixels(rectangle.left + 7.0f)));
        const auto y = static_cast<int>(std::lround(pixels(rectangle.top + 1.0f)));
        const auto width = std::max(1, static_cast<int>(std::lround(
            pixels(rectangle.right - rectangle.left - 14.0f))));
        const auto height = std::max(1, static_cast<int>(std::lround(
            pixels(rectangle.bottom - rectangle.top - 2.0f))));
        SetWindowPos(searchEdit_, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(searchEdit_, activeTab_ == 4 ? SW_SHOWNA : SW_HIDE);
        syncSearchEdit();
    }

    void syncSearchEdit()
    {
        if (!searchEdit_) return;
        SendMessageW(searchEdit_, EM_SETCUEBANNER, FALSE,
            reinterpret_cast<LPARAM>(historyMode_
                ? L"Search history" : L"Search library"));
        const auto& value = historyMode_ ? historyQuery_ : query_;
        std::wstring current(static_cast<std::size_t>(
            GetWindowTextLengthW(searchEdit_)) + 1U, L'\0');
        const auto length = GetWindowTextW(searchEdit_, current.data(),
            static_cast<int>(current.size()));
        current.resize(static_cast<std::size_t>(length));
        if (current == value) return;
        syncingSearchEdit_ = true;
        SetWindowTextW(searchEdit_, value.c_str());
        SendMessageW(searchEdit_, EM_SETSEL, value.size(), value.size());
        syncingSearchEdit_ = false;
    }

    void setSearchFocused(bool focused)
    {
        if (focused) {
            setFocusRegion(FocusRegion::search);
            return;
        }
        searchFocused_ = focused;
        libraryStaticCacheValid_ = false;
        if (focusRegion_ == FocusRegion::search) focusRegion_ = FocusRegion::none;
        if (searchEdit_ && GetFocus() == searchEdit_) SetFocus(window_);
        invalidate();
    }

    void setFocusRegion(FocusRegion region)
    {
        if (activeTab_ != 4 || window_ == nullptr) return;
        if (region >= FocusRegion::genre && region <= FocusRegion::album &&
                geometry_.compactFacets) {
            const auto next = static_cast<FacetKind>(
                static_cast<std::size_t>(region) -
                static_cast<std::size_t>(FocusRegion::genre));
            if (compactFacet_ != next) {
                compactFacet_ = next;
                libraryStaticCacheValid_ = false;
            }
        }
        focusRegion_ = region;
        searchFocused_ = region == FocusRegion::search;
        libraryStaticCacheValid_ = false;
        if (region == FocusRegion::search && searchEdit_)
            SetFocus(searchEdit_);
        else SetFocus(window_);
        if (region >= FocusRegion::genre)
            NotifyWinEvent(EVENT_OBJECT_FOCUS, window_, OBJID_CLIENT,
                static_cast<LONG>(region));
        invalidate();
    }

    void cycleFocus(bool forward)
    {
        if (activeTab_ != 4) return;
        constexpr std::array order{FocusRegion::search, FocusRegion::genre,
            FocusRegion::artist, FocusRegion::album, FocusRegion::results};
        const auto found = std::find(order.begin(), order.end(), focusRegion_);
        const auto current = found == order.end()
            ? (forward ? order.size() - 1U : 0U)
            : static_cast<std::size_t>(found - order.begin());
        setFocusRegion(order[(current + (forward ? 1U : order.size() - 1U)) %
            order.size()]);
    }

    const std::vector<FacetItem>& focusedFacetItems(std::size_t index) const
    {
        if (historyMode_) {
            if (index == 0) return historyPeriods_;
            if (index == 1) return historyArtists_;
            return historyAlbums_;
        }
        if (index == 0) return view_.genres();
        if (index == 1) return view_.artists();
        return view_.albums();
    }

    bool handleFocusedLibraryKey(WPARAM key)
    {
        if (activeTab_ != 4 || focusRegion_ == FocusRegion::none ||
                focusRegion_ == FocusRegion::search) return false;
        if (focusRegion_ >= FocusRegion::genre &&
                focusRegion_ <= FocusRegion::album) {
            const auto index = static_cast<std::size_t>(focusRegion_) -
                static_cast<std::size_t>(FocusRegion::genre);
            const auto count = focusedFacetItems(index).size() + 1U;
            auto& row = focusedFacetRow_[index];
            row = std::min(row, count - 1U);
            if (key == VK_RETURN || key == VK_SPACE) {
                if (historyMode_) selectHistoryFacet(
                    static_cast<FacetKind>(index), row);
                else selectFacet(static_cast<FacetKind>(index), row);
                return true;
            }
            if (key == VK_UP && row > 0) --row;
            else if (key == VK_DOWN && row + 1U < count) ++row;
            else if (key == VK_HOME) row = 0;
            else if (key == VK_END) row = count - 1U;
            else if (key == VK_PRIOR) row = row > 10U ? row - 10U : 0U;
            else if (key == VK_NEXT) row = std::min(count - 1U, row + 10U);
            else return false;
            const auto viewport = geometry_.facetViewport(index);
            const auto top = row * kFacetRowHeight;
            const auto visible = viewport.bottom - viewport.top;
            if (top < facetScroll_[index].position())
                facetScroll_[index].setPosition(top);
            else if (top + kFacetRowHeight >
                    facetScroll_[index].position() + visible)
                facetScroll_[index].setPosition(top + kFacetRowHeight - visible);
            NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, window_, OBJID_CLIENT,
                static_cast<LONG>(focusRegion_));
            invalidate();
            return true;
        }
        const auto count = historyMode_ ? historyRows_.size() : view_.entries().size();
        if (count == 0) return false;
        resultFocusRow_ = std::min(resultFocusRow_, count - 1U);
        if (key == VK_RETURN || key == VK_SPACE) {
            if (historyMode_) playHistoryRow(resultFocusRow_);
            else {
                const auto& entry = view_.entries()[resultFocusRow_];
                playGroup(entry.groupIndex,
                    entry.kind == ResultEntryKind::track
                        ? std::optional<std::size_t>(entry.trackIndex)
                        : std::nullopt);
            }
            return true;
        }
        if (key == VK_UP && resultFocusRow_ > 0) --resultFocusRow_;
        else if (key == VK_DOWN && resultFocusRow_ + 1U < count) ++resultFocusRow_;
        else if (key == VK_HOME) resultFocusRow_ = 0;
        else if (key == VK_END) resultFocusRow_ = count - 1U;
        else if (key == VK_PRIOR) resultFocusRow_ = resultFocusRow_ > 10U
            ? resultFocusRow_ - 10U : 0U;
        else if (key == VK_NEXT) resultFocusRow_ = std::min(count - 1U,
            resultFocusRow_ + 10U);
        else return false;
        if (!historyMode_ && view_.entries()[resultFocusRow_].kind == ResultEntryKind::track) {
            selectTrackOccurrence(view_.entries()[resultFocusRow_].trackIndex);
        }
        const auto viewport = geometry_.resultViewport();
        const auto top = historyMode_
            ? resultFocusRow_ * kHistoryRowHeight
            : view_.entries()[resultFocusRow_].top;
        const auto height = historyMode_ ? kHistoryRowHeight
            : view_.entries()[resultFocusRow_].height;
        const auto visible = viewport.bottom - viewport.top;
        if (top < resultScroll_.position()) resultScroll_.setPosition(top);
        else if (top + height > resultScroll_.position() + visible)
            resultScroll_.setPosition(top + height - visible);
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, window_, OBJID_CLIENT,
            static_cast<LONG>(focusRegion_));
        invalidate();
        return true;
    }

    D2D1_SIZE_F clientSizeDip() const
    {
        RECT rectangle{};
        if (window_ != nullptr) GetClientRect(window_, &rectangle);
        return D2D1::SizeF(
            dip(static_cast<float>(rectangle.right - rectangle.left)),
            dip(static_cast<float>(rectangle.bottom - rectangle.top)));
    }

    void updateGeometryFromClient()
    {
        const auto size = clientSizeDip();
        geometry_ = calculateGeometry(size.width, size.height);
        if (libraryOnly_) {
            geometry_.tabsHeight = 0.0f;
            geometry_.resultsTop = geometry_.modeHeight;
            geometry_.facetTop = geometry_.resultsTop +
                geometry_.contextHeight + geometry_.columnsHeight;
        }
        updateAlbumHeaderDensity();
    }

    void ensureFactories()
    {
        if (!d2dFactory_) {
            d2dFactory_ = RenderDeviceHub::instance().d2dFactory();
            d2dFactoryResult_ = d2dFactory_ ? S_OK : E_FAIL;
        }
        ensurePixelIcons();
        if (!writeFactory_) {
            writeFactory_ = RenderDeviceHub::instance().writeFactory();
            writeFactoryResult_ = writeFactory_ ? S_OK : E_FAIL;
        }
        if (!writeFactory_ || normalFormat_) return;

        createTextFormat(UiFontRole::body, normalFormat_);
        createTextFormat(UiFontRole::information, smallFormat_);
        createTextFormat(UiFontRole::bodyStrong, strongFormat_);
        createTextFormat(UiFontRole::artist, titleFormat_);
        createTextFormat(UiFontRole::artist, heroTitleFormat_);
        createTextFormat(UiFontRole::bodyStrong, tabFormat_);
        createTextFormat(UiFontRole::technical, rightFormat_);
        createTextFormat(UiFontRole::year, rightStrongFormat_);
        createTextFormat(UiFontRole::body, wrapFormat_);
        createTextFormat(UiFontRole::information, wrapSmallFormat_);
        createTextFormat(UiFontRole::album, lyricFormat_);
        if (tabFormat_) tabFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        if (rightFormat_) rightFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (rightStrongFormat_) rightStrongFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        for (auto* format : {wrapFormat_.Get(), wrapSmallFormat_.Get(), lyricFormat_.Get()}) {
            if (format != nullptr) {
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
        if (lyricFormat_) lyricFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void ensurePixelIcons()
    {
        if (!d2dFactory_) return;
        pixelIconPainter_.bindFactory(d2dFactory_.Get());
        if (std::abs(pixelIconDpi_ - dpi_) <= 0.01f &&
                starOutlineIcon_.generation != 0 &&
                starFilledIcon_.generation != 0 &&
                heartOutlineIcon_.generation != 0 &&
                heartFilledIcon_.generation != 0) return;
        auto prepare = [this](goden::pixel::IconId id,
                              goden::pixel::PixelIconPainter::Handle& handle) {
            const auto* glyph = goden::pixel::icon(id, 12);
            if (!glyph) { handle = {}; return; }
            const auto width = static_cast<std::int32_t>(std::lround(
                static_cast<float>(glyph->width) * dpi_ / 96.0f));
            const auto height = static_cast<std::int32_t>(std::lround(
                static_cast<float>(glyph->height) * dpi_ / 96.0f));
            if (FAILED(pixelIconPainter_.prepare(*glyph,
                    std::max<std::int32_t>(glyph->width, width),
                    std::max<std::int32_t>(glyph->height, height), handle))) {
                handle = {};
            }
        };
        prepare(goden::pixel::IconId::StarOutline, starOutlineIcon_);
        prepare(goden::pixel::IconId::StarFilled, starFilledIcon_);
        prepare(goden::pixel::IconId::FavoriteOutline, heartOutlineIcon_);
        prepare(goden::pixel::IconId::FavoriteFilled, heartFilledIcon_);
        pixelIconDpi_ = dpi_;
    }

    void createTextFormat(
        UiFontRole role,
        ComPtr<IDWriteTextFormat>& destination)
    {
        if (!writeFactory_) return;
        const auto& descriptor = UiDesignSystemV1::font(role);
        const auto family = UiDesignSystemV1::resolveFontFamily(
            writeFactory_.Get(),
            descriptor);
        writeFactory_->CreateTextFormat(
            family.data(),
            nullptr,
            descriptor.weight,
            descriptor.style,
            descriptor.stretch,
            descriptor.sizeDip,
            L"en-us",
            destination.ReleaseAndGetAddressOf());
        if (destination) {
            destination->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            destination->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    bool ensureDeviceResources()
    {
        if (!ensureDeviceBackendResources()) return false;
        if (textRendering_.apply(target_.Get(), writeFactory_.Get(), renderMonitor_)) {
            libraryStaticCacheValid_ = false;
            resultWindowCacheValid_ = false;
            for (auto& tile : resultTiles_) tile.index = -1;
            for (auto& facet : facetCaches_) facet.valid = false;
        }
        return true;
    }

    bool ensureDeviceBackendResources()
    {
        ensureFactories();
        if (!d2dFactory_ || !writeFactory_ || window_ == nullptr) return false;
        const auto recoveryEpoch = RenderDeviceHub::instance().recoveryEpoch();
        const auto currentMonitor =
            MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        if (d3dDevice_ &&
            (renderRecoveryEpoch_ != recoveryEpoch ||
             adapterCheckRequired_ || currentMonitor != renderMonitor_)) {
            if (renderRecoveryEpoch_ != recoveryEpoch ||
                !RenderDeviceHub::instance().isDeviceCompatibleWithWindow(
                    d3dDevice_.Get(), window_)) {
                releaseDeviceResources();
            }
        }
        renderMonitor_ = currentMonitor;
        adapterCheckRequired_ = false;
        if (target_ && swapChain_ && swapChainBitmap_) {
            RECT client{};
            GetClientRect(window_, &client);
            const auto width = static_cast<std::uint32_t>(
                std::max<LONG>(0, client.right - client.left));
            const auto height = static_cast<std::uint32_t>(
                std::max<LONG>(0, client.bottom - client.top));
            if (width != 0 && height != 0 &&
                    (width != swapChainWidth_ || height != swapChainHeight_)) {
                // A child can be resized repeatedly before a paint, or its
                // parent can change geometry without another child WM_SIZE.
                // Never draw a new layout into an obsolete DXGI extent.
                resizeTarget();
            }
            if (target_ && swapChain_ && swapChainBitmap_) return true;
        }
        releaseDeviceResources();

        bool softwareRasterizer = false;
        targetResult_ = RenderDeviceHub::instance().acquire(
            window_, d3dDevice_, d3dContext_, softwareRasterizer)
            ? S_OK
            : E_FAIL;
        if (SUCCEEDED(targetResult_))
            renderRecoveryEpoch_ = RenderDeviceHub::instance().recoveryEpoch();
        renderMonitor_ = currentMonitor;
        adapterCheckRequired_ = false;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(targetResult_) ||
            FAILED(d3dDevice_.As(&dxgiDevice)) ||
            FAILED(targetResult_ = d2dFactory_->CreateDevice(
                dxgiDevice.Get(), d2dDevice_.ReleaseAndGetAddressOf())) ||
            FAILED(targetResult_ = d2dDevice_->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                target_.ReleaseAndGetAddressOf()))) {
            releaseDeviceResources();
            return false;
        }

        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(targetResult_ = dxgiDevice->GetAdapter(
                adapter.ReleaseAndGetAddressOf())) ||
            FAILED(targetResult_ = adapter->GetParent(
                __uuidof(IDXGIFactory2),
                reinterpret_cast<void**>(
                    dxgiFactory.ReleaseAndGetAddressOf())))) {
            releaseDeviceResources();
            return false;
        }

        RECT client{};
        GetClientRect(window_, &client);
        swapChainWidth_ = static_cast<std::uint32_t>(
            std::max<LONG>(1, client.right - client.left));
        swapChainHeight_ = static_cast<std::uint32_t>(
            std::max<LONG>(1, client.bottom - client.top));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = swapChainWidth_;
        description.Height = swapChainHeight_;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swapChain;
        if (FAILED(targetResult_ = dxgiFactory->CreateSwapChainForHwnd(
                d3dDevice_.Get(),
                window_,
                &description,
                nullptr,
                nullptr,
                swapChain.ReleaseAndGetAddressOf())) ||
            FAILED(targetResult_ = swapChain.As(&swapChain_)) ||
            FAILED(targetResult_ = swapChain_->SetMaximumFrameLatency(1)) ||
            !bindSwapChainBackBuffer()) {
            releaseDeviceResources();
            return false;
        }
        workspaceSurfaceLifetime_ = gNextWorkspaceSurfaceLifetime.fetch_add(
            1, std::memory_order_relaxed);
        ++workspaceSwapchainEpoch_;
        frameClock_.setReadinessBinding({workspaceSurfaceLifetime_,
            workspaceSwapchainEpoch_,
            swapChain_->GetFrameLatencyWaitableObject()});
        createBrushes();
        return true;
    }

    bool bindSwapChainBackBuffer()
    {
        if (!swapChain_ || !target_) return false;
        ComPtr<IDXGISurface> surface;
        targetResult_ = swapChain_->GetBuffer(
            0,
            __uuidof(IDXGISurface),
            reinterpret_cast<void**>(surface.ReleaseAndGetAddressOf()));
        if (FAILED(targetResult_)) return false;

        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            std::max(1.0f, dpi_),
            std::max(1.0f, dpi_));
        targetResult_ = target_->CreateBitmapFromDxgiSurface(
            surface.Get(),
            &properties,
            swapChainBitmap_.ReleaseAndGetAddressOf());
        if (FAILED(targetResult_)) return false;
        target_->SetTarget(swapChainBitmap_.Get());
        target_->SetDpi(dpi_, dpi_);
        return true;
    }

    void createBrushes()
    {
        if (!target_) return;
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::canvas)),
            brushes_.background);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::surface)),
            brushes_.panel);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::surfaceRaised)),
            brushes_.row);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::canvas)),
            brushes_.alternate);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::surfaceSelected)),
            brushes_.selected);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::separator)),
            brushes_.border);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::text)),
            brushes_.text);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::textMuted)),
            brushes_.muted);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::textBright)),
            brushes_.bright);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::accentPrimary)),
            brushes_.accent);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::accentSecondary)),
            brushes_.secondary);
        createBrush(
            d2dColor(UiDesignSystemV1::color(UiColorToken::focus)),
            brushes_.highlight);
        themeGeneration_ = 0;
        syncThemeBrushes();
    }

    void syncThemeBrushes()
    {
        const auto theme = ThemeService::instance().snapshot();
        if (theme.generation == themeGeneration_) return;
        themeGeneration_ = theme.generation;
        if (themeTransitionCounting_) {
            ++themeTransitionFrames_;
            if (!theme.transitionActive) {
                lastThemeTransitionFrames_ = themeTransitionFrames_;
                themeTransitionCounting_ = false;
            }
        }
        if (brushes_.accent) {
            brushes_.accent->SetColor(D2D1::ColorF(
                theme.accent.red, theme.accent.green, theme.accent.blue));
        }
        if (brushes_.secondary) {
            brushes_.secondary->SetColor(D2D1::ColorF(
                theme.secondary.red,
                theme.secondary.green,
                theme.secondary.blue));
        }
        if (brushes_.highlight) {
            brushes_.highlight->SetColor(D2D1::ColorF(
                theme.highlight.red,
                theme.highlight.green,
                theme.highlight.blue));
        }
        if (brushes_.selected) {
            const auto base = interpolateLinearColor(
                UiDesignSystemV1::color(UiColorToken::surfaceRaised),
                UiDesignSystemV1::color(UiColorToken::surfaceSelected),
                0.72f);
            const UiColor accent{
                theme.accent.red,
                theme.accent.green,
                theme.accent.blue,
                1.0f,
            };
            brushes_.selected->SetColor(d2dColor(
                interpolateLinearColor(base, accent, 0.012f)));
        }
    }

    void updateCurrentTheme()
    {
        if (!surfaceTrack_) return;
        const auto pixels = artCache_.getOrRequest(
            surfaceArtworkIdentity_, surfaceTrack_->handle, 96,
            ArtRequestPriority::visibleMain);
        if (!pixels) {
            if (artCache_.status(
                    surfaceArtworkIdentity_,
                    96) == ArtCacheStatus::missing &&
                ThemeService::instance().resolveWithoutArtwork(
                    surfaceArtworkIdentity_)) {
                startAnimation(qpcSeconds());
                ChromeController::instance().refreshAccent();
                Runtime::instance().refreshPresentationState();
                notifyNativePanelsPlaybackChanged();
            }
            return;
        }
        bool glyphStarted = false;
        if (glyphArtworkIdentity_ != pixels.get() ||
            glyphArtworkKey_ != surfaceArtworkIdentity_) {
            glyphArtworkIdentity_ = pixels.get();
            glyphArtworkKey_ = surfaceArtworkIdentity_;
            glyphStarted = GlyphFlowService::instance().observeArtwork(
                surfaceArtworkIdentity_, *pixels);
        }
        const auto themeStarted = ThemeService::instance().updateFromArtwork(
            surfaceArtworkIdentity_, *pixels);
        if (glyphStarted || themeStarted) {
            startAnimation(qpcSeconds());
        }
        if (themeStarted) {
            ChromeController::instance().refreshAccent();
            Runtime::instance().refreshPresentationState();
            notifyNativePanelsPlaybackChanged();
        }
    }

    void createBrush(
        const D2D1_COLOR_F& color,
        ComPtr<ID2D1SolidColorBrush>& destination)
    {
        target_->CreateSolidColorBrush(color, destination.ReleaseAndGetAddressOf());
    }

    void releaseDeviceResources()
    {
        textRendering_.reset();
        const auto hadDevice = target_ || swapChain_ || d3dDevice_;
        // Drain the frame-latency wait before IDXGISwapChain2 invalidates its
        // borrowed handle. Late callbacks can no longer reference this epoch.
        frameClock_.setReadinessBinding({});
        brushes_ = {};
        artBitmaps_.clear();
        artBitmapBytes_ = 0;
        artBitmapUseCounter_ = 0;
        libraryStaticBitmap_.Reset();
        resultWindowBitmap_.Reset();
        for (auto& tile : resultTiles_) { tile.bitmap.Reset(); tile.index=-1; }
        for (auto& facet : facetCaches_) { facet.bitmap.Reset(); facet.valid=false; }
        libraryStaticCacheValid_ = false;
        resultWindowCacheValid_ = false;
        surfaceArtworkTransition_.discardDeviceResources();
        glyphFlowRenderer_.reset();
        if (target_) target_->SetTarget(nullptr);
        swapChainBitmap_.Reset();
        target_.Reset();
        swapChain_.Reset();
        d2dDevice_.Reset();
        if (d3dContext_) {
            d3dContext_->ClearState();
            d3dContext_->Flush();
        }
        d3dContext_.Reset();
        d3dDevice_.Reset();
        renderRecoveryEpoch_ = 0;
        renderMonitor_ = nullptr;
        adapterCheckRequired_ = true;
        swapChainWidth_ = 0;
        swapChainHeight_ = 0;
        if (hadDevice) presentationRevision_.deviceEpochChanged();
    }

    void resizeTarget()
    {
        if (!swapChain_ || !target_ || window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const auto width = static_cast<std::uint32_t>(
            std::max<LONG>(0, client.right - client.left));
        const auto height = static_cast<std::uint32_t>(
            std::max<LONG>(0, client.bottom - client.top));
        if (width == 0 || height == 0 ||
            (width == swapChainWidth_ && height == swapChainHeight_)) {
            return;
        }

        libraryStaticCacheValid_ = false;
        resultWindowCacheValid_ = false;
        libraryStaticBitmap_.Reset();
        resultWindowBitmap_.Reset();
        target_->SetTarget(nullptr);
        swapChainBitmap_.Reset();
        targetResult_ = swapChain_->ResizeBuffers(
            2,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(targetResult_)) {
            releaseDeviceResources();
            return;
        }
        swapChainWidth_ = width;
        swapChainHeight_ = height;
        if (!bindSwapChainBackBuffer()) {
            releaseDeviceResources();
            return;
        }
        // ResizeBuffers replaces the back buffers, not the swap-chain's
        // frame-latency waitable object. Keep the current readiness epoch:
        // retiring it here can consume the only permit for the first frame
        // at the new size, leaving DWM stretching the last old-size frame.
    }

    void fill(const D2D1_RECT_F& rectangle, ID2D1Brush* brush)
    {
        target_->FillRectangle(rectangle, brush);
    }

    void line(float x1, float y1, float x2, float y2, ID2D1Brush* brush, float width = 1.0f)
    {
        target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, width);
    }

    void text(
        const std::wstring& value,
        const D2D1_RECT_F& rectangle,
        IDWriteTextFormat* format,
        ID2D1Brush* brush)
    {
        if (value.empty() || format == nullptr || brush == nullptr) return;
        target_->DrawTextW(
            value.data(),
            static_cast<UINT32>(value.size()),
            format,
            rectangle,
            brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            format == normalFormat_.Get() || format == smallFormat_.Get() ||
                format == strongFormat_.Get() || format == tabFormat_.Get() ||
                format == rightFormat_.Get() ||
                format == rightStrongFormat_.Get()
            ? DWRITE_MEASURING_MODE_GDI_NATURAL
            : DWRITE_MEASURING_MODE_NATURAL);
    }

    void rating(
        int value,
        const D2D1_RECT_F& rectangle,
        ID2D1Brush* filledBrush,
        ID2D1Brush* emptyBrush)
    {
        if (!target_ || filledBrush == nullptr ||
            emptyBrush == nullptr) {
            return;
        }
        const auto cells = ratingCellsRect(rectangle);
        const auto left = cells.left;
        const auto clampedValue = std::clamp(value, 0, kRatingCellCount);
        const auto height = rectangle.bottom - rectangle.top;
        constexpr float size = 12.0f;

        for (int index = 0; index < kRatingCellCount; ++index) {
            const auto cellLeft = left +
                static_cast<float>(index) * kRatingCellWidth;
            const auto x = cellLeft + (kRatingCellWidth - size) * 0.5f;
            const auto y = rectangle.top + (height - size) * 0.5f;
            const auto handle = index < clampedValue
                ? starFilledIcon_ : starOutlineIcon_;
            const auto oldOpacity = emptyBrush->GetOpacity();
            if (index >= clampedValue) emptyBrush->SetOpacity(0.38f);
            (void)pixelIconPainter_.drawPrepared(
                target_.Get(), handle,
                {x, y, x + size, y + size},
                index < clampedValue ? filledBrush : emptyBrush);
            if (index >= clampedValue) emptyBrush->SetOpacity(oldOpacity);
        }
    }

    struct RatingHit {
        metadb_handle_ptr handle;
        int value = 0;
        std::optional<std::size_t> occurrence;
    };

    struct RatingOverride {
        metadb_handle_ptr handle;
        int value = 0;
        std::uint64_t reconcileAfterGeneration = 0;
    };

    int effectiveRating(const LibraryTrack& track) const noexcept
    {
        const auto match = std::find_if(
            ratingOverrides_.begin(), ratingOverrides_.end(),
            [&](const RatingOverride& item) {
                return item.handle.is_valid() && item.handle == track.handle;
            });
        return match != ratingOverrides_.end() ? match->value : track.rating;
    }

    D2D1_RECT_F trackRatingRect(float y, float height) const noexcept
    {
        return D2D1::RectF(
            geometry_.width - 175.0f,
            y,
            geometry_.width - 92.0f,
            y + height);
    }

    const ResultEntry* firstVisibleTrackEntry()
    {
        ensureView();
        const auto viewportTop = static_cast<double>(resultScroll_.position());
        const auto viewportBottom = viewportTop + std::max(
            0.0f, geometry_.height - geometry_.resultsTop);
        for (const auto& entry : view_.entries()) {
            if (entry.kind == ResultEntryKind::track &&
                entry.top + entry.height > viewportTop &&
                entry.top < viewportBottom) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::optional<RatingHit> ratingHitAt(float x, float y)
    {
        if (activeTab_ == 0 && surfaceTrack_) {
            if (const auto value = ratingValueAt(infoRatingRect_, x, y)) {
                return RatingHit{surfaceTrack_->handle, *value};
            }
            return std::nullopt;
        }
        if (activeTab_ != 4 || historyMode_ ||
            x < geometry_.leftWidth || y < geometry_.resultsTop) {
            return std::nullopt;
        }
        ensureView();
        const auto contentY = y - geometry_.resultsTop + resultScroll_.position();
        const auto* entry = resultEntryAt(contentY);
        const auto& tracks = viewTracks();
        if (entry == nullptr || entry->kind != ResultEntryKind::track ||
            entry->trackIndex >= tracks.size()) {
            return std::nullopt;
        }
        const auto rowY = geometry_.resultsTop +
            static_cast<float>(entry->top - resultScroll_.position());
        if (const auto value = ratingValueAt(
                trackRatingRect(rowY, static_cast<float>(entry->height)), x, y)) {
            return RatingHit{tracks[entry->trackIndex].handle, *value, entry->trackIndex};
        }
        return std::nullopt;
    }

    bool applyRating(const RatingHit& hit)
    {
        auto currentValue = LibraryModel::instance()
            .describeTrack(hit.handle).rating;
        if (const auto override = std::find_if(
                ratingOverrides_.begin(), ratingOverrides_.end(),
                [&](const RatingOverride& item) {
                    return item.handle.is_valid() && item.handle == hit.handle;
                }); override != ratingOverrides_.end()) {
            currentValue = override->value;
        }
        if (surfaceTrack_ && surfaceTrack_->handle == hit.handle) {
            currentValue = surfaceTrack_->rating;
        }
        const auto desired = currentValue == hit.value ? 0 : hit.value;
        if (!setPlaybackStatisticsRating(hit.handle, desired)) {
            if (!ratingUnavailableNotified_) {
                ratingUnavailableNotified_ = true;
                MessageBoxW(
                    GetAncestor(window_, GA_ROOT),
                    L"Ratings require the Playback Statistics component. "
                    L"Install or enable foo_playcount, then restart "
                    L"GODENplayer.",
                    L"Rating unavailable",
                    MB_OK | MB_ICONINFORMATION);
            }
            return false;
        }
        ratingUnavailableNotified_ = false;
        const auto generation = LibraryModel::instance().generation();
        auto override = std::find_if(
            ratingOverrides_.begin(), ratingOverrides_.end(),
            [&](const RatingOverride& item) {
                return item.handle.is_valid() && item.handle == hit.handle;
            });
        if (override == ratingOverrides_.end()) {
            ratingOverrides_.push_back(
                RatingOverride{hit.handle, desired, generation + 1U});
        } else {
            override->value = desired;
            override->reconcileAfterGeneration = generation + 1U;
        }
        if (surfaceTrack_ && surfaceTrack_->handle == hit.handle) {
            surfaceTrack_->rating = desired;
        }
        if (hit.occurrence) selectTrackOccurrence(*hit.occurrence);
        resultWindowCacheValid_ = false;
        LibraryModel::instance().invalidateTrack(hit.handle);
        invalidate();
        return true;
    }

    void reconcileRatingOverrides(std::uint64_t generation)
    {
        if (ratingOverrides_.empty()) return;
        const auto& tracks = viewTracks();
        std::erase_if(ratingOverrides_, [&](const RatingOverride& item) {
            if (generation < item.reconcileAfterGeneration) return false;
            const auto track = std::find_if(
                tracks.begin(), tracks.end(), [&](const LibraryTrack& candidate) {
                    return candidate.handle.is_valid() &&
                        candidate.handle == item.handle;
                });
            return track != tracks.end() && track->rating == item.value;
        });
    }

    LRESULT ratingQaState(WPARAM selector, LPARAM desiredRating)
    {
        const auto* entry = firstVisibleTrackEntry();
        const auto& tracks = viewTracks();
        if (entry == nullptr || entry->trackIndex >= tracks.size()) return -1;
        const auto& track = tracks[entry->trackIndex];
        if (selector == 49) return effectiveRating(track);
        if (selector == 50) {
            return LibraryModel::instance().describeTrack(track.handle).rating;
        }
        if (selector != 51) return E_INVALIDARG;
        const auto value = std::clamp(
            static_cast<int>(desiredRating), 1, kRatingCellCount);
        const auto rowY = geometry_.resultsTop +
            static_cast<float>(entry->top - resultScroll_.position());
        const auto cells = ratingCellsRect(
            trackRatingRect(rowY, static_cast<float>(entry->height)));
        const auto x = static_cast<unsigned>(std::clamp(
            std::lround(cells.left +
                (static_cast<float>(value) - 0.5f) * kRatingCellWidth),
            0L,
            65535L));
        const auto y = static_cast<unsigned>(std::clamp(
            std::lround(rowY + static_cast<float>(entry->height) * 0.5f),
            0L,
            65535L));
        return static_cast<LRESULT>(MAKELONG(x, y));
    }

    void favoriteGlyph(
        bool filled,
        const D2D1_RECT_F& rectangle,
        ID2D1Brush* brush)
    {
        if (!target_ || brush == nullptr) return;
        const auto width = rectangle.right - rectangle.left;
        const auto height = rectangle.bottom - rectangle.top;
        constexpr float size = 12.0f;
        const auto x = rectangle.left + (width - size) * 0.5f;
        const auto y = rectangle.top + (height - size) * 0.5f;
        (void)pixelIconPainter_.drawPrepared(
            target_.Get(),
            filled ? heartFilledIcon_ : heartOutlineIcon_,
            {x, y, x + size, y + size}, brush);
    }

    void paint()
    {
        auto& telemetry = PerformanceTelemetry::instance();
        telemetry.paint(TelemetrySurface::workspace);
        telemetry.frameAttempt(TelemetrySurface::workspace);
        PAINTSTRUCT paintStruct{};
        BeginPaint(window_, &paintStruct);
        const auto prepareSpan = telemetry.beginSpan(
            TelemetrySurface::workspace, 1U);
        if (!ensureDeviceResources()) {
            telemetry.endSpan(TelemetrySurface::workspace, 1U, prepareSpan, 0U);
            telemetry.frameResult(TelemetrySurface::workspace, E_FAIL, false);
            EndPaint(window_, &paintStruct);
            return;
        }

        if (!frameClock_.beginPresentationOpportunity()) {
            telemetry.endSpan(
                TelemetrySurface::workspace, 1U, prepareSpan, 0U);
            EndPaint(window_, &paintStruct);
            return;
        }

        updateGeometryFromClient();
        ensureView();
        updateScrollBounds();
        updateCurrentTheme();
        syncThemeBrushes();
        prepareReaderText();
        prepareOverlayScrollbarGeometry();
        telemetry.inputIncorporated(TelemetrySurface::workspace);
        telemetry.endSpan(TelemetrySurface::workspace, 1U, prepareSpan, 1U);
        const auto drawnFrame = presentationRevision_.capture();

        target_->BeginDraw();
        target_->Clear(d2dColor(
            UiDesignSystemV1::color(UiColorToken::canvas)));
        artCache_.beginVisibleRequests();
        if (activeTab_ == 4 && !historyMode_ &&
            !motion_.needsFrame() && drawLibraryStaticCache()) {
            drawLibraryFacets();
            drawResults();
        } else {
            if (!libraryOnly_) drawTabs();
            if (activeTab_ == 4) drawLibrary();
            else drawInfoSurface();
        }
        if (queueOpen_ || queueProgress() > 0.001) drawQueueDrawer();
        if (paletteOpen_) drawCommandPalette();
        artCache_.endVisibleRequests();
        endDrawResult_ = target_->EndDraw();
        telemetry.endDraw(TelemetrySurface::workspace, endDrawResult_);
        bool presented = false;
        if (confirmedDeviceLost(d3dDevice_.Get(), endDrawResult_)) {
            RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
            releaseDeviceResources();
        } else if (endDrawResult_ == D2DERR_RECREATE_TARGET) {
            releaseDeviceResources();
        } else if (SUCCEEDED(endDrawResult_) && swapChain_) {
            const auto presentResult = swapChain_->Present(
                0, DXGI_PRESENT_DO_NOT_WAIT);
            telemetry.present(
                TelemetrySurface::workspace,
                presentResult,
                presentResult == DXGI_ERROR_WAS_STILL_DRAWING);
            const auto displayed = presentResult == S_OK &&
                DisplayEnvironmentService::instance().presentationVisible(window_);
            telemetry.frameResult(
                TelemetrySurface::workspace, presentResult, displayed);
            presentationRevision_.presented(
                drawnFrame, presentResult, false);
            WindowFrameClock::reportPresentOutcome(
                presentResult == DXGI_ERROR_WAS_STILL_DRAWING);
            if (confirmedDeviceLost(d3dDevice_.Get(), presentResult)) {
                RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
                releaseDeviceResources();
            } else if (presentResult == DXGI_ERROR_WAS_STILL_DRAWING) {
                // The next retained paint uses current state. Never block the
                // UI thread behind an obsolete frame while DWM/GPU is busy.
                presented = false;
            } else {
                presented = presentResult == S_OK;
            }
        }
        else {
            telemetry.frameResult(TelemetrySurface::workspace,
                endDrawResult_, false);
            presentationRevision_.presented(
                drawnFrame, endDrawResult_, false);
        }
        surfaceArtworkTransition_.presented(surfaceArtworkTransition_.pendingPresentation(), presented);
        glyphFlowRenderer_.presented(presented);
        EndPaint(window_, &paintStruct);
        frameClock_.completePresentationOpportunity(
            presentationRevision_.pending() || animationRunning_);
        if (presented) completeScrollQaFrame();
    }

    void completeScrollQaFrame()
    {
        if (!scrollQaArmed_) return;
        LARGE_INTEGER presented{};
        LARGE_INTEGER composited{};
        LARGE_INTEGER frequency{};
        if (QueryPerformanceCounter(&presented) &&
            QueryPerformanceFrequency(&frequency) &&
            frequency.QuadPart > 0 &&
            presented.QuadPart >= scrollQaStartedQpc_) {
            scrollQaLatencyMicros_ = static_cast<std::uint64_t>(
                (presented.QuadPart - scrollQaStartedQpc_) * 1000000LL /
                frequency.QuadPart);
        }
        DwmFlush();
        if (QueryPerformanceCounter(&composited) &&
            frequency.QuadPart > 0 &&
            composited.QuadPart >= scrollQaStartedQpc_) {
            scrollQaDwmLatencyMicros_ = static_cast<std::uint64_t>(
                (composited.QuadPart - scrollQaStartedQpc_) * 1000000LL /
                frequency.QuadPart);
        }
        scrollQaArmed_ = false;
    }

    void setActiveTab(std::uint32_t next)
    {
        if (libraryOnly_) next = 4U;
        next = std::min<std::uint32_t>(next, 4);
        if (next == activeTab_) return;
        resetReaderText();
        const auto animated = motion_.retarget(
            kWorkspaceTabMotion,
            MotionToken::tab,
            motionMode(),
            static_cast<double>(next));
        activeTab_ = next;
        if (activeTab_ != 4) {
            setSearchFocused(false);
            focusRegion_ = FocusRegion::none;
        }
        updateSearchEdit();
        g_activeTab = activeTab_;
        if (animated) startAnimation(qpcSeconds());
    }

    void setQueueOpen(bool open)
    {
        if (open == queueOpen_) return;
        const auto animated = motion_.retarget(
            kWorkspaceQueueMotion,
            MotionToken::drawer,
            motionMode(),
            open ? 1.0 : 0.0);
        queueOpen_ = open;
        if (!queueOpen_) queueSelected_ = pfc::infinite_size;
        updateScrollBounds();
        if (animated) startAnimation(qpcSeconds());
        invalidate();
    }

    double queueProgress() const
    {
        const auto state = motion_.sample(kWorkspaceQueueMotion);
        return std::clamp(
            state.found ? state.value : (queueOpen_ ? 1.0 : 0.0),
            0.0,
            1.0);
    }

    void drawTabs()
    {
        static const std::array<std::wstring, 5> labels{
            L"INF", L"BIO", L"LYR", L"ART", L"LIB"};
        const auto width = geometry_.width / static_cast<float>(labels.size());
        fill(D2D1::RectF(0.0f, 0.0f, geometry_.width, geometry_.tabsHeight), brushes_.panel.Get());
        line(0.0f, geometry_.tabsHeight - 1.0f, geometry_.width, geometry_.tabsHeight - 1.0f, brushes_.border.Get());
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto left = width * static_cast<float>(index);
            const auto right = left + width;
            text(labels[index], D2D1::RectF(left, 0.0f, right, geometry_.tabsHeight),
                tabFormat_.Get(), index == activeTab_ ? brushes_.accent.Get() : brushes_.text.Get());
        }
        const auto tabMotion = motion_.sample(kWorkspaceTabMotion);
        const auto indicatorIndex = std::clamp(
            tabMotion.found
                ? tabMotion.value
                : static_cast<double>(activeTab_),
            0.0,
            static_cast<double>(labels.size() - 1U));
        const auto indicatorLeft =
            width * static_cast<float>(indicatorIndex);
        fill(
            D2D1::RectF(
                indicatorLeft,
                geometry_.tabsHeight - 2.0f,
                indicatorLeft + width,
                geometry_.tabsHeight),
            brushes_.accent.Get());
    }

    void drawLibrary(bool includeResults = true, bool includeFacets = true)
    {
        const auto modeTop = geometry_.tabsHeight;
        const auto modeBottom = modeTop + geometry_.modeHeight;
        fill(D2D1::RectF(0.0f, modeTop, geometry_.width, modeBottom), brushes_.panel.Get());
        line(0.0f, modeBottom - 1.0f, geometry_.width, modeBottom - 1.0f, brushes_.border.Get());
        line(geometry_.leftWidth, modeTop, geometry_.leftWidth, geometry_.height, brushes_.border.Get());

        text(L"FILTER", D2D1::RectF(0.0f, modeTop, geometry_.leftWidth * 0.5f, modeBottom),
            smallFormat_.Get(), historyMode_ ? brushes_.text.Get() : brushes_.accent.Get());
        text(L"HISTORY", D2D1::RectF(geometry_.leftWidth * 0.5f, modeTop,
            geometry_.leftWidth, modeBottom), smallFormat_.Get(),
            historyMode_ ? brushes_.accent.Get() : brushes_.text.Get());

        if (historyMode_) ensureHistoryView();
        const std::wstring resultStatus = historyMode_
            ? L"Session History: " + std::to_wstring(historyRows_.size()) + L" plays"
            : L"Filter Results (Playback): " +
                std::to_wstring(view_.resultTrackCount()) + L" tracks" +
                (temporaryAlbumView_
                    ? L" (" + std::to_wstring(temporaryTracks_.size()) +
                        L" temporary)"
                    : std::wstring{}) +
                L", Length: " + durationText(
                    view_.resultDurationSeconds(), true);
        text(resultStatus, D2D1::RectF(geometry_.leftWidth + 8.0f, modeTop, geometry_.width - 8.0f, modeBottom),
            smallFormat_.Get(), brushes_.bright.Get());

        if (historyMode_) {
            drawHistoryLibraryBody();
            return;
        }

        const auto contextTop = modeBottom;
        const auto contextMiddle = contextTop + geometry_.contextHeight * 0.5f;
        const auto contextBottom = contextTop + geometry_.contextHeight;
        text(L"Genre / Artist / Album", D2D1::RectF(7.0f, contextTop, geometry_.leftWidth - 90.0f, contextMiddle),
            smallFormat_.Get(), brushes_.text.Get());
        const auto libraryCount = LibraryModel::instance().tracks().size();
        text(std::to_wstring(libraryCount) +
                (temporaryAlbumView_
                    ? L" + " + std::to_wstring(temporaryTracks_.size()) + L" temp"
                    : L" tracks"),
            D2D1::RectF(geometry_.leftWidth - 100.0f, contextTop, geometry_.leftWidth - 7.0f, contextMiddle),
            rightFormat_.Get(), brushes_.muted.Get());
        const auto searchRectangle = librarySearchRect();
        if (searchFocused_) {
            fill(searchRectangle, brushes_.selected.Get());
            line(searchRectangle.left, searchRectangle.bottom - 1.5f,
                searchRectangle.right, searchRectangle.bottom - 1.5f,
                brushes_.accent.Get(), 1.5f);
        }
        const auto searchLabel = query_.empty()
            ? (searchFocused_
                ? L"Search: "
                : L"Search library  \u00b7  click and type")
            : L"Search: " + query_;
        if (!searchEdit_) text(searchLabel,
            D2D1::RectF(searchRectangle.left + 7.0f,
                searchRectangle.top, searchRectangle.right - 7.0f,
                searchRectangle.bottom),
            smallFormat_.Get(),
            searchFocused_ || !query_.empty()
                ? brushes_.accent.Get() : brushes_.muted.Get());
        line(0.0f, contextMiddle, geometry_.leftWidth, contextMiddle, brushes_.border.Get());
        line(0.0f, contextBottom, geometry_.leftWidth, contextBottom, brushes_.border.Get());

        const auto columnsBottom = contextBottom + geometry_.columnsHeight;
        static const std::array<std::wstring, 3> columnNames{L"Genre", L"Artist", L"Album"};
        for (std::size_t index = 0; index < 3; ++index) {
            const auto left = geometry_.facetSelectorWidth * static_cast<float>(index);
            const auto right = left + geometry_.facetSelectorWidth;
            const auto activeCompactFacet =
                geometry_.compactFacets &&
                index == static_cast<std::size_t>(compactFacet_);
            text(columnNames[index], D2D1::RectF(left + 4.0f, contextBottom, right - 4.0f, columnsBottom),
                normalFormat_.Get(),
                activeCompactFacet ? brushes_.accent.Get() : brushes_.text.Get());
            if (activeCompactFacet) {
                fill(
                    D2D1::RectF(left, columnsBottom - 2.0f, right, columnsBottom),
                    brushes_.accent.Get());
            }
            if (index > 0) {
                line(
                    left,
                    geometry_.compactFacets ? contextBottom : contextTop,
                    left,
                    geometry_.compactFacets ? columnsBottom : geometry_.height,
                    brushes_.border.Get());
            }
        }
        line(0.0f, columnsBottom, geometry_.leftWidth, columnsBottom, brushes_.border.Get());

        if (includeFacets) drawLibraryFacets();
        if (includeResults) drawResults();
    }

    bool ensureTargetBitmap(
        ComPtr<ID2D1Bitmap1>& bitmap,
        std::uint32_t width,
        std::uint32_t height)
    {
        if (!target_ || width == 0 || height == 0) return false;
        if (bitmap) {
            const auto size = bitmap->GetPixelSize();
            if (size.width == width && size.height == height) return true;
            bitmap.Reset();
        }
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            std::max(1.0f, dpi_),
            std::max(1.0f, dpi_));
        return SUCCEEDED(target_->CreateBitmap(
            D2D1::SizeU(width, height),
            nullptr,
            0,
            properties,
            bitmap.ReleaseAndGetAddressOf())) && bitmap;
    }

    bool drawLibraryStaticCache()
    {
        if (!rolloutFlagEnabled(RolloutFlag::regionRetentionV1)) return false;
        const auto width = static_cast<std::uint32_t>(std::max(
            1.0f, std::ceil(geometry_.width * dpi_ / 96.0f)));
        const auto height = static_cast<std::uint32_t>(std::max(
            1.0f, std::ceil(geometry_.height * dpi_ / 96.0f)));
        const auto bitmapSize = libraryStaticBitmap_
            ? libraryStaticBitmap_->GetPixelSize()
            : D2D1::SizeU(0, 0);
        const auto cacheMatches = libraryStaticCacheValid_ &&
            bitmapSize.width == width && bitmapSize.height == height &&
            libraryStaticViewRevision_ == viewRevision_ &&
            libraryStaticThemeGeneration_ == themeGeneration_ &&
            libraryStaticCompactFacet_ == compactFacet_;
        if (!cacheMatches) {
            std::uint64_t reason = 0;
            if (!libraryStaticCacheValid_) reason |= 1U;
            if (libraryStaticViewRevision_ != viewRevision_) reason |= 2U;
            if (libraryStaticThemeGeneration_ != themeGeneration_) reason |= 4U;
            if (libraryStaticCompactFacet_ != compactFacet_) reason |= 8U;
            PerformanceTelemetry::instance().cacheReason(
                TelemetrySurface::workspace, reason, viewRevision_);
            if (!ensureTargetBitmap(
                    libraryStaticBitmap_, width, height)) {
                libraryStaticCacheValid_ = false;
                return false;
            }

            ComPtr<ID2D1Image> previousTarget;
            target_->GetTarget(previousTarget.ReleaseAndGetAddressOf());
            D2D1_MATRIX_3X2_F previousTransform{};
            target_->GetTransform(&previousTransform);
            target_->SetTarget(libraryStaticBitmap_.Get());
            target_->SetTransform(D2D1::Matrix3x2F::Identity());
            target_->Clear(d2dColor(
                UiDesignSystemV1::color(UiColorToken::canvas)));
            if (!libraryOnly_) drawTabs();
            drawLibrary(false, false);
            target_->SetTarget(previousTarget.Get());
            target_->SetTransform(previousTransform);

            libraryStaticViewRevision_ = viewRevision_;
            libraryStaticThemeGeneration_ = themeGeneration_;
            libraryStaticCompactFacet_ = compactFacet_;
            libraryStaticCacheValid_ = true;
        }

        target_->DrawBitmap(
            libraryStaticBitmap_.Get(),
            D2D1::RectF(0.0f, 0.0f, geometry_.width, geometry_.height),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        return true;
    }

    void drawActiveLibraryFacet()
    {
        switch (compactFacet_) {
        case FacetKind::genre:
            drawFacet(FacetKind::genre, view_.genres(), selectedGenre_);
            break;
        case FacetKind::artist:
            drawFacet(FacetKind::artist, view_.artists(), selectedArtist_);
            break;
        case FacetKind::album:
            drawFacet(FacetKind::album, view_.albums(), selectedAlbum_);
            break;
        }
    }

    void drawLibraryFacets()
    {
        if (geometry_.compactFacets) {
            drawActiveLibraryFacet();
        } else {
            drawFacet(FacetKind::genre, view_.genres(), selectedGenre_);
            drawFacet(FacetKind::artist, view_.artists(), selectedArtist_);
            drawFacet(FacetKind::album, view_.albums(), selectedAlbum_);
        }
    }

    void drawFacet(
        FacetKind kind,
        const std::vector<FacetItem>& items,
        const FacetSelection& selected,
        bool controls = true)
    {
        const auto index = static_cast<std::size_t>(kind);
        const auto viewport = geometry_.facetViewport(index);
        const auto offset = facetScroll_[index].position();
        const auto totalRows = items.size() + 1;
        const auto cached = controls && drawFacetCache(kind, items, selected, viewport, offset);
        target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        if (!cached) fill(viewport, brushes_.background.Get());

        const auto range = retainedVisibleRows(
            offset,
            viewport.bottom - viewport.top,
            kFacetRowHeight,
            totalRows,
            facetScroll_[index].overscanRows(kFacetRowHeight));

        const FacetItem allItem;
        for (std::size_t row = range.first; row < range.last; ++row) {
            const auto y = viewport.top + static_cast<float>(row * kFacetRowHeight - offset);
            const auto rowRect = D2D1::RectF(viewport.left, y, viewport.right, y + kFacetRowHeight);
            const bool isAll = row == 0;
            const auto& item = isAll ? allItem : items[row - 1];
            const bool isSelected = !buildingNeutralFacetCache_ && (isAll ? selected.isAll() :
                (selected.kind == FacetSelectionKind::missing
                    ? item.name.empty()
                    : selected.kind == FacetSelectionKind::value &&
                        sameText(item.name, selected.value)));
            // Retain ordinary rows independently of the animated accent. Paint
            // the selected row opaquely so its text keeps the same antialiasing.
            if (cached && !isSelected) continue;
            if (isSelected) fill(rowRect, brushes_.selected.Get());

            // Borrow resident labels. Only History's All count and missing
            // metadata need a local string that lives through this draw.
            std::wstring fallbackLabel;
            if (isAll && historyMode_) {
                fallbackLabel = L"All (" + std::to_wstring(items.size()) + L")";
            } else if (!isAll && item.name.empty()) {
                fallbackLabel = kind == FacetKind::genre ? L"No genre"
                    : kind == FacetKind::artist ? L"Unknown artist" : L"Unknown album";
            }
            const auto& label = isAll
                ? (historyMode_ ? fallbackLabel : facetAllLabels_[index])
                : (item.name.empty() ? fallbackLabel : item.name);
            if (!historyMode_) {
                const auto baseBlock = 0x200000000ULL +
                    (static_cast<std::uint64_t>(index) << 32U) +
                    static_cast<std::uint64_t>(row) * 2U;
                const auto labelLayout = retainedText(
                    viewRevision_, baseBlock, label, normalFormat_.Get(),
                    std::max(1.0f, viewport.right - viewport.left - 45.0f),
                    kFacetRowHeight);
                drawRetainedText(labelLayout,
                    D2D1::RectF(viewport.left + 7.0f, y,
                        viewport.right - 38.0f, y + kFacetRowHeight),
                    isSelected ? brushes_.accent.Get() : brushes_.text.Get(), y);
                if (!isAll) {
                    const auto countLayout = retainedText(
                        viewRevision_, baseBlock + 1U, item.countText,
                        rightFormat_.Get(), 32.0f, kFacetRowHeight);
                    drawRetainedText(countLayout,
                        D2D1::RectF(viewport.right - 38.0f, y,
                            viewport.right - 6.0f, y + kFacetRowHeight),
                        brushes_.muted.Get(), y);
                }
            } else {
                text(label, D2D1::RectF(viewport.left + 7.0f, y,
                    viewport.right - 38.0f, y + kFacetRowHeight),
                    normalFormat_.Get(),
                    isSelected ? brushes_.accent.Get() : brushes_.text.Get());
                if (!isAll) {
                    text(std::to_wstring(item.trackCount),
                        D2D1::RectF(viewport.right - 38.0f, y,
                            viewport.right - 6.0f, y + kFacetRowHeight),
                        rightFormat_.Get(), brushes_.muted.Get());
                }
            }
        }
        target_->PopAxisAlignedClip();
        if (!controls) return;
        if (focusRegion_ == static_cast<FocusRegion>(
                static_cast<std::size_t>(FocusRegion::genre) + index) &&
                GetFocus() == window_ && focusedFacetRow_[index] < totalRows) {
            const auto y = viewport.top + static_cast<float>(
                focusedFacetRow_[index] * kFacetRowHeight - offset);
            const auto top = std::max(viewport.top, y + 1.0f);
            const auto bottom = std::min(viewport.bottom,
                y + kFacetRowHeight - 1.0f);
            if (top < bottom) target_->DrawRectangle(
                D2D1::RectF(viewport.left + 2.0f, top,
                    viewport.right - 2.0f, bottom),
                brushes_.accent.Get(), 1.5f);
        }
        drawScrollIndicator(viewport, totalRows * kFacetRowHeight,
            facetScroll_[index].position(), &facetScroll_[index]);
    }

    bool drawFacetCache(FacetKind kind, const std::vector<FacetItem>& items,
        const FacetSelection& selected, D2D1_RECT_F viewport, double offset)
    {
        const auto index=static_cast<std::size_t>(kind);
        if(historyMode_ || facetScroll_[index].active() ||
            !rolloutFlagEnabled(RolloutFlag::regionRetentionV1)) return false;
        const auto scale=std::max(1.f,dpi_)/96.f;
        const auto w=static_cast<unsigned>(std::max(1.f,std::ceil((viewport.right-viewport.left)*scale)));
        const auto h=static_cast<unsigned>(std::max(1.f,std::ceil((viewport.bottom-viewport.top)*scale)));
        if(std::uint64_t(w)*h*4 > 8U*1024U*1024U/3U) return false;
        auto& cache=facetCaches_[index];
        const auto size=cache.bitmap?cache.bitmap->GetPixelSize():D2D1::SizeU();
        if(!cache.valid || cache.view!=viewRevision_ ||
            cache.offset!=offset || size.width!=w || size.height!=h ||
            cache.left!=viewport.left || cache.top!=viewport.top || cache.dpi!=dpi_) {
            if(!ensureTargetBitmap(cache.bitmap,w,h)) { cache.valid=false; return false; }
            ComPtr<ID2D1Image> previous;target_->GetTarget(previous.GetAddressOf());
            D2D1_MATRIX_3X2_F transform{};target_->GetTransform(&transform);
            target_->SetTarget(cache.bitmap.Get());
            target_->SetTransform(D2D1::Matrix3x2F::Translation(-viewport.left,-viewport.top));
            target_->Clear(d2dColor(UiDesignSystemV1::color(UiColorToken::canvas)));
            buildingNeutralFacetCache_=true;
            drawFacet(kind,items,selected,false);
            buildingNeutralFacetCache_=false;
            target_->SetTarget(previous.Get());target_->SetTransform(transform);
            cache.valid=true;cache.view=viewRevision_;cache.theme=themeGeneration_;
            cache.offset=offset;cache.selection=selected;cache.left=viewport.left;cache.top=viewport.top;cache.dpi=dpi_;
            ++facetCacheBuilds_;
        } else ++facetCacheHits_;
        // Use exact physical bitmap extents, clipped to the pane. Ceil rounding
        // must not rescale retained text at fractional DPI/column widths.
        target_->PushAxisAlignedClip(viewport,D2D1_ANTIALIAS_MODE_ALIASED);
        target_->DrawBitmap(cache.bitmap.Get(),D2D1::RectF(viewport.left,viewport.top,
            viewport.left+w/scale,viewport.top+h/scale),1,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        target_->PopAxisAlignedClip();
        return true;
    }

    void drawFacet(
        FacetKind kind,
        const std::vector<FacetItem>& items,
        const std::wstring& selected)
    {
        const auto selection = selected.empty()
            ? FacetSelection{}
            : FacetSelection::fromValue(selected);
        drawFacet(kind, items, selection);
    }

    void drawHistoryLibraryBody()
    {
        const auto contextTop = geometry_.resultsTop;
        const auto contextMiddle = contextTop + geometry_.contextHeight * 0.5f;
        const auto contextBottom = contextTop + geometry_.contextHeight;
        text(L"Period / Artist / Album",
            D2D1::RectF(7.0f, contextTop, geometry_.leftWidth - 90.0f, contextMiddle),
            smallFormat_.Get(), brushes_.text.Get());
        const auto eventCount = historySnapshot_ ? historySnapshot_->events.size() : 0;
        text(std::to_wstring(eventCount) + L" events",
            D2D1::RectF(geometry_.leftWidth - 100.0f, contextTop,
                geometry_.leftWidth - 7.0f, contextMiddle),
            rightFormat_.Get(), brushes_.muted.Get());
        const auto searchLabel = historyQuery_.empty()
            ? L"Search history" : L"Search: " + historyQuery_;
        if (!searchEdit_) text(searchLabel,
            D2D1::RectF(7.0f, contextMiddle, geometry_.leftWidth - 7.0f, contextBottom),
            smallFormat_.Get(), historyQuery_.empty()
                ? brushes_.muted.Get() : brushes_.accent.Get());
        line(0.0f, contextMiddle, geometry_.leftWidth, contextMiddle, brushes_.border.Get());
        line(0.0f, contextBottom, geometry_.leftWidth, contextBottom, brushes_.border.Get());

        const auto columnsBottom = contextBottom + geometry_.columnsHeight;
        static const std::array<std::wstring, 3> columnNames{
            L"Period", L"Artist", L"Album"};
        for (std::size_t index = 0; index < columnNames.size(); ++index) {
            const auto left = geometry_.facetSelectorWidth * static_cast<float>(index);
            const auto right = left + geometry_.facetSelectorWidth;
            const auto activeCompactFacet =
                geometry_.compactFacets &&
                index == static_cast<std::size_t>(compactFacet_);
            text(columnNames[index],
                D2D1::RectF(left + 4.0f, contextBottom, right - 4.0f, columnsBottom),
                normalFormat_.Get(),
                activeCompactFacet ? brushes_.accent.Get() : brushes_.text.Get());
            if (activeCompactFacet) {
                fill(
                    D2D1::RectF(left, columnsBottom - 2.0f, right, columnsBottom),
                    brushes_.accent.Get());
            }
            if (index > 0) {
                line(
                    left,
                    geometry_.compactFacets ? contextBottom : contextTop,
                    left,
                    geometry_.compactFacets ? columnsBottom : geometry_.height,
                    brushes_.border.Get());
            }
        }
        line(0.0f, columnsBottom, geometry_.leftWidth, columnsBottom, brushes_.border.Get());

        if (geometry_.compactFacets) {
            switch (compactFacet_) {
            case FacetKind::genre:
                drawFacet(FacetKind::genre, historyPeriods_, selectedHistoryPeriod_);
                break;
            case FacetKind::artist:
                drawFacet(FacetKind::artist, historyArtists_, selectedHistoryArtist_);
                break;
            case FacetKind::album:
                drawFacet(FacetKind::album, historyAlbums_, selectedHistoryAlbum_);
                break;
            }
        } else {
            drawFacet(FacetKind::genre, historyPeriods_, selectedHistoryPeriod_);
            drawFacet(FacetKind::artist, historyArtists_, selectedHistoryArtist_);
            drawFacet(FacetKind::album, historyAlbums_, selectedHistoryAlbum_);
        }
        drawHistoryResults();
    }

    void drawHistoryResults()
    {
        const auto viewport = geometry_.resultViewport();
        target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        fill(viewport, brushes_.background.Get());

        if (!historySnapshot_ || !historySnapshot_->available) {
            const auto message = historySnapshot_ && !historySnapshot_->error.empty()
                ? L"History unavailable" : L"Opening session history";
            text(message, D2D1::RectF(viewport.left + 14.0f, viewport.top + 12.0f,
                viewport.right - 14.0f, viewport.top + 42.0f),
                normalFormat_.Get(), brushes_.muted.Get());
        } else if (historyRows_.empty()) {
            text(L"No plays in this session",
                D2D1::RectF(viewport.left + 14.0f, viewport.top + 12.0f,
                    viewport.right - 14.0f, viewport.top + 42.0f),
                normalFormat_.Get(), brushes_.muted.Get());
        }

        const auto offset = resultScroll_.position();
        const auto first = static_cast<std::size_t>(std::max(
            0.0, std::floor(offset / kHistoryRowHeight)));
        const auto visible = static_cast<std::size_t>(std::ceil(
            (viewport.bottom - viewport.top) / kHistoryRowHeight)) + 3;
        const auto last = std::min(historyRows_.size(), first + visible);
        for (std::size_t rowIndex = first; rowIndex < last; ++rowIndex) {
            const auto& row = historyRows_[rowIndex];
            if (!historySnapshot_ || row.eventIndex >= historySnapshot_->events.size()) continue;
            const auto& event = historySnapshot_->events[row.eventIndex];
            const auto y = viewport.top +
                static_cast<float>(rowIndex * kHistoryRowHeight - offset);
            const auto rectangle = D2D1::RectF(
                viewport.left, y, viewport.right, y + kHistoryRowHeight);
            if (rowIndex == selectedHistoryRow_) fill(rectangle, brushes_.selected.Get());
            else if ((rowIndex & 1U) != 0) fill(rectangle, brushes_.alternate.Get());
            line(viewport.left, rectangle.bottom - 1.0f, viewport.right,
                rectangle.bottom - 1.0f, brushes_.border.Get());

            text(localTimeText(event.utcMs),
                D2D1::RectF(viewport.left + 10.0f, y, viewport.left + 58.0f,
                    y + kHistoryRowHeight),
                smallFormat_.Get(), brushes_.muted.Get());
            text(row.title,
                D2D1::RectF(viewport.left + 58.0f, y, viewport.right - 72.0f,
                    y + kHistoryRowHeight * 0.55f),
                normalFormat_.Get(), rowIndex == selectedHistoryRow_
                    ? brushes_.accent.Get() : brushes_.text.Get());
            text(row.artist + L"  " + row.album,
                D2D1::RectF(viewport.left + 58.0f, y + kHistoryRowHeight * 0.46f,
                    viewport.right - 72.0f, y + kHistoryRowHeight),
                smallFormat_.Get(), brushes_.muted.Get());
            text(durationText(static_cast<double>(event.durationMs) / 1000.0),
                D2D1::RectF(viewport.right - 68.0f, y, viewport.right - 8.0f,
                    y + kHistoryRowHeight),
                rightFormat_.Get(), brushes_.muted.Get());
        }
        target_->PopAxisAlignedClip();
        if (focusRegion_ == FocusRegion::results && GetFocus() == window_ &&
                resultFocusRow_ < historyRows_.size()) {
            const auto y = viewport.top + static_cast<float>(
                resultFocusRow_ * kHistoryRowHeight - resultScroll_.position());
            const auto top = std::max(viewport.top, y + 1.0f);
            const auto bottom = std::min(viewport.bottom,
                y + kHistoryRowHeight - 1.0f);
            if (top < bottom) {
                target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
                fill(D2D1::RectF(viewport.left + 2.0f, top,
                    viewport.left + 6.0f, bottom), brushes_.accent.Get());
                target_->DrawRectangle(
                    D2D1::RectF(viewport.left + 2.0f, top,
                        viewport.right - 2.0f, bottom),
                    brushes_.accent.Get(), 1.5f);
                target_->PopAxisAlignedClip();
            }
        }
        drawScrollIndicator(viewport,
            historyRows_.size() * kHistoryRowHeight,
            resultScroll_.position(), &resultScroll_);
    }

    void ensureHistoryView()
    {
        const auto snapshot = DatabaseService::instance().historySnapshot();
        if (!historyDirty_ && historySnapshot_ && snapshot &&
            historySnapshot_->generation == snapshot->generation) {
            return;
        }
        historySnapshot_ = snapshot;
        historyPeriods_.clear();
        historyArtists_.clear();
        historyAlbums_.clear();
        historyRows_.clear();

        std::map<std::wstring, std::size_t> artistCounts;
        std::map<std::wstring, std::size_t> albumCounts;
        const auto foldedQuery = foldedText(trimText(historyQuery_));
        std::size_t playCount = 0;
        if (historySnapshot_) {
            for (const auto& event : historySnapshot_->events) {
                if (event.type != PlayEventType::start) continue;
                const auto artist = utf8ToWide(event.artist);
                const auto album = utf8ToWide(event.album);
                const auto title = utf8ToWide(event.title);
                if (!containsFolded(artist, foldedQuery) &&
                    !containsFolded(album, foldedQuery) &&
                    !containsFolded(title, foldedQuery)) {
                    continue;
                }
                ++playCount;
                ++artistCounts[artist];
            }
        }
        historyPeriods_.push_back({L"Current session", playCount});
        for (const auto& [artist, count] : artistCounts) {
            historyArtists_.push_back({artist, count});
        }
        const auto hasFacet = [](const std::vector<FacetItem>& items,
                                  const std::wstring& selected) {
            return selected.empty() || std::any_of(items.begin(), items.end(),
                [&](const FacetItem& item) { return sameText(item.name, selected); });
        };
        if (!hasFacet(historyArtists_, selectedHistoryArtist_)) {
            selectedHistoryArtist_.clear();
        }

        if (historySnapshot_) {
            for (const auto& event : historySnapshot_->events) {
                if (event.type != PlayEventType::start) continue;
                const auto artist = utf8ToWide(event.artist);
                const auto album = utf8ToWide(event.album);
                const auto title = utf8ToWide(event.title);
                if ((!selectedHistoryArtist_.empty() &&
                        !sameText(artist, selectedHistoryArtist_)) ||
                    (!containsFolded(artist, foldedQuery) &&
                        !containsFolded(album, foldedQuery) &&
                        !containsFolded(title, foldedQuery))) {
                    continue;
                }
                ++albumCounts[album];
            }
        }
        for (const auto& [album, count] : albumCounts) {
            historyAlbums_.push_back({album, count});
        }
        if (!hasFacet(historyAlbums_, selectedHistoryAlbum_)) {
            selectedHistoryAlbum_.clear();
        }

        if (historySnapshot_) {
            for (std::size_t offset = historySnapshot_->events.size(); offset > 0; --offset) {
                const auto index = offset - 1;
                const auto& event = historySnapshot_->events[index];
                if (event.type != PlayEventType::start) continue;
                HistoryRow row;
                row.eventIndex = index;
                row.artist = utf8ToWide(event.artist);
                row.album = utf8ToWide(event.album);
                row.title = utf8ToWide(event.title);
                if ((!selectedHistoryArtist_.empty() &&
                        !sameText(row.artist, selectedHistoryArtist_)) ||
                    (!selectedHistoryAlbum_.empty() &&
                        !sameText(row.album, selectedHistoryAlbum_)) ||
                    (!containsFolded(row.artist, foldedQuery) &&
                        !containsFolded(row.album, foldedQuery) &&
                        !containsFolded(row.title, foldedQuery))) {
                    continue;
                }
                historyRows_.push_back(std::move(row));
            }
        }
        if (selectedHistoryRow_ >= historyRows_.size()) {
            selectedHistoryRow_ = std::numeric_limits<std::size_t>::max();
        }
        historyDirty_ = false;
        updateScrollBounds();
    }

    void drawResults()
    {
        const auto viewport = geometry_.resultViewport();
        const auto offset = resultScroll_.position();
        // Build a retained target before installing the owner viewport clip.
        // D2D clips belong to the current target coordinate space and must not
        // leak across SetTarget into the cache bitmap.
        const auto cacheReady = drawResultTiles(viewport, offset, false);
        target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        const auto usedCache = cacheReady &&
            drawResultTiles(viewport, offset, true);
        if (!usedCache) {
            ++resultDirectFrames_;
            fill(viewport, brushes_.background.Get());
            drawResultEntries(
                offset,
                offset + (viewport.bottom - viewport.top),
                viewport.top - static_cast<float>(offset));
        } else {
            // Text and separators intentionally use a nearest, pixel-snapped
            // retained layer. Repaint only the tiny album rectangles directly
            // so artwork never inherits that cache's point-sampling policy.
            const auto scale = std::max(1.f, dpi_) / 96.f;
            const auto snapped = std::round(offset * scale) / scale;
            drawResultArtworkOverlays(
                snapped,
                snapped + (viewport.bottom - viewport.top),
                viewport.top - static_cast<float>(snapped));
        }
        target_->PopAxisAlignedClip();
        if (focusRegion_ == FocusRegion::results && GetFocus() == window_ &&
                resultFocusRow_ < view_.entries().size()) {
            const auto& entry = view_.entries()[resultFocusRow_];
            const auto y = viewport.top + static_cast<float>(
                entry.top - resultScroll_.position());
            const auto top = std::max(viewport.top, y + 1.0f);
            const auto bottom = std::min(viewport.bottom,
                y + static_cast<float>(entry.height) - 1.0f);
            if (top < bottom) {
                target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
                fill(D2D1::RectF(viewport.left + 2.0f, top,
                    viewport.left + 6.0f, bottom), brushes_.accent.Get());
                target_->DrawRectangle(
                    D2D1::RectF(viewport.left + 2.0f, top,
                        viewport.right - 2.0f, bottom),
                    brushes_.accent.Get(), 1.5f);
                target_->PopAxisAlignedClip();
            }
        }
        drawScrollIndicator(viewport, view_.resultHeight(),
            resultScroll_.position(), &resultScroll_);
    }

    bool drawResultTiles(D2D1_RECT_F viewport, double offset, bool drawPrepared)
    {
        if(!rolloutFlagEnabled(RolloutFlag::regionRetentionV1) || !target_)return false;
        constexpr unsigned tilePixels=256;
        constexpr std::uint64_t budget=24U*1024U*1024U;
        const auto scale=std::max(1.f,dpi_)/96.f;
        const auto width=static_cast<unsigned>(std::max(1.f,std::ceil((viewport.right-viewport.left)*scale)));
        const auto bytes=std::uint64_t(width)*tilePixels*4;
        const auto capacity=static_cast<unsigned>(std::min<std::uint64_t>(resultTiles_.size(),budget/bytes));
        const double tileHeight=tilePixels/scale;
        const auto snapped=std::round(offset*scale)/scale;
        const double height=viewport.bottom-viewport.top;
        const auto first=static_cast<std::int64_t>(std::floor(snapped/tileHeight));
        const auto last=static_cast<std::int64_t>(std::ceil((snapped+height)/tileHeight))-1;
        if(height<=0 || last-first+1>capacity || !capacity)return false;
        const auto changed=!resultWindowCacheValid_ || resultWindowViewRevision_!=viewRevision_ ||
            resultTileWidth_!=width || resultTileDpi_!=dpi_ || resultTileLeft_!=viewport.left;
        if(changed){
            for(auto& tile:resultTiles_){
                tile.index=-1;
                if(resultTileWidth_!=width || resultTileDpi_!=dpi_)tile.bitmap.Reset();
            }
            // Release surplus allocations when DPI/pane width reduces capacity.
            for(unsigned i=capacity;i<resultTiles_.size();++i)resultTiles_[i].bitmap.Reset();
            resultWindowViewRevision_=viewRevision_;resultWindowThemeGeneration_=themeGeneration_;
            resultWindowSelected_=selectedTrack_;resultWindowPlaying_=nowPlaying_;
            resultTileWidth_=width;resultTileDpi_=dpi_;resultTileLeft_=viewport.left;resultWindowCacheValid_=true;
        }
        for(auto index=first;index<=last;++index){
            auto& tile=resultTiles_[static_cast<std::size_t>(index)%capacity];
            if(tile.index!=index){
                if(drawPrepared || !ensureTargetBitmap(tile.bitmap,width,tilePixels))return false;
                const auto started=qpcSeconds();
                ComPtr<ID2D1Image> previous;target_->GetTarget(previous.GetAddressOf());
                D2D1_MATRIX_3X2_F transform{};target_->GetTransform(&transform);
                target_->SetTarget(tile.bitmap.Get());
                const auto top=index*tileHeight;
                target_->SetTransform(D2D1::Matrix3x2F::Translation(-viewport.left,0));
                target_->Clear(d2dColor(UiDesignSystemV1::color(UiColorToken::canvas)));
                buildingResultTextCache_=true;
                drawResultEntries(top,top+tileHeight,-static_cast<float>(top));
                buildingResultTextCache_=false;
                target_->SetTarget(previous.Get());target_->SetTransform(transform);
                tile.index=index;++resultTileBuilds_;
                maximumTileBuildMs_=std::max(maximumTileBuildMs_,(qpcSeconds()-started)*1000);
            }
            if(drawPrepared){
                const auto y=viewport.top+static_cast<float>(index*tileHeight-snapped);
                target_->DrawBitmap(tile.bitmap.Get(),D2D1::RectF(viewport.left,y,viewport.left+width/scale,y+static_cast<float>(tileHeight)),
                    1,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }
        if(drawPrepared)++resultTileHits_;
        return true;
    }

    void drawResultEntries(double paintTop, double paintBottom, float originY)
    {
        const auto& entries = view_.entries();
        const auto overscan = static_cast<double>(kFacetRowHeight) * 4.0;
        const auto preparedTop = std::max(0.0, paintTop - overscan);
        const auto preparedBottom = paintBottom + overscan;
        auto iterator = std::lower_bound(
            entries.begin(), entries.end(), preparedTop,
            [](const ResultEntry& entry, double top) {
                return entry.top + entry.height < top;
            });
        for (; iterator != entries.end() && iterator->top <= preparedBottom;
                ++iterator) {
            const auto y = originY + static_cast<float>(iterator->top);
            if (iterator->kind == ResultEntryKind::albumHeader) {
                drawAlbumHeader(
                    iterator->groupIndex,
                    y,
                    static_cast<float>(iterator->height));
            } else {
                drawTrackRow(
                    *iterator,
                    y,
                    static_cast<float>(iterator->height));
            }
        }
    }

    void drawResultArtworkOverlays(
        double paintTop,
        double paintBottom,
        float originY)
    {
        const auto& entries = view_.entries();
        auto iterator = std::lower_bound(
            entries.begin(), entries.end(), paintTop,
            [](const ResultEntry& entry, double top) {
                return entry.top + entry.height < top;
            });
        for (; iterator != entries.end() && iterator->top <= paintBottom;
                ++iterator) {
            const auto y = originY + static_cast<float>(iterator->top);
            if (iterator->kind == ResultEntryKind::track) {
                const auto& tracks = viewTracks();
                if (iterator->trackIndex < tracks.size() &&
                        (isSelectedOccurrence(iterator->trackIndex) ||
                         isPlayingOccurrence(iterator->trackIndex))) {
                    drawTrackRow(*iterator, y, static_cast<float>(iterator->height));
                }
                continue;
            }
            if (iterator->kind != ResultEntryKind::albumHeader ||
                iterator->groupIndex >= view_.groups().size()) {
                continue;
            }
            drawArtwork(
                view_.groups()[iterator->groupIndex],
                albumArtworkRect(y, static_cast<float>(iterator->height)));
        }
    }

    bool drawResultWindowCache(const D2D1_RECT_F& viewport, double offset,
        bool drawPrepared)
    {
        if (!rolloutFlagEnabled(RolloutFlag::regionRetentionV1)) return false;
        const auto viewportHeight = static_cast<double>(
            viewport.bottom - viewport.top);
        const auto contentHeight = view_.resultHeight();
        if (viewportHeight <= 0.0 || contentHeight <= 0.0 || !target_) {
            return false;
        }

        // The source rectangle below is always viewport-height tall. Keep the
        // retained bitmap at least that tall, otherwise Direct2D scales short
        // result sets (for example a single album) to fill the viewport.
        // Gesture-time coverage misses already take the direct visible-row
        // path below. Retaining three complete viewports after the gesture
        // therefore only pins a large D3D surface without improving the
        // input-critical path. Keep one exact viewport as the stable layer;
        // the next gesture renders directly until it settles again.
        const auto desiredHeight = viewportHeight;
        const auto wantedWidth = static_cast<std::uint32_t>(std::max(
            1.0f, std::ceil(geometry_.width * dpi_ / 96.0f)));
        const auto wantedHeight = static_cast<std::uint32_t>(std::max(
            1.0, std::ceil(desiredHeight * dpi_ / 96.0)));
        const auto bitmapSize = resultWindowBitmap_
            ? resultWindowBitmap_->GetPixelSize()
            : D2D1::SizeU(0, 0);
        const auto maximumTop = std::max(0.0, contentHeight - desiredHeight);
        const auto coverageMiss = !resultWindowCacheValid_ ||
            bitmapSize.width != wantedWidth ||
            bitmapSize.height != wantedHeight ||
            offset < resultWindowTop_ ||
            offset + viewportHeight > resultWindowTop_ + resultWindowHeight_;
        const auto stateChanged =
            resultWindowViewRevision_ != viewRevision_ ||
            resultWindowThemeGeneration_ != themeGeneration_ ||
            resultWindowSelected_ != selectedTrack_ ||
            resultWindowPlaying_ != nowPlaying_;
        if ((coverageMiss || stateChanged) && resultScroll_.active()) {
            // A retained-window rebuild renders several viewports and is the
            // dominant p95 outlier during a fast gesture. Paint only the
            // currently visible rows on a miss; the cache is rebuilt once the
            // inertial controller settles, outside the input-critical path.
            resultWindowCacheValid_ = false;
            resultWindowLastOffset_ = offset;
            return false;
        }
        if (coverageMiss || stateChanged) {
            const auto movingDown = offset >= resultWindowLastOffset_;
            const auto extraCoverage = std::max(
                0.0, desiredHeight - viewportHeight);
            const auto trailingOverscan = extraCoverage *
                (movingDown ? 0.125 : 0.875);
            const auto nextTop = std::clamp(
                offset - trailingOverscan,
                0.0,
                maximumTop);
            if (!ensureTargetBitmap(resultWindowBitmap_, wantedWidth,
                    wantedHeight)) {
                resultWindowCacheValid_ = false;
                return false;
            }

            ComPtr<ID2D1Image> previousTarget;
            target_->GetTarget(previousTarget.ReleaseAndGetAddressOf());
            D2D1_MATRIX_3X2_F previousTransform{};
            target_->GetTransform(&previousTransform);
            target_->SetTarget(resultWindowBitmap_.Get());
            target_->SetTransform(D2D1::Matrix3x2F::Identity());
            target_->Clear(d2dColor(
                UiDesignSystemV1::color(UiColorToken::canvas)));
            drawResultEntries(
                nextTop,
                nextTop + desiredHeight,
                -static_cast<float>(nextTop));
            target_->SetTarget(previousTarget.Get());
            target_->SetTransform(previousTransform);

            resultWindowTop_ = nextTop;
            resultWindowHeight_ = desiredHeight;
            resultWindowViewRevision_ = viewRevision_;
            resultWindowThemeGeneration_ = themeGeneration_;
            resultWindowSelected_ = selectedTrack_;
            resultWindowPlaying_ = nowPlaying_;
            resultWindowCacheValid_ = true;
        }
        resultWindowLastOffset_ = offset;

        if (!drawPrepared) return true;

        // The cache follows a continuously animated DIP offset. Sampling it at
        // a fractional physical row with linear filtering blends neighbouring
        // text pixels and makes only the right pane look soft. Snap to one
        // physical pixel and copy without interpolation; motion remains smooth
        // while glyphs and separators retain native-pixel contrast.
        const auto physicalScale = std::max(1.0f, dpi_) / 96.0f;
        const auto maximumSourceTop = std::max(
            0.0,
            resultWindowHeight_ - viewportHeight);
        const auto sourceTop = static_cast<float>(std::clamp(
            std::round((offset - resultWindowTop_) * physicalScale) /
                physicalScale,
            0.0,
            maximumSourceTop));
        target_->DrawBitmap(
            resultWindowBitmap_.Get(),
            viewport,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
            D2D1::RectF(
                viewport.left,
                sourceTop,
                viewport.right,
                sourceTop + static_cast<float>(viewportHeight)));
        return true;
    }

    D2D1_RECT_F queueDrawerRect() const
    {
        const auto width = std::clamp(geometry_.width * 0.34f, 300.0f, 380.0f);
        const auto visible = static_cast<float>(queueProgress());
        const auto left = std::max(0.0f, geometry_.width - width) +
            width * (1.0f - visible);
        return D2D1::RectF(
            left,
            geometry_.tabsHeight,
            left + width,
            geometry_.height);
    }

    const LibraryTrack* libraryTrackFor(const metadb_handle_ptr& handle) const
    {
        if (!handle.is_valid() || libraryTrackIndexRevision_ != viewRevision_) {
            return nullptr;
        }
        if(!temporaryAlbumView_){
            const auto snapshot=LibraryModel::instance().sharedSnapshot();
            std::string key(handle->get_path());key+='\x1f';key+=std::to_string(handle->get_subsong_index());
            if(snapshot&&snapshot->tracks){const auto found=snapshot->handleIndex.find(key);
                if(found!=snapshot->handleIndex.end()&&found->second<snapshot->tracks->size())return &(*snapshot->tracks)[found->second];}
            return nullptr;
        }
        const auto identity = utf8ToWide(handle->get_path()) + L"\x1f" +
            std::to_wstring(handle->get_subsong_index());
        const auto found = libraryTrackIndex_.find(identity);
        const auto& tracks = viewTracks();
        return found != libraryTrackIndex_.end() && found->second < tracks.size()
            ? &tracks[found->second] : nullptr;
    }

    std::wstring queueFallbackTitle(const metadb_handle_ptr& handle)
    {
        if (!queueTitleScript_.is_valid()) {
            titleformat_compiler::get()->compile_safe(
                queueTitleScript_, "$if2(%title%,%filename%)  $char(183)  $if2(%artist%,Unknown artist)");
        }
        pfc::string8_fast output;
        if (queueTitleScript_.is_valid() &&
            handle->format_title(nullptr, output, queueTitleScript_, nullptr)) {
            return utf8ToWide(output.c_str());
        }
        return utf8ToWide(handle->get_path());
    }

    struct QueueDisplayItem {
        std::wstring title;
        std::wstring detail;
        std::wstring duration;
    };

    void ensureQueueSnapshot()
    {
        if (!queueSnapshotDirty_) return;
        pfc::list_t<t_playback_queue_item> source;
        playlist_manager::get()->queue_get_contents(source);
        std::vector<QueueDisplayItem> prepared;
        prepared.reserve(source.get_count());
        for (t_size index = 0; index < source.get_count(); ++index) {
            const auto& item = source[index];
            const auto* track = libraryTrackFor(item.m_handle);
            QueueDisplayItem row;
            row.title = track != nullptr
                ? track->title : queueFallbackTitle(item.m_handle);
            row.detail = track != nullptr
                ? track->artist + L"  \x00b7  " + track->album
                : L"Local track";
            if (track != nullptr) row.duration = durationText(track->durationSeconds);
            prepared.push_back(std::move(row));
        }
        queueDisplayItems_ = std::move(prepared);
        ++queueSnapshotRevision_;
        queueSnapshotDirty_ = false;
        PerformanceTelemetry::instance().collectionMaterialized(
            TelemetrySurface::workspace, 3U, queueDisplayItems_.size());
    }

    void drawQueueDrawer()
    {
        const auto drawer = queueDrawerRect();
        fill(drawer, brushes_.panel.Get());
        line(drawer.left, drawer.top, drawer.left, drawer.bottom, brushes_.accent.Get());
        line(drawer.left, drawer.top + kQueueHeaderHeight, drawer.right,
            drawer.top + kQueueHeaderHeight, brushes_.border.Get());

        ensureQueueSnapshot();
        const auto count = static_cast<t_size>(queueDisplayItems_.size());
        text(L"PLAYBACK QUEUE", D2D1::RectF(drawer.left + 12.0f, drawer.top,
            drawer.left + 170.0f, drawer.top + kQueueHeaderHeight),
            strongFormat_.Get(), brushes_.bright.Get());
        text(std::to_wstring(count), D2D1::RectF(drawer.left + 165.0f, drawer.top,
            drawer.left + 205.0f, drawer.top + kQueueHeaderHeight),
            normalFormat_.Get(), brushes_.muted.Get());
        text(L"CLEAR", D2D1::RectF(drawer.right - 106.0f, drawer.top,
            drawer.right - 54.0f, drawer.top + kQueueHeaderHeight),
            smallFormat_.Get(), count > 0 ? brushes_.text.Get() : brushes_.muted.Get());
        text(L"CLOSE", D2D1::RectF(drawer.right - 54.0f, drawer.top,
            drawer.right - 7.0f, drawer.top + kQueueHeaderHeight),
            smallFormat_.Get(), brushes_.accent.Get());

        const auto viewport = D2D1::RectF(
            drawer.left, drawer.top + kQueueHeaderHeight, drawer.right, drawer.bottom);
        target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        const auto offset = queueScroll_.position();
        const auto first = static_cast<t_size>(std::max(0.0, std::floor(offset / kQueueRowHeight)));
        const auto visible = static_cast<t_size>(std::ceil(
            (viewport.bottom - viewport.top) / kQueueRowHeight)) + 2;
        const auto last = std::min(count, first + visible);
        for (t_size index = first; index < last; ++index) {
            const auto y = viewport.top + static_cast<float>(index * kQueueRowHeight - offset);
            const auto row = D2D1::RectF(viewport.left, y, viewport.right, y + kQueueRowHeight);
            if (index == queueSelected_) fill(row, brushes_.selected.Get());
            else if ((index & 1U) != 0) fill(row, brushes_.alternate.Get());

            const auto& item = queueDisplayItems_[index];
            text(std::to_wstring(index + 1),
                D2D1::RectF(row.left + 8.0f, row.top, row.left + 34.0f, row.bottom),
                smallFormat_.Get(), brushes_.muted.Get());
            text(item.title, D2D1::RectF(row.left + 36.0f, row.top + 1.0f,
                row.right - 58.0f, row.top + 21.0f), normalFormat_.Get(), brushes_.bright.Get());
            text(item.detail, D2D1::RectF(row.left + 36.0f, row.top + 19.0f,
                row.right - 58.0f, row.bottom), smallFormat_.Get(), brushes_.muted.Get());
            if (!item.duration.empty()) {
                text(item.duration,
                    D2D1::RectF(row.right - 55.0f, row.top, row.right - 8.0f, row.bottom),
                    rightFormat_.Get(), brushes_.text.Get());
            }
            line(row.left, row.bottom - 1.0f, row.right, row.bottom - 1.0f,
                brushes_.border.Get(), 0.5f);
        }
        target_->PopAxisAlignedClip();
        drawScrollIndicator(viewport, count * kQueueRowHeight,
            queueScroll_.position(), &queueScroll_);
    }

    D2D1_RECT_F commandPaletteRect() const
    {
        const auto width = std::min(680.0f, std::max(320.0f, geometry_.width - 56.0f));
        const auto available = std::max(190.0f, geometry_.height - geometry_.tabsHeight - 52.0f);
        const auto height = std::min(430.0f, available);
        const auto left = (geometry_.width - width) * 0.5f;
        const auto top = geometry_.tabsHeight + 22.0f;
        return D2D1::RectF(left, top, left + width, top + height);
    }

    const std::vector<PaletteItem>& commandPaletteItems()
    {
        const auto userState = DatabaseService::instance().userStateSnapshot();
        const auto userGeneration = userState ? userState->generation : 0U;
        if (paletteCacheValid_ && paletteCacheQuery_ == paletteQuery_ &&
                paletteCacheViewRevision_ == viewRevision_ &&
                paletteCacheUserGeneration_ == userGeneration) {
            return paletteCacheItems_;
        }
        std::vector<PaletteItem> items;
        const auto query = foldedText(trimText(paletteQuery_));
        auto addAction = [&](PaletteKind kind, const wchar_t* label, const wchar_t* detail) {
            if (containsFolded(label, query) || containsFolded(detail, query)) {
                items.push_back({kind, 0, label, detail});
            }
        };
        addAction(PaletteKind::showNowPlaying, L"Show now playing", L"Reveal and center the playing track");
        addAction(PaletteKind::openListen, L"Open Listen", L"Open the separate music lab window");
        addAction(PaletteKind::toggleQueue, L"Playback queue", L"Open or close the queue drawer");
        addAction(PaletteKind::clearFilters, L"Clear library filters", L"Return LIB to the full collection");
        addAction(PaletteKind::playPause, L"Play or pause", L"Toggle foobar2000 playback");
        if (userState && userState->available) {
            addAction(PaletteKind::toggleFavorite,
                userState->favorite ? L"Remove favorite" : L"Add favorite",
                L"Change the current track favorite in GODENplayer");
            addAction(PaletteKind::editNote, L"Edit personal note",
                L"Open the local note attached to the current track");
            addAction(PaletteKind::editPersona, L"Edit artist persona",
                L"Open the local profile, aliases and relationships");
        }

        const auto& tracks = viewTracks();
        for (std::size_t index = 0; index < tracks.size() && items.size() < 18; ++index) {
            const auto& track = tracks[index];
            if (!containsFolded(track.title, query) &&
                !containsFolded(track.artist, query) &&
                !containsFolded(track.albumArtist, query) &&
                !containsFolded(track.album, query)) {
                continue;
            }
            items.push_back({
                PaletteKind::track,
                index,
                track.title,
                track.artist + L"  \x00b7  " + track.album,
            });
        }
        paletteCacheQuery_ = paletteQuery_;
        paletteCacheViewRevision_ = viewRevision_;
        paletteCacheUserGeneration_ = userGeneration;
        paletteCacheItems_ = std::move(items);
        paletteCacheValid_ = true;
        return paletteCacheItems_;
    }

    void drawCommandPalette()
    {
        const auto rectangle = commandPaletteRect();
        brushes_.background->SetOpacity(0.78f);
        fill(D2D1::RectF(0.0f, geometry_.tabsHeight, geometry_.width, geometry_.height),
            brushes_.background.Get());
        brushes_.background->SetOpacity(1.0f);
        fill(rectangle, brushes_.panel.Get());
        line(rectangle.left, rectangle.top, rectangle.right, rectangle.top, brushes_.accent.Get(), 2.0f);
        line(rectangle.left, rectangle.bottom, rectangle.right, rectangle.bottom, brushes_.border.Get());
        line(rectangle.left, rectangle.top, rectangle.left, rectangle.bottom, brushes_.border.Get());
        line(rectangle.right, rectangle.top, rectangle.right, rectangle.bottom, brushes_.border.Get());

        const auto query = paletteQuery_.empty() ? L"Search music or action" : paletteQuery_;
        text(query, D2D1::RectF(rectangle.left + 14.0f, rectangle.top + 1.0f,
            rectangle.right - 14.0f, rectangle.top + 42.0f),
            strongFormat_.Get(), paletteQuery_.empty() ? brushes_.muted.Get() : brushes_.bright.Get());
        line(rectangle.left, rectangle.top + 43.0f, rectangle.right,
            rectangle.top + 43.0f, brushes_.border.Get());

        const auto& items = commandPaletteItems();
        if (items.empty()) {
            text(L"No local match", D2D1::RectF(rectangle.left + 14.0f, rectangle.top + 58.0f,
                rectangle.right - 14.0f, rectangle.top + 92.0f),
                normalFormat_.Get(), brushes_.muted.Get());
            return;
        }
        paletteSelection_ = std::min(paletteSelection_, items.size() - 1);
        constexpr float rowHeight = 42.0f;
        const auto rowsTop = rectangle.top + 44.0f;
        const auto visible = static_cast<std::size_t>(std::max(1.0f,
            std::floor((rectangle.bottom - rowsTop) / rowHeight)));
        const auto first = paletteSelection_ >= visible
            ? paletteSelection_ - visible + 1 : 0;
        const auto last = std::min(items.size(), first + visible);
        for (std::size_t index = first; index < last; ++index) {
            const auto y = rowsTop + static_cast<float>(index - first) * rowHeight;
            const auto row = D2D1::RectF(rectangle.left, y, rectangle.right, y + rowHeight);
            if (index == paletteSelection_) fill(row, brushes_.selected.Get());
            else if ((index & 1U) != 0) fill(row, brushes_.alternate.Get());
            text(items[index].kind == PaletteKind::track ? L"M" : L">",
                D2D1::RectF(row.left + 10.0f, row.top, row.left + 34.0f, row.bottom),
                strongFormat_.Get(), index == paletteSelection_ ? brushes_.accent.Get() : brushes_.muted.Get());
            text(items[index].primary, D2D1::RectF(row.left + 38.0f, row.top + 1.0f,
                row.right - 12.0f, row.top + 22.0f), normalFormat_.Get(), brushes_.bright.Get());
            text(items[index].secondary, D2D1::RectF(row.left + 38.0f, row.top + 19.0f,
                row.right - 12.0f, row.bottom), smallFormat_.Get(), brushes_.muted.Get());
        }
    }

    void drawAlbumHeader(std::size_t groupIndex, float y, float height)
    {
        if (groupIndex >= view_.groups().size()) return;
        const auto& group = view_.groups()[groupIndex];
        const auto left = geometry_.leftWidth;
        const auto right = geometry_.width;
        fill(D2D1::RectF(left, y, right, y + height), brushes_.panel.Get());
        line(left, y, right, y, brushes_.border.Get());

        const auto compact = height <
            UiDesignSystemV1::metrics().albumHeaderDip;
        const auto art = albumArtworkRect(y, height);
        if(!buildingResultTextCache_)drawArtwork(group, art);

        const auto contentLeft = art.right + 10.0f;
        if (compact) {
            text(
                group.artist,
                D2D1::RectF(
                    contentLeft,
                    y,
                    right - 64.0f,
                    y + height * 0.52f),
                strongFormat_.Get(),
                brushes_.bright.Get());
            text(
                group.year,
                D2D1::RectF(
                    right - 62.0f,
                    y,
                    right - 8.0f,
                    y + height * 0.52f),
                rightStrongFormat_.Get(),
                brushes_.bright.Get());
            text(
                group.album,
                D2D1::RectF(
                    contentLeft,
                    y + height * 0.46f,
                    right - 8.0f,
                    y + height),
                smallFormat_.Get(),
                brushes_.text.Get());
            return;
        }

        text(
            group.artist,
            D2D1::RectF(
                contentLeft,
                y + 2.0f,
                right - 74.0f,
                y + 28.0f),
            titleFormat_.Get(),
            brushes_.bright.Get());
        text(
            group.year,
            D2D1::RectF(
                right - 74.0f,
                y + 2.0f,
                right - 9.0f,
                y + 28.0f),
            rightStrongFormat_.Get(),
            brushes_.bright.Get());
        text(
            group.album,
            D2D1::RectF(
                contentLeft,
                y + 27.0f,
                right - 10.0f,
                y + 50.0f),
            strongFormat_.Get(),
            brushes_.text.Get());

        text(
            group.displaySummary,
            D2D1::RectF(
                contentLeft,
                y + 52.0f,
                right - 10.0f,
                y + height - 3.0f),
            smallFormat_.Get(),
            brushes_.muted.Get());
        line(
            contentLeft,
            y + 50.0f,
            right - 10.0f,
            y + 50.0f,
            brushes_.border.Get());
    }

    D2D1_RECT_F albumArtworkRect(float y, float height) const noexcept
    {
        const auto compact = height <
            UiDesignSystemV1::metrics().albumHeaderDip;
        const auto padding = compact ? 4.0f : 8.0f;
        const auto artSize = std::clamp(
            height - padding * 2.0f,
            32.0f,
            64.0f);
        return D2D1::RectF(
            geometry_.leftWidth + padding,
            y + padding,
            geometry_.leftWidth + padding + artSize,
            y + padding + artSize);
    }

    void drawArtwork(const AlbumGroup& group, const D2D1_RECT_F& rectangle)
    {
        fill(rectangle, brushes_.selected.Get());
        line(rectangle.left, rectangle.top, rectangle.right, rectangle.top, brushes_.border.Get());
        line(rectangle.left, rectangle.top, rectangle.left, rectangle.bottom, brushes_.border.Get());
        drawArtworkImage(group, rectangle);
    }

    bool drawArtworkImage(
        const AlbumGroup& group,
        const D2D1_RECT_F& rectangle)
    {
        if (group.trackIndices.empty()) return false;
        const auto& tracks = viewTracks();
        const auto trackIndex = group.trackIndices.front();
        if (trackIndex >= tracks.size()) return false;
        const auto& track = tracks[trackIndex];
        return drawTrackArtwork(track, rectangle, 160);
    }

    bool drawTrackArtwork(
        const LibraryTrack& track,
        const D2D1_RECT_F& rectangle,
        std::uint32_t,
        float opacity = 1.0f)
    {
        const auto pixels = artCache_.getOrRequestVariant(
            artworkTrackIdentity(track.handle),
            track.handle,
            physicalArtworkEdge(rectangle));
        return drawPixels(pixels, rectangle, opacity);
    }

    bool drawFileArtwork(
        const std::filesystem::path& path,
        const D2D1_RECT_F& rectangle,
        std::uint32_t,
        float opacity = 1.0f)
    {
        const auto pixels = artCache_.getOrRequestFileVariant(
            path.wstring(),
            path,
            physicalArtworkEdge(rectangle));
        return drawPixels(pixels, rectangle, opacity);
    }

    bool drawPixels(
        const std::shared_ptr<const ArtPixels>& pixels,
        const D2D1_RECT_F& rectangle,
        float opacity = 1.0f)
    {
        if (!pixels || pixels->bgra.empty()) return false;

        const ArtworkBitmapKey bitmapKey{pixels->contentHash,
            pixels->width, pixels->height,
            static_cast<unsigned>(std::lround(dpi_))};
        auto iterator = artBitmaps_.find(bitmapKey);
        if (iterator == artBitmaps_.end()) {
            const auto incomingBytes = static_cast<std::size_t>(pixels->width) *
                static_cast<std::size_t>(pixels->height) * 4U;
            evictArtworkBitmaps(incomingBytes);
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED),
                dpi_,
                dpi_);
            ArtBitmapEntry entry;
            if (FAILED(target_->CreateBitmap(
                D2D1::SizeU(pixels->width, pixels->height),
                pixels->bgra.data(),
                pixels->width * 4U,
                properties,
                entry.bitmap.ReleaseAndGetAddressOf())) || !entry.bitmap) {
                return false;
            }
            entry.bytes = incomingBytes;
            entry.lastUse = ++artBitmapUseCounter_;
            artBitmapBytes_ += incomingBytes;
            iterator = artBitmaps_.emplace(
                bitmapKey, std::move(entry)).first;
        }
        iterator->second.lastUse = ++artBitmapUseCounter_;
        target_->DrawBitmap(
            iterator->second.bitmap.Get(),
            containedArtworkRectangle(
                rectangle, pixels->width, pixels->height),
            std::clamp(opacity, 0.0f, 1.0f),
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return true;
    }

    std::uint32_t physicalArtworkEdge(
        const D2D1_RECT_F& rectangle) const noexcept
    {
        const auto scale = std::max(1.0f, dpi_) / 96.0f;
        const auto edge = std::max(
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top) * scale;
        return static_cast<std::uint32_t>(std::max(
            1.0,
            std::ceil(static_cast<double>(edge))));
    }

    D2D1_RECT_F containedArtworkRectangle(
        const D2D1_RECT_F& rectangle,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight) const noexcept
    {
        const auto scale = std::max(1.0f, dpi_) / 96.0f;
        const auto left = std::lround(rectangle.left * scale);
        const auto top = std::lround(rectangle.top * scale);
        const auto right = std::lround(rectangle.right * scale);
        const auto bottom = std::lround(rectangle.bottom * scale);
        const auto availableWidth = std::max<long>(1, right - left);
        const auto availableHeight = std::max<long>(1, bottom - top);
        const auto fit = std::min(
            static_cast<double>(availableWidth) /
                std::max<std::uint32_t>(1, sourceWidth),
            static_cast<double>(availableHeight) /
                std::max<std::uint32_t>(1, sourceHeight));
        const auto width = std::max<long>(
            1, std::lround(sourceWidth * fit));
        const auto height = std::max<long>(
            1, std::lround(sourceHeight * fit));
        const auto destinationLeft = left + (availableWidth - width) / 2;
        const auto destinationTop = top + (availableHeight - height) / 2;
        return D2D1::RectF(
            static_cast<float>(destinationLeft) / scale,
            static_cast<float>(destinationTop) / scale,
            static_cast<float>(destinationLeft + width) / scale,
            static_cast<float>(destinationTop + height) / scale);
    }

    void evictArtworkBitmaps(std::size_t incomingBytes)
    {
        while (!artBitmaps_.empty() &&
            (artBitmaps_.size() >= kArtworkGpuCacheMaximumEntries ||
                artBitmapBytes_ + incomingBytes >
                    kArtworkGpuCacheBudgetBytes)) {
            const auto oldest = std::min_element(
                artBitmaps_.begin(), artBitmaps_.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastUse < right.second.lastUse;
                });
            if (oldest == artBitmaps_.end()) break;
            artBitmapBytes_ -= std::min(
                artBitmapBytes_, oldest->second.bytes);
            artBitmaps_.erase(oldest);
        }
    }

    void drawTrackRow(
        const ResultEntry& entry,
        float y,
        float height)
    {
        const auto& tracks = viewTracks();
        if (entry.trackIndex >= tracks.size()) return;
        const auto& track = tracks[entry.trackIndex];
        const auto left = geometry_.leftWidth;
        const auto right = geometry_.width;
        const auto selected = !buildingResultTextCache_ && isSelectedOccurrence(entry.trackIndex);
        const auto playing = !buildingResultTextCache_ && isPlayingOccurrence(entry.trackIndex);

        const auto stripe = entry.localTrackIndex;
        auto* background = stripe % 2 == 0
            ? brushes_.row.Get()
            : brushes_.alternate.Get();
        if (selected) background = brushes_.selected.Get();
        fill(D2D1::RectF(left, y, right, y + height), background);
        if (playing) fill(D2D1::RectF(left, y, left + 2.0f, y + height), brushes_.accent.Get());

        const auto textBlock = 0x300000000ULL +
            static_cast<std::uint64_t>(entry.trackIndex) * 4U;
        drawRetainedRowText(textBlock, entry.displayTrackNumber,
            D2D1::RectF(left + 10.0f, y, left + 40.0f, y + height),
            smallFormat_.Get(), playing ? brushes_.accent.Get() : brushes_.muted.Get());

        drawRetainedRowText(textBlock + 1U, entry.displayTitle,
            D2D1::RectF(left + 42.0f, y, right - 178.0f, y + height),
            normalFormat_.Get(), playing ? brushes_.accent.Get() : brushes_.text.Get());
        rating(effectiveRating(track), trackRatingRect(y, height),
            playing ? brushes_.accent.Get() : brushes_.text.Get(), brushes_.muted.Get());
        drawRetainedRowText(textBlock + 2U, entry.displayPlayCount,
            D2D1::RectF(right - 90.0f, y, right - 50.0f, y + height),
            rightFormat_.Get(), brushes_.muted.Get(),libraryCountRevision_);
        drawRetainedRowText(textBlock + 3U, entry.displayDuration,
            D2D1::RectF(right - 48.0f, y, right - 8.0f, y + height),
            rightFormat_.Get(), playing ? brushes_.accent.Get() : brushes_.text.Get());
        line(left, y + height - 1.0f, right, y + height - 1.0f, brushes_.border.Get(), 0.5f);
    }

    struct OverlayScrollbarState {
        nekorox::ScrollController* controller = nullptr;
        D2D1_RECT_F viewport{};
        D2D1_RECT_F thumb{};
        D2D1_RECT_F trackHit{};
        D2D1_RECT_F thumbHit{};
        double contentHeight = 0.0;
        double expansion = 0.0;
        double expansionTarget = 0.0;
        double lastMotionSeconds = 0.0;
        double collapseAfterSeconds = 0.0;
        float dragGrabY = 0.0f;
        bool seen = false;
        bool hovered = false;
        bool dragging = false;
    };

    OverlayScrollbarState& overlayScrollbar(
        nekorox::ScrollController* controller)
    {
        const auto iterator = std::find_if(
            overlayScrollbars_.begin(), overlayScrollbars_.end(),
            [controller](const auto& item) {
                return item.controller == controller;
            });
        if (iterator != overlayScrollbars_.end()) return *iterator;
        overlayScrollbars_.push_back({});
        overlayScrollbars_.back().controller = controller;
        return overlayScrollbars_.back();
    }

    void prepareScrollIndicatorGeometry(
        const D2D1_RECT_F& viewport,
        double contentHeight,
        double offset,
        nekorox::ScrollController* controller)
    {
        if (controller == nullptr) return;
        auto& overlay = overlayScrollbar(controller);
        const auto geometry = retainedScrollbarGeometry(
            viewport, geometry_.width, contentHeight, offset,
            overlay.expansion);
        overlay.seen = geometry.visible;
        overlay.viewport = geometry.viewport;
        overlay.contentHeight = geometry.contentHeight;
        overlay.thumb = geometry.thumb;
        overlay.trackHit = geometry.trackHit;
        overlay.thumbHit = geometry.thumbHit;
    }

    void prepareOverlayScrollbarGeometry()
    {
        for (auto& scrollbar : overlayScrollbars_) scrollbar.seen = false;
        if (activeTab_ != 4U || historyMode_) return;

        const auto prepareFacet = [&](std::size_t index, std::size_t rows) {
            prepareScrollIndicatorGeometry(
                geometry_.facetViewport(index),
                static_cast<double>(rows + 1U) * kFacetRowHeight,
                facetScroll_[index].position(), &facetScroll_[index]);
        };
        if (geometry_.compactFacets) {
            const auto index = static_cast<std::size_t>(compactFacet_);
            const std::array<std::size_t, 3> counts{
                view_.genres().size(), view_.artists().size(), view_.albums().size()};
            prepareFacet(index, counts[index]);
        } else {
            prepareFacet(0U, view_.genres().size());
            prepareFacet(1U, view_.artists().size());
            prepareFacet(2U, view_.albums().size());
        }
        const auto results = geometry_.resultViewport();
        prepareScrollIndicatorGeometry(
            results, view_.resultHeight(), resultScroll_.position(),
            &resultScroll_);
    }

    void drawScrollIndicator(
        const D2D1_RECT_F& viewport,
        double contentHeight,
        double offset,
        nekorox::ScrollController* controller)
    {
        prepareScrollIndicatorGeometry(
            viewport, contentHeight, offset, controller);
        auto& overlay = overlayScrollbar(controller);
        if (!overlay.seen) return;
        const auto& scrollViewport = overlay.viewport;
        const auto thickness = overlay.thumb.right - overlay.thumb.left;
        if (overlay.expansion > 0.02) {
            auto* trackBrush = brushes_.selected.Get();
            trackBrush->SetOpacity(static_cast<float>(
                0.10 + overlay.expansion * 0.18));
            fill(D2D1::RectF(
                scrollViewport.right - thickness,
                scrollViewport.top,
                scrollViewport.right,
                scrollViewport.bottom), trackBrush);
            trackBrush->SetOpacity(1.0f);
        }
        auto* thumbBrush = brushes_.muted.Get();
        thumbBrush->SetOpacity(static_cast<float>(
            0.68 + overlay.expansion * 0.32));
        fill(overlay.thumb, thumbBrush);
        thumbBrush->SetOpacity(1.0f);
    }

    std::shared_ptr<const RetainedTextDocument> retainedText(
        std::uint64_t revision,
        std::uint64_t block,
        const std::wstring& value,
        IDWriteTextFormat* format,
        float width,
        float height = 1000000.0f)
    {
        if (value.empty() || format == nullptr || !writeFactory_ || width <= 0.0f) {
            return {};
        }
        // QA rollback preserves the legacy semantic: every call is prepared
        // afresh, while the returned document still lets measurement and draw
        // share one DirectWrite result within the current paint.
        if (!rolloutFlagEnabled(RolloutFlag::textRetentionV1)) {
            textLayoutCache_.clear();
        }
        std::uint64_t formatRevision = 0;
        const std::array<IDWriteTextFormat*, 11> formats{
            normalFormat_.Get(), smallFormat_.Get(), strongFormat_.Get(),
            titleFormat_.Get(), heroTitleFormat_.Get(), tabFormat_.Get(),
            rightFormat_.Get(), rightStrongFormat_.Get(), wrapFormat_.Get(),
            wrapSmallFormat_.Get(), lyricFormat_.Get()};
        const auto found = std::find(formats.begin(), formats.end(), format);
        if (found != formats.end()) {
            formatRevision = static_cast<std::uint64_t>(
                std::distance(formats.begin(), found) + 1);
        }
        return textLayoutCache_.getOrCreate(writeFactory_.Get(), revision,
            block, formatRevision, value, format, width, height);
    }

    void drawRetainedText(
        const std::shared_ptr<const RetainedTextDocument>& document,
        const D2D1_RECT_F& viewport,
        ID2D1Brush* brush,
        float contentTop,
        float offset = 0.0f)
    {
        if (!document) return;
        textLayoutCache_.drawVisible(
            target_.Get(), *document, viewport, brush, contentTop, offset);
    }

    void drawRetainedRowText(
        std::uint64_t block,
        const std::wstring& value,
        const D2D1_RECT_F& rectangle,
        IDWriteTextFormat* format,
        ID2D1Brush* brush,std::uint64_t revision=0)
    {
        const auto document = retainedText(
            revision?revision:viewRevision_, block, value, format,
            std::max(1.0f, rectangle.right - rectangle.left),
            std::max(1.0f, rectangle.bottom - rectangle.top));
        drawRetainedText(document, rectangle, brush, rectangle.top);
    }

    void drawSectionLabel(
        const std::wstring& value,
        float left,
        float top,
        float right)
    {
        text(value, D2D1::RectF(left, top, right, top + 20.0f),
            smallFormat_.Get(), brushes_.secondary.Get());
        line(left, top + 20.0f, right, top + 20.0f, brushes_.border.Get());
        line(left, top + 20.0f, std::min(right, left + 68.0f),
            top + 20.0f, brushes_.accent.Get());
    }

    void drawInfoValue(
        const std::wstring& label,
        const std::wstring& value,
        const D2D1_RECT_F& rectangle,
        bool emphasized = false)
    {
        text(label,
            D2D1::RectF(
                rectangle.left,
                rectangle.top,
                rectangle.right,
                rectangle.top + 16.0f),
            smallFormat_.Get(), brushes_.secondary.Get());
        text(value.empty() ? L"--" : value,
            D2D1::RectF(
                rectangle.left,
                rectangle.top + 15.0f,
                rectangle.right,
                rectangle.bottom - 2.0f),
            emphasized ? strongFormat_.Get() : normalFormat_.Get(),
            emphasized ? brushes_.highlight.Get() : brushes_.text.Get());
        line(rectangle.left, rectangle.bottom - 1.0f,
            rectangle.right, rectangle.bottom - 1.0f,
            brushes_.border.Get());
    }

    std::shared_ptr<const LocalTrackContent> localContent()
    {
        if (!surfaceTrack_ || !surfaceTrack_->handle.is_valid()) return {};
        return localContentCache_.getOrRequest(
            surfaceTrack_->handle,
            surfaceTrack_->artist,
            surfaceTrack_->album,
            surfaceTrack_->title);
    }

    void drawSurfaceArtwork(
        const D2D1_RECT_F& rectangle,
        std::uint32_t decodeEdge,
        const std::shared_ptr<const LocalTrackContent>& content,
        bool preferArtist)
    {
        fill(rectangle, brushes_.panel.Get());
        line(rectangle.left, rectangle.top, rectangle.right, rectangle.top,
            brushes_.border.Get());
        line(rectangle.left, rectangle.top, rectangle.left, rectangle.bottom,
            brushes_.border.Get());
        if (preferArtist && content && !content->artistImages.empty()) {
            drawFileArtwork(content->artistImages.front(), rectangle, decodeEdge);
        } else if (surfaceTrack_) {
            const auto requestedEdge = std::max<std::uint32_t>(720U, decodeEdge);
            const auto pixels = artCache_.getOrRequest(
                surfaceArtworkIdentity_,
                surfaceTrack_->handle,
                requestedEdge,
                ArtRequestPriority::visibleMain);
            if (pixels) {
                if (surfaceArtworkTransition_.update(
                        target_.Get(), dpi_, surfaceArtworkIdentity_, pixels)) {
                    startAnimation(qpcSeconds());
                }
            } else if (artCache_.status(
                    surfaceArtworkIdentity_, requestedEdge) ==
                ArtCacheStatus::missing) {
                surfaceArtworkTransition_.clear();
            }
            surfaceArtworkTransition_.draw(
                target_.Get(), rectangle, motionMode());
        }
    }

    void drawSurfaceIdentity(float left, float top, float right)
    {
        favoriteRect_ = {};
        if (!surfaceTrack_) return;
        auto titleRight = right;
        const auto userState = DatabaseService::instance().userStateSnapshot();
        if (userState->available) {
            favoriteRect_ = D2D1::RectF(right - 32.0f, top + 1.0f,
                right, top + 31.0f);
            titleRight = favoriteRect_.left - 8.0f;
            favoriteGlyph(
                userState->favorite,
                favoriteRect_,
                userState->favorite
                    ? brushes_.accent.Get() : brushes_.muted.Get());
        }
        text(surfaceTrack_->title,
            D2D1::RectF(left, top, titleRight, top + 34.0f),
            heroTitleFormat_.Get(), brushes_.bright.Get());
        text(surfaceTrack_->artist,
            D2D1::RectF(left, top + 35.0f, right, top + 58.0f),
            strongFormat_.Get(), brushes_.secondary.Get());
        auto albumLine = surfaceTrack_->album;
        if (!surfaceTrack_->year.empty()) albumLine += L"  " + surfaceTrack_->year;
        text(albumLine,
            D2D1::RectF(left, top + 58.0f, right, top + 80.0f),
            normalFormat_.Get(), brushes_.text.Get());
    }

    void drawInfoTab(const std::shared_ptr<const LocalTrackContent>& content)
    {
        const auto top = geometry_.tabsHeight;
        const auto padding = 16.0f;
        const auto availableHeight = std::max(120.0f, geometry_.height - top - 2.0f * padding);
        const auto artSize = std::clamp(
            std::min(geometry_.width * 0.34f, availableHeight * 0.72f),
            170.0f, 380.0f);
        const auto artRect = D2D1::RectF(
            padding, top + padding, padding + artSize, top + padding + artSize);
        drawSurfaceArtwork(artRect, 640, content, false);
        const auto detailLeft = artRect.right + 20.0f;
        const auto detailRight = geometry_.width - padding;
        drawSurfaceIdentity(detailLeft, top + padding, detailRight);

        if (!surfaceTrack_) return;
        auto y = top + padding + 92.0f;
        drawSectionLabel(L"NOW PLAYING", detailLeft, y, detailRight);
        y += 25.0f;
        const auto playback = Runtime::instance().playbackSnapshot();
        const auto state = playback.state == PlaybackState::playing
            ? L"PLAYING" : playback.state == PlaybackState::paused ? L"PAUSED" : L"STOPPED";
        text(state, D2D1::RectF(detailLeft, y, detailLeft + 90.0f, y + 22.0f),
            strongFormat_.Get(), brushes_.accent.Get());
        text(durationText(playback.positionSeconds) + L" / " +
                durationText(surfaceTrack_->durationSeconds),
            D2D1::RectF(detailLeft + 90.0f, y, detailRight, y + 22.0f),
            rightFormat_.Get(), brushes_.text.Get());
        y += 34.0f;

        auto format = surfaceTrack_->codec.empty() ? L"LOCAL AUDIO" : surfaceTrack_->codec;
        if (surfaceTrack_->bitsPerSample > 0) {
            format += L" / " + std::to_wstring(surfaceTrack_->bitsPerSample) + L" bit";
        }
        const auto columnGap = 18.0f;
        const auto columnWidth = std::max(
            80.0f,
            (detailRight - detailLeft - columnGap) * 0.5f);
        const auto leftColumn = detailLeft;
        const auto rightColumn = detailLeft + columnWidth + columnGap;
        constexpr float cellHeight = 47.0f;
        drawInfoValue(L"FORMAT", format,
            D2D1::RectF(leftColumn, y, leftColumn + columnWidth, y + cellHeight),
            true);
        drawInfoValue(L"SAMPLE RATE", sampleRateText(surfaceTrack_->sampleRate),
            D2D1::RectF(rightColumn, y, detailRight, y + cellHeight));
        y += cellHeight + 7.0f;
        drawInfoValue(L"GENRE", surfaceTrack_->genre,
            D2D1::RectF(leftColumn, y, leftColumn + columnWidth, y + cellHeight));
        drawInfoValue(L"PLAYS", std::to_wstring(surfaceTrack_->playCount),
            D2D1::RectF(rightColumn, y, detailRight, y + cellHeight));
        y += cellHeight + 7.0f;
        auto trackNumber = surfaceTrack_->track > 0
            ? std::to_wstring(surfaceTrack_->track) : L"--";
        if (surfaceTrack_->disc > 0) {
            trackNumber = std::to_wstring(surfaceTrack_->disc) + L"." + trackNumber;
        }
        drawInfoValue(L"DISC / TRACK", trackNumber,
            D2D1::RectF(leftColumn, y, leftColumn + columnWidth, y + cellHeight));
        text(L"RATING",
            D2D1::RectF(rightColumn, y, detailRight, y + 16.0f),
            smallFormat_.Get(), brushes_.secondary.Get());
        infoRatingRect_ = D2D1::RectF(
            rightColumn, y + 14.0f, detailRight, y + cellHeight);
        rating(effectiveRating(*surfaceTrack_), infoRatingRect_,
            brushes_.highlight.Get(), brushes_.muted.Get());
        line(rightColumn, y + cellHeight - 1.0f,
            detailRight, y + cellHeight - 1.0f, brushes_.border.Get());

        const auto pathTop = std::max(artRect.bottom + 16.0f, y + cellHeight + 18.0f);
        if (pathTop + 42.0f < geometry_.height) {
            drawSectionLabel(L"SOURCE", padding, pathTop, geometry_.width - padding);
            text(displaySourcePath(surfaceTrack_->path),
                D2D1::RectF(padding, pathTop + 24.0f,
                    geometry_.width - padding, pathTop + 46.0f),
                smallFormat_.Get(), brushes_.muted.Get());
        }
    }

    float drawPersonaGraph(
        const PersistedPersona& persona,
        const D2D1_RECT_F& viewport,
        float top)
    {
        constexpr float height = 190.0f;
        const auto graph = D2D1::RectF(
            viewport.left, top, viewport.right, top + height);
        target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        fill(graph, brushes_.panel.Get());
        const auto centerX = (graph.left + graph.right) * 0.5f;
        const auto centerY = (graph.top + graph.bottom) * 0.5f;
        const auto count = (std::min<std::size_t>)(persona.relationships.size(), 8U);
        constexpr float pi = 3.14159265358979323846f;
        for (std::size_t index = 0; index < count; ++index) {
            const auto angle = -pi * 0.5f +
                2.0f * pi * static_cast<float>(index) /
                    static_cast<float>((std::max<std::size_t>)(1U, count));
            const auto graphWidth = graph.right - graph.left;
            const auto radiusX = std::clamp(graphWidth * 0.30f, 50.0f,
                (std::max)(50.0f, graphWidth * 0.5f - 62.0f));
            constexpr float radiusY = 66.0f;
            const auto x = centerX + std::cos(angle) * radiusX;
            const auto y = centerY + std::sin(angle) * radiusY;
            line(centerX, centerY, x, y, brushes_.border.Get());
            const auto node = D2D1::RectF(x - 58.0f, y - 13.0f,
                x + 58.0f, y + 13.0f);
            fill(node, brushes_.selected.Get());
            text(utf8ToWide(persona.relationships[index].targetDisplayName),
                node, tabFormat_.Get(), brushes_.text.Get());
            const auto labelX = (centerX + x) * 0.5f;
            const auto labelY = (centerY + y) * 0.5f;
            text(utf8ToWide(persona.relationships[index].relation),
                D2D1::RectF(labelX - 45.0f, labelY - 10.0f,
                    labelX + 45.0f, labelY + 10.0f),
                smallFormat_.Get(), brushes_.muted.Get());
        }
        const auto center = D2D1::RectF(centerX - 72.0f, centerY - 16.0f,
            centerX + 72.0f, centerY + 16.0f);
        fill(center, brushes_.selected.Get());
        line(center.left, center.bottom, center.right, center.bottom,
            brushes_.accent.Get());
        text(utf8ToWide(persona.displayName), center,
            tabFormat_.Get(), brushes_.accent.Get());
        target_->PopAxisAlignedClip();
        return height;
    }

    void suspendReaderText()
    {
        readerText_.cancel();
        readerFrame_.reset();
        readerDemand_ = false;
    }

    void resetReaderText()
    {
        suspendReaderText();
        readerBatch_.reset();
        readerContent_.reset();
        readerPersona_.reset();
        readerUserState_.reset();
        readerLyrics_.reset();
        readerFailed_ = false;
        readerPendingWheel_ = 0;
        readerRoute_ = ReaderRoute::unknown;
        biographyPreparedRevision_ = 0;
    }

    void prepareReaderText() try
    {
        if (!surfaceTrack_ || (activeTab_ != 1U && activeTab_ != 2U) ||
            !DisplayEnvironmentService::instance().presentationVisible(window_)) {
            suspendReaderText();
            return;
        }
        readerDemand_ = true;
        if (readerFailed_) return;
        if (activeTab_ == 1U) {
            readerRoute_ = ReaderRoute::asynchronous;
            readerContent_ = localContent();
            readerPersona_ = DatabaseService::instance().personaSnapshot();
            readerUserState_ = DatabaseService::instance().userStateSnapshot();
            // A provisional empty document has no authoritative scroll bounds.
            // Wait for the cache's completion message, retaining wheel intent.
            if (!readerContent_) {
                readerText_.cancel();
                readerFrame_.reset();
                return;
            }
            const auto& content = readerContent_;
            const auto& personaState = readerPersona_;
            const auto hasPersona = personaState->available && personaState->persona.found;
        const auto& userState = readerUserState_;
        auto contentRevision = content ? content->revision : 0U;
        contentRevision ^= personaState->generation + 0x9e3779b97f4a7c15ULL +
            (contentRevision << 6U) + (contentRevision >> 2U);
        contentRevision ^= userState->generation + 0x9e3779b97f4a7c15ULL +
            (contentRevision << 6U) + (contentRevision >> 2U);
        if (biographyPreparedRevision_ != contentRevision) {
            biographyPrepared_ =
                hasPersona && !personaState->persona.profile.empty()
                    ? utf8ToWide(personaState->persona.profile)
                    : content && !content->biography.empty()
                        ? content->biography : L"No local artist profile";
            biographyAliasesPrepared_.clear();
            if (hasPersona) {
                for (const auto& alias : personaState->persona.aliases) {
                    if (!biographyAliasesPrepared_.empty()) {
                        biographyAliasesPrepared_ += L" / ";
                    }
                    biographyAliasesPrepared_ += utf8ToWide(alias.display);
                }
            }
            biographyReviewPrepared_ = content && !content->review.empty()
                ? content->review : L"No local album notes";
            biographyNotePrepared_ =
                userState->available && !userState->note.empty()
                    ? utf8ToWide(userState->note) : L"No personal note";
            biographyPreparedRevision_ = contentRevision;
        }

            const auto showMap = personaMapVisible_ && hasPersona && !personaState->persona.relationships.empty();
            if (!readerBatch_ || readerSourceRevision_ != contentRevision || readerShowMap_ != showMap) {
                auto batch = std::make_shared<AsyncTextBatch>();
                batch->styles = {AsyncTextStyle::fromFormat(wrapFormat_.Get()), AsyncTextStyle::fromFormat(wrapSmallFormat_.Get())};
                auto append = [&](const std::wstring& value, std::size_t style, float before, float after) {
                    batch->blocks.push_back({std::make_shared<const std::wstring>(value), style, before, 1.f, after});
                };
                append(biographyPrepared_, 0U, 29.f, 26.f);
                readerAliases_ = !biographyAliasesPrepared_.empty();
                if (readerAliases_) append(biographyAliasesPrepared_, 1U, 29.f, 26.f);
                append(biographyReviewPrepared_, 1U, 29.f + (showMap ? 245.f : 0.f), 26.f);
                append(biographyNotePrepared_, 1U, 29.f, 12.f);
                readerShowMap_ = showMap;
                readerSourceRevision_ = contentRevision;
                readerBatch_ = std::move(batch);
                readerFrame_.reset();
                ++readerRevision_;
            }
            const auto leftWidth = std::clamp(geometry_.width * .31f, 210.f, 350.f);
            readerViewport_ = D2D1::RectF(leftWidth + 18.f, geometry_.tabsHeight + 14.f,
                std::min(geometry_.width - 18.f, leftWidth + 998.f), geometry_.height - 12.f);
        } else {
            const auto snapshot = LyricsService::instance().snapshot();
            const auto& lyrics = presenterLyrics(snapshot);
            const auto revision = lyricsBodyRevision_;
            ensureLyricsTimeline(revision, lyrics);
            if (lyrics.empty() || lyricsTimeline_.synchronized()) {
                suspendReaderText();
                readerRoute_ = lyrics.empty() ? ReaderRoute::empty : ReaderRoute::synchronous;
                if (lyrics.empty()) readerPendingWheel_ = 0;
                return;
            }
            readerRoute_ = ReaderRoute::asynchronous;
            if (!readerBatch_ || readerSourceRevision_ != revision) {
                auto batch = std::make_shared<AsyncTextBatch>();
                batch->styles.push_back(AsyncTextStyle::fromFormat(lyricFormat_.Get()));
                batch->blocks.push_back({std::make_shared<const std::wstring>(lyrics)});
                readerBatch_ = std::move(batch);
                readerLyrics_ = snapshot;
                readerSourceRevision_ = revision;
                readerFrame_.reset();
                ++readerRevision_;
            }
            readerViewport_ = lyricsViewport();
        }
        const auto width = readerViewport_.right - readerViewport_.left;
        const auto height = readerViewport_.bottom - readerViewport_.top;
        if (width <= 0.f || height <= 0.f) { suspendReaderText(); return; }
        auto& scroll = tabScroll_[activeTab_];
        readerFrame_ = readerText_.prepareBatch(readerOwner_, readerRevision_, readerBatch_,
            width, height, static_cast<float>(scroll.position()), window_);
        if (readerFrame_ && readerFrame_->generation == readerText_.stats().generation) {
            scroll.setBounds(0.0, std::max(0.0, double(readerFrame_->geometry->height - height)));
            if (readerPendingWheel_ != 0.0) {
                scroll.setResponseScale(scrollResponseScale());
                scroll.addWheelImpulseAt(readerPendingWheel_, qpcSeconds());
                readerPendingWheel_ = 0;
                if (scroll.active() && !startAnimation(qpcSeconds())) scroll.setPosition(scroll.target());
            }
            if (!readerFrame_->covers(static_cast<float>(scroll.position()), height))
                readerFrame_ = readerText_.prepareBatch(readerOwner_, readerRevision_, readerBatch_,
                    width, height, static_cast<float>(scroll.position()), window_);
        }
    } catch (...) {
        readerText_.cancel();
        readerFrame_.reset();
        readerFailed_ = true; // Retry only after explicitly reopening the surface.
    }

    float readerDrawViewport(D2D1_RECT_F& viewport) const
    {
        const auto& frame = *readerFrame_;
        const auto desired = static_cast<float>(tabScroll_[activeTab_].position());
        const auto covered = frame.covers(desired, viewport.bottom - viewport.top);
        viewport.right = std::min(viewport.right, viewport.left + frame.width);
        if (!covered) viewport.bottom = std::min(viewport.bottom, viewport.top + frame.viewportHeight);
        return covered ? desired : frame.scroll;
    }

    void drawReaderStatus(const D2D1_RECT_F& viewport)
    {
        if (readerFrame_ && !readerFailed_ && readerText_.stats().status != AsyncTextStatus::failed) return;
        auto status = viewport;
        status.bottom = std::min(status.bottom, status.top + 30.f);
        text(readerFailed_ || readerText_.stats().status == AsyncTextStatus::failed
            ? L"Text unavailable. Reopen this tab to retry." : L"Preparing text\u2026",
            status, normalFormat_.Get(), brushes_.muted.Get());
    }

    void drawBiographyTab(const std::shared_ptr<const LocalTrackContent>& content)
    {
        noteEditRect_ = {};
        personaEditRect_ = {};
        personaMapRect_ = {};
        const auto top = geometry_.tabsHeight;
        const auto padding = 16.0f;
        const auto leftWidth = std::clamp(geometry_.width * 0.31f, 210.0f, 350.0f);
        const auto artSize = std::min(leftWidth - 2.0f * padding,
            std::max(150.0f, geometry_.height - top - 116.0f));
        const auto artRect = D2D1::RectF(
            padding, top + padding, padding + artSize, top + padding + artSize);
        drawSurfaceArtwork(artRect, 720, content, true);
        const auto personaState = readerPersona_ ? readerPersona_ : DatabaseService::instance().personaSnapshot();
        const auto hasPersona = personaState->available && personaState->persona.found;
        if (surfaceTrack_) {
            text(hasPersona ? utf8ToWide(personaState->persona.displayName) :
                    surfaceTrack_->artist,
                D2D1::RectF(padding, artRect.bottom + 12.0f,
                    leftWidth - padding, artRect.bottom + 38.0f),
                titleFormat_.Get(), brushes_.bright.Get());
            text(surfaceTrack_->album,
                D2D1::RectF(padding, artRect.bottom + 38.0f,
                    leftWidth - padding, artRect.bottom + 60.0f),
                normalFormat_.Get(), brushes_.text.Get());
        }
        line(leftWidth, top, leftWidth, geometry_.height, brushes_.border.Get());

        const auto viewport = D2D1::RectF(
            leftWidth + 18.0f, top + 14.0f,
            geometry_.width - 18.0f, geometry_.height - 12.0f);
        const auto bodyViewport = D2D1::RectF(
            viewport.left,
            viewport.top,
            std::min(viewport.right, viewport.left + 980.0f),
            viewport.bottom);
        const auto userState = readerUserState_ ? readerUserState_ : DatabaseService::instance().userStateSnapshot();
        if (readerFrame_) {
            auto clipped = bodyViewport;
            const auto offset = readerDrawViewport(clipped);
            auto sectionViewport = viewport;
            sectionViewport.bottom = clipped.bottom;
            const auto& frame = *readerFrame_;
            const auto& blocks = frame.geometry->blocks;
            auto drawBlock = [&](std::size_t index, const wchar_t* label, ID2D1Brush* brush) {
                const auto& block = blocks[index];
                const auto y = viewport.top + block.top - offset;
                target_->PushAxisAlignedClip(sectionViewport, D2D1_ANTIALIAS_MODE_ALIASED);
                drawSectionLabel(label, viewport.left, y - 29.f, viewport.right);
                target_->PopAxisAlignedClip();
                (void)frame.cache->drawPrepared(target_.Get(), *block.document, clipped, brush,
                    viewport.top + block.top, offset);
            };
            drawBlock(0U, L"ARTIST PROFILE", brushes_.text.Get());
            std::size_t index = 1U;
            if (readerAliases_) drawBlock(index++, L"ALIASES", brushes_.muted.Get());
            if (readerShowMap_) {
                const auto y = viewport.top + blocks[index].top - offset - 274.f;
                target_->PushAxisAlignedClip(sectionViewport, D2D1_ANTIALIAS_MODE_ALIASED);
                drawSectionLabel(L"RELATIONSHIP MAP", viewport.left, y, viewport.right);
                target_->PopAxisAlignedClip();
                drawPersonaGraph(personaState->persona, sectionViewport, y + 29.f);
            }
            drawBlock(index++, L"ALBUM NOTES", brushes_.muted.Get());
            drawBlock(index, L"MY NOTE", userState->available && !userState->note.empty()
                ? brushes_.text.Get() : brushes_.muted.Get());
            drawScrollIndicator(viewport, frame.geometry->height, offset, &tabScroll_[1]);
        }
        drawReaderStatus(bodyViewport);

        if (userState->available) {
            noteEditRect_ = D2D1::RectF(
                viewport.right - 86.0f, viewport.top,
                viewport.right, viewport.top + 23.0f);
            fill(noteEditRect_, brushes_.panel.Get());
            line(noteEditRect_.left, noteEditRect_.bottom,
                noteEditRect_.right, noteEditRect_.bottom,
                brushes_.accent.Get());
            text(L"EDIT NOTE", noteEditRect_, tabFormat_.Get(), brushes_.accent.Get());
        }
        if (personaState->available) {
            const auto right = noteEditRect_.right > noteEditRect_.left
                ? noteEditRect_.left - 6.0f : viewport.right;
            personaEditRect_ = D2D1::RectF(
                right - 98.0f, viewport.top, right, viewport.top + 23.0f);
            fill(personaEditRect_, brushes_.panel.Get());
            line(personaEditRect_.left, personaEditRect_.bottom,
                personaEditRect_.right, personaEditRect_.bottom,
                brushes_.accent.Get());
            text(L"EDIT PERSONA", personaEditRect_,
                tabFormat_.Get(), brushes_.accent.Get());
        }
        if (hasPersona && !personaState->persona.relationships.empty()) {
            personaMapRect_ = D2D1::RectF(
                personaEditRect_.left - 48.0f, viewport.top,
                personaEditRect_.left - 6.0f, viewport.top + 23.0f);
            fill(personaMapRect_, brushes_.panel.Get());
            line(personaMapRect_.left, personaMapRect_.bottom,
                personaMapRect_.right, personaMapRect_.bottom,
                brushes_.accent.Get());
            text(L"MAP", personaMapRect_, tabFormat_.Get(),
                personaMapVisible_ ? brushes_.accent.Get() : brushes_.muted.Get());
        }
    }

    const std::wstring& presenterLyrics(
        const std::shared_ptr<const LyricsSnapshot>& snapshot)
    {
        if (lyricsPresenterSnapshot_ != snapshot) {
            lyricsPresenterSnapshot_ = snapshot;
            auto next = lyricsPresenterText(snapshot);
            if (next != lyricsPresenterCache_) {
                lyricsPresenterCache_ = std::move(next);
                ++lyricsBodyRevision_;
            }
        }
        return lyricsPresenterCache_;
    }

    void drawLyricsTab(const std::shared_ptr<const LocalTrackContent>& content)
    {
        const auto top = geometry_.tabsHeight;
        const auto padding = 18.0f;
        if (surfaceTrack_) {
            text(surfaceTrack_->title,
                D2D1::RectF(padding, top + 9.0f,
                    geometry_.width - padding, top + 34.0f),
                titleFormat_.Get(), brushes_.bright.Get());
            text(surfaceTrack_->artist,
                D2D1::RectF(padding, top + 32.0f,
                    geometry_.width - padding, top + 54.0f),
                normalFormat_.Get(), brushes_.secondary.Get());
        }
        line(padding, top + 60.0f, geometry_.width - padding,
            top + 60.0f, brushes_.border.Get());
        const auto viewport = D2D1::RectF(
            padding, top + 72.0f, geometry_.width - padding,
            geometry_.height - 14.0f);
        const auto lyricsSnapshot = LyricsService::instance().snapshot();
        const auto& lyrics = presenterLyrics(lyricsSnapshot);
        const auto hasLyrics = !lyrics.empty();
        if (!hasLyrics) {
            tabScroll_[2].setBounds(0.0, 0.0);
            const auto emptyTop = viewport.top + std::clamp(
                (viewport.bottom - viewport.top - 150.0f) * 0.22f,
                20.0f,
                86.0f);
            const auto artSize = std::clamp(
                (viewport.bottom - viewport.top) * 0.22f,
                78.0f,
                128.0f);
            const auto art = D2D1::RectF(
                viewport.left,
                emptyTop,
                viewport.left + artSize,
                emptyTop + artSize);
            drawSurfaceArtwork(art, 256, content, false);
            const auto copyLeft = art.right + 18.0f;
            const auto copyRight = std::min(
                viewport.right,
                copyLeft + 360.0f);
            text(L"LYRICS",
                D2D1::RectF(copyLeft, emptyTop + 4.0f,
                    copyRight, emptyTop + 24.0f),
                smallFormat_.Get(), brushes_.secondary.Get());
            const auto status = LyricsService::instance().statusText();
            text(status,
                D2D1::RectF(copyLeft, emptyTop + 27.0f,
                    copyRight, emptyTop + 56.0f),
                titleFormat_.Get(), brushes_.bright.Get());
            if (surfaceTrack_) {
                text(surfaceTrack_->album,
                    D2D1::RectF(copyLeft, emptyTop + 59.0f,
                        copyRight, emptyTop + 82.0f),
                    normalFormat_.Get(), brushes_.text.Get());
                text(surfaceTrack_->year,
                    D2D1::RectF(copyLeft, emptyTop + 82.0f,
                        copyRight, emptyTop + 104.0f),
                    smallFormat_.Get(), brushes_.muted.Get());
            }
            line(copyLeft, emptyTop + 112.0f,
                std::min(copyRight, copyLeft + 92.0f), emptyTop + 112.0f,
                brushes_.accent.Get());
            return;
        }
        const auto lyricsRevision = lyricsBodyRevision_;
        ensureLyricsTimeline(lyricsRevision, lyrics);
        if (!lyricsTimeline_.synchronized()) {
            if (readerFrame_) {
                auto clipped = viewport;
                const auto offset = readerDrawViewport(clipped);
                (void)readerFrame_->cache->drawPrepared(target_.Get(), *readerFrame_->document,
                    clipped, brushes_.text.Get(), viewport.top, offset);
                drawScrollIndicator(viewport, readerFrame_->geometry->height, offset, &tabScroll_[2]);
            }
            drawReaderStatus(viewport);
            return;
        }

        constexpr float rowHeight = 34.0f;
        const auto contentHeight =
            static_cast<float>(lyricsLinesWide_.size()) * rowHeight;
        const auto viewportHeight = viewport.bottom - viewport.top;
        const auto lyricWidth = std::max(
            1.0f, viewport.right - viewport.left - 20.0f);
        if (lyricsLineLayoutRevision_ != lyricsRevision ||
            lyricsLineLayoutWidth_ != lyricWidth ||
            lyricsLineLayouts_.size() != lyricsLinesWide_.size()) {
            lyricsLineLayouts_.clear();
            // Timed lyrics use a fixed row height, so offscreen rows do not
            // need text shaping to determine the full scroll extent.
            lyricsLineLayouts_.resize(lyricsLinesWide_.size());
            lyricsLineLayoutRevision_ = lyricsRevision;
            lyricsLineLayoutWidth_ = lyricWidth;
        }
        tabScroll_[2].setBounds(0.0, std::max(
            0.0,
            static_cast<double>(contentHeight - viewportHeight)));
        if (readerPendingWheel_ != 0.0) {
            tabScroll_[2].setResponseScale(scrollResponseScale());
            tabScroll_[2].addWheelImpulseAt(readerPendingWheel_, qpcSeconds());
            readerPendingWheel_ = 0;
            if (tabScroll_[2].active() && !startAnimation(qpcSeconds()))
                tabScroll_[2].setPosition(tabScroll_[2].target());
        }

        const auto playback = Runtime::instance().playbackSnapshot();
        const auto active =
            lyricsTimeline_.activeLineIndex(playback.positionSeconds);
        if (active != lyricActiveLine_) {
            const auto now = qpcSeconds();
            const auto userRecentlyScrolled =
                lastWheelSeconds_[7] > 0.0 &&
                now - lastWheelSeconds_[7] < 1.6;
            lyricActiveLine_ = active;
            if (active && !userRecentlyScrolled) {
                tabScroll_[2].setPosition(
                    static_cast<double>(*active) * rowHeight -
                    viewportHeight * 0.42);
            }
        }

        const auto offset = tabScroll_[2].position();
        const auto first = static_cast<std::size_t>(std::max(
            0.0,
            std::floor(offset / rowHeight) - 2.0));
        const auto visible = static_cast<std::size_t>(
            std::ceil(viewportHeight / rowHeight)) + 5U;
        const auto last = std::min(
            lyricsLinesWide_.size(),
            first + visible);
        target_->PushAxisAlignedClip(
            viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        for (std::size_t index = first; index < last; ++index) {
            if (!lyricsLineLayouts_[index]) {
                lyricsLineLayouts_[index] = retainedText(lyricsRevision,
                    0x100000ULL + index, lyricsLinesWide_[index],
                    lyricFormat_.Get(), lyricWidth);
            }
            const auto y = viewport.top +
                static_cast<float>(index * rowHeight - offset);
            const auto row = D2D1::RectF(
                viewport.left, y, viewport.right, y + rowHeight);
            const auto isActive = active && *active == index;
            if (isActive) {
                fill(row, brushes_.selected.Get());
                fill(
                    D2D1::RectF(
                        row.left, row.top + 4.0f,
                        row.left + 2.0f, row.bottom - 4.0f),
                    brushes_.accent.Get());
            }
            drawRetainedText(
                lyricsLineLayouts_[index],
                D2D1::RectF(
                    row.left + 10.0f, row.top,
                    row.right - 10.0f, row.bottom),
                isActive ? brushes_.highlight.Get() : brushes_.text.Get(),
                row.top);
        }
        target_->PopAxisAlignedClip();
        drawScrollIndicator(
            viewport, contentHeight, tabScroll_[2].position(),
            &tabScroll_[2]);
    }

    void ensureLyricsTimeline(
        std::uint64_t contentRevision,
        const std::wstring& lyrics)
    {
        if (contentRevision == lyricsTimelineRevision_) return;
        lyricsTimelineRevision_ = contentRevision;
        lyricsTimeline_ =
            ::nekorox::LyricsTimelineV1::parseUtf8(wideToUtf8(lyrics));
        lyricsLinesWide_.clear();
        lyricsLineLayouts_.clear();
        lyricsLineLayoutRevision_ = 0;
        lyricsLineLayoutWidth_ = 0.0f;
        lyricsLinesWide_.reserve(lyricsTimeline_.lines().size());
        for (const auto& line : lyricsTimeline_.lines()) {
            lyricsLinesWide_.push_back(utf8ToWide(line.text));
        }
        lyricActiveLine_.reset();
        tabScroll_[2].setPosition(0.0);
    }

    D2D1_RECT_F lyricsViewport() const
    {
        constexpr float padding = 18.0f;
        return D2D1::RectF(
            padding,
            geometry_.tabsHeight + 72.0f,
            geometry_.width - padding,
            geometry_.height - 14.0f);
    }

    void drawArtworkTab(const std::shared_ptr<const LocalTrackContent>& content)
    {
        const auto top = geometry_.tabsHeight;
        const auto padding = 12.0f;
        const auto imageCount = 1U + static_cast<unsigned>(
            content ? content->artistImages.size() : 0U);
        artSelection_ = std::min<unsigned>(artSelection_, imageCount - 1U);
        const auto previousCount = coverflow_.itemCount();
        coverflow_.setItemCount(imageCount);
        coverflow_.setMotionMode(
            motionMode() == MotionMode::normal
                ? CoverflowMotionMode::normal
                : motionMode() == MotionMode::reduced
                    ? CoverflowMotionMode::reduced
                    : CoverflowMotionMode::off);
        if (previousCount != imageCount) {
            coverflow_.setSelectedIndex(artSelection_, false);
        }

        artViewport_ = D2D1::RectF(
            padding,
            top + padding,
            geometry_.width - padding,
            std::max(top + padding + 80.0f, geometry_.height - 52.0f));
        fill(artViewport_, brushes_.background.Get());
        line(
            artViewport_.left,
            artViewport_.bottom,
            artViewport_.right,
            artViewport_.bottom,
            brushes_.border.Get());

        artHitRects_.assign(imageCount, {});
        target_->PushAxisAlignedClip(
            artViewport_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        std::optional<D2D1_RECT_F> glyphFrame;
        if (artInspectOpen_) {
            const auto frame = D2D1::RectF(
                artViewport_.left + 8.0f,
                artViewport_.top + 8.0f,
                artViewport_.right - 8.0f,
                artViewport_.bottom - 8.0f);
            artHitRects_[artSelection_] = frame;
            glyphFrame = frame;
            fill(frame, brushes_.panel.Get());
            drawArtworkItem(content, artSelection_, frame, 1440, 1.0f);
            line(frame.left, frame.top, frame.right, frame.top,
                brushes_.accent.Get(), 1.0f);
            line(frame.left, frame.bottom, frame.right, frame.bottom,
                brushes_.accent.Get(), 1.0f);
            line(frame.left, frame.top, frame.left, frame.bottom,
                brushes_.accent.Get(), 1.0f);
            line(frame.right, frame.top, frame.right, frame.bottom,
                brushes_.accent.Get(), 1.0f);
        } else {
            const auto items = coverflow_.layout({
                artViewport_.left,
                artViewport_.top,
                artViewport_.right - artViewport_.left,
                artViewport_.bottom - artViewport_.top,
            });
            constexpr double pi = 3.14159265358979323846;
            for (const auto& item : items) {
                auto frame = D2D1::RectF(
                    static_cast<float>(item.frame.x),
                    static_cast<float>(item.frame.y),
                    static_cast<float>(item.frame.x + item.frame.width),
                    static_cast<float>(item.frame.y + item.frame.height));
                const auto center = (frame.left + frame.right) * 0.5f;
                const auto perspective = static_cast<float>(std::max(
                    0.42,
                    std::cos(std::abs(item.angleDegrees) * pi / 180.0)));
                const auto halfWidth =
                    (frame.right - frame.left) * perspective * 0.5f;
                frame.left = center - halfWidth;
                frame.right = center + halfWidth;
                artHitRects_[item.itemIndex] = frame;
                if (item.center) glyphFrame = frame;

                fill(frame, brushes_.panel.Get());
                drawArtworkItem(
                    content,
                    item.itemIndex,
                    frame,
                    720,
                    static_cast<float>(item.opacity));
                auto* border = item.center
                    ? brushes_.accent.Get() : brushes_.secondary.Get();
                const auto borderWidth = item.center ? 2.0f : 1.0f;
                line(frame.left, frame.top, frame.right, frame.top,
                    border, borderWidth);
                line(frame.left, frame.bottom, frame.right, frame.bottom,
                    border, borderWidth);
                line(frame.left, frame.top, frame.left, frame.bottom,
                    border, borderWidth);
                line(frame.right, frame.top, frame.right, frame.bottom,
                    border, borderWidth);
            }
            artSelection_ =
                static_cast<unsigned>(coverflow_.selectedIndex());
        }
        if (glyphFrame) {
            const auto theme = ThemeService::instance().snapshot();
            GlyphFlowDrawStyle style;
            style.color = D2D1::ColorF(
                theme.accent.red,
                theme.accent.green,
                theme.accent.blue,
                1.0f);
            style.strength = 0.11f;
            glyphFlowRenderer_.draw(
                target_.Get(), *glyphFrame, motionMode(), style);
        }
        target_->PopAxisAlignedClip();
        if (surfaceTrack_) {
            const auto label = artSelection_ == 0
                ? L"TRACK / ALBUM ART" : L"ARTIST / PERSONA " +
                    std::to_wstring(artSelection_);
            text(label, D2D1::RectF(
                    padding, artViewport_.bottom + 8.0f,
                    geometry_.width * 0.38f, geometry_.height - 5.0f),
                smallFormat_.Get(), brushes_.accent.Get());
            text(surfaceTrack_->artist + L"  \x00b7  " + surfaceTrack_->album,
                D2D1::RectF(
                    geometry_.width * 0.38f,
                    artViewport_.bottom + 8.0f,
                    geometry_.width - padding,
                    geometry_.height - 5.0f),
                normalFormat_.Get(), brushes_.secondary.Get());
        }
    }

    void drawArtworkItem(
        const std::shared_ptr<const LocalTrackContent>& content,
        std::size_t itemIndex,
        const D2D1_RECT_F& frame,
        std::uint32_t decodeEdge,
        float opacity)
    {
        if (itemIndex == 0 && surfaceTrack_) {
            drawTrackArtwork(
                *surfaceTrack_,
                frame,
                decodeEdge,
                opacity);
        } else if (content && itemIndex > 0 &&
            itemIndex - 1U < content->artistImages.size()) {
            drawFileArtwork(
                content->artistImages[itemIndex - 1U],
                frame,
                decodeEdge,
                opacity);
        }
    }

    void drawInfoSurface()
    {
        const auto top = geometry_.tabsHeight;
        fill(D2D1::RectF(0.0f, top, geometry_.width, geometry_.height),
            brushes_.background.Get());
        if (!surfaceTrack_) {
            text(L"STOPPED", D2D1::RectF(18.0f, top + 18.0f,
                    geometry_.width - 18.0f, top + 52.0f),
                titleFormat_.Get(), brushes_.muted.Get());
            return;
        }
        const auto content = localContent();
        switch (activeTab_) {
        case 0: drawInfoTab(content); break;
        case 1: drawBiographyTab(content); break;
        case 2: drawLyricsTab(content); break;
        case 3: drawArtworkTab(content); break;
        default: break;
        }
    }

    void ensureView()
    {
        auto& library = LibraryModel::instance();
        const auto& libraryTracks = library.tracks();
        const auto generation = library.generation();
        const auto temporaryGeneration = temporaryAlbumGeneration();
        if (!viewDirty_ && observedGeneration_ == generation &&
            observedTemporaryAlbumGeneration_ == temporaryGeneration) {
            return;
        }
        const auto snapshot=library.sharedSnapshot();
        if(!viewDirty_&&!temporaryAlbumView_&&snapshot&&snapshot->layoutRevision==observedLayoutRevision_&&
            observedTemporaryAlbumGeneration_==temporaryGeneration){
            if(view_.refreshStatistics(libraryTracks))++libraryCountRevision_;
            observedGeneration_=generation;
            reconcileRatingOverrides(generation);refreshNowPlaying();
            // This revision changes ratings/play counts, not the headers,
            // counts or facets. Preserve any existing structural-cache state;
            // result rows still need rebuilding even for rating-only changes.
            resultWindowCacheValid_=false;return;
        }
        metadb_handle_list temporaryItems;
        const auto temporaryActive = temporaryAlbumItems(temporaryItems);

        const auto enteringTemporary = temporaryActive && !temporaryAlbumView_;
        if (enteringTemporary) {
            savedLibraryGenre_ = selectedGenre_;
            savedLibraryArtist_ = selectedArtist_;
            savedLibraryAlbum_ = selectedAlbum_;
            savedLibraryQuery_ = query_;
            selectedGenre_.clear();
            selectedArtist_.clear();
            selectedAlbum_.clear();
            query_.clear();
            syncSearchEdit();
            for (auto& controller : facetScroll_) controller.setPosition(0.0);
            resultScroll_.setPosition(0.0);
            temporaryAlbumView_ = true;
        }

        if (temporaryActive &&
            (enteringTemporary ||
                observedTemporaryAlbumGeneration_ != temporaryGeneration ||
                observedGeneration_ != generation)) {
            temporaryTracks_.clear();
            temporaryTracks_.reserve(temporaryItems.get_count());
            for (t_size index = 0; index < temporaryItems.get_count(); ++index) {
                auto track = library.describeTrack(temporaryItems[index]);
                track.genre = L"Temporary";
                track.albumArtist = L"TEMPORARY ALBUM";
                track.albumArtists = {track.albumArtist};
                track.album = L"Dropped files";
                track.year.clear();
                temporaryTracks_.push_back(std::move(track));
            }
            combinedTracks_.clear();
            combinedTracks_.reserve(
                temporaryTracks_.size() + libraryTracks.size());
            combinedTracks_.insert(
                combinedTracks_.end(),
                temporaryTracks_.begin(),
                temporaryTracks_.end());
            combinedTracks_.insert(
                combinedTracks_.end(),
                libraryTracks.begin(),
                libraryTracks.end());
        } else if (!temporaryActive && temporaryAlbumView_) {
            temporaryAlbumView_ = false;
            temporaryTracks_.clear();
            combinedTracks_.clear();
            selectedGenre_ = std::move(savedLibraryGenre_);
            selectedArtist_ = std::move(savedLibraryArtist_);
            selectedAlbum_ = std::move(savedLibraryAlbum_);
            query_ = std::move(savedLibraryQuery_);
            syncSearchEdit();
            for (auto& controller : facetScroll_) controller.setPosition(0.0);
            resultScroll_.setPosition(0.0);
        }

        view_.rebuild(
            temporaryAlbumView_ ? combinedTracks_ : libraryTracks,
            selectedGenre_,
            selectedArtist_,
            selectedAlbum_,
            query_,
            compactAlbumHeaders_
                ? UiDesignSystemV1::metrics().albumHeaderCompactDip
                : UiDesignSystemV1::metrics().albumHeaderDip);
        PerformanceTelemetry::instance().collectionMaterialized(
            TelemetrySurface::workspace,
            temporaryAlbumView_ ? 2U : 1U,
            static_cast<std::uint64_t>(
                temporaryAlbumView_ ? combinedTracks_.size() :
                    libraryTracks.size()));
        ++viewRevision_;
        ++libraryCountRevision_;
        facetAllLabels_[0] = L"All (" + std::to_wstring(view_.genres().size()) + L")";
        facetAllLabels_[1] = L"All (" + std::to_wstring(view_.artists().size()) + L")";
        facetAllLabels_[2] = L"All (" + std::to_wstring(view_.albums().size()) + L")";
        libraryTrackIndex_.clear();
        const auto& indexedTracks = viewTracks();
        if(temporaryAlbumView_)libraryTrackIndex_.reserve(indexedTracks.size());
        for (std::size_t index = 0; temporaryAlbumView_ && index < indexedTracks.size(); ++index) {
            const auto& track = indexedTracks[index];
            if (!track.handle.is_valid()) continue;
            const auto identity = utf8ToWide(track.handle->get_path()) + L"\x1f" +
                std::to_wstring(track.handle->get_subsong_index());
            libraryTrackIndex_.try_emplace(std::move(identity), index);
        }
        libraryTrackIndexRevision_ = viewRevision_;
        queueSnapshotDirty_ = true;
        libraryStaticCacheValid_ = false;
        resultWindowCacheValid_ = false;
        observedGeneration_ = generation;
        observedLayoutRevision_ = snapshot?snapshot->layoutRevision:0;
        observedTemporaryAlbumGeneration_ = temporaryGeneration;
        viewDirty_ = false;
        updateScrollBounds();
        reconcileRatingOverrides(generation);
        resolveSelectedOccurrence();
        refreshNowPlaying();
    }

    const std::vector<LibraryTrack>& viewTracks() const
    {
        return temporaryAlbumView_
            ? combinedTracks_
            : LibraryModel::instance().tracks();
    }

    // Selection identifies a playlist occurrence, not every equal handle.
    void selectTrackOccurrence(std::size_t index)
    {
        const auto& tracks = viewTracks();
        if (index >= tracks.size()) return;
        selectedTrack_ = tracks[index].handle;
        selectedOccurrence_ = index;
        selectedOccurrenceTemporary_ = temporaryAlbumView_ && index < temporaryTracks_.size();
        selectedOccurrenceGeneration_ = temporaryAlbumGeneration();
        selectedOccurrenceOrdinal_ = 0;
        const auto first = temporaryAlbumView_ && !selectedOccurrenceTemporary_
            ? temporaryTracks_.size() : 0U;
        for (auto candidate = first; candidate < index; ++candidate) {
            if (tracks[candidate].handle == selectedTrack_) ++selectedOccurrenceOrdinal_;
        }
    }

    void resolveSelectedOccurrence()
    {
        selectedOccurrence_.reset();
        if (!selectedTrack_.is_valid()) return;
        if (selectedOccurrenceTemporary_ && (!temporaryAlbumView_ ||
                selectedOccurrenceGeneration_ != temporaryAlbumGeneration())) {
            selectedTrack_.release();
            return;
        }
        const auto& tracks = viewTracks();
        const auto first = temporaryAlbumView_ && !selectedOccurrenceTemporary_
            ? temporaryTracks_.size() : 0U;
        const auto last = selectedOccurrenceTemporary_ ? temporaryTracks_.size() : tracks.size();
        std::size_t ordinal = 0;
        for (auto index = first; index < last; ++index) {
            if (tracks[index].handle == selectedTrack_ && ordinal++ == selectedOccurrenceOrdinal_) {
                selectedOccurrence_ = index;
                return;
            }
        }
    }

    bool isSelectedOccurrence(std::size_t index) const noexcept
    {
        return selectedOccurrence_ && *selectedOccurrence_ == index;
    }

    bool isPlayingOccurrence(std::size_t index) const noexcept
    {
        return playingOccurrence_ && *playingOccurrence_ == index;
    }

    void updateAlbumHeaderDensity()
    {
        const auto viewport = geometry_.resultViewport();
        const auto width = viewport.right - viewport.left;
        const auto height = viewport.bottom - viewport.top;
        // Widths around 400 DIP still leave more than 280 DIP for title,
        // artist and metadata beside a full 64-DIP cover.  The old threshold
        // collapsed those useful layouts to a 32-DIP thumbnail.  Reserve the
        // compact card for genuinely constrained panels and keep hysteresis so
        // a resize near the boundary cannot oscillate between layouts.
        const auto nextCompact = compactAlbumHeaders_
            ? width < 360.0f || height < 260.0f
            : width < 330.0f || height < 230.0f;
        if (nextCompact == compactAlbumHeaders_) return;

        std::optional<std::pair<std::size_t, double>> anchor;
        const auto oldOffset = resultScroll_.position();
        const auto& oldEntries = view_.entries();
        for (std::size_t index = 0; index < oldEntries.size(); ++index) {
            const auto& entry = oldEntries[index];
            if (oldOffset < entry.top || oldOffset >= entry.top + entry.height) {
                continue;
            }
            const auto fraction = entry.height > 0.0
                ? (oldOffset - entry.top) / entry.height
                : 0.0;
            anchor = std::make_pair(index, std::clamp(fraction, 0.0, 1.0));
            break;
        }

        compactAlbumHeaders_ = nextCompact;
        viewDirty_ = true;
        ensureView();
        if (!anchor || anchor->first >= view_.entries().size()) return;
        const auto& entry = view_.entries()[anchor->first];
        resultScroll_.setPosition(
            entry.top + entry.height * anchor->second);
    }

    void updateScrollBounds()
    {
        if (geometry_.height <= 0.0f) return;
        const auto facetHeight = std::max(0.0f, geometry_.height - geometry_.facetTop);
        const std::array<std::size_t, 3> counts = historyMode_
            ? std::array<std::size_t, 3>{
                historyPeriods_.size() + 1,
                historyArtists_.size() + 1,
                historyAlbums_.size() + 1}
            : std::array<std::size_t, 3>{
                view_.genres().size() + 1,
                view_.artists().size() + 1,
                view_.albums().size() + 1};
        for (std::size_t index = 0; index < facetScroll_.size(); ++index) {
            const auto content = static_cast<double>(counts[index]) * kFacetRowHeight;
            facetScroll_[index].setBounds(0.0, std::max(0.0, content - facetHeight));
        }
        const auto resultHeight = std::max(0.0f, geometry_.height - geometry_.resultsTop);
        const auto resultContent = historyMode_
            ? historyRows_.size() * kHistoryRowHeight : view_.resultHeight();
        resultScroll_.setBounds(0.0, std::max(0.0, resultContent - resultHeight));
        const auto drawer = queueDrawerRect();
        const auto queueHeight = std::max(0.0f, drawer.bottom - drawer.top - kQueueHeaderHeight);
        const auto queueContent = static_cast<double>(playlist_manager::get()->queue_get_count()) * kQueueRowHeight;
        queueScroll_.setBounds(0.0, std::max(0.0, queueContent - queueHeight));
    }

    std::optional<std::size_t> coverflowItemAt(float x, float y) const
    {
        std::optional<std::size_t> best;
        auto bestDistance = std::numeric_limits<double>::max();
        for (std::size_t index = 0; index < artHitRects_.size(); ++index) {
            if (!pointIn(artHitRects_[index], x, y)) continue;
            const auto distance = std::abs(
                static_cast<double>(index) -
                coverflow_.visualPosition());
            if (distance < bestDistance) {
                best = index;
                bestDistance = distance;
            }
        }
        return best;
    }

    bool beginCoverflowPointer(LPARAM lParam)
    {
        if (activeTab_ != 3 || artViewport_.right <= artViewport_.left) {
            return false;
        }
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        if (!pointIn(artViewport_, x, y)) return false;
        if (artInspectOpen_) return true;

        coverflowPointerDown_ = true;
        coverflowPointerMoved_ = false;
        coverflowPointerStartX_ = x;
        coverflowPointerLastX_ = x;
        coverflowPointerLastSeconds_ = qpcSeconds();
        coverflowPointerItem_ = coverflowItemAt(x, y);
        coverflow_.beginDrag(x);
        SetCapture(window_);
        return true;
    }

    bool handleCoverflowDoubleClick(LPARAM lParam)
    {
        if (activeTab_ != 3 || artViewport_.right <= artViewport_.left) {
            return false;
        }
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        if (!pointIn(artViewport_, x, y)) return false;

        if (artInspectOpen_) {
            artInspectOpen_ = false;
            invalidate();
            return true;
        }
        const auto item = coverflowItemAt(x, y);
        if (!item) return true;
        coverflow_.setSelectedIndex(*item, false);
        artSelection_ = static_cast<unsigned>(*item);
        artInspectOpen_ = true;
        invalidate();
        return true;
    }

    void updateCoverflowPointer(LPARAM lParam)
    {
        if (!coverflowPointerDown_) return;
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        if (std::abs(x - coverflowPointerStartX_) >= 3.0f) {
            coverflowPointerMoved_ = true;
        }
        coverflow_.updateDrag(x);
        artSelection_ =
            static_cast<unsigned>(coverflow_.selectedIndex());
        coverflowPointerLastX_ = x;
        coverflowPointerLastSeconds_ = qpcSeconds();
        invalidate();
    }

    void endCoverflowPointer(LPARAM lParam)
    {
        if (!coverflowPointerDown_) return;
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        if (coverflowPointerMoved_) {
            const auto now = qpcSeconds();
            const auto elapsed = std::max(
                1.0 / 1000.0,
                now - coverflowPointerLastSeconds_);
            const auto velocity =
                static_cast<double>(x - coverflowPointerLastX_) / elapsed;
            coverflow_.endDrag(velocity);
        } else {
            coverflow_.cancelDrag();
            auto item = coverflowItemAt(x, y);
            if (!item) item = coverflowPointerItem_;
            if (item) {
                coverflow_.setSelectedIndex(*item, true);
            }
        }
        coverflowPointerDown_ = false;
        coverflowPointerItem_.reset();
        if (GetCapture() == window_) ReleaseCapture();
        artSelection_ =
            static_cast<unsigned>(coverflow_.selectedIndex());
        if (coverflow_.active()) startAnimation(qpcSeconds());
        invalidate();
    }

    void cancelCoverflowPointer()
    {
        if (!coverflowPointerDown_) return;
        coverflowPointerDown_ = false;
        coverflowPointerItem_.reset();
        coverflow_.cancelDrag();
        artSelection_ =
            static_cast<unsigned>(coverflow_.selectedIndex());
        if (coverflow_.active()) startAnimation(qpcSeconds());
        invalidate();
    }

    bool beginOverlayScrollbarPointer(LPARAM lParam)
    {
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        for (auto iterator = overlayScrollbars_.rbegin();
            iterator != overlayScrollbars_.rend(); ++iterator) {
            auto& scrollbar = *iterator;
            if (!scrollbar.seen || scrollbar.controller == nullptr ||
                !pointIn(scrollbar.trackHit, x, y)) {
                continue;
            }
            scrollbar.expansionTarget = 1.0;
            scrollbar.lastMotionSeconds = qpcSeconds();
            if (pointIn(scrollbar.thumbHit, x, y)) {
                scrollbar.dragging = true;
                scrollbar.dragGrabY = y - scrollbar.thumb.top;
                overlayScrollbarDragging_ = true;
                overlayDraggingController_ = scrollbar.controller;
                SetCapture(window_);
            } else {
                const auto direction = y < scrollbar.thumb.top ? -1.0 : 1.0;
                scrollbar.controller->setPosition(
                    scrollbar.controller->target() + direction *
                        (scrollbar.viewport.bottom - scrollbar.viewport.top) *
                        0.90);
            }
            startAnimation(qpcSeconds());
            frameClock_.requestImmediate();
            invalidate();
            return true;
        }
        return false;
    }

    void updateOverlayScrollbarPointer(LPARAM lParam)
    {
        if (!overlayScrollbarDragging_ ||
            overlayDraggingController_ == nullptr) return;
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        auto& scrollbar = overlayScrollbar(overlayDraggingController_);
        const auto viewportHeight = static_cast<double>(
            scrollbar.viewport.bottom - scrollbar.viewport.top);
        const auto thumbHeight = static_cast<double>(
            scrollbar.thumb.bottom - scrollbar.thumb.top);
        const auto travel = std::max(1.0, viewportHeight - thumbHeight);
        const auto top = std::clamp(
            static_cast<double>(y - scrollbar.dragGrabY),
            static_cast<double>(scrollbar.viewport.top),
            static_cast<double>(scrollbar.viewport.bottom) - thumbHeight);
        const auto amount = (top - scrollbar.viewport.top) / travel;
        const auto range = std::max(0.0,
            scrollbar.contentHeight - viewportHeight);
        scrollbar.controller->setPosition(amount * range);
        scrollbar.expansion = 1.0;
        scrollbar.expansionTarget = 1.0;
        frameClock_.requestImmediate();
        invalidate();
    }

    void endOverlayScrollbarPointer()
    {
        if (!overlayScrollbarDragging_) return;
        if (overlayDraggingController_ != nullptr) {
            auto& scrollbar = overlayScrollbar(overlayDraggingController_);
            scrollbar.dragging = false;
            scrollbar.collapseAfterSeconds = qpcSeconds() + 0.700;
        }
        overlayScrollbarDragging_ = false;
        overlayDraggingController_ = nullptr;
        if (GetCapture() == window_) ReleaseCapture();
        SetTimer(window_, kOverlayScrollbarCollapseTimer, 700U, nullptr);
    }

    void cancelOverlayScrollbarPointer()
    {
        if (overlayDraggingController_ != nullptr) {
            auto& scrollbar = overlayScrollbar(overlayDraggingController_);
            scrollbar.dragging = false;
            scrollbar.collapseAfterSeconds = qpcSeconds() + 0.700;
        }
        overlayScrollbarDragging_ = false;
        overlayDraggingController_ = nullptr;
        SetTimer(window_, kOverlayScrollbarCollapseTimer, 700U, nullptr);
    }

    void updateOverlayScrollbarHover(LPARAM lParam)
    {
        if (!overlayMouseTracking_) {
            TRACKMOUSEEVENT tracking{
                sizeof(tracking), TME_LEAVE, window_, HOVER_DEFAULT};
            TrackMouseEvent(&tracking);
            overlayMouseTracking_ = true;
        }
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        const auto now = qpcSeconds();
        bool changed = false;
        for (auto& scrollbar : overlayScrollbars_) {
            const auto hovered = scrollbar.seen &&
                pointIn(scrollbar.trackHit, x, y);
            if (hovered == scrollbar.hovered) continue;
            scrollbar.hovered = hovered;
            scrollbar.lastMotionSeconds = now;
            if (hovered) {
                scrollbar.expansionTarget = 1.0;
                scrollbar.collapseAfterSeconds = now + 0.700;
                SetTimer(window_, kOverlayScrollbarCollapseTimer, 700U, nullptr);
            } else {
                scrollbar.collapseAfterSeconds = now + 0.700;
                SetTimer(window_, kOverlayScrollbarCollapseTimer, 700U, nullptr);
            }
            changed = true;
        }
        if (changed) {
            startAnimation(now);
            invalidate();
        }
    }

    void leaveOverlayScrollbars()
    {
        const auto now = qpcSeconds();
        for (auto& scrollbar : overlayScrollbars_) {
            scrollbar.hovered = false;
            if (!scrollbar.dragging) {
                scrollbar.collapseAfterSeconds = now + 0.700;
            }
        }
        SetTimer(window_, kOverlayScrollbarCollapseTimer, 700U, nullptr);
    }

    void collapseIdleOverlayScrollbars()
    {
        const auto now = qpcSeconds();
        bool changed = false;
        for (auto& scrollbar : overlayScrollbars_) {
            if (!scrollbar.dragging &&
                now >= scrollbar.collapseAfterSeconds) {
                changed = scrollbar.expansionTarget != 0.0 || changed;
                scrollbar.expansionTarget = 0.0;
                scrollbar.lastMotionSeconds = now;
            }
        }
        if (changed) {
            startAnimation(now);
            invalidate();
        }
    }

    bool advanceOverlayScrollbarAnimation(double now)
    {
        bool active = false;
        for (auto& scrollbar : overlayScrollbars_) {
            if (scrollbar.lastMotionSeconds <= 0.0) {
                scrollbar.lastMotionSeconds = now;
            }
            const auto elapsed = std::clamp(
                now - scrollbar.lastMotionSeconds, 0.0, 0.05);
            scrollbar.lastMotionSeconds = now;
            const auto amount = 1.0 - std::exp(-elapsed / 0.040);
            scrollbar.expansion +=
                (scrollbar.expansionTarget - scrollbar.expansion) * amount;
            if (std::abs(scrollbar.expansionTarget -
                    scrollbar.expansion) < 0.002) {
                scrollbar.expansion = scrollbar.expansionTarget;
            } else {
                active = true;
            }
        }
        return active;
    }

    void handleWheel(WPARAM wParam, LPARAM lParam)
    {
        InteractionLatencyGuard::instance().noteScroll();
        if (activeTab_ == 3) {
            const auto wheel = static_cast<double>(
                GET_WHEEL_DELTA_WPARAM(wParam));
            if (coverflow_.addWheelDelta(wheel)) {
                artSelection_ = static_cast<unsigned>(
                    coverflow_.selectedIndex());
                if (coverflow_.active()) startAnimation(qpcSeconds());
                invalidate();
            }
            return;
        }
        if (activeTab_ != 4) {
            if (activeTab_ >= tabScroll_.size()) return;
            const auto region = 5U + activeTab_;
            const auto now = qpcSeconds();
            advanceScrollAnimation(now);
            const auto elapsed = lastWheelSeconds_[region] > 0.0
                ? now - lastWheelSeconds_[region]
                : 1.0 / 60.0;
            const auto newGesture =
                lastWheelSeconds_[region] <= 0.0 || elapsed >= 0.085;
            lastWheelSeconds_[region] = now;
            const auto wheel = static_cast<double>(
                GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            tabScroll_[activeTab_].setResponseScale(scrollResponseScale());
            if ((activeTab_ == 1U || activeTab_ == 2U) &&
                (readerRoute_ == ReaderRoute::unknown || (readerRoute_ == ReaderRoute::asynchronous &&
                    (!readerFrame_ || readerFrame_->generation != readerText_.stats().generation)))) {
                readerPendingWheel_ = std::clamp(readerPendingWheel_ - wheel * 84.0, -100000000.0, 100000000.0);
                presentWheelFrame(newGesture);
                return;
            }
            if (rolloutFlagEnabled(RolloutFlag::scrollControllerV2)) {
                tabScroll_[activeTab_].addWheelImpulseAt(-wheel * 84.0, now);
            } else {
                tabScroll_[activeTab_].addWheelImpulse(-wheel * 84.0, elapsed);
            }
            if (tabScroll_[activeTab_].active() && !startAnimation(now)) {
                tabScroll_[activeTab_].setPosition(
                    tabScroll_[activeTab_].target());
            }
            presentWheelFrame(newGesture);
            return;
        }
        if (historyMode_) ensureHistoryView();
        else ensureView();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window_, &point);
        const auto x = dip(static_cast<float>(point.x));
        const auto y = dip(static_cast<float>(point.y));
        std::size_t region = 3;
        if (queueOpen_ && pointIn(queueDrawerRect(), x, y)) {
            region = 4;
        } else if (x < geometry_.leftWidth && y >= geometry_.facetTop) {
            region = geometry_.compactFacets
                ? static_cast<std::size_t>(compactFacet_)
                : std::min<std::size_t>(
                    2,
                    static_cast<std::size_t>(x / geometry_.facetWidth));
        } else if (x < geometry_.leftWidth || y < geometry_.resultsTop) {
            return;
        }

        const auto now = qpcSeconds();
        advanceScrollAnimation(now);
        const auto elapsed = lastWheelSeconds_[region] > 0.0
            ? now - lastWheelSeconds_[region]
            : 1.0 / 60.0;
        const auto newGesture =
            lastWheelSeconds_[region] <= 0.0 || elapsed >= 0.085;
        lastWheelSeconds_[region] = now;
        const auto wheel = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        const auto impulse = -wheel * kFacetRowHeight * 3.25;
        auto& controller = region < 3
            ? facetScroll_[region]
            : region == 3 ? resultScroll_ : queueScroll_;
        controller.setResponseScale(scrollResponseScale());
        if (rolloutFlagEnabled(RolloutFlag::scrollControllerV2)) {
            controller.addWheelImpulseAt(impulse, now);
        } else {
            controller.addWheelImpulse(impulse, elapsed);
        }
        if (controller.active() && !startAnimation(now)) {
            controller.setPosition(controller.target());
        }
        presentWheelFrame(newGesture);
    }

    void settleScrollControllers()
    {
        resultScroll_.setPosition(resultScroll_.target());
        queueScroll_.setPosition(queueScroll_.target());
        for (auto& controller : facetScroll_) {
            controller.setPosition(controller.target());
        }
        for (auto& controller : tabScroll_) {
            controller.setPosition(controller.target());
        }
    }

    void stopAnimation()
    {
        frameClock_.stop();
        animationRunning_ = false;
        lastAnimationSeconds_ = 0.0;
    }

    bool startAnimation(double now)
    {
        if (animationRunning_) return true;
        if (window_ == nullptr || !frameClock_.start()) {
            animationRunning_ = false;
            return false;
        }
        animationRunning_ = true;
        lastAnimationSeconds_ = now;
        return true;
    }

    bool advanceScrollAnimation(double now)
    {
        if (!animationRunning_) return false;
        const auto elapsed = std::max(0.0, now - lastAnimationSeconds_);
        lastAnimationSeconds_ = now;
        const auto v2 = rolloutFlagEnabled(RolloutFlag::scrollControllerV2);
        const auto advance = [v2, now, elapsed](auto& controller) {
            return v2 ? controller.advanceTo(now) : controller.update(elapsed);
        };
        bool changed = advance(resultScroll_);
        for (auto& controller : facetScroll_) {
            changed = advance(controller) || changed;
        }
        for (auto& controller : tabScroll_) {
            changed = advance(controller) || changed;
        }
        changed = advance(queueScroll_) || changed;
        bool active = resultScroll_.active() || queueScroll_.active();
        for (const auto& controller : facetScroll_) active = controller.active() || active;
        for (const auto& controller : tabScroll_) active = controller.active() || active;
        active = coverflow_.update(elapsed) || active;
        active = advanceOverlayScrollbarAnimation(now) || active;
        if (coverflow_.itemCount() > 0) {
            artSelection_ =
                static_cast<unsigned>(coverflow_.selectedIndex());
        }
        return active || changed;
    }

    void presentWheelFrame(bool force)
    {
        (void)force;
        if (window_ == nullptr) return;
        invalidate();
        // Scroll is an input-semantic revision. Queue one immediate coordinator
        // pulse instead of waiting up to a full refresh period; the clock's
        // pending bit still coalesces bursts and Present remains non-blocking.
        frameClock_.requestImmediate();
        // Input mutates only the scroll model. The frame clock coalesces paint
        // and Present on the next display opportunity, so a wheel handler can
        // never wait behind artwork reconstruction or an occluded DWM swap.
    }

    void animate()
    {
        const auto now = qpcSeconds();
        const auto scrollActive = advanceScrollAnimation(now);
        if (!resultScroll_.active() && resultArtworkRefreshPending_) {
            resultArtworkRefreshPending_ = false;
            resultWindowCacheValid_ = false;
        }
        const auto themeActive = ThemeService::instance().transitionActive();
        // Glyphs belong to Artwork; the cover transition is used by Info,
        // Biography and the empty Lyrics page. Match actual draw ownership.
        const auto glyphActive = activeTab_ == 3 &&
            GlyphFlowService::instance().active(motionMode());
        const auto drawsSurfaceArtwork = activeTab_ < 2 ||
            (activeTab_ == 2 && presenterLyrics(LyricsService::instance().snapshot()).empty());
        const auto artworkActive = drawsSurfaceArtwork &&
            surfaceArtworkTransition_.active(motionMode());
        const auto semanticActive =
            themeActive || glyphActive || artworkActive;
        const auto active = scrollActive || motion_.needsFrame() ||
            semanticActive;
        frameClock_.setMaximumRefreshHz(
            scrollActive || motion_.needsFrame() || glyphActive || artworkActive
                ? 0.0 : themeActive ? kThemeSampleHz : 0.0);
        if (themeActive || themeWasActive_) {
            ChromeController::instance().refreshAccent();
        }
        themeWasActive_ = themeActive;
        // The consumed opportunity already authorizes this animation frame.
        // Only actual new input/state may bypass the next monitor deadline.
        invalidate(false);
        if (!active) {
            // consume() transfers the sole DXGI readiness permit to the UI
            // thread, and invalidate() above schedules the WM_PAINT that must
            // spend it.  Stopping the frame clock here used to clear that
            // permit before WM_PAINT ran, leaving the semantic revision dirty
            // until another wheel/click forced a redraw.  Mark animation as
            // settled, but let completePresentationOpportunity() stop the
            // clock after this already-authorized frame is presented.
            animationRunning_ = false;
            lastAnimationSeconds_ = 0.0;
        }
    }

    void handleClick(WPARAM, LPARAM lParam, bool doubleClick)
    {
        ensureView();
        const auto x = dip(static_cast<float>(GET_X_LPARAM(lParam)));
        const auto y = dip(static_cast<float>(GET_Y_LPARAM(lParam)));
        if (paletteOpen_) {
            handlePaletteClick(x, y, doubleClick);
            return;
        }
        if (queueOpen_ && pointIn(queueDrawerRect(), x, y)) {
            handleQueueClick(x, y);
            return;
        }
        if (const auto hit = ratingHitAt(x, y)) {
            applyRating(*hit);
            return;
        }
        if (activeTab_ == 4 && !historyMode_ &&
            pointIn(librarySearchRect(), x, y)) {
            setSearchFocused(true);
            return;
        }
        if (searchFocused_) setSearchFocused(false);
        if (y < geometry_.tabsHeight) {
            if (doubleClick) lastDoubleClickRegion_ = 1;
            const auto tabWidth = geometry_.width / 5.0f;
            setActiveTab(std::min<std::uint32_t>(
                4,
                static_cast<std::uint32_t>(x / tabWidth)));
            invalidate();
            return;
        }
        if (activeTab_ != 4) {
            if (doubleClick) lastDoubleClickRegion_ = 2;
            if (activeTab_ == 0 && pointIn(favoriteRect_, x, y)) {
                DatabaseService::instance().toggleCurrentFavorite();
                return;
            }
            if (activeTab_ == 1 && pointIn(noteEditRect_, x, y)) {
                NoteEditorWindow::instance().show(GetAncestor(window_, GA_ROOT));
                return;
            }
            if (activeTab_ == 1 && pointIn(personaEditRect_, x, y)) {
                const auto content = localContent();
                PersonaEditorWindow::instance().show(
                    GetAncestor(window_, GA_ROOT),
                    content ? content->biography : std::wstring{});
                return;
            }
            if (activeTab_ == 1 && pointIn(personaMapRect_, x, y)) {
                personaMapVisible_ = !personaMapVisible_;
                invalidate();
                return;
            }
            if (activeTab_ == 2) {
                const auto lyricsSnapshot = LyricsService::instance().snapshot();
                const auto& lyrics = presenterLyrics(lyricsSnapshot);
                const auto revision = lyricsBodyRevision_;
                ensureLyricsTimeline(revision, lyrics);
                const auto viewport = lyricsViewport();
                if (lyricsTimeline_.synchronized() &&
                    pointIn(viewport, x, y)) {
                    constexpr float rowHeight = 34.0f;
                    const auto index = static_cast<std::size_t>(std::max(
                        0.0,
                        std::floor(
                            (y - viewport.top +
                                tabScroll_[2].position()) /
                            rowHeight)));
                    const auto target =
                        lyricsTimeline_.seekTargetSeconds(index);
                    auto control = playback_control::get();
                    if (target && control->playback_can_seek()) {
                        control->playback_seek(*target);
                        lyricActiveLine_ = index;
                        invalidate();
                    }
                }
                return;
            }
            if (activeTab_ == 3) {
                for (std::size_t index = 0; index < artHitRects_.size(); ++index) {
                    if (pointIn(artHitRects_[index], x, y)) {
                        artSelection_ = static_cast<unsigned>(index);
                        invalidate();
                        break;
                    }
                }
            }
            return;
        }

        if (y >= geometry_.tabsHeight && y < geometry_.resultsTop &&
            x < geometry_.leftWidth) {
            if (doubleClick) lastDoubleClickRegion_ = 3;
            const bool nextHistory = x >= geometry_.leftWidth * 0.5f;
            if (nextHistory != historyMode_) {
                historyMode_ = nextHistory;
                syncSearchEdit();
                for (auto& controller : facetScroll_) controller.setPosition(0.0);
                resultScroll_.setPosition(0.0);
                if (historyMode_) ensureHistoryView();
                else ensureView();
                updateScrollBounds();
            }
            invalidate();
            return;
        }

        if (geometry_.compactFacets &&
            x < geometry_.leftWidth &&
            y >= geometry_.facetTop - geometry_.columnsHeight &&
            y < geometry_.facetTop) {
            if (doubleClick) lastDoubleClickRegion_ = 4;
            const auto nextFacet = static_cast<FacetKind>(
                std::min<std::size_t>(
                    2,
                    static_cast<std::size_t>(
                        x / geometry_.facetSelectorWidth)));
            if (nextFacet != compactFacet_) {
                compactFacet_ = nextFacet;
                // The selector underline lives in the static layer while the
                // selected facet body is dynamic. Retire both halves so the
                // click is presented atomically, without waiting for scroll.
                libraryStaticCacheValid_ = false;
                resultWindowCacheValid_ = false;
            }
            invalidate();
            return;
        }

        if (x < geometry_.leftWidth && y >= geometry_.facetTop) {
            if (doubleClick) lastDoubleClickRegion_ = 5;
            const auto facet = geometry_.compactFacets
                ? compactFacet_
                : static_cast<FacetKind>(std::min<std::size_t>(
                    2, static_cast<std::size_t>(x / geometry_.facetWidth)));
            const auto facetIndex = static_cast<std::size_t>(facet);
            const auto row = static_cast<std::size_t>(std::max(
                0.0, std::floor((y - geometry_.facetTop + facetScroll_[facetIndex].position()) / kFacetRowHeight)));
            focusedFacetRow_[facetIndex] = row;
            setFocusRegion(static_cast<FocusRegion>(
                static_cast<std::size_t>(FocusRegion::genre) + facetIndex));
            if (historyMode_) selectHistoryFacet(facet, row);
            else {
                selectFacet(facet, row);
                if (doubleClick) playRandomVisibleAlbum();
            }
            return;
        }
        if (x >= geometry_.leftWidth && y >= geometry_.resultsTop) {
            setFocusRegion(FocusRegion::results);
            if (historyMode_) {
                const auto row = static_cast<std::size_t>(std::max(
                    0.0, std::floor((y - geometry_.resultsTop +
                        resultScroll_.position()) / kHistoryRowHeight)));
                if (row < historyRows_.size()) {
                    resultFocusRow_ = row;
                    selectedHistoryRow_ = row;
                    if (doubleClick) playHistoryRow(row);
                    else invalidate();
                }
                return;
            }
            const auto contentY = y - geometry_.resultsTop + resultScroll_.position();
            const auto entry = resultEntryAt(contentY);
            if (entry == nullptr) return;
            resultFocusRow_ = static_cast<std::size_t>(
                entry - view_.entries().data());
            if (entry->kind == ResultEntryKind::track) {
                if (doubleClick) lastDoubleClickRegion_ = 6;
                const auto& tracks = viewTracks();
                if (entry->trackIndex >= tracks.size()) return;
                selectTrackOccurrence(entry->trackIndex);
                if (doubleClick) playGroup(entry->groupIndex, entry->trackIndex);
            } else if (doubleClick) {
                lastDoubleClickRegion_ = 7;
                playGroup(entry->groupIndex, std::nullopt);
            }
            invalidate();
        }
    }

    void executePaletteItem(const PaletteItem& item)
    {
        paletteOpen_ = false;
        paletteQuery_.clear();
        paletteSelection_ = 0;
        switch (item.kind) {
        case PaletteKind::showNowPlaying:
            revealNowPlaying();
            break;
        case PaletteKind::openListen:
            ListenWindow::instance().show();
            break;
        case PaletteKind::toggleQueue:
            setQueueOpen(!queueOpen_);
            break;
        case PaletteKind::clearFilters:
            setActiveTab(4);
            historyMode_ = false;
            selectedGenre_.clear();
            selectedArtist_.clear();
            selectedAlbum_.clear();
            query_.clear();
            viewDirty_ = true;
            ensureView();
            resultScroll_.setPosition(0.0);
            break;
        case PaletteKind::playPause:
            requestPlaybackPauseToggle();
            break;
        case PaletteKind::toggleFavorite:
            DatabaseService::instance().toggleCurrentFavorite();
            break;
        case PaletteKind::editNote:
            NoteEditorWindow::instance().show(GetAncestor(window_, GA_ROOT));
            break;
        case PaletteKind::editPersona:
            PersonaEditorWindow::instance().show(GetAncestor(window_, GA_ROOT));
            break;
        case PaletteKind::track: {
            setActiveTab(4);
            historyMode_ = false;
            selectedGenre_.clear();
            selectedArtist_.clear();
            selectedAlbum_.clear();
            query_.clear();
            viewDirty_ = true;
            ensureView();
            for (std::size_t groupIndex = 0; groupIndex < view_.groups().size(); ++groupIndex) {
                const auto& indices = view_.groups()[groupIndex].trackIndices;
                if (std::find(indices.begin(), indices.end(), item.trackIndex) != indices.end()) {
                    playGroup(groupIndex, item.trackIndex);
                    break;
                }
            }
            break;
        }
        }
        invalidate();
    }

    void handlePaletteClick(float x, float y, bool doubleClick)
    {
        const auto rectangle = commandPaletteRect();
        if (!pointIn(rectangle, x, y)) {
            paletteOpen_ = false;
            paletteQuery_.clear();
            paletteSelection_ = 0;
            invalidate();
            return;
        }
        if (y < rectangle.top + 44.0f) return;

        const auto& items = commandPaletteItems();
        if (items.empty()) return;
        constexpr float rowHeight = 42.0f;
        const auto visible = static_cast<std::size_t>(std::max(1.0f,
            std::floor((rectangle.bottom - rectangle.top - 44.0f) / rowHeight)));
        const auto first = paletteSelection_ >= visible
            ? paletteSelection_ - visible + 1 : 0;
        const auto row = static_cast<std::size_t>(std::max(0.0f,
            std::floor((y - rectangle.top - 44.0f) / rowHeight)));
        const auto index = first + row;
        if (index >= items.size()) return;
        paletteSelection_ = index;
        if (doubleClick) executePaletteItem(items[index]);
        else invalidate();
    }

    void handleQueueClick(float x, float y)
    {
        const auto drawer = queueDrawerRect();
        if (y < drawer.top + kQueueHeaderHeight) {
            if (x >= drawer.right - 54.0f) {
                setQueueOpen(false);
            } else if (x >= drawer.right - 106.0f) {
                playlist_manager::get()->queue_flush();
                queueSelected_ = pfc::infinite_size;
                queueScroll_.setPosition(0.0);
            }
            invalidate();
            return;
        }

        const auto index = static_cast<t_size>(std::max(0.0,
            std::floor((y - drawer.top - kQueueHeaderHeight + queueScroll_.position()) /
                kQueueRowHeight)));
        const auto count = playlist_manager::get()->queue_get_count();
        if (index < count) queueSelected_ = index;
        invalidate();
    }

    void showContextMenuAt(POINT screenPoint)
    {
        if (activeTab_ != 4 || contextMenuOpen_) return;
        contextMenuOpen_ = true;
        struct ResetOpenFlag final {
            bool& flag;
            ~ResetOpenFlag() { flag = false; }
        } resetOpenFlag{contextMenuOpen_};
        ensureView();

        POINT clientPoint = screenPoint;
        ScreenToClient(window_, &clientPoint);
        const auto x = dip(static_cast<float>(clientPoint.x));
        const auto y = dip(static_cast<float>(clientPoint.y));

        metadb_handle_list handles;
        if (x >= geometry_.leftWidth && y >= geometry_.resultsTop) {
            const auto contentY = y - geometry_.resultsTop + resultScroll_.position();
            const auto* entry = resultEntryAt(contentY);
            if (entry != nullptr && entry->groupIndex < view_.groups().size()) {
                const auto& tracks = viewTracks();
                if (entry->kind == ResultEntryKind::track && entry->trackIndex < tracks.size()) {
                    selectTrackOccurrence(entry->trackIndex);
                    handles.add_item(selectedTrack_);
                } else {
                    for (const auto trackIndex : view_.groups()[entry->groupIndex].trackIndices) {
                        if (trackIndex < tracks.size()) handles.add_item(tracks[trackIndex].handle);
                    }
                }
            }
        }
        if (handles.get_count() == 0 && selectedTrack_.is_valid()) handles.add_item(selectedTrack_);
        if (handles.get_count() == 0) return;

        constexpr UINT kShowNowPlaying = 1;
        constexpr UINT kOpenQueue = 2;
        constexpr UINT kContextBase = 1000;
        constexpr UINT kContextCount = 24000;
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return;

        const bool hasNowPlaying = playback_control::get()->is_playing();
        AppendMenuW(menu, MF_STRING | (hasNowPlaying ? 0U : MF_GRAYED),
            kShowNowPlaying, L"Show now playing");
        AppendMenuW(menu, MF_STRING, kOpenQueue, L"Open playback queue");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        auto manager = contextmenu_manager::g_create();
        constexpr unsigned flags = contextmenu_manager::flag_show_shortcuts |
            contextmenu_manager::flag_show_shortcuts_global |
            contextmenu_manager::flag_view_full;
        manager->init_context(handles, flags);
        manager->win32_build_menu(menu, kContextBase, kContextCount);
        menu_helpers::win32_auto_mnemonics(menu);

        auto root = GetAncestor(window_, GA_ROOT);
        if (root == nullptr) root = window_;
        SetForegroundWindow(root);
        const auto selected = TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screenPoint.x, screenPoint.y, window_, nullptr);
        if (selected == kShowNowPlaying) revealNowPlaying();
        else if (selected == kOpenQueue) {
            setQueueOpen(true);
        } else if (selected >= kContextBase && selected < kContextBase + kContextCount) {
            manager->execute_by_id(selected - kContextBase);
        }
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
    }

    void removeSelectedQueueItem()
    {
        const auto count = playlist_manager::get()->queue_get_count();
        if (queueSelected_ >= count) return;
        playlist_manager::get()->queue_remove_mask(pfc::bit_array_one(queueSelected_));
        const auto nextCount = playlist_manager::get()->queue_get_count();
        if (nextCount == 0) queueSelected_ = pfc::infinite_size;
        else queueSelected_ = std::min(queueSelected_, nextCount - 1);
        updateScrollBounds();
        invalidate();
    }

    void selectHistoryFacet(FacetKind facet, std::size_t row)
    {
        auto requested = [row](const std::vector<FacetItem>& items)
            -> std::optional<std::wstring> {
            if (row == 0) return std::wstring{};
            if (row - 1 >= items.size()) return std::nullopt;
            return items[row - 1].name;
        };
        bool changed = false;
        switch (facet) {
        case FacetKind::genre: {
            const auto next = requested(historyPeriods_);
            if (!next || sameText(*next, selectedHistoryPeriod_)) return;
            selectedHistoryPeriod_ = *next;
            changed = true;
            break;
        }
        case FacetKind::artist: {
            const auto next = requested(historyArtists_);
            if (!next || sameText(*next, selectedHistoryArtist_)) return;
            selectedHistoryArtist_ = *next;
            selectedHistoryAlbum_.clear();
            facetScroll_[2].setPosition(0.0);
            changed = true;
            break;
        }
        case FacetKind::album: {
            const auto next = requested(historyAlbums_);
            if (!next || sameText(*next, selectedHistoryAlbum_)) return;
            selectedHistoryAlbum_ = *next;
            changed = true;
            break;
        }
        }
        if (!changed) return;
        resultScroll_.setPosition(resultScroll_.position());
        historyDirty_ = true;
        ensureHistoryView();
        invalidate();
    }

    void playHistoryRow(std::size_t rowIndex)
    {
        if (rowIndex >= historyRows_.size()) return;
        const auto& row = historyRows_[rowIndex];
        const auto& tracks = LibraryModel::instance().tracks();
        if (!historySnapshot_ || row.eventIndex >= historySnapshot_->events.size()) return;
        const auto& event = historySnapshot_->events[row.eventIndex];
        const LibraryTrack* selected = nullptr;
        for (const auto& track : tracks) {
            bool matches = false;
            if (!event.path.empty()) {
                if (track.handle.is_empty() ||
                    track.handle->get_subsong_index() != event.subsong) continue;
                try {
                    const auto native = filesystem::g_get_native_path(track.handle->get_path());
                    matches = normalizeWindowsPath(utf8ToWide(native.c_str())) ==
                        normalizeWindowsPath(utf8ToWide(event.path));
                } catch (...) { continue; }
            } else {
                // Legacy rows without a location may use tags only when the
                // match is unique. Identical labels never select an arbitrary file.
                matches = sameText(track.artist, row.artist) &&
                    sameText(track.album, row.album) && sameText(track.title, row.title);
            }
            if (!matches) continue;
            if (selected && event.path.empty()) return;
            selected = &track;
            if (!event.path.empty()) break;
        }
        if (!selected) return;

        metadb_handle_list handles;
        handles.add_item(selected->handle);
        auto playlists = playlist_manager::get();
        const auto playlist = playlists->find_or_create_playlist_unlocked("NekoRoX History");
        if (playlist == pfc_infinite) return;
        playlists->playlist_undo_backup(playlist);
        playlists->playlist_clear(playlist);
        playlists->playlist_insert_items(playlist, 0, handles, bit_array_false());
        playlists->set_active_playlist(playlist);
        playlists->playlist_clear_selection(playlist);
        playlists->playlist_set_selection_single(playlist, 0, true);
        playlists->playlist_set_focus_item(playlist, 0);
        playlists->playlist_execute_default_action(playlist, 0);
    }

    void selectFacet(FacetKind facet, std::size_t row)
    {
        auto requested = [row](const std::vector<FacetItem>& items)
            -> std::optional<FacetSelection> {
            if (row == 0) return FacetSelection{};
            if (row - 1 >= items.size()) return std::nullopt;
            return FacetSelection::fromValue(items[row - 1].name);
        };
        bool changed = false;
        switch (facet) {
        case FacetKind::genre: {
            const auto next = requested(view_.genres());
            if (!next || *next == selectedGenre_) return;
            selectedGenre_ = *next;
            selectedArtist_.clear();
            selectedAlbum_.clear();
            facetScroll_[1].setPosition(0.0);
            facetScroll_[2].setPosition(0.0);
            changed = true;
            break;
        }
        case FacetKind::artist: {
            const auto next = requested(view_.artists());
            if (!next || *next == selectedArtist_) return;
            selectedArtist_ = *next;
            selectedAlbum_.clear();
            facetScroll_[2].setPosition(0.0);
            changed = true;
            break;
        }
        case FacetKind::album: {
            const auto next = requested(view_.albums());
            if (!next || *next == selectedAlbum_) return;
            selectedAlbum_ = *next;
            changed = true;
            break;
        }
        }
        if (!changed) return;
        resultScroll_.setPosition(resultScroll_.position());
        viewDirty_ = true;
        ensureView();
        invalidate();
    }

    const ResultEntry* resultEntryAt(double contentY) const
    {
        const auto& entries = view_.entries();
        const auto iterator = std::lower_bound(
            entries.begin(), entries.end(), contentY,
            [](const ResultEntry& entry, double y) {
                return entry.top + entry.height <= y;
            });
        if (iterator == entries.end() || contentY < iterator->top) return nullptr;
        return &*iterator;
    }

    bool handleKeyDown(WPARAM key)
    {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (control && key == 'Q') {
            paletteOpen_ = false;
            paletteQuery_.clear();
            setQueueOpen(!queueOpen_);
            return true;
        }
        if (key == VK_TAB && activeTab_ == 4 && !queueOpen_ && !paletteOpen_) {
            cycleFocus((GetKeyState(VK_SHIFT) & 0x8000) == 0);
            return true;
        }
        if (control && key == 'K') {
            SetFocus(window_);
            paletteOpen_ = true;
            paletteQuery_.clear();
            paletteSelection_ = 0;
            invalidate();
            return true;
        }
        if (paletteOpen_) {
            const auto& items = commandPaletteItems();
            if (key == VK_ESCAPE) {
                paletteOpen_ = false;
                paletteQuery_.clear();
                paletteSelection_ = 0;
            } else if (key == VK_UP && !items.empty()) {
                paletteSelection_ = paletteSelection_ == 0
                    ? items.size() - 1 : paletteSelection_ - 1;
            } else if (key == VK_DOWN && !items.empty()) {
                paletteSelection_ = (paletteSelection_ + 1) % items.size();
            } else if (key == VK_RETURN && !items.empty()) {
                paletteSelection_ = std::min(paletteSelection_, items.size() - 1);
                executePaletteItem(items[paletteSelection_]);
                return true;
            } else {
                return true;
            }
            invalidate();
            return true;
        }
        if (key == VK_ESCAPE && artInspectOpen_) {
            artInspectOpen_ = false;
            invalidate();
            return true;
        }
        if (key == VK_ESCAPE && queueOpen_) {
            setQueueOpen(false);
            return true;
        }
        if (queueOpen_ && key == VK_DELETE) {
            removeSelectedQueueItem();
            return true;
        }
        if (queueOpen_ && (key == VK_UP || key == VK_DOWN)) {
            const auto count = playlist_manager::get()->queue_get_count();
            if (count == 0) return true;
            if (queueSelected_ >= count) queueSelected_ = 0;
            else if (key == VK_UP && queueSelected_ > 0) --queueSelected_;
            else if (key == VK_DOWN && queueSelected_ + 1 < count) ++queueSelected_;
            const auto drawer = queueDrawerRect();
            const auto viewportHeight = drawer.bottom - drawer.top - kQueueHeaderHeight;
            queueScroll_.setPosition(queueSelected_ * kQueueRowHeight - viewportHeight * 0.5);
            invalidate();
            return true;
        }
        if (!control && handleFocusedLibraryKey(key)) return true;
        if (!control && !queueOpen_ && activeTab_ == 3) {
            std::optional<CoverflowNavigationKey> navigation;
            switch (key) {
            case VK_LEFT:
            case VK_UP:
                navigation = CoverflowNavigationKey::previous;
                break;
            case VK_RIGHT:
            case VK_DOWN:
                navigation = CoverflowNavigationKey::next;
                break;
            case VK_HOME:
                navigation = CoverflowNavigationKey::first;
                break;
            case VK_END:
                navigation = CoverflowNavigationKey::last;
                break;
            case VK_PRIOR:
                navigation = CoverflowNavigationKey::pagePrevious;
                break;
            case VK_NEXT:
                navigation = CoverflowNavigationKey::pageNext;
                break;
            default:
                break;
            }
            if (navigation && coverflow_.navigate(*navigation)) {
                artSelection_ = static_cast<unsigned>(
                    coverflow_.selectedIndex());
                if (coverflow_.active()) startAnimation(qpcSeconds());
                invalidate();
                return true;
            }
        }
        auto& activeQuery = historyMode_ ? historyQuery_ : query_;
        if (key == VK_ESCAPE && searchFocused_ && activeQuery.empty()) {
            setSearchFocused(false);
            return true;
        }
        if (key == VK_ESCAPE && !activeQuery.empty()) {
            activeQuery.clear();
            onQueryChanged();
            syncSearchEdit();
            return true;
        }
        if (key == VK_RETURN && activeTab_ == 4) {
            if (historyMode_ && selectedHistoryRow_ < historyRows_.size()) {
                playHistoryRow(selectedHistoryRow_);
                return true;
            }
            if (!historyMode_ && !view_.groups().empty()) {
                playGroup(0, std::nullopt);
                return true;
            }
        }
        return false;
    }

    bool handleCharacter(wchar_t character)
    {
        if (paletteOpen_) {
            if (character == L'\b') {
                if (!paletteQuery_.empty()) paletteQuery_.pop_back();
            } else if (character >= 0x20 && character != 0x7f && paletteQuery_.size() < 128) {
                paletteQuery_.push_back(character);
            } else {
                return true;
            }
            paletteSelection_ = 0;
            invalidate();
            return true;
        }
        if (activeTab_ != 4) return false;
        if (focusRegion_ != FocusRegion::search && character == L' ') return true;
        if (searchEdit_ && GetFocus() != searchEdit_ &&
                character >= 0x20 && character != 0x7f) {
            setSearchFocused(true);
        }
        auto& activeQuery = historyMode_ ? historyQuery_ : query_;
        if (character == L'\b') {
            if (!activeQuery.empty()) {
                activeQuery.pop_back();
                onQueryChanged();
                syncSearchEdit();
            }
            return true;
        }
        if (character >= 0x20 && character != 0x7f && activeQuery.size() < 128) {
            activeQuery.push_back(character);
            onQueryChanged();
            syncSearchEdit();
            return true;
        }
        return false;
    }

    void onQueryChanged()
    {
        focusedFacetRow_.fill(0);
        resultFocusRow_ = 0;
        if (historyMode_) {
            selectedHistoryArtist_.clear();
            selectedHistoryAlbum_.clear();
            for (auto& controller : facetScroll_) controller.setPosition(0.0);
            resultScroll_.setPosition(0.0);
            historyDirty_ = true;
            ensureHistoryView();
            invalidate();
            return;
        }
        selectedGenre_.clear();
        selectedArtist_.clear();
        selectedAlbum_.clear();
        for (auto& controller : facetScroll_) controller.setPosition(0.0);
        resultScroll_.setPosition(0.0);
        viewDirty_ = true;
        ensureView();
        invalidate();
    }

    void playRandomVisibleAlbum()
    {
        ++randomAlbumRequests_;
        if (view_.groups().empty()) return;
        std::uniform_int_distribution<std::size_t> distribution(0, view_.groups().size() - 1);
        playGroup(
            distribution(random_),
            std::nullopt,
            PostPlaybackAction::revealNowPlaying);
    }

    bool playGroup(
        std::size_t groupIndex,
        std::optional<std::size_t> requestedTrack,
        PostPlaybackAction postPlaybackAction =
            PostPlaybackAction::none)
    {
        ++playGroupRequests_;
        pendingRandomAlbumReveal_.release();
        if (groupIndex >= view_.groups().size()) return false;
        const auto& tracks = viewTracks();
        const auto& group = view_.groups()[groupIndex];
        if (group.trackIndices.empty()) return false;

        metadb_handle_list handles;
        std::size_t startIndex = 0;
        for (std::size_t index = 0; index < group.trackIndices.size(); ++index) {
            const auto trackIndex = group.trackIndices[index];
            if (trackIndex >= tracks.size()) continue;
            handles.add_item(tracks[trackIndex].handle);
            if (requestedTrack && *requestedTrack == trackIndex) startIndex = index;
        }
        if (handles.get_count() == 0) return false;

        const auto temporaryGroup = temporaryAlbumView_ &&
            std::all_of(
                group.trackIndices.begin(),
                group.trackIndices.end(),
                [&](std::size_t trackIndex) {
                    return trackIndex < temporaryTracks_.size();
                });
        if (temporaryGroup) {
            KeepPlayingService::instance().clearContext();
            return playTemporaryAlbumItem(startIndex);
        }

        std::vector<PlaybackAlbum> playbackContext;
        playbackContext.reserve(view_.groups().size());
        std::size_t capturedAlbumIndex = 0;
        for (std::size_t contextIndex = 0;
            contextIndex < view_.groups().size(); ++contextIndex) {
            const auto& contextGroup = view_.groups()[contextIndex];
            PlaybackAlbum contextAlbum;
            contextAlbum.reserve(contextGroup.trackIndices.size());
            for (const auto trackIndex : contextGroup.trackIndices) {
                if (trackIndex < tracks.size() &&
                        tracks[trackIndex].handle.is_valid()) {
                    contextAlbum.push_back(tracks[trackIndex].handle);
                }
            }
            if (!contextAlbum.empty()) {
                if (contextIndex == groupIndex) {
                    capturedAlbumIndex = playbackContext.size();
                }
                playbackContext.push_back(std::move(contextAlbum));
            }
        }
        KeepPlayingService::instance().captureLibraryContext(
            std::move(playbackContext), capturedAlbumIndex);

        auto playlists = playlist_manager::get();
        const auto playlist = playlists->find_or_create_playlist_unlocked("NekoRoX Selection");
        if (playlist == pfc_infinite) return false;
        playlists->playlist_undo_backup(playlist);
        playlists->playlist_clear(playlist);
        playlists->playlist_insert_items(playlist, 0, handles, bit_array_false());

        playlists->set_active_playlist(playlist);
        playlists->playlist_clear_selection(playlist);
        playlists->playlist_set_selection_single(
            playlist, startIndex, true);
        playlists->playlist_set_focus_item(playlist, startIndex);
        if (postPlaybackAction == PostPlaybackAction::revealNowPlaying) {
            pendingRandomAlbumReveal_ = handles[startIndex];
        }
        if (!playlists->playlist_execute_default_action(
                playlist, startIndex)) {
            pendingRandomAlbumReveal_.release();
            return false;
        }
        return true;
    }

    bool playFirstLibraryAlbumForQa()
    {
        const auto& tracks = LibraryModel::instance().tracks();
        if (tracks.empty()) return false;

        metadb_handle_list handles;
        const auto& first = tracks.front();
        for (const auto& track : tracks) {
            if (!sameText(track.albumArtist, first.albumArtist) ||
                !sameText(track.album, first.album)) {
                if (handles.get_count() > 0) break;
                continue;
            }
            handles.add_item(track.handle);
        }
        if (handles.get_count() == 0) return false;

        auto playlists = playlist_manager::get();
        const auto playlist = playlists->find_or_create_playlist_unlocked(
            "NekoRoX Selection");
        if (playlist == pfc_infinite) return false;
        playlists->playlist_undo_backup(playlist);
        playlists->playlist_clear(playlist);
        playlists->playlist_insert_items(
            playlist, 0, handles, bit_array_false());
        playlists->set_active_playlist(playlist);
        playlists->playlist_clear_selection(playlist);
        playlists->playlist_set_selection_single(playlist, 0, true);
        playlists->playlist_set_focus_item(playlist, 0);
        const auto started = playlists->playlist_execute_default_action(
            playlist, 0);
        if (started) {
            // Re-check after the default action has committed its playing
            // location. Some output pipelines publish the new-track callback
            // before playlist_manager exposes the new location.
            fb2k::inMainThread([] { notifyTemporaryAlbumPlaybackChanged(); });
        }
        return started;
    }

    void refreshNowPlaying()
    {
        playingOccurrence_.reset();
        const auto current = Runtime::instance().currentTrackHandle();
        const auto hadTrack = surfaceTrack_.has_value();
        const auto previous = hadTrack ? surfaceTrack_->handle : metadb_handle_ptr{};
        if (!current.is_valid()) {
            nowPlaying_.release();
            surfaceTrack_.reset();
        } else {
            nowPlaying_ = current;
            auto& library = LibraryModel::instance();
            bool indexed = false;
            if (!temporaryAlbumView_) {
                const auto snapshot = library.sharedSnapshot();
                if (snapshot && snapshot->tracks &&
                        snapshot->generation == observedGeneration_) {
                    std::string key(current->get_path());
                    key.push_back('\x1f');
                    key += std::to_string(current->get_subsong_index());
                    const auto found = snapshot->handleIndex.find(key);
                    if (found != snapshot->handleIndex.end() &&
                            found->second < snapshot->tracks->size()) {
                        surfaceTrack_ = (*snapshot->tracks)[found->second];
                        playingOccurrence_ = found->second;
                        indexed = true;
                    }
                }
            } else if (libraryTrackIndexRevision_ == viewRevision_) {
                const auto identity = utf8ToWide(current->get_path()) +
                    L"\x1f" + std::to_wstring(current->get_subsong_index());
                const auto found = libraryTrackIndex_.find(identity);
                const auto& tracks = viewTracks();
                if (found != libraryTrackIndex_.end() &&
                        found->second < tracks.size()) {
                    surfaceTrack_ = tracks[found->second];
                    playingOccurrence_ = found->second;
                    indexed = true;
                }
                if (const auto item = temporaryAlbumPlayingItem(); item &&
                        *item < temporaryTracks_.size() && tracks[*item].handle == current) {
                    surfaceTrack_ = tracks[*item];
                    playingOccurrence_ = *item;
                    indexed = true;
                }
            }
            if (indexed) {
                library.recordIndexedNowPlayingHit();
            } else {
                // Uses the immutable script bundle compiled at model start;
                // volume and position callbacks never compile titleformat.
                surfaceTrack_ = library.describeTrack(current);
            }
        }
        const auto changed = hadTrack != surfaceTrack_.has_value() ||
            (surfaceTrack_ && previous != surfaceTrack_->handle);
        if (changed) {
            surfaceArtworkIdentity_ = surfaceTrack_
                ? artworkTrackIdentity(surfaceTrack_->handle) : std::wstring{};
            resetReaderText();
            ++readerOwner_;
            for (auto& controller : tabScroll_) controller.setPosition(0.0);
            artInspectOpen_ = false;
            glyphArtworkIdentity_ = nullptr;
            glyphArtworkKey_.clear();
            artSelection_ = 0;
            artHitRects_.clear();
            artViewport_ = {};
            coverflow_.setItemCount(0);
            lyricsTimelineRevision_ = 0;
            lyricsTimeline_ = {};
            lyricsLinesWide_.clear();
            lyricsLineLayouts_.clear();
            lyricsLineLayoutRevision_ = 0;
            lyricsLineLayoutWidth_ = 0.0f;
            lyricActiveLine_.reset();
            personaMapVisible_ = false;
            biographyPreparedRevision_ = 0;
            biographyPrepared_.clear();
            biographyAliasesPrepared_.clear();
            biographyReviewPrepared_.clear();
            biographyNotePrepared_.clear();
            ThemeService::instance().resetForTrack(
                surfaceArtworkIdentity_);
            ChromeController::instance().refreshAccent();
            startAnimation(qpcSeconds());
            notifyNativePanelsPlaybackChanged();
        }
    }

    void invalidateCurrentArtwork()
    {
        if (!surfaceTrack_) return;
        artBitmaps_.clear();
        artBitmapBytes_ = 0;
        glyphArtworkIdentity_ = nullptr;
        glyphArtworkKey_.clear();
        ThemeService::instance().resetForTrack(surfaceArtworkIdentity_);
        ChromeController::instance().refreshAccent();
        Runtime::instance().refreshPresentationState();
        startAnimation(qpcSeconds());
        invalidate();
    }

    bool revealNowPlaying(bool refresh = true)
    {
        if (refresh) refreshNowPlaying();
        if (!nowPlaying_.is_valid()) return false;
        setActiveTab(4);
        historyMode_ = false;
        ensureView();
        updateScrollBounds();

        auto findEntry = [&]() -> const ResultEntry* {
            const auto& tracks = viewTracks();
            for (const auto& entry : view_.entries()) {
                if (entry.kind == ResultEntryKind::track &&
                    entry.trackIndex < tracks.size() &&
                    isPlayingOccurrence(entry.trackIndex)) {
                    return &entry;
                }
            }
            return nullptr;
        };

        auto entry = findEntry();
        if (entry == nullptr) {
            selectedGenre_.clear();
            selectedArtist_.clear();
            selectedAlbum_.clear();
            query_.clear();
            viewDirty_ = true;
            ensureView();
            entry = findEntry();
        }
        if (entry == nullptr) return false;

        const auto viewportHeight = std::max(0.0f, geometry_.height - geometry_.resultsTop);
        const auto center = entry->top + entry->height * 0.5 - viewportHeight * 0.5;
        resultScroll_.setPosition(center);
        selectTrackOccurrence(entry->trackIndex);
        invalidate();
        return true;
    }

    LRESULT nowPlayingCenterErrorMilliDip()
    {
        if (!nowPlaying_.is_valid()) return -1;
        ensureView();
        const auto& tracks = viewTracks();
        const auto iterator = std::find_if(
            view_.entries().begin(), view_.entries().end(),
            [&](const ResultEntry& entry) {
                return entry.kind == ResultEntryKind::track &&
                    entry.trackIndex < tracks.size() &&
                    isPlayingOccurrence(entry.trackIndex);
            });
        if (iterator == view_.entries().end()) return -1;

        const auto viewportHeight = std::max(
            0.0f, geometry_.height - geometry_.resultsTop);
        const auto maximum = std::max(
            0.0,
            view_.resultHeight() - static_cast<double>(viewportHeight));
        const auto ideal = std::clamp(
            iterator->top + iterator->height * 0.5 -
                static_cast<double>(viewportHeight) * 0.5,
            0.0,
            maximum);
        return static_cast<LRESULT>(std::llround(
            std::abs(resultScroll_.position() - ideal) * 1000.0));
    }

    HWND window_ = nullptr;
    Accessible* accessible_ = nullptr;
    float dpi_ = 96.0f;
    Geometry geometry_{};
    std::uint32_t activeTab_ = 4;
    bool libraryOnly_ = false;
    std::uint64_t observedGeneration_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t observedLayoutRevision_ = 0;
    std::uint64_t libraryCountRevision_ = 1;
    std::uint64_t observedTemporaryAlbumGeneration_ =
        std::numeric_limits<std::uint64_t>::max();
    bool viewDirty_ = true;
    bool compactAlbumHeaders_ = false;
    WorkspaceViewModel view_;
    bool temporaryAlbumView_ = false;
    std::vector<LibraryTrack> temporaryTracks_;
    std::vector<LibraryTrack> combinedTracks_;
    FacetSelection savedLibraryGenre_;
    FacetSelection savedLibraryArtist_;
    FacetSelection savedLibraryAlbum_;
    std::wstring savedLibraryQuery_;
    FacetSelection selectedGenre_;
    FacetSelection selectedArtist_;
    FacetSelection selectedAlbum_;
    std::wstring query_;
    HWND searchEdit_ = nullptr;
    HBRUSH searchBrush_ = nullptr;
    HFONT searchFont_ = nullptr;
    bool syncingSearchEdit_ = false;
    bool searchFocused_ = false;
    FocusRegion focusRegion_ = FocusRegion::none;
    std::array<std::size_t, 3> focusedFacetRow_{};
    std::size_t resultFocusRow_ = 0;
    FacetKind compactFacet_ = FacetKind::genre;
    bool historyMode_ = false;
    bool historyDirty_ = true;
    std::shared_ptr<const HistorySnapshot> historySnapshot_;
    std::vector<FacetItem> historyPeriods_;
    std::vector<FacetItem> historyArtists_;
    std::vector<FacetItem> historyAlbums_;
    std::vector<HistoryRow> historyRows_;
    std::wstring selectedHistoryPeriod_ = L"Current session";
    std::wstring selectedHistoryArtist_;
    std::wstring selectedHistoryAlbum_;
    std::wstring historyQuery_;
    std::size_t selectedHistoryRow_ = std::numeric_limits<std::size_t>::max();
    metadb_handle_ptr selectedTrack_;
    metadb_handle_ptr nowPlaying_;
    std::optional<std::size_t> selectedOccurrence_;
    std::optional<std::size_t> playingOccurrence_;
    std::size_t selectedOccurrenceOrdinal_ = 0;
    std::uint64_t selectedOccurrenceGeneration_ = 0;
    bool selectedOccurrenceTemporary_ = false;
    bool contextMenuOpen_ = false;
    std::optional<LibraryTrack> surfaceTrack_;
    std::wstring surfaceArtworkIdentity_;
    D2D1_RECT_F infoRatingRect_{};
    std::vector<RatingOverride> ratingOverrides_;
    bool ratingUnavailableNotified_ = false;
    ArtCache artCache_;
    LocalContentCache localContentCache_;
    WindowFrameClock frameClock_;
    PresentationRevision presentationRevision_;
    MotionCoordinator motion_;
    GlyphFlowRenderer glyphFlowRenderer_;
    ArtworkTransitionRenderer surfaceArtworkTransition_;
    const ArtPixels* glyphArtworkIdentity_ = nullptr;
    std::wstring glyphArtworkKey_;
    std::array<nekorox::ScrollController, 3> facetScroll_{};
    std::array<std::wstring, 3> facetAllLabels_{};
    std::array<nekorox::ScrollController, 4> tabScroll_{};
    nekorox::ScrollController resultScroll_;
    nekorox::ScrollController queueScroll_;
    std::vector<OverlayScrollbarState> overlayScrollbars_;
    nekorox::ScrollController* overlayDraggingController_ = nullptr;
    bool overlayScrollbarDragging_ = false;
    bool overlayMouseTracking_ = false;
    std::array<double, 9> lastWheelSeconds_{};
    double lastAnimationSeconds_ = 0.0;
    bool animationRunning_ = false;
    bool dispatchingAuthorizedPaint_ = false;
    bool themeWasActive_ = false;
    bool queueOpen_ = false;
    t_size queueSelected_ = pfc::infinite_size;
    bool queueSnapshotDirty_ = true;
    std::uint64_t queueSnapshotRevision_ = 0;
    std::vector<QueueDisplayItem> queueDisplayItems_;
    std::unordered_map<std::wstring, std::size_t> libraryTrackIndex_;
    std::uint64_t libraryTrackIndexRevision_ = 0;
    bool paletteOpen_ = false;
    std::wstring paletteQuery_;
    std::size_t paletteSelection_ = 0;
    bool paletteCacheValid_ = false;
    std::wstring paletteCacheQuery_;
    std::uint64_t paletteCacheViewRevision_ = 0;
    std::uint64_t paletteCacheUserGeneration_ = 0;
    std::vector<PaletteItem> paletteCacheItems_;
    std::mt19937 random_;
    metadb_handle_ptr pendingRandomAlbumReveal_;
    std::uint64_t randomAlbumRevealAttempts_ = 0;
    std::uint64_t randomAlbumRevealSuccesses_ = 0;
    std::uint64_t randomAlbumRequests_ = 0;
    std::uint64_t playGroupRequests_ = 0;
    std::uint32_t lastDoubleClickRegion_ = 0;
    ::nekorox::LyricsTimelineV1 lyricsTimeline_;
    std::shared_ptr<const LyricsSnapshot> lyricsPresenterSnapshot_;
    std::wstring lyricsPresenterCache_;
    std::uint64_t lyricsBodyRevision_ = 1;
    std::uint64_t lyricsTimelineRevision_ = 0;
    std::vector<std::wstring> lyricsLinesWide_;
    std::vector<std::shared_ptr<const RetainedTextDocument>>
        lyricsLineLayouts_;
    std::uint64_t lyricsLineLayoutRevision_ = 0;
    float lyricsLineLayoutWidth_ = 0.0f;
    std::shared_ptr<TextLayoutBudget> readerBudget_ = std::make_shared<TextLayoutBudget>();
    TextLayoutCache textLayoutCache_{readerBudget_, 2048U};
    AsyncTextView readerText_{readerBudget_};
    std::shared_ptr<const AsyncTextFrame> readerFrame_;
    std::shared_ptr<const AsyncTextBatch> readerBatch_;
    std::shared_ptr<const LocalTrackContent> readerContent_;
    std::shared_ptr<const PersonaSnapshot> readerPersona_;
    std::shared_ptr<const TrackUserStateSnapshot> readerUserState_;
    std::shared_ptr<const LyricsSnapshot> readerLyrics_;
    std::uint64_t readerOwner_ = 1, readerRevision_ = 0;
    std::uint64_t readerSourceRevision_ = 0;
    bool readerDemand_ = false, readerFailed_ = false;
    enum class ReaderRoute { unknown, synchronous, asynchronous, empty };
    ReaderRoute readerRoute_ = ReaderRoute::unknown;
    bool readerAliases_ = false, readerShowMap_ = false;
    double readerPendingWheel_ = 0;
    D2D1_RECT_F readerViewport_{};
    std::uint64_t biographyPreparedRevision_ = 0;
    std::wstring biographyPrepared_;
    std::wstring biographyAliasesPrepared_;
    std::wstring biographyReviewPrepared_;
    std::wstring biographyNotePrepared_;
    std::optional<std::size_t> lyricActiveLine_;
    CoverflowModel coverflow_;
    D2D1_RECT_F artViewport_{};
    bool coverflowPointerDown_ = false;
    bool coverflowPointerMoved_ = false;
    float coverflowPointerStartX_ = 0.0f;
    float coverflowPointerLastX_ = 0.0f;
    double coverflowPointerLastSeconds_ = 0.0;
    std::optional<std::size_t> coverflowPointerItem_;
    bool artInspectOpen_ = false;
    unsigned artSelection_ = 0;
    std::vector<D2D1_RECT_F> artHitRects_;
    D2D1_RECT_F favoriteRect_{};
    D2D1_RECT_F noteEditRect_{};
    D2D1_RECT_F personaEditRect_{};
    D2D1_RECT_F personaMapRect_{};
    bool personaMapVisible_ = false;
    titleformat_object::ptr queueTitleScript_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<IDWriteFactory> writeFactory_;
    MonitorTextRendering textRendering_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> target_;
    ComPtr<IDXGISwapChain2> swapChain_;
    ComPtr<ID2D1Bitmap1> swapChainBitmap_;
    std::uint32_t swapChainWidth_ = 0;
    std::uint32_t swapChainHeight_ = 0;
    std::uint64_t workspaceSurfaceLifetime_ = 0;
    std::uint64_t workspaceSwapchainEpoch_ = 0;
    std::uint64_t renderRecoveryEpoch_ = 0;
    HMONITOR renderMonitor_ = nullptr;
    bool adapterCheckRequired_ = true;
    HRESULT d2dFactoryResult_ = E_PENDING;
    HRESULT writeFactoryResult_ = E_PENDING;
    HRESULT targetResult_ = E_PENDING;
    HRESULT endDrawResult_ = E_PENDING;
    std::int64_t scrollQaStartedQpc_ = 0;
    std::uint64_t scrollQaLatencyMicros_ = 0;
    std::uint64_t scrollQaDwmLatencyMicros_ = 0;
    bool scrollQaArmed_ = false;
    ComPtr<IDWriteTextFormat> normalFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> strongFormat_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> heroTitleFormat_;
    ComPtr<IDWriteTextFormat> tabFormat_;
    ComPtr<IDWriteTextFormat> rightFormat_;
    ComPtr<IDWriteTextFormat> rightStrongFormat_;
    ComPtr<IDWriteTextFormat> wrapFormat_;
    ComPtr<IDWriteTextFormat> wrapSmallFormat_;
    ComPtr<IDWriteTextFormat> lyricFormat_;
    goden::pixel::PixelIconPainter pixelIconPainter_;
    goden::pixel::PixelIconPainter::Handle starOutlineIcon_{};
    goden::pixel::PixelIconPainter::Handle starFilledIcon_{};
    goden::pixel::PixelIconPainter::Handle heartOutlineIcon_{};
    goden::pixel::PixelIconPainter::Handle heartFilledIcon_{};
    float pixelIconDpi_ = 0.0f;
    std::unordered_map<ArtworkBitmapKey, ArtBitmapEntry,
        ArtworkBitmapKeyHash> artBitmaps_;
    std::size_t artBitmapBytes_ = 0;
    std::uint64_t artBitmapUseCounter_ = 0;
    ComPtr<ID2D1Bitmap1> libraryStaticBitmap_;
    ComPtr<ID2D1Bitmap1> resultWindowBitmap_;
    struct ResultTile { ComPtr<ID2D1Bitmap1> bitmap;std::int64_t index=-1; };
    std::array<ResultTile,12> resultTiles_{};
    struct FacetCache {
        ComPtr<ID2D1Bitmap1> bitmap;bool valid=false;
        std::uint64_t view=0,theme=0;double offset=0;
        FacetSelection selection;float left=0,top=0,dpi=0;
    };
    std::array<FacetCache,3> facetCaches_{};
    unsigned resultTileWidth_=0;float resultTileDpi_=0,resultTileLeft_=0;
    bool buildingResultTextCache_=false;
    bool buildingNeutralFacetCache_=false;
    std::uint64_t resultTileBuilds_=0,resultTileHits_=0,resultDirectFrames_=0,facetCacheHits_=0,facetCacheBuilds_=0;
    double maximumTileBuildMs_=0;
    std::uint64_t viewRevision_ = 0;
    std::uint64_t libraryStaticViewRevision_ = 0;
    std::uint64_t libraryStaticThemeGeneration_ = 0;
    FacetKind libraryStaticCompactFacet_ = FacetKind::genre;
    bool libraryStaticCacheValid_ = false;
    double resultWindowTop_ = 0.0;
    double resultWindowHeight_ = 0.0;
    double resultWindowLastOffset_ = 0.0;
    std::uint64_t resultWindowViewRevision_ = 0;
    std::uint64_t resultWindowThemeGeneration_ = 0;
    metadb_handle_ptr resultWindowSelected_;
    metadb_handle_ptr resultWindowPlaying_;
    bool resultWindowCacheValid_ = false;
    bool resultArtworkRefreshPending_ = false;
    std::uint64_t themeGeneration_ = 0;
    std::uint64_t themeTransitionFrames_ = 0;
    std::uint64_t lastThemeTransitionFrames_ = 0;
    bool themeTransitionCounting_ = false;
};

WorkspacePanel::WorkspacePanel() : impl_(std::make_unique<Impl>()) {}
WorkspacePanel::~WorkspacePanel() = default;

const GUID& WorkspacePanel::get_extension_guid() const
{
    return kWorkspaceGuid;
}

void WorkspacePanel::get_name(pfc::string_base& output) const
{
    output = "GODENplayer Workspace";
}

void WorkspacePanel::get_category(pfc::string_base& output) const
{
    output = "Panels";
}

unsigned WorkspacePanel::get_type() const
{
    return uie::type_panel;
}

uie::container_window_v3_config WorkspacePanel::get_window_config()
{
    uie::container_window_v3_config config(L"NekoRoXWorkspaceV3", false, CS_DBLCLKS);
    config.class_background = nullptr;
    return config;
}

LRESULT WorkspacePanel::on_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return impl_->onMessage(window, message, wParam, lParam);
}

void notifyWorkspaceModelChanged()
{
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kModelChangedMessage, 0, 0);
    }
}

void notifyWorkspacePlaybackChanged(bool positionOnly)
{
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::workspace,
        positionOnly ? UiDirty::position : UiDirty::semantic);
}

void notifyWorkspacePlaybackSettled()
{
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kPlaybackChangedMessage, 2, 0);
    }
}

void notifyWorkspaceArtworkChanged()
{
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) {
            PostMessageW(window, kArtworkInvalidatedMessage, 0, 0);
        }
    }
}

bool revealNowPlayingInWorkspace()
{
    if (!requestPlayerShellMode(PlayerShellMode::normal)) return false;
    bool revealed = false;
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window) && SendMessageW(window, kRevealNowPlayingMessage, 0, 0) != 0) {
            revealed = true;
        }
    }
    return revealed;
}

void toggleWorkspaceQueue()
{
    if (!requestPlayerShellMode(PlayerShellMode::normal)) return;
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kToggleQueueMessage, 0, 0);
    }
}

void notifyWorkspaceQueueChanged()
{
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kQueueChangedMessage, 0, 0);
    }
}

void notifyWorkspaceHistoryChanged()
{
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kHistoryChangedMessage, 0, 0);
    }
}

void showWorkspaceHistory()
{
    if (!requestPlayerShellMode(PlayerShellMode::normal)) return;
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kShowHistoryMessage, 0, 0);
    }
}

void openWorkspaceCommandPalette()
{
    if (!requestPlayerShellMode(PlayerShellMode::normal)) return;
    for (const auto window : workspaceWindows()) {
        if (IsWindow(window)) PostMessageW(window, kOpenPaletteMessage, 0, 0);
    }
}

uie::window_factory<WorkspacePanel> g_workspacePanelFactory;

bool applyPlaybackStatisticsRating(const metadb_handle_ptr& handle,int rating) noexcept
{
    // Same resolver, same component and storage as Library. No duplicate rating DB.
    return setPlaybackStatisticsRating(handle,rating);
}

} // namespace nekorox::component
