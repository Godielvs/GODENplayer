#include "qa_hooks.h"
#include "tag_editor_window.h"

#include "tag_edit_service.h"
#include "database_service.h"
#include "text_util.h"
#include "library_model.h"
#include "library_roots_provider.h"
#include "media_file_operation_service.h"
#include "staging_media_editor.h"
#include "rollout_flags.h"
#include "ui_invalidation_bus.h"

#include <nekorox/qa_contracts.h>
#include <nekorox/track_identity.h>

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <map>
#include <set>
#include <shobjidl.h>
#include <sstream>
#include <uxtheme.h>

namespace nekorox::component {
namespace {

constexpr UINT kUndoRecoveryCompleteMessage = WM_APP + 0x582;
constexpr UINT kTrackRowsMessage = WM_APP + 0x583;
constexpr UINT kPreviewPreparationMessage = WM_APP + 0x584;
constexpr UINT kPreviewRowsMessage = WM_APP + 0x585;
constexpr UINT kResultRowsMessage = WM_APP + 0x586;
constexpr UINT kLocationsMessage = WM_APP + 0x587;
constexpr UINT_PTR kTrackRowsTimer = 0x5a1;
constexpr UINT_PTR kPreviewPreparationTimer = 0x5a2;
constexpr UINT_PTR kPreviewRowsTimer = 0x5a3;
constexpr UINT_PTR kResultRowsTimer = 0x5a4;
constexpr UINT_PTR kLocationsTimer = 0x5a5;
constexpr std::size_t kMaxUiBatchRows = 32;
constexpr auto kUiBatchBudget = std::chrono::milliseconds(2);

bool readCachedMetadata(const metadb_handle_ptr& track,
    nekorox::TagValueMap& metadata)
{
    const auto container = track->get_info_ref();
    if (!container.is_valid()) return false;
    const auto& info = container->info();
    for (t_size field = 0; field < info.meta_get_count(); ++field) {
        std::vector<std::string> values;
        for (t_size value = 0; value < info.meta_enum_value_count(field); ++value)
            values.emplace_back(info.meta_enum_value(field, value));
        auto name = std::string(info.meta_enum_name(field));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        metadata.emplace(std::move(name), std::move(values));
    }
    return true;
}

constexpr wchar_t kWindowClass[] = L"GodenTagEditorV1";
constexpr int kListId = 100;
constexpr int kStatusId = 101;
constexpr int kApplyId = 102;
constexpr int kRevertId = 103;
constexpr int kUndoId = 104;
constexpr int kRootsId = 110;
constexpr int kChooseDestinationId = 111;
constexpr int kLibraryPreferencesId = 112;
constexpr int kPreviewCopyId = 113;
constexpr int kExecuteCopyId = 114;
constexpr int kOrganizationModeId = 115;
constexpr int kCollisionModeId = 116;
constexpr int kArtworkModeBase = 500;
constexpr int kArtworkChooseBase = 510;
constexpr int kViewportId = 580;
constexpr int kModeBase = 200;
constexpr int kEditBase = 300;
constexpr COLORREF kBackground = RGB(8, 10, 10);
constexpr COLORREF kPanel = RGB(17, 20, 20);
constexpr COLORREF kText = RGB(216, 223, 223);
constexpr COLORREF kMuted = RGB(124, 137, 139);
struct FieldDescriptor {
    const char* field;
    const wchar_t* label;
};
constexpr std::array<FieldDescriptor, 12> kFieldDescriptors{{
    {"TITLE", L"Title"}, {"ARTIST", L"Artist"},
    {"ALBUM ARTIST", L"Album artist"}, {"ALBUM", L"Album"},
    {"DATE", L"Year / date"}, {"GENRE", L"Genre"},
    {"TRACKNUMBER", L"Track"}, {"TOTALTRACKS", L"Total tracks"},
    {"DISCNUMBER", L"Disc"}, {"TOTALDISCS", L"Total discs"},
    {"COMMENT", L"Comment"}, {"__ADVANCED__", L"Advanced"},
}};
constexpr std::array<const wchar_t*, 4> kArtworkLabels{
    L"Front", L"Back", L"Artist", L"Other / disc"};
constexpr std::array<ArtworkRole, 4> kArtworkRoles{
    ArtworkRole::front, ArtworkRole::back, ArtworkRole::artist, ArtworkRole::other};
constexpr std::uint64_t kArtworkDraftBudget = 64ULL * 1024ULL * 1024ULL;

std::string artworkContentHash(
    const std::shared_ptr<const ValidatedBytes>& bytes)
{
    if (!bytes) return {};
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : *bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string output(16, '0');
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((15U - index) * 4U);
        output[index] = digits[(hash >> shift) & 0x0fU];
    }
    return output;
}

bool qaBackgroundRequested() noexcept
{
    if (!qa::hooksAllowed()) return false;
    wchar_t hooks[4]{}, profile[4]{}, background[4]{};
    return GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks, 4) == 1 && hooks[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile, 4) == 1 && profile[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_BACKGROUND", background, 4) == 1 && background[0] == L'1';
}

int dip(HWND window, int value) noexcept
{
    return MulDiv(value, static_cast<int>((std::max)(96U, GetDpiForWindow(window))), 96);
}

HWND makeControl(HWND parent, const wchar_t* type, const wchar_t* text,
    DWORD style, int id)
{
    const auto result = CreateWindowExW(0, type, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        core_api::get_my_instance(), nullptr);
    if (result) SetWindowTheme(result, L"DarkMode_Explorer", nullptr);
    return result;
}

LRESULT CALLBACK multiSelectListProc(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_KEYDOWN && wParam == 'A' &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        SendMessageW(window, LB_SETSEL, TRUE, -1);
        SendMessageW(GetParent(window), WM_COMMAND,
            MAKEWPARAM(kListId, LBN_SELCHANGE), reinterpret_cast<LPARAM>(window));
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

std::wstring controlText(HWND control)
{
    const auto length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::vector<std::string> splitValues(std::wstring value)
{
    std::vector<std::string> result;
    std::wstring line;
    std::wistringstream stream(value);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) result.push_back(wideToUtf8(line));
    }
    return result;
}

std::wstring joinValues(const std::vector<std::string>& values)
{
    std::wstring result;
    for (const auto& value : values) {
        if (!result.empty()) result += L"\r\n";
        result += utf8ToWide(value);
    }
    return result;
}

std::vector<std::string> fieldValues(const metadb_handle_ptr& track,
    const char* field)
{
    std::vector<std::string> values;
    const auto container = track->get_info_ref();
    if (!container.is_valid()) return values;
    const auto& info = container->info();
    const auto index = info.meta_find(field);
    if (index == pfc_infinite) return values;
    for (t_size item = 0; item < info.meta_enum_value_count(index); ++item) {
        values.emplace_back(info.meta_enum_value(index, item));
    }
    return values;
}

std::wstring firstFieldValue(const metadb_handle_ptr& track, const char* field)
{
    const auto values = fieldValues(track, field);
    return values.empty() ? std::wstring{} : utf8ToWide(values.front());
}

std::wstring plannedFieldValue(const metadb_handle_ptr& track, const char* field,
    const std::vector<nekorox::TagMutation>& mutations)
{
    for (const auto& mutation : mutations) {
        if (_stricmp(mutation.field.c_str(), field) != 0 ||
            mutation.mode == nekorox::TagMutationMode::keep) continue;
        if (mutation.mode == nekorox::TagMutationMode::remove ||
            mutation.values.empty()) return {};
        return utf8ToWide(mutation.values.front());
    }
    return firstFieldValue(track, field);
}

std::filesystem::path nativeTrackPath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        return std::filesystem::path(utf8ToWide(
            filesystem::g_get_native_path(track->get_path()).c_str()));
    } catch (...) {
        return {};
    }
}

std::wstring foldedPath(const std::filesystem::path& path)
{
    auto value = path.lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    return value;
}

bool libraryPolicyAddable(const std::filesystem::path& candidate)
{
    try {
        pfc::string8 canonical;
        filesystem::g_get_canonical_path(
            wideToUtf8(candidate.wstring()).c_str(), canonical);
        return library_manager::get()->is_path_addable(canonical.c_str());
    } catch (...) {
        return false;
    }
}

HBITMAP previewBitmap(const ValidatedArtwork& artwork)
{
    if (artwork.previewWidth == 0 || artwork.previewHeight == 0 ||
        artwork.previewBgra.size() != static_cast<std::size_t>(
            artwork.previewWidth) * artwork.previewHeight * 4U) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(artwork.previewWidth);
    info.bmiHeader.biHeight = -static_cast<LONG>(artwork.previewHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    std::memcpy(pixels, artwork.previewBgra.data(), artwork.previewBgra.size());
    return bitmap;
}

} // namespace

struct TagEditorWindow::PendingLocations {
    std::vector<std::filesystem::path> files;
    std::vector<std::string> utf8;
    pfc::list_t<const char*> urls;
    std::size_t index = 0;
};

TagEditorWindow& TagEditorWindow::instance()
{
    static TagEditorWindow value;
    return value;
}

TagEditorWindow::~TagEditorWindow()
{
    shutdownAndDrain();
}

void TagEditorWindow::show(HWND owner)
{
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    lastOwner_ = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    if (window_) {
        ShowWindow(window_, SW_RESTORE);
        scheduleLocations();
        scheduleTrackRows();
        schedulePreviewPreparation();
        schedulePreviewRows();
        scheduleResultRows();
        if (!qaBackgroundRequested()) SetForegroundWindow(window_);
        return;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &TagEditorWindow::windowProc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    const auto monitor = MonitorFromWindow(lastOwner_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const auto dpiValue = lastOwner_ ? GetDpiForWindow(lastOwner_) : 96U;
    const auto desiredWidth = MulDiv(980, dpiValue, 96);
    const auto desiredHeight = MulDiv(740, dpiValue, 96);
    const auto workWidth = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
    const auto workHeight = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
    const auto width = (std::min)(desiredWidth, workWidth);
    const auto height = (std::min)(desiredHeight, workHeight);
    const auto left = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
    const auto top = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
    window_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kWindowClass,
        L"GODENplayer TAG", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        left, top, width, height, nullptr, nullptr,
        core_api::get_my_instance(), this);
    if (!window_) return;
    ShowWindow(window_, qaBackgroundRequested() ? SW_SHOWNA : SW_SHOW);
    scheduleLocations();
    scheduleTrackRows();
    scheduleResultRows();
    if (!qaBackgroundRequested()) SetForegroundWindow(window_);
}

void TagEditorWindow::hide()
{
    uiBatchWorker_.cancelPending();
    invalidateOrganizationPreview();
    if (window_) KillTimer(window_, kTrackRowsTimer);
    if (window_) KillTimer(window_, kLocationsTimer);
    if (window_) KillTimer(window_, kResultRowsTimer);
    if (window_) ShowWindow(window_, SW_HIDE);
}

void TagEditorWindow::shutdownAndDrain() noexcept
{
    if (shuttingDown_.exchange(true, std::memory_order_acq_rel)) return;
    scanGeneration_.fetch_add(1, std::memory_order_acq_rel);
    artworkGeneration_.fetch_add(1, std::memory_order_acq_rel);
    scanWorker_.cancelPending();
    artworkWorker_.cancelPending();
    invalidateOrganizationPreview();
    uiBatchWorker_.shutdownAndDrain();
    if (libraryRootsStarted_) LibraryRootsProvider::instance().shutdownAndDrain();
    if (window_) DestroyWindow(window_);
    scanWorker_.shutdownAndDrain();
    artworkWorker_.shutdownAndDrain();
    organizationWorker_.shutdownAndDrain();
}

LRESULT CALLBACK TagEditorWindow::windowProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    TagEditorWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<TagEditorWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TagEditorWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->handleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TagEditorWindow::handleMessage(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        window_ = window;
        libraryRootsStarted_ = true;
        DragAcceptFiles(window, TRUE);
        background_ = CreateSolidBrush(kBackground);
        fieldBackground_ = CreateSolidBrush(kPanel);
        if (!viewport_.create(window_, kViewportId)) return -1;
        createControls();
        TagEditService::instance().beginUndoRecovery(
            window_, kUndoRecoveryCompleteMessage);
        if (TagEditService::instance().undoRecoveryPending())
            setStatus(L"Recovering Undo in the background. Edits remain available.");
        updateFont();
        updateDirtyUi();
        layout();
        // The visible-window handler subscribes and queues the refresh. The
        // retained snapshot is available immediately without scanning paths.
        showLibraryRoots();
        return 0;
    case kUndoRecoveryCompleteMessage:
        if (undo_) EnableWindow(undo_,
            !trackLoading_ && (TagEditService::instance().hasUndo() ||
            MediaFileOperationService::instance().hasInPlaceUndo()));
        if (!applying_) {
            const auto& service = TagEditService::instance();
            if (service.undoRecoveryLimited())
                setStatus(service.hasUndo()
                    ? L"Undo recovered. Recovery limit reached; other journals were not checked."
                    : L"Undo recovery incomplete: limit reached; unexamined journals may remain.");
            else if (service.hasUndo())
                setStatus(L"Recovered the last TAG Undo plan.");
        }
        updateDirtyUi();
        return 0;
    case kTrackRowsMessage:
        if (static_cast<std::uint64_t>(wParam) == scanGeneration_.load()) {
            trackRowsPosted_ = false;
            publishTrackRows(static_cast<std::uint64_t>(wParam));
        }
        return 0;
    case kLocationsMessage:
        if (static_cast<std::uint64_t>(wParam) == scanGeneration_.load()) {
            locationsPosted_ = false;
            publishLocations(static_cast<std::uint64_t>(wParam));
        }
        return 0;
    case kPreviewPreparationMessage:
        if (static_cast<std::uint64_t>(wParam) == organizationGeneration_.load()) {
            previewPreparationPosted_ = false;
            preparePreviewBatch(static_cast<std::uint64_t>(wParam));
        }
        return 0;
    case kPreviewRowsMessage:
        if (static_cast<std::uint64_t>(wParam) == organizationGeneration_.load()) {
            previewRowsPosted_ = false;
            publishPreviewRows(static_cast<std::uint64_t>(wParam));
        }
        return 0;
    case kResultRowsMessage:
        if (static_cast<std::uint64_t>(wParam) == resultGeneration_) {
            resultRowsPosted_ = false;
            publishResultRows(static_cast<std::uint64_t>(wParam));
        }
        return 0;
    case WM_TIMER:
        if (wParam == kTrackRowsTimer) {
            KillTimer(window, kTrackRowsTimer);
            publishTrackRows(scanGeneration_.load());
            return 0;
        }
        if (wParam == kLocationsTimer) {
            KillTimer(window, kLocationsTimer);
            publishLocations(scanGeneration_.load());
            return 0;
        }
        if (wParam == kPreviewPreparationTimer) {
            KillTimer(window, kPreviewPreparationTimer);
            preparePreviewBatch(organizationGeneration_.load());
            return 0;
        }
        if (wParam == kPreviewRowsTimer) {
            KillTimer(window, kPreviewRowsTimer);
            publishPreviewRows(organizationGeneration_.load());
            return 0;
        }
        if (wParam == kResultRowsTimer) {
            KillTimer(window, kResultRowsTimer);
            publishResultRows(resultGeneration_);
            return 0;
        }
        break;
    case WM_DROPFILES:
        beginDrop(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_COPYDATA: {
        const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!qaBackgroundRequested() || copy == nullptr ||
            (copy->dwData != nekorox::kFileDropCopyDataTagV1 &&
             copy->dwData != nekorox::kTagSetTitleCopyDataTagV1 &&
             copy->dwData != nekorox::kTagSetDestinationCopyDataTagV1 &&
             copy->dwData != nekorox::kTagSetFrontArtworkCopyDataTagV1) ||
            copy->lpData == nullptr || copy->cbData < sizeof(wchar_t) ||
            copy->cbData % sizeof(wchar_t) != 0) return FALSE;
        const auto* value = static_cast<const wchar_t*>(copy->lpData);
        const auto count = copy->cbData / sizeof(wchar_t);
        if (value[count - 1U] != L'\0') return FALSE;
        if (trackLoading_ &&
                copy->dwData == nekorox::kTagSetFrontArtworkCopyDataTagV1)
            return FALSE;
        if (copy->dwData == nekorox::kTagSetFrontArtworkCopyDataTagV1) {
            beginArtwork(std::filesystem::path(value), 0);
        } else if (copy->dwData == nekorox::kTagSetDestinationCopyDataTagV1) {
            const std::filesystem::path candidate(value);
            std::error_code error;
            const auto online = std::filesystem::is_directory(candidate, error) && !error;
            bool addable = false;
            if (online) {
                try {
                    addable = libraryPolicyAddable(candidate /
                        L"goden-library-policy-probe.flac");
                } catch (...) {
                }
            }
            if (!online) return FALSE;
            selectedDestination_ = candidate.lexically_normal();
            selectedDestinationAddable_ = addable;
            invalidateOrganizationPreview();
            EnableWindow(executeCopy_, FALSE);
            showLibraryRoots();
        } else if (copy->dwData == nekorox::kTagSetTitleCopyDataTagV1) {
            if (trackLoading_) return FALSE;
            internalControlUpdate_ = true;
            SendMessageW(modes_[0], CB_SETCURSEL, 1, 0);
            SetWindowTextW(edits_[0], value);
            internalControlUpdate_ = false;
            commitFieldIntent(0);
        } else {
            beginPaths({std::filesystem::path(value)});
        }
        return TRUE;
    }
    case WM_SIZE:
        layout();
        refreshLibraryRoots();
        return 0;
    case WM_SHOWWINDOW:
        if (wParam && !IsIconic(window)) {
            LibraryRootsProvider::instance().observe([this](auto) {
                if (window_ && IsWindowVisible(window_) && !IsIconic(window_)) showLibraryRoots();
            });
        } else LibraryRootsProvider::instance().suspend();
        return 0;
    case WM_MOUSEWHEEL:
        if (viewport_.viewport() != nullptr)
            SendMessageW(viewport_.viewport(), message, wParam, lParam);
        return 0;
    case WM_DPICHANGED: {
        const auto* bounds = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        updateFont();
        layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = dip(window, 600);
        info->ptMinTrackSize.y = dip(window, 420);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, message == WM_CTLCOLORSTATIC ? kMuted : kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(fieldBackground_);
    }
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(reinterpret_cast<HDC>(wParam), &bounds, background_);
        return TRUE;
    }
    case WM_COMMAND: {
        const auto id = LOWORD(wParam);
        const auto notification = HIWORD(wParam);
        if (trackLoading_ && (id == kApplyId || id == kRevertId ||
                id == kUndoId || id == kPreviewCopyId ||
                id == kExecuteCopyId || id == kListId ||
                (id >= kModeBase && id < kModeBase +
                    static_cast<int>(kFieldDescriptors.size())) ||
                (id >= kEditBase && id < kEditBase +
                    static_cast<int>(kFieldDescriptors.size())) ||
                (id >= kArtworkModeBase && id < kArtworkModeBase + 4) ||
                (id >= kArtworkChooseBase && id < kArtworkChooseBase + 4)))
            return 0;
        if (id == kApplyId && notification == BN_CLICKED) apply();
        else if (id == kRevertId && notification == BN_CLICKED) {
            invalidateOrganizationPreview();
            tagDocument_.revertAll();
            refreshFields();
            updateDirtyUi();
        }
        else if (id == kUndoId && notification == BN_CLICKED) undoLast();
        else if (id == kChooseDestinationId && notification == BN_CLICKED)
            chooseDestination();
        else if (id == kLibraryPreferencesId && notification == BN_CLICKED)
            LibraryRootsProvider::instance().openLibraryPreferences();
        else if (id == kPreviewCopyId && notification == BN_CLICKED)
            previewOrganization();
        else if (id == kExecuteCopyId && notification == BN_CLICKED)
            executeOrganization();
        else if ((id == kOrganizationModeId || id == kCollisionModeId) &&
                notification == CBN_SELCHANGE) {
            invalidateOrganizationPreview();
            EnableWindow(executeCopy_, FALSE);
        }
        else if (id >= kArtworkChooseBase && id < kArtworkChooseBase + 4 &&
                notification == BN_CLICKED) {
            chooseArtwork(static_cast<std::size_t>(id - kArtworkChooseBase));
        }
        else if (id >= kArtworkModeBase && id < kArtworkModeBase + 4 &&
                notification == CBN_SELCHANGE) {
            const auto index = static_cast<std::size_t>(id - kArtworkModeBase);
            const auto selected = SendMessageW(artworkModes_[index], CB_GETCURSEL, 0, 0);
            artworkMutations_[index].mode = selected == 1 ? ArtworkMutationMode::set
                : selected == 2 ? ArtworkMutationMode::remove
                : selected == 3 ? ArtworkMutationMode::setSidecar
                                : ArtworkMutationMode::keep;
            if (artworkMutations_[index].mode != ArtworkMutationMode::set ||
                (artworkMutations_[index].originalBytes &&
                    !artworkMutations_[index].originalBytes->empty())) {
                for (const auto& item : tagDocument_.selectedIds()) {
                    auto& drafts = artworkDrafts_[item];
                    for (std::size_t role = 0; role < drafts.size(); ++role)
                        drafts[role].mutation.role = kArtworkRoles[role];
                    drafts[index] = {artworkMutations_[index],
                        nextArtworkDraftRevision_++,
                        artworkContentHash(
                            artworkMutations_[index].originalBytes)};
                    if (std::all_of(drafts.begin(), drafts.end(), [](const auto& value) {
                            return value.mutation.mode == ArtworkMutationMode::keep;
                        })) artworkDrafts_.erase(item);
                }
            }
            setDirty(true);
        }
        else if (id == kListId && notification == LBN_SELCHANGE) {
            selectionChanged();
        }
        else if (id == kRootsId && notification == LBN_SELCHANGE) {
            const auto selected = static_cast<int>(SendMessageW(
                roots_, LB_GETCURSEL, 0, 0));
            const auto snapshot = LibraryRootsProvider::instance().snapshot();
            const auto manualOffset = selectedDestination_.empty() ? 0 : 1;
            const auto configuredOffset = snapshot->configuredDestination.empty() ? 0 : 1;
            const auto rootIndex = selected - manualOffset - configuredOffset;
            if (selected >= 0 && manualOffset != 0 && selected == 0) {
                // Existing verified manual destination remains selected.
            } else if (selected >= manualOffset && configuredOffset != 0 &&
                    selected == manualOffset) {
                selectedDestination_ = snapshot->configuredDestination;
                selectedDestinationAddable_ = true;
            } else if (rootIndex >= 0 &&
                    rootIndex < static_cast<int>(snapshot->roots.size()) &&
                    snapshot->roots[static_cast<std::size_t>(rootIndex)].online) {
                const auto& root = snapshot->roots[static_cast<std::size_t>(rootIndex)];
                selectedDestination_ = root.path;
                selectedDestinationAddable_ = root.policyAddable;
            }
            invalidateOrganizationPreview();
            EnableWindow(executeCopy_, FALSE);
            showLibraryRoots();
        }
        else if (!internalControlUpdate_ && id >= kModeBase &&
                id < kModeBase + static_cast<int>(kFieldDescriptors.size()) &&
                notification == CBN_SELCHANGE) {
            commitFieldIntent(static_cast<std::size_t>(id - kModeBase));
        }
        else if (!internalControlUpdate_ && id >= kEditBase &&
                id < kEditBase + static_cast<int>(kFieldDescriptors.size()) &&
                notification == EN_CHANGE) {
            const auto index = id - kEditBase;
            if (SendMessageW(modes_[index], CB_GETCURSEL, 0, 0) == 0) {
                internalControlUpdate_ = true;
                SendMessageW(modes_[index], CB_SETCURSEL, 1, 0);
                internalControlUpdate_ = false;
            }
            commitFieldIntent(static_cast<std::size_t>(index));
        }
        return 0;
    }
    case WM_CLOSE:
        hide();
        return 0;
    case WM_DESTROY:
        LibraryRootsProvider::instance().suspend();
        KillTimer(window, kTrackRowsTimer);
        KillTimer(window, kLocationsTimer);
        KillTimer(window, kPreviewPreparationTimer);
        KillTimer(window, kPreviewRowsTimer);
        KillTimer(window, kResultRowsTimer);
        pendingTracks_.reset();
        pendingLocations_.reset();
        pendingDocumentItems_.clear();
        refreshingTrackMetadata_ = false;
        metadataRefreshRows_.clear();
        previewPreparation_.reset();
        resultRows_.reset();
        TagEditService::instance().clearUndoRecoveryObserver(window);
        invalidateOrganizationPreview();
        scanGeneration_.fetch_add(1);
        scanWorker_.cancelPending();
        artworkGeneration_.fetch_add(1);
        artworkWorker_.cancelPending();
        viewport_.destroy();
        if (font_) DeleteObject(font_);
        if (background_) DeleteObject(background_);
        if (fieldBackground_) DeleteObject(fieldBackground_);
        if (artworkPreviewBitmap_) {
            SendMessageW(artworkPreview_, STM_SETIMAGE, IMAGE_BITMAP, 0);
            DeleteObject(artworkPreviewBitmap_);
            artworkPreviewBitmap_ = nullptr;
        }
        window_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void TagEditorWindow::createControls()
{
    const auto content = viewport_.content();
    title_ = makeControl(window_, L"STATIC", L"GODENplayer TAG EDITOR",
        SS_LEFT, 540);
    list_ = makeControl(content, L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT |
        LBS_EXTENDEDSEL | LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
        kListId);
    SetWindowSubclass(list_, multiSelectListProc, 1, 0);
    status_ = makeControl(window_, L"STATIC",
        L"Drop audio files or folders here. Nothing is written until Apply.",
        SS_LEFT, kStatusId);
    apply_ = makeControl(window_, L"BUTTON", L"Apply changes to 0 files",
        BS_PUSHBUTTON | WS_TABSTOP,
        kApplyId);
    revert_ = makeControl(window_, L"BUTTON", L"Revert pending edits",
        BS_PUSHBUTTON | WS_TABSTOP, kRevertId);
    undo_ = makeControl(window_, L"BUTTON", L"Undo last operation",
        BS_PUSHBUTTON | WS_TABSTOP, kUndoId);
    rootsLabel_ = makeControl(content, L"STATIC",
        L"Library folders found · coverage: observed only", SS_LEFT, 410);
    roots_ = makeControl(content, L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT |
        LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, kRootsId);
    organizationPreview_ = makeControl(content, L"LISTBOX", L"",
        LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 117);
    chooseDestination_ = makeControl(content, L"BUTTON", L"Choose destination…",
        BS_PUSHBUTTON | WS_TABSTOP, kChooseDestinationId);
    libraryPreferences_ = makeControl(content, L"BUTTON", L"Library preferences",
        BS_PUSHBUTTON | WS_TABSTOP, kLibraryPreferencesId);
    organizationMode_ = makeControl(content, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_TABSTOP, kOrganizationModeId);
    SendMessageW(organizationMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Copy (source stays)"));
    SendMessageW(organizationMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Move (explicit)"));
    SendMessageW(organizationMode_, CB_SETCURSEL, 0, 0);
    SendMessageW(organizationMode_, CB_SETMINVISIBLE, 2, 0);
    collisionMode_ = makeControl(content, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_TABSTOP, kCollisionModeId);
    SendMessageW(collisionMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Collision: Skip"));
    SendMessageW(collisionMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Collision: Rename"));
    SendMessageW(collisionMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Collision: Replace + backup"));
    SendMessageW(collisionMode_, CB_SETCURSEL, 0, 0);
    SendMessageW(collisionMode_, CB_SETMINVISIBLE, 3, 0);
    previewCopy_ = makeControl(content, L"BUTTON", L"Preview organization…",
        BS_PUSHBUTTON | WS_TABSTOP, kPreviewCopyId);
    executeCopy_ = makeControl(content, L"BUTTON", L"Run planned operation",
        BS_PUSHBUTTON | WS_TABSTOP, kExecuteCopyId);
    const auto importEnabled = rolloutFlagEnabled(RolloutFlag::tagImportV1);
    ShowWindow(previewCopy_, importEnabled ? SW_SHOW : SW_HIDE);
    ShowWindow(executeCopy_, importEnabled ? SW_SHOW : SW_HIDE);
    ShowWindow(organizationMode_, importEnabled ? SW_SHOW : SW_HIDE);
    ShowWindow(collisionMode_, importEnabled ? SW_SHOW : SW_HIDE);
    EnableWindow(executeCopy_, FALSE);
    labels_.resize(kFieldDescriptors.size());
    modes_.resize(kFieldDescriptors.size());
    edits_.resize(kFieldDescriptors.size());
    for (std::size_t index = 0; index < kFieldDescriptors.size(); ++index) {
        labels_[index] = makeControl(content, L"STATIC",
            kFieldDescriptors[index].label, SS_LEFT,
            400 + static_cast<int>(index));
        modes_[index] = makeControl(content, L"COMBOBOX", L"",
            CBS_DROPDOWNLIST | WS_TABSTOP, kModeBase + static_cast<int>(index));
        SendMessageW(modes_[index], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Keep"));
        SendMessageW(modes_[index], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Set"));
        SendMessageW(modes_[index], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Remove"));
        SendMessageW(modes_[index], CB_SETCURSEL, 0, 0);
        SendMessageW(modes_[index], CB_SETMINVISIBLE, 3, 0);
        edits_[index] = makeControl(content, L"EDIT", L"",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_BORDER |
                WS_TABSTOP, kEditBase + static_cast<int>(index));
    }
    for (std::size_t index = 0; index < kArtworkRoles.size(); ++index) {
        artworkMutations_[index].role = kArtworkRoles[index];
        artworkLabels_[index] = makeControl(content, L"STATIC", kArtworkLabels[index],
            SS_LEFT, 520 + static_cast<int>(index));
        artworkModes_[index] = makeControl(content, L"COMBOBOX", L"",
            CBS_DROPDOWNLIST | WS_TABSTOP,
            kArtworkModeBase + static_cast<int>(index));
        SendMessageW(artworkModes_[index], CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"Keep"));
        SendMessageW(artworkModes_[index], CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"Set embedded…"));
        SendMessageW(artworkModes_[index], CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"Remove"));
        SendMessageW(artworkModes_[index], CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"Set sidecar…"));
        SendMessageW(artworkModes_[index], CB_SETCURSEL, 0, 0);
        SendMessageW(artworkModes_[index], CB_SETMINVISIBLE, 4, 0);
        artworkChoose_[index] = makeControl(content, L"BUTTON", L"Choose image…",
            BS_PUSHBUTTON | WS_TABSTOP,
            kArtworkChooseBase + static_cast<int>(index));
    }
    artworkPreview_ = makeControl(content, L"STATIC", L"",
        SS_BITMAP | SS_CENTERIMAGE | WS_BORDER, 530);
    for (const auto control : {list_, rootsLabel_, roots_, organizationPreview_,
            chooseDestination_, libraryPreferences_, previewCopy_, executeCopy_,
            organizationMode_, collisionMode_, artworkPreview_})
        viewport_.trackControl(control);
    for (const auto control : labels_) viewport_.trackControl(control);
    for (const auto control : modes_) viewport_.trackControl(control);
    for (const auto control : edits_) viewport_.trackControl(control);
    for (const auto control : artworkLabels_) viewport_.trackControl(control);
    for (const auto control : artworkModes_) viewport_.trackControl(control);
    for (const auto control : artworkChoose_) viewport_.trackControl(control);
    EnableWindow(apply_, FALSE);
    EnableWindow(undo_, TagEditService::instance().hasUndo() ||
        MediaFileOperationService::instance().hasInPlaceUndo());
}

void TagEditorWindow::updateFont()
{
    if (font_) DeleteObject(font_);
    LOGFONTW lf{};
    lf.lfHeight = -dip(window_, 14);
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    font_ = CreateFontIndirectW(&lf);
    const std::array fixed{title_, list_, status_, apply_, revert_, undo_, rootsLabel_,
        roots_, organizationPreview_, chooseDestination_, libraryPreferences_, previewCopy_, executeCopy_,
        organizationMode_, collisionMode_};
    for (const auto control : fixed) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto control : labels_) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto control : modes_) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto control : edits_) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto control : artworkLabels_) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto control : artworkModes_) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto control : artworkChoose_) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font_), TRUE);
}

void TagEditorWindow::layout()
{
    if (!window_) return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    const auto clientWidth = static_cast<int>(bounds.right - bounds.left);
    const auto clientHeight = static_cast<int>(bounds.bottom - bounds.top);
    const auto dpiValue = static_cast<int>((std::max)(96U,
        GetDpiForWindow(window_)));
    auto px = [&](int value) { return MulDiv(value, dpiValue, 96); };
    const auto headerHeight = px(44);
    const auto footerHeight = px(88);
    const auto preservedScroll = viewport_.scrollOffset();
    viewport_.scrollTo(0);
    viewport_.setBounds(0, headerHeight, clientWidth,
        (std::max)(1, clientHeight - headerHeight - footerHeight), px(980));
    RECT contentBounds{};
    GetClientRect(viewport_.content(), &contentBounds);
    const auto widthDip = MulDiv(static_cast<int>(contentBounds.right), 96,
        dpiValue);
    constexpr int pad = 16;
    constexpr int gap = 16;
    auto place = [&](HWND control, int x, int y, int width, int height) {
        MoveWindow(control, px(x), px(y), px((std::max)(1, width)),
            px((std::max)(1, height)), TRUE);
        ShowWindow(control, SW_SHOWNA);
    };
    auto placeCombo = [&](HWND control, int x, int y, int width,
            int) {
        place(control, x, y, width, 30);
    };

    int leftBottom = pad;
    int formTop = pad;
    int formLeft = pad;
    int formWidth = (std::max)(1, widthDip - pad * 2);
    const bool wide = widthDip >= 820;
    if (wide) {
        const auto leftWidth = (std::min)(300,
            (std::max)(260, (widthDip - pad * 2 - gap) / 3));
        formLeft = pad + leftWidth + gap;
        formWidth = (std::max)(1, widthDip - formLeft - pad);
        auto y = pad;
        place(list_, pad, y, leftWidth, 250); y += 250 + 8;
        place(rootsLabel_, pad, y, leftWidth, 20); y += 24;
        place(roots_, pad, y, leftWidth, 104); y += 112;
        place(organizationPreview_, pad, y, leftWidth, 64); y += 72;
        const auto half = (std::max)(1, (leftWidth - 8) / 2);
        place(chooseDestination_, pad, y, half, 30);
        place(libraryPreferences_, pad + half + 8, y, half, 30); y += 42;
        placeCombo(organizationMode_, pad, y, half, 220);
        placeCombo(collisionMode_, pad + half + 8, y, half, 220); y += 42;
        place(previewCopy_, pad, y, half, 30);
        place(executeCopy_, pad + half + 8, y, half, 30); y += 30;
        leftBottom = y + pad;
    } else {
        auto y = pad;
        place(list_, pad, y, formWidth, 160); y += 168;
        place(rootsLabel_, pad, y, formWidth, 20); y += 24;
        place(roots_, pad, y, formWidth, 80); y += 88;
        place(organizationPreview_, pad, y, formWidth, 64); y += 72;
        const auto half = (std::max)(1, (formWidth - 8) / 2);
        place(chooseDestination_, pad, y, half, 30);
        place(libraryPreferences_, pad + half + 8, y, half, 30); y += 42;
        placeCombo(organizationMode_, pad, y, half, 220);
        placeCombo(collisionMode_, pad + half + 8, y, half, 220); y += 42;
        place(previewCopy_, pad, y, half, 30);
        place(executeCopy_, pad + half + 8, y, half, 30); y += 50;
        formTop = y;
    }

    auto y = formTop;
    const bool stackedFields = formWidth < 540;
    for (std::size_t index = 0; index < kFieldDescriptors.size(); ++index) {
        const auto multiline = index >= kFieldDescriptors.size() - 2;
        const auto editHeight = multiline ? 72 : 30;
        if (stackedFields) {
            place(labels_[index], formLeft, y, formWidth, 20); y += 24;
            constexpr int modeWidth = 104;
            placeCombo(modes_[index], formLeft, y, modeWidth, 220);
            place(edits_[index], formLeft + modeWidth + 8, y,
                formWidth - modeWidth - 8, editHeight);
            y += editHeight + 12;
        } else {
            constexpr int labelWidth = 120;
            constexpr int modeWidth = 104;
            place(labels_[index], formLeft, y + 5, labelWidth, 20);
            placeCombo(modes_[index], formLeft + labelWidth, y,
                modeWidth, 220);
            place(edits_[index], formLeft + labelWidth + modeWidth + 8, y,
                formWidth - labelWidth - modeWidth - 8, editHeight);
            y += editHeight + 12;
        }
    }

    y += 8;
    const auto artModeWidth = (std::min)(130,
        (std::max)(96, formWidth / 3));
    for (std::size_t index = 0; index < kArtworkRoles.size(); ++index) {
        place(artworkLabels_[index], formLeft, y, formWidth, 20); y += 24;
        placeCombo(artworkModes_[index], formLeft, y, artModeWidth, 180);
        place(artworkChoose_[index], formLeft + artModeWidth + 8, y,
            formWidth - artModeWidth - 8, 30);
        y += 42;
    }
    const auto previewSize = (std::min)(112, formWidth);
    place(artworkPreview_, formLeft, y, previewSize, 112); y += 112 + pad;
    const auto contentHeightDip = (std::max)(leftBottom, y);
    viewport_.setBounds(0, headerHeight, clientWidth,
        (std::max)(1, clientHeight - headerHeight - footerHeight),
        px(contentHeightDip));

    MoveWindow(title_, px(16), px(11),
        (std::max)(1, clientWidth - px(32)), px(24), TRUE);
    const auto footerTop = clientHeight - footerHeight;
    MoveWindow(status_, px(16), footerTop + px(4),
        (std::max)(1, clientWidth - px(32)), px(40), TRUE);
    const auto buttonY = footerTop + px(52);
    const auto useful = (std::max)(3, clientWidth - px(32) - px(16));
    const auto buttonWidth = (std::max)(1, useful / 3);
    MoveWindow(undo_, px(16), buttonY, buttonWidth, px(32), TRUE);
    MoveWindow(revert_, px(16) + buttonWidth + px(8), buttonY,
        buttonWidth, px(32), TRUE);
    MoveWindow(apply_, px(16) + (buttonWidth + px(8)) * 2, buttonY,
        buttonWidth, px(32), TRUE);
    // Geometry never grants capability. A resize/DPI relayout may position
    // rollout-owned controls, but it must not make them visible or actionable.
    const auto importEnabled = rolloutFlagEnabled(RolloutFlag::tagImportV1);
    for (const auto control : {previewCopy_, executeCopy_,
             organizationMode_, collisionMode_}) {
        ShowWindow(control, importEnabled ? SW_SHOWNA : SW_HIDE);
        if (!importEnabled) EnableWindow(control, FALSE);
        else if (control != executeCopy_) EnableWindow(control,
            control != previewCopy_ || (!trackLoading_ &&
                tracks_.get_count() != 0 && !tagDocument_.loaded().empty()));
    }
    viewport_.scrollTo(preservedScroll);
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void TagEditorWindow::beginDrop(HDROP drop)
{
    std::vector<std::filesystem::path> roots;
    const auto count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
    for (UINT index = 0; index < count; ++index) {
        const auto length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        if (DragQueryFileW(drop, index, value.data(), static_cast<UINT>(value.size()))) {
            value.resize(length);
            roots.emplace_back(std::move(value));
        }
    }
    DragFinish(drop);
    beginPaths(std::move(roots));
}

void TagEditorWindow::beginPaths(std::vector<std::filesystem::path> roots)
{
    if (roots.empty() || shuttingDown_.load(std::memory_order_acquire)) return;
    if (applying_ || MediaFileOperationService::instance().busy()) {
        setStatus(L"Wait for the current TAG operation before loading other files.");
        return;
    }
    invalidateOrganizationPreview();
    const auto generation = scanGeneration_.fetch_add(1) + 1;
    if (window_) KillTimer(window_, kTrackRowsTimer);
    if (window_) KillTimer(window_, kLocationsTimer);
    if (window_) KillTimer(window_, kResultRowsTimer);
    ++resultGeneration_;
    resultRows_.reset();
    resultRowsPosted_ = false;
    resultSummary_.clear();
    pendingTracks_.reset();
    pendingLocations_.reset();
    locationsPosted_ = false;
    pendingDocumentItems_.clear();
    pendingTrackIndex_ = 0;
    trackRowsPosted_ = false;
    refreshingTrackMetadata_ = false;
    metadataRefreshRows_.clear();
    pendingTitleScript_.release();
    artworkGeneration_.fetch_add(1, std::memory_order_acq_rel);
    artworkWorker_.cancelPending();
    tracks_.remove_all();
    itemIds_.clear();
    tagDocument_.reset({});
    artworkDrafts_.clear();
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    SendMessageW(organizationPreview_, LB_RESETCONTENT, 0, 0);
    setDirty(false);
    setTrackLoading(true);
    refreshFields();
    setStatus(L"Loading files…");
    if (!scanWorker_.submit([this, roots = std::move(roots), generation](
            std::stop_token stop) mutable {
        std::vector<std::filesystem::path> files;
        std::set<std::wstring, std::less<>> seen;
        const auto add = [&](const std::filesystem::path& path) {
            if (files.size() >= 10000U) return;
            auto key = std::filesystem::absolute(path).lexically_normal().wstring();
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            if (seen.insert(key).second) files.push_back(path);
        };
        for (const auto& root : roots) {
            if (stop.stop_requested()) return;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(root, error);
            if (error || std::filesystem::is_symlink(status)) continue;
            if (std::filesystem::is_regular_file(status)) {
                add(root);
                continue;
            }
            if (!std::filesystem::is_directory(status)) continue;
            for (std::filesystem::recursive_directory_iterator iterator(root,
                    std::filesystem::directory_options::skip_permission_denied, error), end;
                    !stop.stop_requested() && !error && iterator != end &&
                    files.size() < 10000U; iterator.increment(error)) {
                const auto entryStatus = iterator->symlink_status(error);
                if (error) { error.clear(); continue; }
                if (std::filesystem::is_symlink(entryStatus)) {
                    if (std::filesystem::is_directory(entryStatus)) iterator.disable_recursion_pending();
                    continue;
                }
                if (std::filesystem::is_regular_file(entryStatus)) add(iterator->path());
            }
        }
        if (!stop.stop_requested()) {
            fb2k::inMainThread([this, files = std::move(files), generation]() mutable {
                resolveFiles(std::move(files), generation);
            });
        }
    })) {
        setTrackLoading(false);
        setStatus(L"The persistent file scanner could not start.");
    }
}

void TagEditorWindow::resolveFiles(std::vector<std::filesystem::path> files,
    std::uint64_t generation)
{
    if (!window_ || generation != scanGeneration_.load()) return;
    if (files.empty()) {
        setTrackLoading(false);
        setStatus(L"No supported local files found.");
        return;
    }
    pendingLocations_ = std::make_unique<PendingLocations>();
    pendingLocations_->files = std::move(files);
    pendingLocations_->utf8.reserve(pendingLocations_->files.size());
    setStatus(L"Resolving file paths in small batches…");
    scheduleLocations();
}

void TagEditorWindow::continueUiBatch(HWND window, UINT_PTR timer,
    UINT message, std::uint64_t generation) noexcept
{
    // Import, preview and result publication are mutually exclusive stages.
    // Leave a real gap for input and paint, then post the next bounded batch.
    // WM_TIMER alone can starve under continuous renderer messages.
    if (!uiBatchWorker_.submit([window,message,generation](std::stop_token stop) {
        std::mutex waitMutex;
        std::condition_variable_any wake;
        std::unique_lock lock(waitMutex);
        wake.wait_for(lock,stop,std::chrono::milliseconds(16),[]{return false;});
        if (!stop.stop_requested())
            PostMessageW(window,message,static_cast<WPARAM>(generation),0);
    })) SetTimer(window,timer,16,nullptr);
}

void TagEditorWindow::scheduleLocations()
{
    if (!window_ || !IsWindowVisible(window_) || !pendingLocations_ ||
            locationsPosted_) return;
    const auto generation = scanGeneration_.load(std::memory_order_acquire);
    locationsPosted_ = PostMessageW(window_, kLocationsMessage,
        static_cast<WPARAM>(generation), 0) != FALSE;
    if (!locationsPosted_) SetTimer(window_, kLocationsTimer, 16, nullptr);
}

void TagEditorWindow::publishLocations(std::uint64_t generation)
{
    if (!window_ || !IsWindowVisible(window_) || !pendingLocations_ ||
            generation != scanGeneration_.load()) return;
    auto& state = *pendingLocations_;
    const auto started = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    while (state.index < state.files.size() &&
            processed < kMaxUiBatchRows &&
            (processed == 0 ||
                std::chrono::steady_clock::now() - started < kUiBatchBudget)) {
        state.utf8.push_back(wideToUtf8(state.files[state.index++].wstring()));
        state.urls.add_item(state.utf8.back().c_str());
        ++processed;
    }
    if (state.index < state.files.size()) {
        setStatus(L"Resolving file paths · " + std::to_wstring(state.index) +
            L" / " + std::to_wstring(state.files.size()));
        continueUiBatch(window_, kLocationsTimer, kLocationsMessage, generation);
        return;
    }
    auto notify = process_locations_notify::create(
        [this, generation](metadb_handle_list_cref resolved) {
            if (shuttingDown_.load(std::memory_order_acquire) ||
                    generation != scanGeneration_.load(std::memory_order_acquire))
                return;
            auto owned = std::make_shared<metadb_handle_list>();
            owned->add_items(resolved);
            fb2k::inMainThread([this, owned, generation] {
                acceptTracks(owned, generation);
            });
        });
    try {
        playlist_incoming_item_filter_v2::get()->process_locations_async(
            state.urls, playlist_incoming_item_filter_v2::op_flag_delay_ui |
                playlist_incoming_item_filter_v2::op_flag_no_filter,
            nullptr, nullptr, window_, notify);
        pendingLocations_.reset();
    } catch (...) {
        pendingLocations_.reset();
        setTrackLoading(false);
        setStatus(L"Could not resolve those file paths.");
    }
}

std::string TagEditorWindow::itemId(const metadb_handle_ptr& track) const
{
    if (!track.is_valid()) return {};
    std::string value = track->get_path();
    value += "#";
    value += std::to_string(track->get_subsong_index());
    return value;
}

void TagEditorWindow::acceptTracks(std::shared_ptr<metadb_handle_list> tracks,
    std::uint64_t generation)
{
    if (!window_ || generation != scanGeneration_.load()) return;
    tracks_.remove_all();
    artworkDrafts_.clear();
    itemIds_.clear();
    pendingTracks_ = std::move(tracks);
    pendingDocumentItems_.clear();
    pendingDocumentItems_.reserve(pendingTracks_->get_count());
    pendingTrackIndex_ = 0;
    SendMessageW(organizationPreview_, LB_RESETCONTENT, 0, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    titleformat_compiler::get()->compile_safe(pendingTitleScript_,
        "$if2(%album artist% - %album% / %artist% - %title%,$filename(%path%))");
    setStatus(L"Loading track details in small batches…");
    scheduleTrackRows();
}

void TagEditorWindow::setTrackLoading(bool loading)
{
    trackLoading_ = loading;
    const auto editable = !loading && tracks_.get_count() != 0 &&
        !tagDocument_.loaded().empty();
    if (list_) EnableWindow(list_, !loading);
    if (revert_) EnableWindow(revert_, editable);
    if (previewCopy_) EnableWindow(previewCopy_, editable &&
        rolloutFlagEnabled(RolloutFlag::tagImportV1));
    if (executeCopy_ && loading) EnableWindow(executeCopy_, FALSE);
    for (const auto control : modes_) EnableWindow(control, editable);
    for (const auto control : edits_) EnableWindow(control, editable);
    for (const auto control : artworkModes_) EnableWindow(control, editable);
    for (const auto control : artworkChoose_) EnableWindow(control, editable);
    if (undo_) EnableWindow(undo_, !loading &&
        (TagEditService::instance().hasUndo() ||
         MediaFileOperationService::instance().hasInPlaceUndo()));
    updateDirtyUi();
}

void TagEditorWindow::scheduleTrackRows()
{
    if (!window_ || !IsWindowVisible(window_) || !trackLoading_ ||
            (!pendingTracks_ && !refreshingTrackMetadata_) || trackRowsPosted_) return;
    const auto generation = scanGeneration_.load(std::memory_order_acquire);
    trackRowsPosted_ = PostMessageW(window_, kTrackRowsMessage,
        static_cast<WPARAM>(generation), 0) != FALSE;
    if (!trackRowsPosted_)
        SetTimer(window_, kTrackRowsTimer, 16, nullptr);
}

void TagEditorWindow::publishTrackRows(std::uint64_t generation)
{
    if (refreshingTrackMetadata_) {
        publishTrackMetadata(generation);
        return;
    }
    if (!window_ || !IsWindowVisible(window_) || !trackLoading_ ||
            !pendingTracks_ || generation != scanGeneration_.load()) return;
    const auto started = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    while (pendingTrackIndex_ < pendingTracks_->get_count() &&
            processed < kMaxUiBatchRows &&
            (processed == 0 ||
                std::chrono::steady_clock::now() - started < kUiBatchBudget)) {
        const auto track = (*pendingTracks_)[pendingTrackIndex_++];
        ++processed;
        if (!track.is_valid()) continue;
        pfc::string8 title;
        track->format_title(nullptr, title, pendingTitleScript_, nullptr);
        const auto display = utf8ToWide(title.c_str());
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        nekorox::TagDocumentItem documentItem;
        documentItem.itemId = itemId(track);
        itemIds_.push_back(documentItem.itemId);
        readCachedMetadata(track, documentItem.metadata);
        tracks_.add_item(track);
        pendingDocumentItems_.push_back(std::move(documentItem));
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, FALSE);
    if (pendingTrackIndex_ < pendingTracks_->get_count()) {
        setStatus(L"Loading track details · " +
            std::to_wstring(pendingTrackIndex_) + L" / " +
            std::to_wstring(pendingTracks_->get_count()));
        continueUiBatch(window_, kTrackRowsTimer, kTrackRowsMessage, generation);
        return;
    }
    pendingTracks_.reset();
    pendingTitleScript_.release();
    tagDocument_.reset(std::move(pendingDocumentItems_));
    setTrackLoading(false);
    if (tracks_.get_count() != 0)
        SendMessageW(list_, LB_SETSEL, TRUE, 0);
    selectionChanged();
    for (std::size_t role = 0; role < artworkMutations_.size(); ++role) {
        artworkMutations_[role] = {kArtworkRoles[role], ArtworkMutationMode::keep, {}};
        SendMessageW(artworkModes_[role], CB_SETCURSEL, 0, 0);
        SetWindowTextW(artworkChoose_[role], L"Choose image…");
    }
    if (artworkPreviewBitmap_) {
        SendMessageW(artworkPreview_, STM_SETIMAGE, IMAGE_BITMAP, 0);
        DeleteObject(artworkPreviewBitmap_);
        artworkPreviewBitmap_ = nullptr;
    }
    refreshArtworkStates();
    invalidateOrganizationPreview();
    EnableWindow(executeCopy_, FALSE);
    setStatus(std::to_wstring(tracks_.get_count()) +
        L" loaded · " + std::to_wstring(tagDocument_.selectedIds().size()) +
        L" selected · nothing pending");
}

void TagEditorWindow::refreshTrackMetadataForPaths(const std::set<std::wstring>& paths)
{
    std::set<std::string> refreshedItems;
    if (!paths.empty()) {
        for (std::size_t index = 0; index < tracks_.get_count() &&
                index < itemIds_.size(); ++index) {
            if (paths.contains(foldedPath(nativeTrackPath(tracks_[index]))))
                refreshedItems.insert(itemIds_[index]);
        }
    }
    refreshTrackMetadata(refreshedItems);
}

void TagEditorWindow::refreshTrackMetadata(const std::set<std::string>& itemIds)
{
    if (!window_ || shuttingDown_.load(std::memory_order_acquire) || trackLoading_)
        return;
    metadataRefreshRows_.clear();
    if (!itemIds.empty()) {
        for (std::size_t index = 0; index < itemIds_.size(); ++index) {
            if (itemIds.contains(itemIds_[index])) metadataRefreshRows_.push_back(index);
        }
    }
    if (metadataRefreshRows_.empty()) { refreshFields(); return; }
    // The file service has already refreshed these handles. Only cached metadata
    // and labels are refreshed here; do not rescan paths or reset edit drafts.
    invalidateOrganizationPreview();
    refreshingTrackMetadata_ = true;
    pendingTrackIndex_ = 0;
    titleformat_compiler::get()->compile_safe(pendingTitleScript_,
        "$if2(%album artist% - %album% / %artist% - %title%,$filename(%path%))");
    setTrackLoading(true);
    scheduleTrackRows();
}

void TagEditorWindow::publishTrackMetadata(std::uint64_t generation)
{
    if (!window_ || !IsWindowVisible(window_) || !refreshingTrackMetadata_ ||
        generation != scanGeneration_.load()) return;
    const auto started = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    const auto top = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
    const auto caret = SendMessageW(list_, LB_GETCARETINDEX, 0, 0);
    const auto anchor = SendMessageW(list_, LB_GETANCHORINDEX, 0, 0);
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    while (pendingTrackIndex_ < metadataRefreshRows_.size() &&
        processed < kMaxUiBatchRows && (processed == 0 ||
            std::chrono::steady_clock::now() - started < kUiBatchBudget)) {
        const auto index = metadataRefreshRows_[pendingTrackIndex_++];
        ++processed;
        if (index >= tracks_.get_count()) continue;
        const auto& track = tracks_[index];
        if (!track.is_valid() || index >= itemIds_.size()) continue;
        nekorox::TagValueMap metadata;
        if (!readCachedMetadata(track, metadata) ||
            !tagDocument_.refreshMetadata(index, itemIds_[index], std::move(metadata)))
            continue;
        pfc::string8 title;
        track->format_title(nullptr, title, pendingTitleScript_, nullptr);
        const auto display = utf8ToWide(title.c_str());
        const auto selected = SendMessageW(list_, LB_GETSEL, index, 0) > 0;
        SendMessageW(list_, LB_DELETESTRING, index, 0);
        SendMessageW(list_, LB_INSERTSTRING, index,
            reinterpret_cast<LPARAM>(display.c_str()));
        if (selected) SendMessageW(list_, LB_SETSEL, TRUE, index);
    }
    if (caret >= 0) SendMessageW(list_, LB_SETCARETINDEX, caret, FALSE);
    if (anchor >= 0) SendMessageW(list_, LB_SETANCHORINDEX, anchor, 0);
    if (top >= 0) SendMessageW(list_, LB_SETTOPINDEX, top, 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, FALSE);
    if (pendingTrackIndex_ < metadataRefreshRows_.size()) {
        continueUiBatch(window_, kTrackRowsTimer, kTrackRowsMessage, generation);
        return;
    }
    refreshingTrackMetadata_ = false;
    metadataRefreshRows_.clear();
    pendingTitleScript_.release();
    setTrackLoading(false);
    refreshFields();
}

void TagEditorWindow::refreshLibraryRoots()
{
    if (!window_ || !IsWindowVisible(window_) || IsIconic(window_)) {
        LibraryRootsProvider::instance().suspend();
        return;
    }
    LibraryRootsProvider::instance().observe([this](auto) {
        if (window_ && IsWindowVisible(window_) && !IsIconic(window_)) showLibraryRoots();
    });
}

void TagEditorWindow::showLibraryRoots()
{
    if (!roots_) return;
    const auto snapshot = LibraryRootsProvider::instance().snapshot();
    SendMessageW(roots_, LB_RESETCONTENT, 0, 0);
    if (!selectedDestination_.empty()) {
        const auto selected = std::wstring(L"Selected destination · manual verified") +
            (selectedDestinationAddable_ ? L" · addable · "
                                         : L" · outside current library policy · ") +
            selectedDestination_.wstring();
        SendMessageW(roots_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(selected.c_str()));
    }
    if (!snapshot->configuredDestination.empty()) {
        const auto configured = L"Configured file-operations destination · " +
            snapshot->configuredDestination.wstring();
        SendMessageW(roots_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(configured.c_str()));
    }
    for (const auto& root : snapshot->roots) {
        auto text = std::wstring(L"Observed library root · ") +
            (root.online ? L"online" : L"offline") +
            (root.policyAddable ? L" · addable · " : L" · not addable · ") +
            root.path.wstring();
        SendMessageW(roots_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (SendMessageW(roots_, LB_GETCOUNT, 0, 0) == 0) {
        SendMessageW(roots_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(
                L"No observed roots. Choose a destination or open Library preferences."));
    }
}

void TagEditorWindow::chooseDestination()
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        setStatus(L"Folder picker is unavailable.");
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
        FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
    if (FAILED(dialog->Show(window_))) { dialog->Release(); return; }
    IShellItem* selected = nullptr;
    if (FAILED(dialog->GetResult(&selected))) { dialog->Release(); return; }
    PWSTR path = nullptr;
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        selected->Release();
        dialog->Release();
        return;
    }
    const std::filesystem::path candidate(path);
    CoTaskMemFree(path);
    selected->Release();
    dialog->Release();
    std::error_code error;
    const auto online = std::filesystem::is_directory(candidate, error) && !error;
    bool addable = false;
    if (online) {
        const auto probe = candidate / L"goden-library-policy-probe.flac";
        try {
            addable = libraryPolicyAddable(probe);
        } catch (...) {
        }
    }
    if (!online) {
        setStatus(L"Selected folder is offline or inaccessible.");
        return;
    }
    selectedDestination_ = candidate.lexically_normal();
    selectedDestinationAddable_ = addable;
    invalidateOrganizationPreview();
    EnableWindow(executeCopy_, FALSE);
    showLibraryRoots();
    setStatus(addable
        ? L"Manual destination verified and addable. No files move until explicit Copy."
        : L"Manual destination verified but outside current library policy; Copy is safe, automatic library inclusion needs Preferences.");
}

void TagEditorWindow::invalidateOrganizationPreview()
{
    const bool hadPreview = organizationPreviewPending_ || copyPlan_.has_value();
    organizationPreviewPending_ = false;
    organizationGeneration_.fetch_add(1, std::memory_order_acq_rel);
    organizationWorker_.cancelPending();
    previewPreparation_.reset();
    previewPreparationPosted_ = false;
    previewRowsPosted_ = false;
    previewRowIndex_ = 0;
    if (window_) {
        KillTimer(window_, kPreviewPreparationTimer);
        KillTimer(window_, kPreviewRowsTimer);
    }
    copyPlan_.reset();
    plannedTagMutations_.clear();
    plannedArtworkMutations_.clear();
    plannedMoveIdentities_.clear();
    if (executeCopy_) EnableWindow(executeCopy_, FALSE);
    if (hadPreview && organizationPreview_)
        SendMessageW(organizationPreview_, LB_RESETCONTENT, 0, 0);
    if (hadPreview && !shuttingDown_.load(std::memory_order_acquire))
        setStatus(L"Preview cleared. Generate a new preview before organizing files.");
}

void TagEditorWindow::previewOrganization()
{
    if (!rolloutFlagEnabled(RolloutFlag::tagImportV1)) return;
    if (trackLoading_ || applying_ ||
            MediaFileOperationService::instance().busy()) return;
    ++resultGeneration_;
    resultRows_.reset();
    resultRowsPosted_ = false;
    if (window_) KillTimer(window_, kResultRowsTimer);
    invalidateOrganizationPreview();
    if (selectedDestination_.empty() || tracks_.get_count() == 0) {
        setStatus(L"Choose a verified destination and add tracks before preview.");
        return;
    }
    std::vector<nekorox::TagMutation> mutations;
    if (!collectTagMutations(mutations)) return;
    plannedTagMutations_ = mutations;
    plannedArtworkMutations_.clear();
    for (const auto& artwork : artworkMutations_) {
        if ((artwork.mode == ArtworkMutationMode::set ||
                artwork.mode == ArtworkMutationMode::setSidecar) &&
            (!artwork.originalBytes || artwork.originalBytes->empty())) {
            setStatus(L"Set artwork requires a validated image.");
            return;
        }
        plannedArtworkMutations_.push_back(artwork);
    }
    const auto mode = SendMessageW(organizationMode_, CB_GETCURSEL, 0, 0) == 1
        ? MediaOrganizationMode::move : MediaOrganizationMode::copy;
    const auto collisionSelection = SendMessageW(collisionMode_, CB_GETCURSEL, 0, 0);
    const auto collision = collisionSelection == 1 ? MediaCollisionPolicy::rename
        : collisionSelection == 2 ? MediaCollisionPolicy::replace
                                  : MediaCollisionPolicy::skip;
    SendMessageW(organizationPreview_, LB_RESETCONTENT, 0, 0);
    organizationPreviewPending_ = true;
    setStatus(L"Preparing organization preview… You can continue editing.");
    previewPreparation_.emplace();
    previewPreparation_->mutations = std::move(mutations);
    previewPreparation_->destination = selectedDestination_;
    previewPreparation_->mode = mode;
    previewPreparation_->collision = collision;
    schedulePreviewPreparation();
}

void TagEditorWindow::schedulePreviewPreparation()
{
    if (!window_ || !IsWindowVisible(window_) || !previewPreparation_ ||
            previewPreparationPosted_) return;
    const auto generation = organizationGeneration_.load(std::memory_order_acquire);
    previewPreparationPosted_ = PostMessageW(window_, kPreviewPreparationMessage,
        static_cast<WPARAM>(generation), 0) != FALSE;
    if (!previewPreparationPosted_)
        SetTimer(window_, kPreviewPreparationTimer, 16, nullptr);
}

void TagEditorWindow::preparePreviewBatch(std::uint64_t generation)
{
    if (!window_ || !IsWindowVisible(window_) || !previewPreparation_ ||
            generation != organizationGeneration_.load()) return;
    auto& preparation = *previewPreparation_;
    const auto started = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    while (preparation.index < tracks_.get_count() &&
            processed < kMaxUiBatchRows &&
            (processed == 0 ||
                std::chrono::steady_clock::now() - started < kUiBatchBudget)) {
        const auto& track = tracks_[preparation.index++];
        ++processed;
        if (!track.is_valid()) continue;
        const auto source = nativeTrackPath(track);
        if (!preparation.buildingRequests) {
            preparation.identities.push_back({source, {},
                static_cast<std::uint32_t>(track->get_subsong_index())});
            const auto disc = firstFieldValue(track, "DISCNUMBER");
            if (!disc.empty()) preparation.discs.insert(disc);
            continue;
        }
        if (source.empty()) continue;
        auto key = source.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (!preparation.seen.insert(key).second) continue;
        auto year = plannedFieldValue(track, "DATE", preparation.mutations);
        if (year.size() > 4) year.resize(4);
        MediaNamingFields fields{
            plannedFieldValue(track, "ALBUM ARTIST", preparation.mutations),
            plannedFieldValue(track, "ARTIST", preparation.mutations),
            plannedFieldValue(track, "ALBUM", preparation.mutations),
            std::move(year),
            plannedFieldValue(track, "DISCNUMBER", preparation.mutations),
            plannedFieldValue(track, "TRACKNUMBER", preparation.mutations),
            plannedFieldValue(track, "TITLE", preparation.mutations),
            static_cast<unsigned>((std::max)(std::size_t{1},
                preparation.discs.size()))};
        preparation.requests.push_back({source,
            buildDefaultRelativeDestination(source, fields)});
    }
    if (preparation.index < tracks_.get_count()) {
        continueUiBatch(window_, kPreviewPreparationTimer,
            kPreviewPreparationMessage, generation);
        return;
    }
    if (!preparation.buildingRequests) {
        preparation.buildingRequests = true;
        preparation.index = 0;
        continueUiBatch(window_, kPreviewPreparationTimer,
            kPreviewPreparationMessage, generation);
        return;
    }
    auto requests = std::move(preparation.requests);
    auto identities = std::move(preparation.identities);
    const auto destination = preparation.destination;
    const auto mode = preparation.mode;
    const auto collision = preparation.collision;
    previewPreparation_.reset();
    if (!organizationWorker_.submit([this, generation, destination, mode, collision,
            requests = std::move(requests), identities = std::move(identities)](
                std::stop_token stop) mutable {
        std::optional<MediaPlan> plan;
        try {
            // SDK handles and control values were copied on the UI thread.
            // Only immutable paths and metadata cross this worker boundary.
            for (auto& seed : identities) {
                if (stop.stop_requested()) return;
                std::error_code error;
                const auto size = std::filesystem::file_size(seed.source, error);
                if (error) continue;
                const auto modified = std::filesystem::last_write_time(seed.source, error);
                if (error) continue;
                seed.previousRevisionTrackId = nekorox::makeTrackId({seed.source,
                    seed.subsong, size,
                    static_cast<std::uint64_t>(modified.time_since_epoch().count())}).value;
            }
            std::erase_if(identities, [](const auto& seed) {
                return seed.previousRevisionTrackId.empty();
            });
            if (stop.stop_requested()) return;
            plan = buildMediaPlan(destination, requests, mode, collision, stop);
        } catch (...) {
            plan.reset();
        }
        if (stop.stop_requested()) return;
        fb2k::inMainThread([this, generation, mode, plan = std::move(plan),
                identities = std::move(identities)]() mutable {
            if (shuttingDown_.load(std::memory_order_acquire) || !window_ ||
                    !IsWindowVisible(window_) ||
                    generation != organizationGeneration_.load(std::memory_order_acquire)) return;
            if (!plan) {
                organizationPreviewPending_ = false;
                setStatus(L"Could not prepare the preview. Check the source and destination folders.");
                return;
            }
            copyPlan_ = std::move(plan);
            plannedMoveIdentities_ = std::move(identities);
            previewRowIndex_ = 0;
            previewReady_ = previewSkipped_ = previewRejected_ = 0;
            EnableWindow(executeCopy_, FALSE);
            schedulePreviewRows();
        });
    })) {
        organizationPreviewPending_ = false;
        setStatus(L"Could not start the organization preview.");
    }
}

void TagEditorWindow::schedulePreviewRows()
{
    if (!window_ || !IsWindowVisible(window_) || !copyPlan_ ||
            !organizationPreviewPending_ || previewRowsPosted_) return;
    const auto generation = organizationGeneration_.load(std::memory_order_acquire);
    previewRowsPosted_ = PostMessageW(window_, kPreviewRowsMessage,
        static_cast<WPARAM>(generation), 0) != FALSE;
    if (!previewRowsPosted_)
        SetTimer(window_, kPreviewRowsTimer, 16, nullptr);
}

void TagEditorWindow::publishPreviewRows(std::uint64_t generation)
{
    if (!window_ || !IsWindowVisible(window_) || !copyPlan_ ||
            !organizationPreviewPending_ ||
            generation != organizationGeneration_.load()) return;
    const auto started = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    SendMessageW(organizationPreview_, WM_SETREDRAW, FALSE, 0);
    while (previewRowIndex_ < copyPlan_->items.size() &&
            processed < kMaxUiBatchRows &&
            (processed == 0 ||
                std::chrono::steady_clock::now() - started < kUiBatchBudget)) {
        const auto& item = copyPlan_->items[previewRowIndex_++];
        ++processed;
        if (item.disposition == MediaPlanDisposition::ready) ++previewReady_;
        else if (item.disposition == MediaPlanDisposition::skipped) ++previewSkipped_;
        else ++previewRejected_;
        const auto line = item.source.filename().wstring() + L" → " +
            item.destination.wstring() + L" — " + item.detail;
        SendMessageW(organizationPreview_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(line.c_str()));
    }
    SendMessageW(organizationPreview_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(organizationPreview_, nullptr, FALSE);
    if (previewRowIndex_ < copyPlan_->items.size()) {
        setStatus(L"Showing organization preview · " +
            std::to_wstring(previewRowIndex_) + L" / " +
            std::to_wstring(copyPlan_->items.size()));
        continueUiBatch(window_, kPreviewRowsTimer, kPreviewRowsMessage,
            generation);
        return;
    }
    organizationPreviewPending_ = false;
    EnableWindow(executeCopy_, previewReady_ != 0 && previewRejected_ == 0 &&
        !trackLoading_);
    setStatus(std::wstring(copyPlan_->mode == MediaOrganizationMode::move
            ? L"Move" : L"Copy") +
        L" preview · ready " + std::to_wstring(previewReady_) +
        L" · skipped " + std::to_wstring(previewSkipped_) +
        L" · rejected " + std::to_wstring(previewRejected_) +
        (dirty_ ? L" · pending tags will be written only to destination" : L""));
}

void TagEditorWindow::scheduleResultRows()
{
    if (!window_ || !IsWindowVisible(window_) || !resultRows_ ||
            resultRowsPosted_) return;
    resultRowsPosted_ = PostMessageW(window_, kResultRowsMessage,
        static_cast<WPARAM>(resultGeneration_), 0) != FALSE;
    if (!resultRowsPosted_)
        SetTimer(window_, kResultRowsTimer, 16, nullptr);
}

void TagEditorWindow::publishResultRows(std::uint64_t generation)
{
    if (!window_ || !IsWindowVisible(window_) || !resultRows_ ||
            generation != resultGeneration_) return;
    const auto started = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    SendMessageW(organizationPreview_, WM_SETREDRAW, FALSE, 0);
    while (resultRowIndex_ < resultRows_->size() &&
            processed < kMaxUiBatchRows &&
            (processed == 0 ||
                std::chrono::steady_clock::now() - started < kUiBatchBudget)) {
        const auto& item = (*resultRows_)[resultRowIndex_++];
        ++processed;
        const auto line = item.source.filename().wstring() + L" → " +
            item.destination.wstring() + L" — " + item.detail;
        SendMessageW(organizationPreview_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(line.c_str()));
    }
    SendMessageW(organizationPreview_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(organizationPreview_, nullptr, FALSE);
    if (resultRowIndex_ < resultRows_->size()) {
        setStatus(L"Showing organization results · " +
            std::to_wstring(resultRowIndex_) + L" / " +
            std::to_wstring(resultRows_->size()));
        continueUiBatch(window_, kResultRowsTimer, kResultRowsMessage,
            generation);
        return;
    }
    resultRows_.reset();
    setStatus(resultSummary_);
}

void TagEditorWindow::executeOrganization()
{
    if (!copyPlan_ || organizationPreviewPending_ || trackLoading_ || applying_ ||
        !rolloutFlagEnabled(RolloutFlag::tagImportV1)) return;
    MediaCopyExecutionOptions options;
    static constexpr std::array<const wchar_t*, 4> sidecarStems{
        L"folder", L"back", L"artist", L"disc"};
    for (std::size_t index = 0; index < plannedArtworkMutations_.size(); ++index) {
        const auto& artwork = plannedArtworkMutations_[index];
        if (artwork.mode != ArtworkMutationMode::setSidecar) continue;
        options.sidecars.push_back({sidecarStems[index], artwork.sourceExtension,
            artwork.originalBytes});
    }
    if (dirty_) {
        const auto edits = StagingMediaEdits{
            plannedTagMutations_, plannedArtworkMutations_};
        options.mutateStaging = [edits](const std::filesystem::path& path,
            std::wstring& error) { return applyAndVerifyStagingEdits(path, edits, error); };
    }
    const auto operationMode = copyPlan_->mode;
    const auto moveIdentities = plannedMoveIdentities_;
    applying_ = MediaFileOperationService::instance().startPlan(*copyPlan_,
        [this, moveIdentities](std::vector<MediaCopyResult> results) {
            applying_ = false;
            invalidateOrganizationPreview();
            auto completed = std::make_shared<std::vector<MediaCopyResult>>(
                std::move(results));
            std::size_t copied = 0, moved = 0, skipped = 0, attention = 0;
            for (const auto& item : *completed) {
                const bool sidecarIncomplete = item.sidecarsExpected != 0 &&
                    item.sidecarsCommitted != item.sidecarsExpected;
                if (sidecarIncomplete) ++attention;
                else if (item.outcome == MediaCopyOutcome::copied) ++copied;
                else if (item.outcome == MediaCopyOutcome::moved) ++moved;
                else if (item.outcome == MediaCopyOutcome::skipped) ++skipped;
                else ++attention;
            }
            if (window_) {
                EnableWindow(executeCopy_, FALSE);
                resultSummary_ = L"Organization complete · copied " + std::to_wstring(copied) +
                    L" · moved " + std::to_wstring(moved) +
                    L" · skipped " + std::to_wstring(skipped) +
                    L" · needs attention " + std::to_wstring(attention);
                setStatus(resultSummary_);
                ++resultGeneration_;
                resultRows_ = completed;
                resultRowIndex_ = 0;
                resultRowsPosted_ = false;
                SendMessageW(organizationPreview_, LB_RESETCONTENT, 0, 0);
                scheduleResultRows();
            }
            if (window_) updateDirtyUi();
            if (copied != 0 || moved != 0) LibraryModel::instance().invalidate();
            if (moved != 0) {
                std::map<std::wstring, std::vector<const MoveIdentitySeed*>,
                    std::less<>> seedsBySource;
                for (const auto& seed : moveIdentities) {
                    auto key = seed.source.lexically_normal().wstring();
                    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
                    seedsBySource[std::move(key)].push_back(&seed);
                }
                std::vector<TrackRevisionRebind> rebinds;
                for (const auto& result : *completed) {
                    if (result.outcome != MediaCopyOutcome::moved) continue;
                    auto resultSource = result.source.lexically_normal().wstring();
                    std::transform(resultSource.begin(), resultSource.end(),
                        resultSource.begin(), ::towlower);
                    if (const auto found = seedsBySource.find(resultSource);
                            found != seedsBySource.end()) {
                        for (const auto* seed : found->second) rebinds.push_back({
                            seed->previousRevisionTrackId, result.destination,
                            seed->subsong});
                    }
                }
                if (!rebinds.empty()) DatabaseService::instance().rebindTagRevisions(
                    std::move(rebinds), [](bool success, std::string error) {
                        if (!success) {
                            console::formatter log;
                            log << "GODENplayer TAG Move identity rebind failed: "
                                << error.c_str();
                        }
                    });
            }
        }, std::move(options));
    if (applying_) {
        EnableWindow(executeCopy_, FALSE);
        setStatus(operationMode == MediaOrganizationMode::move
            ? L"Moving with verified journal and source-protection boundary…"
            : L"Copying through verified staging…");
    }
}

bool TagEditorWindow::collectTagMutations(
    std::vector<nekorox::TagMutation>& mutations)
{
    mutations.clear();
    for (std::size_t index = 0; index < kFieldDescriptors.size(); ++index) {
        nekorox::TagMutation mutation;
        mutation.field = kFieldDescriptors[index].field;
        const auto selected = SendMessageW(modes_[index], CB_GETCURSEL, 0, 0);
        mutation.mode = selected == 1 ? nekorox::TagMutationMode::set
            : selected == 2 ? nekorox::TagMutationMode::remove
                            : nekorox::TagMutationMode::keep;
        if (mutation.mode == nekorox::TagMutationMode::set) {
            mutation.values = splitValues(controlText(edits_[index]));
            if (mutation.values.empty()) {
                setStatus(L"Set requires at least one non-empty line.");
                return false;
            }
        }
        mutations.push_back(std::move(mutation));
    }
    std::string validationError;
    if (!nekorox::validateTagMutations(mutations, validationError)) {
        setStatus(L"Invalid tag edit: " + utf8ToWide(validationError));
        return false;
    }
    return true;
}

void TagEditorWindow::chooseArtwork(std::size_t role)
{
    if (trackLoading_ || role >= artworkMutations_.size() || applying_) return;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        setStatus(L"Artwork picker is unavailable.");
        return;
    }
    const COMDLG_FILTERSPEC filters[]{{L"Images", L"*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.gif"},
        {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
        FOS_DONTADDTORECENT);
    if (FAILED(dialog->Show(window_))) { dialog->Release(); return; }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) { dialog->Release(); return; }
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
        item->Release();
        dialog->Release();
        return;
    }
    const std::filesystem::path path(rawPath);
    CoTaskMemFree(rawPath);
    item->Release();
    dialog->Release();
    beginArtwork(std::move(path), role);
}

void TagEditorWindow::beginArtwork(std::filesystem::path path, std::size_t role)
{
    if (trackLoading_ || role >= artworkMutations_.size() ||
        shuttingDown_.load(std::memory_order_acquire)) return;
    const auto generation = artworkGeneration_.fetch_add(1) + 1;
    const auto targetIds = tagDocument_.selectedIds();
    if (targetIds.empty()) {
        setStatus(L"Select at least one loaded file before choosing artwork.");
        return;
    }
    setStatus(L"Validating artwork bytes and dimensions…");
    if (!artworkWorker_.submit([this, path, role, generation, targetIds](std::stop_token stop) {
        ValidatedArtwork validated;
        std::wstring error;
        const auto valid = !stop.stop_requested() &&
            loadAndValidateArtwork(path, validated, error, stop);
        if (stop.stop_requested()) return;
        fb2k::inMainThread([this, path, role, generation, valid, targetIds,
                validated = std::move(validated), error = std::move(error)]() mutable {
            if (!window_ || generation != artworkGeneration_.load()) return;
            if (!valid) {
                setStatus(error);
                return;
            }
            auto sharedBytes = std::make_shared<const ValidatedBytes>(
                std::move(validated.originalBytes));
            std::set<const ValidatedBytes*> uniquePayloads;
            std::uint64_t aggregateBytes = sharedBytes->size();
            uniquePayloads.insert(sharedBytes.get());
            for (const auto& [itemId, drafts] : artworkDrafts_) {
                for (std::size_t draftRole = 0;
                     draftRole < drafts.size(); ++draftRole) {
                    if (draftRole == role && targetIds.contains(itemId))
                        continue;
                    const auto& bytes = drafts[draftRole].mutation.originalBytes;
                    if (bytes && uniquePayloads.insert(bytes.get()).second)
                        aggregateBytes += bytes->size();
                }
            }
            if (aggregateBytes > kArtworkDraftBudget) {
                setStatus(L"Artwork drafts exceed the 64 MiB shared budget; "
                    L"apply or remove another pending image first.");
                return;
            }
            const auto bitmap = previewBitmap(validated);
            if (bitmap) {
                const auto previous = reinterpret_cast<HBITMAP>(SendMessageW(
                    artworkPreview_, STM_SETIMAGE, IMAGE_BITMAP,
                    reinterpret_cast<LPARAM>(bitmap)));
                if (previous) DeleteObject(previous);
                artworkPreviewBitmap_ = bitmap;
            }
            artworkMutations_[role].role = kArtworkRoles[role];
            const auto requestedMode = SendMessageW(
                artworkModes_[role], CB_GETCURSEL, 0, 0) == 3
                ? ArtworkMutationMode::setSidecar : ArtworkMutationMode::set;
            artworkMutations_[role].mode = requestedMode;
            artworkMutations_[role].originalBytes = std::move(sharedBytes);
            artworkMutations_[role].sourceExtension = validated.format;
            for (const auto& item : targetIds) {
                auto& drafts = artworkDrafts_[item];
                for (std::size_t draftRole = 0; draftRole < drafts.size(); ++draftRole)
                    drafts[draftRole].mutation.role = kArtworkRoles[draftRole];
                drafts[role] = {artworkMutations_[role],
                    nextArtworkDraftRevision_++,
                    artworkContentHash(artworkMutations_[role].originalBytes)};
            }
            SendMessageW(artworkModes_[role], CB_SETCURSEL,
                requestedMode == ArtworkMutationMode::setSidecar ? 3 : 1, 0);
            const auto label = path.filename().wstring() + L" · " +
                std::to_wstring(validated.width) + L"×" +
                std::to_wstring(validated.height) + L" · " + validated.mime +
                L" · original bytes";
            SetWindowTextW(artworkChoose_[role], label.c_str());
            setDirty(true);
            setStatus(requestedMode == ArtworkMutationMode::setSidecar
                ? L"Sidecar artwork staged; it will be written only beside an organized destination."
                : L"Embedded artwork staged; unselected roles will be preserved.");
        });
    })) setStatus(L"The persistent artwork worker could not start.");
}

void TagEditorWindow::refreshArtworkStates()
{
    if (trackLoading_ || shuttingDown_.load(std::memory_order_acquire)) return;
    const auto generation = artworkGeneration_.fetch_add(1) + 1;
    std::vector<std::filesystem::path> paths;
    std::set<std::wstring, std::less<>> seen;
    const auto& selectedIds = tagDocument_.selectedIds();
    std::size_t matched = 0;
    for (t_size index = 0; index < tracks_.get_count(); ++index) {
        if (index >= itemIds_.size() || !selectedIds.contains(itemIds_[index])) continue;
        ++matched;
        auto path = nativeTrackPath(tracks_[index]);
        auto key = path.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (!path.empty() && seen.insert(key).second) paths.push_back(std::move(path));
        if (matched == selectedIds.size()) break;
    }
    if (!artworkWorker_.submit([this, paths = std::move(paths), generation](
            std::stop_token stop) {
        std::array<std::size_t, 4> present{};
        for (const auto& path : paths) {
            if (stop.stop_requested()) return;
            try {
                pfc::string8 canonical;
                filesystem::g_get_canonical_path(
                    wideToUtf8(path.wstring()).c_str(), canonical);
                abort_callback_impl abort;
                std::stop_callback cancelled(stop, [&abort] { abort.abort(); });
                auto extractor = album_art_extractor::g_open_allowempty(
                    nullptr, canonical.c_str(), abort);
                for (std::size_t role = 0; role < kArtworkRoles.size(); ++role) {
                    try {
                        const GUID* guid = &album_art_ids::cover_front;
                        if (role == 1) guid = &album_art_ids::cover_back;
                        else if (role == 2) guid = &album_art_ids::artist;
                        else if (role == 3) guid = &album_art_ids::disc;
                        if (extractor->query(*guid, abort).is_valid()) ++present[role];
                    } catch (...) {
                    }
                }
            } catch (...) {
            }
        }
        if (stop.stop_requested()) return;
        fb2k::inMainThread([this, generation, present, total = paths.size()] {
            if (!window_ || generation != artworkGeneration_.load()) return;
            for (std::size_t role = 0; role < kArtworkRoles.size(); ++role) {
                const auto state = present[role] == 0 ? L"none"
                    : present[role] == total ? L"all" : L"mixed";
                const auto label = std::wstring(kArtworkLabels[role]) + L" · " + state;
                SetWindowTextW(artworkLabels_[role], label.c_str());
            }
        });
    })) setStatus(L"The persistent artwork scanner could not start.");
}

void TagEditorWindow::refreshFields()
{
    internalControlUpdate_ = true;
    for (std::size_t field = 0; field < kFieldDescriptors.size(); ++field) {
        if (std::strcmp(kFieldDescriptors[field].field, "__ADVANCED__") == 0) {
            const auto aggregate = tagDocument_.aggregate("__ADVANCED__");
            const auto invalid = !aggregate.mixed &&
                aggregate.intent == nekorox::TagMutationMode::invalidPending;
            SendMessageW(modes_[field], CB_SETCURSEL, invalid ? 1 : 0, 0);
            SetWindowTextW(edits_[field], invalid
                ? utf8ToWide(aggregate.rawText).c_str() : L"");
            SendMessageW(edits_[field], EM_SETCUEBANNER, TRUE,
                reinterpret_cast<LPARAM>(invalid
                    ? utf8ToWide(aggregate.error).c_str()
                    : L"FIELD=value 1 | value 2 (one field per line)"));
            const auto enabled = !tagDocument_.selectedIds().empty();
            EnableWindow(labels_[field], enabled);
            EnableWindow(modes_[field], enabled);
            EnableWindow(edits_[field], enabled);
            continue;
        }
        const auto aggregate = tagDocument_.aggregate(kFieldDescriptors[field].field);
        const auto selectedMode = aggregate.mixed ? 0 :
            aggregate.intent == nekorox::TagMutationMode::set ? 1 :
            aggregate.intent == nekorox::TagMutationMode::invalidPending ? 1 :
            aggregate.intent == nekorox::TagMutationMode::remove ? 2 : 0;
        SendMessageW(modes_[field], CB_SETCURSEL, selectedMode, 0);
        const auto display = aggregate.mixed ? std::wstring{} :
            aggregate.intent == nekorox::TagMutationMode::invalidPending
                ? utf8ToWide(aggregate.rawText) : joinValues(aggregate.values);
        SendMessageW(edits_[field], EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(aggregate.mixed
                ? L"Mixed values or intents · edit to set selection"
                : aggregate.intent == nekorox::TagMutationMode::invalidPending
                    ? utf8ToWide(aggregate.error).c_str() : L""));
        SetWindowTextW(edits_[field], display.c_str());
        EnableWindow(labels_[field], aggregate.hasSelection);
        EnableWindow(modes_[field], aggregate.hasSelection);
        EnableWindow(edits_[field], aggregate.hasSelection);
    }
    internalControlUpdate_ = false;
}

void TagEditorWindow::selectionChanged()
{
    if (trackLoading_) return;
    invalidateOrganizationPreview();
    std::set<std::string> selected;
    const auto count = static_cast<int>(SendMessageW(list_, LB_GETSELCOUNT, 0, 0));
    if (count > 0) {
        std::vector<int> indices(static_cast<std::size_t>(count));
        SendMessageW(list_, LB_GETSELITEMS, count,
            reinterpret_cast<LPARAM>(indices.data()));
        for (const auto index : indices) {
            if (index >= 0 && static_cast<std::size_t>(index) < itemIds_.size())
                selected.insert(itemIds_[static_cast<std::size_t>(index)]);
        }
    }
    tagDocument_.select(std::move(selected));
    refreshFields();
    internalControlUpdate_ = true;
    for (std::size_t role = 0; role < artworkMutations_.size(); ++role) {
        bool first = true, mixed = false;
        ArtworkMutation common{kArtworkRoles[role], ArtworkMutationMode::keep, {}};
        for (const auto& id : tagDocument_.selectedIds()) {
            ArtworkMutation value{kArtworkRoles[role], ArtworkMutationMode::keep, {}};
            if (const auto found = artworkDrafts_.find(id);
                found != artworkDrafts_.end())
                value = found->second[role].mutation;
            if (first) { common = value; first = false; }
            else if (common.mode != value.mode ||
                !(common == value)) mixed = true;
        }
        artworkMutations_[role] = mixed
            ? ArtworkMutation{kArtworkRoles[role], ArtworkMutationMode::keep, {}}
            : common;
        const auto mode = mixed ? 0 : common.mode == ArtworkMutationMode::set ? 1
            : common.mode == ArtworkMutationMode::remove ? 2
            : common.mode == ArtworkMutationMode::setSidecar ? 3 : 0;
        SendMessageW(artworkModes_[role], CB_SETCURSEL, mode, 0);
    }
    internalControlUpdate_ = false;
    updateDirtyUi();
    refreshArtworkStates();
}

void TagEditorWindow::commitFieldIntent(std::size_t field)
{
    if (trackLoading_) return;
    invalidateOrganizationPreview();
    if (field >= kFieldDescriptors.size()) return;
    if (std::strcmp(kFieldDescriptors[field].field, "__ADVANCED__") == 0) {
        const auto selected = SendMessageW(modes_[field], CB_GETCURSEL, 0, 0);
        if (selected == 0) return;
        const auto rawText = controlText(edits_[field]);
        std::wistringstream lines(rawText);
        std::wstring line;
        std::vector<nekorox::TagMutation> parsed;
        std::string parseError;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty()) continue;
            const auto separator = line.find(L'=');
            const auto name = line.substr(0, separator);
            if (name.empty() || (selected != 2 && separator == std::wstring::npos)) {
                parseError = "Advanced fields require FIELD=value syntax";
                break;
            }
            nekorox::TagMutation advanced;
            advanced.field = wideToUtf8(name);
            advanced.mode = selected == 2 ? nekorox::TagMutationMode::remove
                                          : nekorox::TagMutationMode::set;
            if (advanced.mode == nekorox::TagMutationMode::set) {
                std::wistringstream values(line.substr(separator + 1));
                std::wstring value;
                while (std::getline(values, value, L'|')) {
                    const auto first = value.find_first_not_of(L" \t");
                    const auto last = value.find_last_not_of(L" \t");
                    if (first != std::wstring::npos)
                        advanced.values.push_back(wideToUtf8(
                            value.substr(first, last - first + 1)));
                }
                if (advanced.values.empty()) {
                    parseError = "Advanced Set requires a non-empty value";
                    break;
                }
            }
            parsed.push_back(std::move(advanced));
        }
        if (parseError.empty() && parsed.empty())
            parseError = "Advanced draft is empty";
        if (parseError.empty() &&
            !nekorox::validateTagMutations(parsed, parseError)) {
            // Validation is intentionally performed for the complete block
            // before any field draft is changed.
        }
        std::string error;
        if (!parseError.empty()) {
            nekorox::TagMutation invalid;
            invalid.field = "__ADVANCED__";
            invalid.mode = nekorox::TagMutationMode::invalidPending;
            invalid.rawText = wideToUtf8(rawText);
            invalid.error = parseError;
            (void)tagDocument_.applyIntent(invalid, error);
            setStatus(L"Invalid advanced draft: " + utf8ToWide(parseError));
            updateDirtyUi();
            return;
        }
        (void)tagDocument_.applyIntent(
            {"__ADVANCED__", nekorox::TagMutationMode::keep, {}}, error);
        for (const auto& advanced : parsed) {
            if (!tagDocument_.applyIntent(advanced, error)) {
                setStatus(L"Invalid advanced field: " + utf8ToWide(error));
                return;
            }
        }
        updateDirtyUi();
        return;
    }
    nekorox::TagMutation mutation;
    mutation.field = kFieldDescriptors[field].field;
    const auto selected = SendMessageW(modes_[field], CB_GETCURSEL, 0, 0);
    mutation.mode = selected == 1 ? nekorox::TagMutationMode::set
        : selected == 2 ? nekorox::TagMutationMode::remove
                        : nekorox::TagMutationMode::keep;
    if (mutation.mode == nekorox::TagMutationMode::set) {
        const auto rawText = controlText(edits_[field]);
        mutation.values = splitValues(rawText);
        if (mutation.values.empty()) {
            mutation.mode = nekorox::TagMutationMode::invalidPending;
            mutation.rawText = wideToUtf8(rawText);
            mutation.error = "Set requires at least one non-empty line";
            std::string error;
            (void)tagDocument_.applyIntent(mutation, error);
            setStatus(L"Set requires at least one non-empty line; Apply is blocked.");
            updateDirtyUi();
            return;
        }
    }
    std::string error;
    if (!tagDocument_.applyIntent(mutation, error)) {
        if (!error.empty() && error != "no tag items selected")
            setStatus(L"Invalid tag edit: " + utf8ToWide(error));
        return;
    }
    updateDirtyUi();
}

void TagEditorWindow::updateDirtyUi()
{
    const auto dirtyIds = tagDocument_.dirtyIds();
    const auto artworkDirty = !artworkDrafts_.empty();
    dirty_ = !dirtyIds.empty() || artworkDirty;
    std::set<std::string> affected = dirtyIds;
    for (const auto& [itemId, _] : artworkDrafts_) affected.insert(itemId);
    const auto invalid = tagDocument_.hasInvalidDrafts();
    const auto label = invalid ? L"Resolve invalid fields before Apply" :
        L"Apply changes to " + std::to_wstring(affected.size()) + L" files";
    if (apply_) SetWindowTextW(apply_, label.c_str());
    if (apply_) EnableWindow(apply_, dirty_ && !invalid && !applying_ &&
        !trackLoading_);
    if (window_) {
        auto caption = std::wstring(L"GODENplayer TAG");
        if (dirty_) {
            caption += L" · Pending edits in " +
                std::to_wstring(affected.size()) +
                (affected.size() == 1 ? L" file" : L" files") +
                L" (kept when closed)";
        }
        if (TagEditService::instance().undoRecoveryLimited())
            caption += L" · Undo recovery incomplete";
        SetWindowTextW(window_, caption.c_str());
    }
}

void TagEditorWindow::apply()
{
    if (trackLoading_) return;
    invalidateOrganizationPreview();
    if (applying_ || tracks_.get_count() == 0) return;
    // The native list is authoritative at the mutation boundary.  This also
    // protects against a delayed/missed notification during rapid mouse input.
    selectionChanged();
    if (tagDocument_.hasInvalidDrafts()) {
        const auto invalid = tagDocument_.firstInvalidDraft();
        setStatus(L"Apply blocked: " + utf8ToWide(invalid
            ? invalid->error : std::string("invalid tag draft")));
        return;
    }
    const auto frozen = tagDocument_.freezePlan();
    const auto frozenArtwork = artworkDrafts_;
    if (frozen.empty() && artworkDrafts_.empty()) {
        setStatus(L"No tag changes are pending for the loaded files.");
        return;
    }
    std::map<std::string, metadb_handle_ptr> handles;
    for (t_size index = 0; index < tracks_.get_count(); ++index)
        handles.emplace(itemIds_[index], tracks_[index]);
    std::set<std::string> previewIds;
    for (const auto& item : frozen) previewIds.insert(item.itemId);
    for (const auto& [itemId, _] : frozenArtwork) previewIds.insert(itemId);
    if (previewIds.size() > 1) {
        bool homogeneousTitle = false;
        std::optional<std::vector<std::string>> titleValue;
        std::set<std::string> changedFields;
        for (const auto& item : frozen) {
            for (const auto& mutation : item.mutations) {
                changedFields.insert(mutation.field);
                if (mutation.field != "TITLE" ||
                    mutation.mode != nekorox::TagMutationMode::set) continue;
                if (!titleValue) titleValue = mutation.values;
                else if (*titleValue == mutation.values) homogeneousTitle = true;
            }
        }
        std::wstring preview = L"Batch preview\n\nTargets: " +
            std::to_wstring(previewIds.size()) + L" files\nFields: ";
        if (changedFields.empty()) preview += L"artwork only";
        else {
            bool first = true;
            for (const auto& field : changedFields) {
                if (!first) preview += L", ";
                preview += utf8ToWide(field);
                first = false;
            }
            if (!frozenArtwork.empty()) preview += L", artwork";
        }
        preview += L"\n\nEvery file keeps its own immutable draft and "
            L"will be verified separately.";
        if (homogeneousTitle) {
            preview += L"\n\nWARNING: the same TITLE will be written to "
                L"multiple files.";
        }
        preview += L"\n\nApply this batch?";
        if (MessageBoxW(window_, preview.c_str(), L"GODENplayer TAG preview",
                MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
            setStatus(L"Batch Apply cancelled; drafts were preserved.");
            return;
        }
    }
    if (!frozenArtwork.empty()) {
        std::map<std::string, std::vector<nekorox::TagMutation>> tagsById;
        for (const auto& item : frozen) tagsById[item.itemId] = item.mutations;
        std::set<std::string> targetIds;
        for (const auto& [id, _] : tagsById) targetIds.insert(id);
        for (const auto& [id, _] : frozenArtwork) targetIds.insert(id);
        std::vector<MediaPlanRequest> requests;
        std::map<std::wstring, PhysicalFileEditPlan> editsByPath;
        std::map<std::wstring, std::set<std::string>> idsByPath;
        std::map<std::string, std::wstring> pathById;
        std::optional<std::vector<MediaSidecarWrite>> commonSidecars;
        static constexpr std::array<const wchar_t*, 4> sidecarStems{
            L"folder", L"back", L"artist", L"disc"};
        for (const auto& id : targetIds) {
            const auto found = handles.find(id);
            if (found == handles.end()) continue;
            const auto source = nativeTrackPath(found->second);
            if (source.empty()) continue;
            const auto key = foldedPath(source);
            auto [planEntry, insertedPath] = editsByPath.try_emplace(key);
            auto& edits = planEntry->second;
            if (insertedPath) edits.path = source;
            if (const auto tags = tagsById.find(id); tags != tagsById.end()) {
                const auto subsong = static_cast<std::uint32_t>(
                    found->second->get_subsong_index());
                const auto existing = edits.bySubsong.find(subsong);
                if (existing == edits.bySubsong.end())
                    edits.bySubsong.emplace(subsong, tags->second);
                else if (existing->second != tags->second) {
                    setStatus(L"Conflicting drafts target the same physical file/subsong.");
                    return;
                }
            }
            std::vector<MediaSidecarWrite> sidecars;
            ArtworkDelta embeddedArtwork;
            if (const auto art = frozenArtwork.find(id);
                art != frozenArtwork.end()) {
                for (std::size_t role = 0; role < art->second.size(); ++role) {
                    const auto& mutation = art->second[role].mutation;
                    if (mutation.mode == ArtworkMutationMode::setSidecar) {
                        sidecars.push_back({sidecarStems[role], mutation.sourceExtension,
                            mutation.originalBytes});
                    } else if (mutation.mode != ArtworkMutationMode::keep) {
                        embeddedArtwork.push_back(mutation);
                    }
                }
            }
            if (!embeddedArtwork.empty()) {
                if (edits.fileArtwork.empty()) edits.fileArtwork = embeddedArtwork;
                else if (edits.fileArtwork != embeddedArtwork) {
                    setStatus(L"Conflicting embedded artwork drafts target one physical file.");
                    return;
                }
            }
            if (!sidecars.empty()) {
                if (edits.sidecars.empty()) edits.sidecars = sidecars;
                else if (edits.sidecars != sidecars) {
                    setStatus(L"Conflicting sidecar drafts target one physical file.");
                    return;
                }
            }
            idsByPath[key].insert(id);
            pathById[id] = key;
        }
        for (const auto& [_, edits] : editsByPath) {
            requests.push_back({edits.path, {}});
            if (!commonSidecars) commonSidecars = edits.sidecars;
            else if (*commonSidecars != edits.sidecars) {
                setStatus(L"Different sidecar drafts per file require separate Apply operations.");
                return;
            }
        }
        auto mediaPlan = buildMediaPlan({}, requests,
            MediaOrganizationMode::inPlace, MediaCollisionPolicy::replace);
        MediaCopyExecutionOptions options;
        if (commonSidecars) options.sidecars = *commonSidecars;
        options.mutateStagingForSource = [editsByPath](const auto& source,
                const auto& staging, std::wstring& error) {
            const auto found = editsByPath.find(foldedPath(source));
            return found != editsByPath.end() &&
                applyAndVerifyStagingEdits(staging, found->second, error);
        };
        applying_ = MediaFileOperationService::instance().startPlan(
            std::move(mediaPlan), [this, frozen, frozenArtwork,
                idsByPath, pathById](auto results) {
                applying_ = false;
                std::size_t committed = 0, attention = 0, refreshFailures = 0, identityFailures = 0;
                std::set<std::wstring> committedPaths;
                for (const auto& result : results) {
                    if (result.outcome == MediaCopyOutcome::copied) {
                        ++committed;
                        if (result.identityRefreshFailed) ++identityFailures;
                        if (result.metadataRefreshFailed || result.identityRefreshFailed) ++attention;
                        if (result.metadataRefreshFailed) {
                            ++refreshFailures;
                            continue; // Keep the visible draft until metadata can be verified.
                        }
                        const auto path = foldedPath(result.source);
                        committedPaths.insert(path);
                        const auto found = idsByPath.find(path);
                        if (found != idsByPath.end()) {
                            for (const auto& id : found->second) {
                                const auto sent = frozenArtwork.find(id);
                                const auto current = artworkDrafts_.find(id);
                                if (sent == frozenArtwork.end() ||
                                    current == artworkDrafts_.end()) continue;
                                for (std::size_t role = 0;
                                     role < current->second.size(); ++role) {
                                    const auto& sentDraft = sent->second[role];
                                    auto& currentDraft = current->second[role];
                                    if (currentDraft.revision ==
                                            sentDraft.revision &&
                                        currentDraft.contentHash ==
                                            sentDraft.contentHash) {
                                        currentDraft = {};
                                        currentDraft.mutation.role =
                                            kArtworkRoles[role];
                                    }
                                }
                                if (std::all_of(current->second.begin(),
                                        current->second.end(), [](const auto& draft) {
                                            return draft.mutation.mode ==
                                                ArtworkMutationMode::keep;
                                        })) {
                                    artworkDrafts_.erase(current);
                                }
                            }
                        }
                    } else ++attention;
                }
                std::vector<nekorox::TagItemMutationPlan> acknowledged;
                for (const auto& item : frozen) {
                    const auto path = pathById.find(item.itemId);
                    if (path != pathById.end() &&
                        committedPaths.contains(path->second))
                        acknowledged.push_back(item);
                }
                tagDocument_.discardAppliedPlan(acknowledged);
                if (committed != 0) {
                    LibraryModel::instance().invalidate();
                    auto& bus = UiInvalidationBus::instance();
                    const auto dirty = UiDirty::semantic | UiDirty::artwork |
                        UiDirty::analysis;
                    bus.publish(UiInvalidationSurface::nativePanels, dirty);
                    bus.publish(UiInvalidationSurface::workspace, dirty);
                    bus.publish(UiInvalidationSurface::waveSeek, dirty);
                    bus.publish(UiInvalidationSurface::listen, dirty);
                }
                if (window_) {
                    setStatus(L"In-place Apply · committed " +
                        std::to_wstring(committed) + L" · needs attention " +
                        std::to_wstring(attention) + L" · verified backups retained" +
                        (refreshFailures ? L" · metadata refresh failed: " +
                            std::to_wstring(refreshFailures) : L"") +
                        (identityFailures ? L" · favorite/note association failed: " +
                            std::to_wstring(identityFailures) : L""));
                    refreshArtworkStates();
                    refreshTrackMetadataForPaths(committedPaths);
                    updateDirtyUi();
                    EnableWindow(undo_, !trackLoading_ &&
                        (MediaFileOperationService::instance().hasInPlaceUndo() ||
                        TagEditService::instance().hasUndo()));
                }
            }, std::move(options));
        if (applying_) {
            EnableWindow(apply_, FALSE);
            setStatus(L"Applying tags/artwork in place through verified staging…");
        } else setStatus(L"In-place Apply could not start.");
        return;
    }
    std::vector<TagEditPlanItem> plan;
    plan.reserve(frozen.size());
    for (const auto& item : frozen) {
        const auto found = handles.find(item.itemId);
        if (found != handles.end())
            plan.push_back({found->second, item.itemId, item.mutations,
                item.draftRevision});
    }
    applying_ = TagEditService::instance().start(std::move(plan),
        window_, [this, frozen](TagApplySummary summary) {
            applying_ = false;
            std::size_t committed = 0, deferred = 0, failed = 0;
            for (const auto& item : summary.files) {
                if (item.outcome == TagFileOutcome::committed ||
                    item.outcome == TagFileOutcome::noChange) ++committed;
                else if (item.outcome == TagFileOutcome::deferredUntilReleased) ++deferred;
                else ++failed;
            }
            if (committed != 0) {
                LibraryModel::instance().invalidate();
                const auto dirty = UiDirty::semantic | UiDirty::artwork |
                    UiDirty::analysis;
                auto& bus = UiInvalidationBus::instance();
                bus.publish(UiInvalidationSurface::nativePanels, dirty);
                bus.publish(UiInvalidationSurface::workspace, dirty);
                bus.publish(UiInvalidationSurface::waveSeek, dirty);
                bus.publish(UiInvalidationSurface::listen, dirty);
            }
            if (window_) {
                std::vector<nekorox::TagItemMutationPlan> acknowledged;
                for (const auto& result : summary.items) {
                    if (result.outcome != TagFileOutcome::committed &&
                        result.outcome != TagFileOutcome::noChange) continue;
                    const auto found = std::find_if(frozen.begin(), frozen.end(),
                        [&](const auto& item) {
                            return item.itemId == result.itemId &&
                                item.draftRevision == result.draftRevision;
                        });
                    if (found != frozen.end()) acknowledged.push_back(*found);
                }
                tagDocument_.discardAppliedPlan(acknowledged);
                setStatus(L"Applied " + std::to_wstring(committed) + L" · deferred " +
                    std::to_wstring(deferred) + L" · needs attention " +
                    std::to_wstring(failed));
                std::set<std::string> refreshedItems;
                for (const auto& item : acknowledged) refreshedItems.insert(item.itemId);
                refreshTrackMetadata(refreshedItems);
                updateDirtyUi();
                EnableWindow(undo_, !trackLoading_ && TagEditService::instance().hasUndo());
            }
        });
    if (applying_) {
        EnableWindow(apply_, FALSE);
        setStatus(L"Applying serially with verified backups…");
    } else {
        setStatus(L"Apply could not start; review the selected operations.");
    }
}

void TagEditorWindow::undoLast()
{
    if (trackLoading_) return;
    invalidateOrganizationPreview();
    if (applying_) return;
    const auto mediaUndoSequence =
        MediaFileOperationService::instance().undoOperationSequence();
    const auto tagUndoSequence =
        TagEditService::instance().undoOperationSequence();
    if (mediaUndoSequence != 0 && mediaUndoSequence >= tagUndoSequence) {
        applying_ = MediaFileOperationService::instance().undoLastInPlace(
            [this](std::vector<MediaCopyResult> results) {
                applying_ = false;
                std::size_t restored = 0, attention = 0, refreshFailures = 0, deferred = 0, identityFailures = 0;
                std::set<std::wstring> restoredPaths;
                for (const auto& item : results) {
                    if (item.outcome == MediaCopyOutcome::moved) {
                        ++restored;
                        if (item.identityRefreshFailed) ++identityFailures;
                        if (item.metadataRefreshFailed || item.identityRefreshFailed) ++attention;
                        if (item.metadataRefreshFailed) {
                            ++refreshFailures;
                        } else restoredPaths.insert(foldedPath(item.destination));
                    }
                    else if (item.outcome == MediaCopyOutcome::skipped) ++deferred;
                    else ++attention;
                }
                setStatus(L"Restored " + std::to_wstring(restored) +
                    L" in-place files · needs attention " +
                    std::to_wstring(attention) +
                    (refreshFailures ? L" · metadata refresh failed: " +
                        std::to_wstring(refreshFailures) : L"") +
                    (identityFailures ? L" · favorite/note association failed: " +
                        std::to_wstring(identityFailures) : L"") +
                    (deferred ? L" · playback protected: " + std::to_wstring(deferred) +
                        L" · stop playback and retry Undo" : L""));
                refreshArtworkStates();
                refreshTrackMetadataForPaths(restoredPaths);
                EnableWindow(undo_, !trackLoading_ &&
                    (MediaFileOperationService::instance().hasInPlaceUndo() ||
                    TagEditService::instance().hasUndo()));
                if (restored != 0) {
                    LibraryModel::instance().invalidate();
                    auto& bus = UiInvalidationBus::instance();
                    const auto dirty = UiDirty::semantic | UiDirty::artwork |
                        UiDirty::analysis;
                    bus.publish(UiInvalidationSurface::nativePanels, dirty);
                    bus.publish(UiInvalidationSurface::workspace, dirty);
                    bus.publish(UiInvalidationSurface::waveSeek, dirty);
                    bus.publish(UiInvalidationSurface::listen, dirty);
                }
            });
        if (applying_) {
            EnableWindow(apply_, FALSE);
            EnableWindow(undo_, FALSE);
            setStatus(L"Revalidating and restoring in-place recovery backups…");
        }
        return;
    }
    applying_ = TagEditService::instance().undoLast(window_,
        [this](TagApplySummary summary) {
            applying_ = false;
            std::size_t restored = 0, attention = 0, protectedFiles = 0, journalWarnings = 0;
            std::set<std::wstring> restoredPaths;
            for (const auto& item : summary.files) {
                if (item.outcome == TagFileOutcome::restored) {
                    ++restored;
                    restoredPaths.insert(foldedPath(item.path));
                    if (item.detail.find(L"journal update failed") != std::wstring::npos) ++journalWarnings;
                }
                else if (item.outcome == TagFileOutcome::deferredUntilReleased) ++protectedFiles;
                else ++attention;
            }
            setStatus(L"Restored " + std::to_wstring(restored) +
                L" · needs attention " + std::to_wstring(attention) +
                (protectedFiles ? L" · playback protected: " + std::to_wstring(protectedFiles) +
                    L" · stop playback and retry Undo" : L"") +
                (journalWarnings ? L" · recovery journal warnings: " + std::to_wstring(journalWarnings) : L""));
            refreshTrackMetadataForPaths(restoredPaths);
            EnableWindow(undo_, !trackLoading_ && (TagEditService::instance().hasUndo() ||
                MediaFileOperationService::instance().hasInPlaceUndo()));
            if (restored != 0) {
                LibraryModel::instance().invalidate();
                auto& bus = UiInvalidationBus::instance();
                const auto dirty = UiDirty::semantic | UiDirty::artwork |
                    UiDirty::analysis;
                bus.publish(UiInvalidationSurface::nativePanels, dirty);
                bus.publish(UiInvalidationSurface::workspace, dirty);
                bus.publish(UiInvalidationSurface::waveSeek, dirty);
                bus.publish(UiInvalidationSurface::listen, dirty);
            }
        });
    if (applying_) {
        EnableWindow(apply_, FALSE);
        EnableWindow(undo_, FALSE);
        setStatus(L"Restoring verified backups…");
    } else {
        setStatus(L"No completed operation is available to undo.");
    }
}

void TagEditorWindow::setStatus(std::wstring text)
{
    if (!status_) return;
    const auto total = itemIds_.size();
    const auto selected = tagDocument_.selectedIds().size();
    if (total != 0) {
        const auto target = selected == 1
            ? L"Editing 1 of " + std::to_wstring(total)
            : L"Editing " + std::to_wstring(selected) + L" tracks";
        text = target + L" · " + std::move(text);
    }
    SetWindowTextW(status_, text.c_str());
}

void TagEditorWindow::setDirty(bool value)
{
    if (!value) dirty_ = false;
    if (value) {
        invalidateOrganizationPreview();
        if (executeCopy_) EnableWindow(executeCopy_, FALSE);
    }
    updateDirtyUi();
}

} // namespace nekorox::component
