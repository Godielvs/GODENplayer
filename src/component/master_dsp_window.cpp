#include "master_dsp_window.h"

#include "master_dsp_controller.h"
#include "dsp_meter_paint.h"
#include "display_environment.h"
#include "telemetry_span.h"
#include "frameless_window.h"
#include "rollout_flags.h"
#include "theme_service.h"
#include "ui_invalidation_bus.h"

#include "mastering/eq/eq_band.h"
#include "rt/biquad_design.h"
#include "util/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <commdlg.h>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windowsx.h>

namespace nekorox::component {
namespace {

constexpr wchar_t kClassName[] = L"GODENplayer.MasterDSP.V1";
constexpr UINT kRefreshMessage = WM_APP + 0x4d1;
constexpr int kJsonEditId = 4101;
constexpr int kApplyId = 4102;
constexpr int kAddSearchId = 4103;
constexpr int kAddListId = 4104;
constexpr int kParameterValueId = 4106;
constexpr int kParameterSetId = 4107;
constexpr UINT_PTR kMeterTimer = 1;

std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(std::max(count, 0)), L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), output.data(), count);
    return output;
}

std::string narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(std::max(count, 0)), '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), output.data(), count, nullptr, nullptr);
    return output;
}

COLORREF color(float r, float g, float b)
{
    return RGB(static_cast<BYTE>(std::clamp(r, 0.0f, 1.0f) * 255.0f),
        static_cast<BYTE>(std::clamp(g, 0.0f, 1.0f) * 255.0f),
        static_cast<BYTE>(std::clamp(b, 0.0f, 1.0f) * 255.0f));
}

std::wstring oneDecimal(float value)
{
    if (!std::isfinite(value)) return L"--";
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f", value);
    return text;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains(const std::string& value, const char* needle)
{
    return value.find(needle) != std::string::npos;
}

goden_audio::ProcessorVisualizationProviderV1 visualizationProviderFor(
    const goden_audio::MasterDspModuleV1& module)
{
    using Class = goden_audio::ProcessorVisualizationClass;
    const auto type = lowerAscii(module.type);
    if (contains(type, "eq") || contains(type, "autoeq") ||
            module.type.rfind("multiband.", 0) == 0 ||
            contains(type, "dynamics") || contains(type, "limiter") ||
            contains(type, "maximizer") || contains(type, "delay") ||
            module.type == "utility.bitDepth" ||
            module.type == "saturation.bitcrusher")
        return {Class::analytical, "ANALYTICAL"};
    if (module.type == "utility.dither" ||
            module.type == "final.bitDepth" || contains(type, "reverb") ||
            contains(type, "modulation") || contains(type, "spectral") ||
            contains(type, "stereo") || contains(type, "crossfeed") ||
            contains(type, "monobass") || contains(type, "saturation") ||
            contains(type, "clip") || contains(type, "waveshaper") ||
            module.type == "effects.acoustic.roomMorph")
        return {Class::illustrative, "ILLUSTRATIVE"};
    return {Class::none, "NO VISUAL MODEL"};
}

std::wstring humanizeParameter(const std::string& key)
{
    std::string leaf = key;
    std::string prefix;
    const auto dot = leaf.rfind('.');
    if (dot != std::string::npos) {
        const auto group = leaf.substr(0, dot);
        leaf = leaf.substr(dot + 1);
        if (group.rfind("band", 0) == 0) {
            const auto number = group.substr(4);
            if (!number.empty() && std::all_of(number.begin(), number.end(), ::isdigit))
                prefix = "Band " + std::to_string(std::stoi(number) + 1) + " / ";
        }
    }
    std::string output = prefix;
    for (std::size_t index = 0; index < leaf.size(); ++index) {
        const auto ch = leaf[index];
        if (index != 0 && std::isupper(static_cast<unsigned char>(ch)) &&
                !output.empty() && output.back() != ' ')
            output.push_back(' ');
        output.push_back(ch);
    }
    const std::array suffixes{" Hz", " Db", " Ms", " Us"};
    for (const auto* suffix : suffixes) {
        if (output.size() >= std::strlen(suffix) &&
                output.compare(output.size() - std::strlen(suffix),
                    std::strlen(suffix), suffix) == 0) {
            output.resize(output.size() - std::strlen(suffix));
            break;
        }
    }
    if (!output.empty()) output[0] = static_cast<char>(
        std::toupper(static_cast<unsigned char>(output[0])));
    return widen(output);
}

struct ParameterUiSpec {
    std::string key;
    std::wstring label;
    std::wstring unit;
    float minimum = 0.0f;
    float maximum = 1.0f;
    double hardMinimum = 0.0;
    double hardMaximum = 1.0;
    float defaultValue = 0.0f;
    float step = 0.01f;
    bool logarithmic = false;
    bool integral = false;
    bool toggle = false;
    std::vector<std::wstring> choices;
    std::vector<float> choiceValues;
};

ParameterUiSpec parameterSpec(const std::string& key,
    const std::string& moduleType = {})
{
    if (!moduleType.empty()) {
        if (const auto descriptor =
                MasterDspController::instance().parameterDescriptor(moduleType, key)) {
            ParameterUiSpec exact;
            exact.key = key;
            exact.label = widen(descriptor->displayName);
            exact.unit = widen(descriptor->unit);
            exact.minimum = static_cast<float>(descriptor->minimum);
            exact.maximum = static_cast<float>(descriptor->maximum);
            exact.hardMinimum = descriptor->hardMinimum;
            exact.hardMaximum = descriptor->hardMaximum;
            exact.defaultValue = static_cast<float>(descriptor->defaultValue);
            exact.step = static_cast<float>(descriptor->step);
            exact.logarithmic = descriptor->scale ==
                goden_audio::MasterDspParameterScale::logarithmic;
            exact.integral = descriptor->kind ==
                    goden_audio::MasterDspParameterKind::integer ||
                descriptor->kind == goden_audio::MasterDspParameterKind::enumeration ||
                descriptor->kind == goden_audio::MasterDspParameterKind::boolean;
            exact.toggle = descriptor->kind ==
                goden_audio::MasterDspParameterKind::boolean;
            for (const auto& label : descriptor->enumLabels)
                exact.choices.push_back(widen(label));
            for (const auto value : descriptor->enumValues)
                exact.choiceValues.push_back(static_cast<float>(value));
            return exact;
        }
    }
    // Unknown parameters are deliberately not assigned a guessed slider.
    // parameterKeys() filters them from the editor; this inert value only
    // protects legacy call sites while future modules remain preserved and
    // bypassed until they receive a qualified descriptor.
    ParameterUiSpec unavailable;
    unavailable.key = key;
    unavailable.label = humanizeParameter(key);
    unavailable.minimum = 0.0f;
    unavailable.maximum = 0.0f;
    unavailable.hardMinimum = 0.0;
    unavailable.hardMaximum = 0.0;
    unavailable.step = 0.0f;
    return unavailable;

}

float normalizedValue(const ParameterUiSpec& spec, float value)
{
    value = std::clamp(value, spec.minimum, spec.maximum);
    if (spec.logarithmic && spec.minimum > 0.0f)
        return std::log(value / spec.minimum) /
            std::log(spec.maximum / spec.minimum);
    return (value - spec.minimum) /
        std::max(spec.maximum - spec.minimum, 1.0e-9f);
}

float valueFromNormalized(const ParameterUiSpec& spec, float normalized)
{
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    auto value = spec.logarithmic && spec.minimum > 0.0f
        ? spec.minimum * std::pow(spec.maximum / spec.minimum, normalized)
        : spec.minimum + (spec.maximum - spec.minimum) * normalized;
    if (spec.integral) value = std::round(value);
    return value;
}

std::wstring formatParameterValue(const ParameterUiSpec& spec, float value,
    bool explicitValue)
{
    wchar_t buffer[64]{};
    if (spec.toggle) {
        swprintf_s(buffer, L"%s%s", value >= 0.5f ? L"ON" : L"OFF",
            explicitValue ? L"" : L"  default");
        return buffer;
    }
    if (!spec.choices.empty()) {
        auto index = std::clamp(static_cast<int>(std::lround(value)), 0,
            static_cast<int>(spec.choices.size()) - 1);
        if (spec.choiceValues.size() == spec.choices.size()) {
            float bestDistance = std::numeric_limits<float>::max();
            for (std::size_t candidate = 0; candidate < spec.choiceValues.size();
                    ++candidate) {
                const auto distance = std::abs(spec.choiceValues[candidate] - value);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    index = static_cast<int>(candidate);
                }
            }
        }
        return spec.choices[static_cast<std::size_t>(index)];
    }
    if (spec.integral) swprintf_s(buffer, L"%.0f", value);
    else if (std::fabs(value) >= 1000.0f) swprintf_s(buffer, L"%.0f", value);
    else if (std::fabs(value) >= 100.0f) swprintf_s(buffer, L"%.1f", value);
    else swprintf_s(buffer, L"%.2f", value);
    std::wstring result(buffer);
    if (!spec.unit.empty()) result += L" " + spec.unit;
    if (!explicitValue) result += L"  default";
    return result;
}

float amplitudeDb(float value) noexcept
{
    return value > 1.0e-6f ? 20.0f * std::log10(value) : -120.0f;
}

thread_local float gEqPreviewSampleRate = 48000.0f;

float eqMagnitudeDb(int type, float frequency, float center,
    float gainDb, float q) noexcept
{
    const auto sampleRate = std::clamp(gEqPreviewSampleRate,
        8000.0f, 384000.0f);
    constexpr float twoPi = 6.28318530717958647692f;
    frequency = std::clamp(frequency, 1.0f, sampleRate * 0.5f - 1.0f);
    center = std::clamp(center, 1.0f, sampleRate * 0.5f - 1.0f);
    q = std::max(q, 1.0e-4f);
    const auto omega = twoPi * frequency / sampleRate;
    const auto centerOmega = twoPi * center / sampleRate;
    try {
        sonare::rt::BiquadCoeffs coefficients;
        switch (type) {
        case 1: coefficients = sonare::rt::rbj_low_shelf(centerOmega, q, gainDb); break;
        case 2: coefficients = sonare::rt::rbj_high_shelf(centerOmega, q, gainDb); break;
        case 3: coefficients = sonare::rt::rbj_lowpass(centerOmega, q); break;
        case 4: coefficients = sonare::rt::rbj_highpass(centerOmega, q); break;
        case 5: coefficients = sonare::rt::rbj_bandpass(centerOmega, q); break;
        case 6: coefficients = sonare::rt::rbj_notch(centerOmega, q); break;
        case 7:
        case 8: {
            const auto low = sonare::rt::biquad_magnitude(
                sonare::rt::rbj_low_shelf(centerOmega, q, -gainDb * 0.5f), omega);
            const auto high = sonare::rt::biquad_magnitude(
                sonare::rt::rbj_high_shelf(centerOmega, q, gainDb * 0.5f), omega);
            return 20.0f * std::log10(std::max(low * high, 1.0e-8f));
        }
        default: coefficients = sonare::rt::rbj_peak(centerOmega, q, gainDb); break;
        }
        const auto magnitude = sonare::rt::biquad_magnitude(coefficients, omega);
        return 20.0f * std::log10(std::max(magnitude, 1.0e-8f));
    } catch (...) {
        return 0.0f;
    }
}

std::wstring openPath(HWND owner, const wchar_t* filter, bool save)
{
    wchar_t path[MAX_PATH * 4]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    dialog.lpstrDefExt = save ? L"goden-dsp-preset" : nullptr;
    const auto accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    return accepted ? std::wstring(path) : std::wstring{};
}

void fill(HDC dc, const RECT& rectangle, COLORREF value)
{
    const auto brush = CreateSolidBrush(value);
    FillRect(dc, &rectangle, brush);
    DeleteObject(brush);
}

void frame(HDC dc, const RECT& rectangle, COLORREF value)
{
    const auto brush = CreateSolidBrush(value);
    FrameRect(dc, &rectangle, brush);
    DeleteObject(brush);
}

void text(HDC dc, const std::wstring& value, RECT rectangle,
    COLORREF valueColor, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, valueColor);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()),
        &rectangle, format | DT_END_ELLIPSIS);
}

} // namespace

struct MasterDspWindow::Impl {
    struct CardHit {
        RECT body{};
        RECT drag{};
        RECT power{};
        RECT up{};
        RECT down{};
        RECT remove{};
        std::string uuid;
    };

    struct ParameterHit {
        RECT body{};
        RECT track{};
        RECT value{};
        ParameterUiSpec spec;
        float currentValue = 0.0f;
        bool explicitValue = false;
        int band = -1;
    };

    struct EqNodeHit {
        RECT body{};
        int band = 0;
        std::string frequencyKey;
        std::string gainKey;
        bool gainOnly = false;
    };

    MasterDspWindow* owner = nullptr;
    goden_audio::MasterDspStateV1 paintState;
    bool paintStateValid = false;
    bool meterDemand = false;
    std::uint64_t paintedThemeGeneration = 0;
    HWND window = nullptr;
    HWND parent = nullptr;
    HWND jsonEdit = nullptr;
    HWND applyButton = nullptr;
    HWND addSearch = nullptr;
    HWND addList = nullptr;
    HWND parameterValue = nullptr;
    HWND parameterSet = nullptr;
    HFONT font = nullptr;
    HFONT heading = nullptr;
    HBRUSH editBrush = nullptr;
    HBRUSH buttonBrush = nullptr;
    WNDPROC parameterEditOriginal = nullptr;
    std::string selected;
    std::vector<CardHit> cardHits;
    std::vector<ParameterHit> parameterHits;
    std::vector<EqNodeHit> eqNodeHits;
    std::vector<goden_audio::MasterDspCatalogEntryV1> addCatalog;
    bool addBrowserVisible = false;
    bool showDetails = false;
    int parameterScroll = 0;
    int parameterScrollMaximum = 0;
    int rackScroll = 0;
    int rackScrollMaximum = 0;
    bool draggingRackScroll = false;
    int rackScrollDragAnchor = 0;
    int rackScrollDragOrigin = 0;
    bool draggingParameterScroll = false;
    int parameterScrollDragAnchor = 0;
    int parameterScrollDragOrigin = 0;
    int selectedBand = 0;
    int selectedNestedBand = 0;
    bool selectedSideChannel = false;
    bool sortEqBandsByFrequency = false;
    std::string editingParameter;
    ParameterUiSpec editingParameterSpec;
    std::string draggedParameter;
    float draggedParameterValue = 0.0f;
    std::string draggedEqFrequencyKey;
    std::string draggedEqGainKey;
    std::string autoEqHeadroomJson;
    float autoEqPeakBoostCache = 0.0f;
    bool draggedEqGainOnly = false;
    float hudDbHalfRange = 18.0f;
    float draggedEqHalfRange = 18.0f;
    std::string hudCurveCacheKey;
    RECT hudCurveCacheRect{};
    std::vector<POINT> hudCurveCache;
    std::vector<POINT> hudReferenceCache;
    std::string draggedUuid;
    int dragAnchorY = 0;
    int dragCurrentY = 0;
    RECT powerRect{}, bypassRect{}, presetRect{}, abRect{}, copyAToBRect{},
        copyBToARect{}, undoRect{}, redoRect{};
    RECT addRect{}, importApoRect{}, importIrRect{}, importPresetRect{}, exportPresetRect{};
    RECT detailsRect{}, parameterViewport{}, hudFooterRect{};
    RECT rackViewport{}, rackScrollRail{}, rackScrollThumb{};
    RECT parameterScrollRail{}, parameterScrollThumb{};
    RECT bandPreviousRect{}, bandNextRect{}, bandAddRect{}, bandDeleteRect{};
    RECT bandChannelRect{}, bandNestedRect{}, bandSortRect{}, hudGraphRect{};

    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = window;
        }
        return self ? self->handle(message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK parameterEditProc(
        HWND edit, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Impl*>(GetPropW(edit, L"Goden.MasterDsp.Owner"));
        if (self && message == WM_KEYDOWN) {
            if (wParam == VK_RETURN) {
                self->finishDirectEdit();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                self->editingParameter.clear();
                self->editingParameterSpec = {};
                ShowWindow(self->parameterValue, SW_HIDE);
                ShowWindow(self->parameterSet, SW_HIDE);
                SetFocus(self->window);
                return 0;
            }
        }
        return self && self->parameterEditOriginal
            ? CallWindowProcW(self->parameterEditOriginal, edit, message, wParam, lParam)
            : DefWindowProcW(edit, message, wParam, lParam);
    }

    void registerClass()
    {
        WNDCLASSEXW descriptor{sizeof(descriptor)};
        descriptor.style = CS_DBLCLKS;
        descriptor.lpfnWndProc = &windowProc;
        descriptor.hInstance = core_api::get_my_instance();
        descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        descriptor.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        descriptor.hbrBackground = nullptr;
        descriptor.lpszClassName = kClassName;
        RegisterClassExW(&descriptor);
    }

    void show(HWND ownerWindow)
    {
        if (!rolloutFlagEnabled(RolloutFlag::masterDspUiV1)) return;
        if (window && IsWindow(window)) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
        parent = ownerWindow != nullptr
            ? GetAncestor(ownerWindow, GA_ROOT)
            : static_cast<HWND>(core_api::get_main_window());
        if (parent == nullptr) parent = ownerWindow;
        registerClass();
        const auto dpi = parent ? GetDpiForWindow(parent) : 96U;
        RECT bounds{0, 0, MulDiv(1000, dpi, 96), MulDiv(720, dpi, 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW,
            FALSE, WS_EX_APPWINDOW, dpi);
        const auto width = bounds.right - bounds.left;
        const auto height = bounds.bottom - bounds.top;
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const auto monitor = MonitorFromWindow(
            parent, MONITOR_DEFAULTTONEAREST);
        RECT work{};
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        else
            work = monitorInfo.rcWork;
        const auto x = work.left + (work.right - work.left - width) / 2;
        const auto y = work.top + (work.bottom - work.top - height) / 2;
        // WS_EX_APPWINDOW surfaces must remain independent from the playback
        // window. `parent` is only the monitor/DPI anchor.
        window = CreateWindowExW(WS_EX_APPWINDOW, kClassName,
            L"GODENplayer Master DSP", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, nullptr, nullptr,
            core_api::get_my_instance(), this);
        if (!window) return;
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }

    void recreateFonts()
    {
        const auto dpi = GetDpiForWindow(window);
        const auto size = -MulDiv(12, dpi, 72);
        if (font) DeleteObject(font);
        if (heading) DeleteObject(heading);
        font = CreateFontW(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        heading = CreateFontW(size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (const auto child : {jsonEdit, applyButton, addSearch, addList,
                parameterValue, parameterSet})
            if (child) SendMessageW(child, WM_SETFONT,
                reinterpret_cast<WPARAM>(font), TRUE);
    }

    void createChildren()
    {
        recreateFonts();
        jsonEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"{}",
            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN, 0, 0, 100, 100, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kJsonEditId)),
            core_api::get_my_instance(), nullptr);
        applyButton = CreateWindowExW(0, L"BUTTON", L"Apply JSON",
            WS_CHILD | BS_PUSHBUTTON, 0, 0, 140, 30, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyId)),
            core_api::get_my_instance(), nullptr);
        addSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL, 0, 0, 100, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddSearchId)),
            core_api::get_my_instance(), nullptr);
        SendMessageW(addSearch, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"Search processors..."));
        addList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 100, 100, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddListId)),
            core_api::get_my_instance(), nullptr);
        parameterValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL, 0, 0, 100, 26, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kParameterValueId)),
            core_api::get_my_instance(), nullptr);
        parameterSet = CreateWindowExW(0, L"BUTTON", L"Set",
            WS_CHILD | BS_PUSHBUTTON, 0, 0, 56, 26, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kParameterSetId)),
            core_api::get_my_instance(), nullptr);
        SetPropW(parameterValue, L"Goden.MasterDsp.Owner", this);
        parameterEditOriginal = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            parameterValue, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&parameterEditProc)));
        editBrush = CreateSolidBrush(RGB(12, 18, 19));
        buttonBrush = CreateSolidBrush(RGB(20, 29, 30));
        SendMessageW(jsonEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(applyButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(addSearch, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(addList, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(parameterValue, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(parameterSet, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        UiInvalidationBus::instance().registerWindow(
            UiInvalidationSurface::masterDsp, window, kRefreshMessage);
        addCatalog = MasterDspController::instance().catalog();
        refreshSelection();
    }

    void layoutChildren()
    {
        RECT client{}; GetClientRect(window, &client);
        const int width = static_cast<int>(client.right);
        const int height = static_cast<int>(client.bottom);
        const auto wide = width >= 840;
        const int editorLeft = wide ? 342 : 18;
        const int editorWidth = std::max(100, width - editorLeft - 18);
        const int editorTop = wide ? 128 : height / 2 + 50;
        const int advancedTop = editorTop + 194;
        MoveWindow(jsonEdit, editorLeft, advancedTop,
            editorWidth, std::max(80, height - advancedTop - 54), TRUE);
        MoveWindow(applyButton, editorLeft,
            std::max(advancedTop + 40, height - 44), 116, 30, TRUE);
        ShowWindow(jsonEdit, SW_HIDE);
        ShowWindow(applyButton, SW_HIDE);
        if (editingParameter.empty()) {
            ShowWindow(parameterValue, SW_HIDE);
            ShowWindow(parameterSet, SW_HIDE);
        }
        const int rackRight = wide ? 330 : width - 18;
        const int rackBottom = wide ? height - 14 : height / 2 - 8;
        MoveWindow(addSearch, 24, 124, std::max(80, rackRight - 48), 27, TRUE);
        MoveWindow(addList, 24, 155, std::max(80, rackRight - 48),
            std::max(80, rackBottom - 179), TRUE);
    }

    void refreshSelection()
    {
        const auto state = MasterDspController::instance().snapshot();
        const auto& rack = state.selectedB ? state.rackB : state.rackA;
        auto found = std::find_if(rack.modules.begin(), rack.modules.end(),
            [&](const auto& module) { return module.uuid == selected; });
        if (found == rack.modules.end() && !rack.modules.empty()) {
            found = rack.modules.begin();
            selected = found->uuid;
        }
        if (found != rack.modules.end() && jsonEdit && GetFocus() != jsonEdit) {
            SetWindowTextW(jsonEdit, widen(found->parametersJson).c_str());
        } else if (found == rack.modules.end() && jsonEdit && GetFocus() != jsonEdit) {
            SetWindowTextW(jsonEdit, L"{}");
        }
        parameterScroll = std::clamp(parameterScroll, 0, parameterScrollMaximum);
    }

    void resetModuleViewState()
    {
        selectedBand = 0;
        selectedNestedBand = 0;
        selectedSideChannel = false;
        showDetails = false;
        parameterScroll = 0;
        editingParameter.clear();
        editingParameterSpec = {};
        ShowWindow(parameterValue, SW_HIDE);
        ShowWindow(parameterSet, SW_HIDE);
        eqNodeHits.clear();
        parameterHits.clear();
        bandPreviousRect = {}; bandNextRect = {}; bandAddRect = {};
        bandDeleteRect = {}; bandChannelRect = {}; bandNestedRect = {};
        bandSortRect = {};
        hudGraphRect = {};
    }

    bool hasBandEditor(const std::string& type) const
    {
        return type == "autoeq.equalizerApo" || type == "eq.parametric" ||
            type == "eq.equalizer" || type == "eq.minimumPhase" ||
            type == "eq.linearPhase" || type == "eq.dynamic" ||
            type == "eq.midSide" || type.rfind("multiband.", 0) == 0 ||
            type == "saturation.multibandExciter";
    }

    bool hasEqBandTable(const std::string& type) const
    {
        return type == "autoeq.equalizerApo" || type == "eq.parametric" ||
            type == "eq.equalizer" || type == "eq.minimumPhase" ||
            type == "eq.linearPhase" || type == "eq.dynamic" ||
            type == "eq.midSide";
    }

    int bandCountForType(const std::string& type) const
    {
        if (type == "autoeq.equalizerApo") return 128;
        if (type.rfind("multiband.", 0) == 0 ||
                type == "saturation.multibandExciter") return 3;
        if (type == "eq.dynamic") return 8;
        return hasBandEditor(type) ? 24 : 0;
    }

    int bandCountForModule(const goden_audio::MasterDspModuleV1& module) const
    {
        if (module.type == "saturation.multibandExciter") return 3;
        if (module.type.rfind("multiband.", 0) != 0)
            return bandCountForType(module.type);
        int cutoffCount = 0;
        try {
            const auto document = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson);
            for (int index = 0; index < 8; ++index)
                if (document.find("cutoff" + std::to_string(index) + "Hz"))
                    ++cutoffCount;
        } catch (...) {
        }
        return cutoffCount == 0 ? 3 : std::clamp(cutoffCount + 1, 2, 9);
    }

    std::string selectedBandPrefix(const std::string& type) const
    {
        if (type == "eq.midSide") return std::string(
            selectedSideChannel ? "sideBand" : "midBand") +
                std::to_string(selectedBand) + ".";
        if (type == "multiband.dynamicEq")
            return "band" + std::to_string(selectedBand) + ".dyn" +
                std::to_string(selectedNestedBand) + ".";
        return "band" + std::to_string(selectedBand) + ".";
    }

    bool isDetailParameter(const std::string& key) const
    {
        const auto name = lowerAscii(key);
        return contains(name, "coeffmode") || contains(name, "proportional") ||
            contains(name, "sidechain") || contains(name, "soloed") ||
            contains(name, "lookahead") || contains(name, "placement") ||
            contains(name, "phase") || contains(name, "fft") ||
            contains(name, "kernel") || contains(name, "partition") ||
            contains(name, "resolution") || contains(name, "seed") ||
            contains(name, "fir") || contains(name, "applygain") ||
            contains(name, "crest") || contains(name, "smoothing") ||
            contains(name, "ismorder") || contains(name, "maxseconds") ||
            contains(name, "listener") || contains(name, "source") ||
            contains(name, "componentmodel") || contains(name, "ditherseed");
    }

    std::vector<std::string> parameterKeys(
        const goden_audio::MasterDspModuleV1& module,
        const goden_audio::MasterDspCatalogEntryV1* catalog) const
    {
        if (module.type == "eq.graphic") return {};
        std::vector<std::string> result;
        std::set<std::string> seen;
        auto add = [&](std::string key) {
            if (key.empty() || !seen.insert(key).second) return;
            if (!MasterDspController::instance().parameterDescriptor(
                    module.type, key)) return;
            result.push_back(std::move(key));
        };
        const auto bandEditor = hasBandEditor(module.type);
        const auto prefix = selectedBandPrefix(module.type);
        auto belongsToAnotherBand = [&](const std::string& key) {
            if (!bandEditor) return false;
            const auto lower = lowerAscii(key);
            const auto grouped = lower.rfind("band", 0) == 0 ||
                lower.rfind("midband", 0) == 0 || lower.rfind("sideband", 0) == 0;
            return grouped && key.rfind(prefix, 0) != 0;
        };
        auto allowed = [&](const std::string& key) {
            if (belongsToAnotherBand(key)) return false;
            if (!showDetails && isDetailParameter(key)) return false;
            if (module.type == "saturation.multibandExciter" &&
                    key.rfind("cutoff", 0) == 0)
                return key == "cutoff0Hz" || key == "cutoff1Hz";
            if (bandEditor && key.rfind("cutoff", 0) == 0) return true;
            return true;
        };
        if (catalog) {
            for (const auto& key : catalog->parameterNames) {
                const auto marker = key.find("bandN.");
                const auto resolved = marker == std::string::npos ? key
                    : key.substr(0, marker) + "band" +
                        std::to_string(selectedBand) + "." + key.substr(marker + 6);
                if (allowed(resolved)) add(resolved);
            }
        }
        if (module.type == "eq.dynamic") {
            for (const auto* suffix : {"frequencyHz", "staticGainDb", "q",
                    "thresholdDb", "ratio", "rangeDb", "attackMs", "releaseMs",
                    "lookaheadMs", "sidechainFreqHz", "sidechainQ", "enabled"})
                if (showDetails || std::strcmp(suffix, "frequencyHz") == 0)
                    add(prefix + suffix);
        }
        auto addBandFields = [&](std::initializer_list<const char*> suffixes) {
            for (const auto* suffix : suffixes) add(prefix + suffix);
        };
        if (module.type == "multiband.compressor")
            addBandFields({"thresholdDb", "ratio", "attackMs", "releaseMs",
                "makeupGainDb", "kneeDb"});
        else if (module.type == "multiband.expander")
            addBandFields({"thresholdDb", "ratio", "rangeDb", "attackMs", "releaseMs"});
        else if (module.type == "multiband.limiter")
            addBandFields({"thresholdDb", "releaseMs", "lookaheadMs"});
        else if (module.type == "multiband.imager")
            addBandFields({"enabled", "width", "decorrelationAmount", "preserveEnergy"});
        else if (module.type == "multiband.saturation")
            addBandFields({"driveDb", "mix", "outputGainDb"});
        else if (module.type == "saturation.multibandExciter")
            addBandFields({"amount", "driveDb", "evenOddMix", "frequencyHz", "q"});
        else if (module.type == "multiband.dynamicEq")
            addBandFields({"frequencyHz", "staticGainDb", "q", "thresholdDb",
                "ratio", "rangeDb", "attackMs", "releaseMs", "lookaheadMs",
                "sidechainFreqHz", "sidechainQ", "enabled"});
        try {
            const auto document = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson);
            for (const auto& [key, value] : document.as_object()) {
                if ((value.is_number() || value.is_bool()) && allowed(key)) add(key);
            }
        } catch (...) {
        }
        if (result.empty() && catalog && !showDetails) {
            for (const auto& key : catalog->parameterNames) {
                if (!belongsToAnotherBand(key)) add(key);
            }
        }
        if (bandEditor)
            std::stable_partition(result.begin(), result.end(), [&](const auto& key) {
                return key.rfind(prefix, 0) == 0;
            });
        return result;
    }

    float readParameterValue(const goden_audio::MasterDspModuleV1& module,
        const ParameterUiSpec& spec, bool& explicitValue) const
    {
        explicitValue = false;
        try {
            const auto document = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson);
            const auto* value = document.find(spec.key);
            if (value && value->is_number()) {
                explicitValue = true;
                return value->as_float();
            }
            if (value && value->is_bool()) {
                explicitValue = true;
                return value->as_bool() ? 1.0f : 0.0f;
            }
        } catch (...) {
        }
        return spec.defaultValue;
    }

    bool setParameterValue(const std::string& key, double value,
        bool allowHardRange = false)
    {
        if (selected.empty()) return false;
        const auto state = MasterDspController::instance().snapshot();
        const auto& rack = state.selectedB ? state.rackB : state.rackA;
        const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
            [&](const auto& item) { return item.uuid == selected; });
        if (module == rack.modules.end()) return false;
        const auto spec = parameterSpec(key, module->type);
        value = allowHardRange
            ? std::clamp(value, spec.hardMinimum, spec.hardMaximum)
            : std::clamp(value, static_cast<double>(spec.minimum),
                static_cast<double>(spec.maximum));
        if (spec.integral) value = std::round(value);
        try {
            auto document = sonare::util::json::parse_strict(
                module->parametersJson.empty() ? "{}" : module->parametersJson);
            auto object = document.as_object();
            object[key] = spec.toggle
                ? sonare::util::json::Value(value >= 0.5f)
                : sonare::util::json::Value(value);
            const auto updated = sonare::util::json::dump(
                sonare::util::json::Value(std::move(object)));
            const auto accepted = MasterDspController::instance().setModuleParameters(
                selected, updated);
            if (accepted && GetFocus() != jsonEdit)
                SetWindowTextW(jsonEdit, widen(updated).c_str());
            return accepted;
        } catch (...) {
            return false;
        }
    }

    bool commitSelectedObject(sonare::util::json::Object object)
    {
        if (selected.empty()) return false;
        const auto updated = sonare::util::json::dump(
            sonare::util::json::Value(std::move(object)));
        const auto accepted = MasterDspController::instance().setModuleParameters(
            selected, updated);
        if (accepted) SetWindowTextW(jsonEdit, widen(updated).c_str());
        return accepted;
    }

    void updateDraggedEqPoint(POINT point)
    {
        if ((draggedEqGainKey.empty() && draggedEqFrequencyKey.empty()) ||
                hudGraphRect.right <= hudGraphRect.left ||
                hudGraphRect.bottom <= hudGraphRect.top) return;
        const auto state = MasterDspController::instance().snapshot();
        const auto& rack = state.selectedB ? state.rackB : state.rackA;
        const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
            [&](const auto& item) { return item.uuid == selected; });
        if (module == rack.modules.end()) return;
        try {
            auto document = sonare::util::json::parse_strict(
                module->parametersJson.empty() ? "{}" : module->parametersJson);
            auto object = document.as_object();
            const auto x = std::clamp((point.x - hudGraphRect.left) /
                static_cast<float>(std::max(1L,
                    hudGraphRect.right - hudGraphRect.left)), 0.0f, 1.0f);
            const auto y = std::clamp((point.y - hudGraphRect.top) /
                static_cast<float>(std::max(1L,
                    hudGraphRect.bottom - hudGraphRect.top)), 0.0f, 1.0f);
            if (!draggedEqGainOnly && !draggedEqFrequencyKey.empty())
                object[draggedEqFrequencyKey] = 20.0f * std::pow(1000.0f, x);
            if (!draggedEqGainKey.empty()) {
                const auto halfRange = std::max(6.0f, draggedEqHalfRange);
                object[draggedEqGainKey] = std::clamp(
                    (0.5f - y) * halfRange * 2.0f,
                    -halfRange, halfRange);
            }
            commitSelectedObject(std::move(object));
            InvalidateRect(window, nullptr, FALSE);
        } catch (...) {
        }
    }

    bool bandIsActive(const goden_audio::MasterDspModuleV1& module,
        int band) const
    {
        if (module.type.rfind("multiband.", 0) == 0 ||
                module.type == "saturation.multibandExciter") return band < 3;
        const auto prefix = module.type == "eq.midSide"
            ? std::string(selectedSideChannel ? "sideBand" : "midBand") +
                std::to_string(band) + "."
            : std::string("band") + std::to_string(band) + ".";
        try {
            const auto document = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson);
            for (const auto& [key, value] : document.as_object()) {
                (void)value;
                if (key.rfind(prefix, 0) == 0) return true;
            }
        } catch (...) {
        }
        return false;
    }

    void addEqBand(const POINT* graphPoint = nullptr)
    {
        const auto state = MasterDspController::instance().snapshot();
        const auto& rack = state.selectedB ? state.rackB : state.rackA;
        const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
            [&](const auto& item) { return item.uuid == selected; });
        if (module == rack.modules.end() || !hasBandEditor(module->type) ||
                module->type.rfind("multiband.", 0) == 0 ||
                module->type == "saturation.multibandExciter") return;
        const auto count = bandCountForType(module->type);
        int target = selectedBand;
        if (bandIsActive(*module, target)) {
            target = -1;
            for (int index = 0; index < count; ++index) {
                if (!bandIsActive(*module, index)) { target = index; break; }
            }
        }
        if (target < 0) return;
        try {
            auto document = sonare::util::json::parse_strict(
                module->parametersJson.empty() ? "{}" : module->parametersJson);
            auto object = document.as_object();
            selectedBand = target;
            const auto prefix = selectedBandPrefix(module->type);
            const auto position = graphPoint && hudGraphRect.right > hudGraphRect.left
                ? std::clamp((graphPoint->x - hudGraphRect.left) /
                    static_cast<float>(hudGraphRect.right - hudGraphRect.left), 0.0f, 1.0f)
                : (count <= 1 ? 0.5f : (target + 1.0f) / (count + 1.0f));
            const auto frequency = 20.0f * std::pow(1000.0f, position);
            object[prefix + "frequencyHz"] = frequency;
            if (module->type != "eq.dynamic") {
                const auto y = graphPoint && hudGraphRect.bottom > hudGraphRect.top
                    ? std::clamp((graphPoint->y - hudGraphRect.top) /
                        static_cast<float>(hudGraphRect.bottom - hudGraphRect.top), 0.0f, 1.0f)
                    : 0.5f;
                const auto halfRange = std::max(6.0f, hudDbHalfRange);
                object[prefix + "gainDb"] = std::clamp(
                    (0.5f - y) * halfRange * 2.0f,
                    -halfRange, halfRange);
            }
            if (module->type != "eq.dynamic") {
                object[prefix + "q"] = 0.707f;
                object[prefix + "type"] = 0;
                object[prefix + "enabled"] = true;
            }
            auto& controller = MasterDspController::instance();
            controller.beginParameterGesture();
            commitSelectedObject(std::move(object));
            controller.endParameterGesture();
            parameterScroll = 0;
        } catch (...) {
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void deleteSelectedBand()
    {
        const auto state = MasterDspController::instance().snapshot();
        const auto& rack = state.selectedB ? state.rackB : state.rackA;
        const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
            [&](const auto& item) { return item.uuid == selected; });
        if (module == rack.modules.end() || !hasBandEditor(module->type) ||
                module->type.rfind("multiband.", 0) == 0 ||
                module->type == "saturation.multibandExciter") return;
        try {
            auto document = sonare::util::json::parse_strict(
                module->parametersJson.empty() ? "{}" : module->parametersJson);
            auto object = document.as_object();
            const auto prefix = selectedBandPrefix(module->type);
            for (auto item = object.begin(); item != object.end();) {
                if (item->first.rfind(prefix, 0) == 0) item = object.erase(item);
                else ++item;
            }
            auto& controller = MasterDspController::instance();
            controller.beginParameterGesture();
            commitSelectedObject(std::move(object));
            controller.endParameterGesture();
            parameterScroll = 0;
        } catch (...) {
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void beginDirectEdit(const ParameterHit& hit)
    {
        editingParameter = hit.spec.key;
        editingParameterSpec = hit.spec;
        auto editRect = hit.value;
        editRect.right -= 52;
        MoveWindow(parameterValue, editRect.left, editRect.top,
            std::max(54L, editRect.right - editRect.left),
            editRect.bottom - editRect.top, TRUE);
        MoveWindow(parameterSet, editRect.right + 4, editRect.top,
            48, editRect.bottom - editRect.top, TRUE);
        wchar_t value[48]{};
        swprintf_s(value, hit.spec.integral ? L"%.0f" : L"%.4g",
            hit.currentValue);
        SetWindowTextW(parameterValue, value);
        ShowWindow(parameterValue, SW_SHOW);
        ShowWindow(parameterSet, SW_SHOW);
        SetFocus(parameterValue);
        SendMessageW(parameterValue, EM_SETSEL, 0, -1);
    }

    void finishDirectEdit()
    {
        if (editingParameter.empty()) return;
        wchar_t buffer[96]{};
        GetWindowTextW(parameterValue, buffer, static_cast<int>(std::size(buffer)));
        wchar_t* end = nullptr;
        auto value = wcstod(buffer, &end);
        if (end != buffer && (*end == L'k' || *end == L'K')) {
            value *= 1000.0;
            ++end;
        } else if (end != buffer && *end == L'M' && end[1] != L's' &&
                end[1] != L'S') {
            value *= 1000000.0;
            ++end;
        }
        while (end && std::iswspace(*end)) ++end;
        if (end && *end && !editingParameterSpec.unit.empty()) {
            const auto unit = editingParameterSpec.unit;
            if (_wcsnicmp(end, unit.c_str(), unit.size()) == 0)
                end += unit.size();
            while (std::iswspace(*end)) ++end;
        }
        if (end == buffer || !std::isfinite(value) || (end && *end != L'\0')) {
            showError("Enter a finite numeric value.");
            return;
        }
        if (editingParameterSpec.integral) {
            const auto rounded = std::round(value);
            if (std::abs(value - rounded) > 1.0e-7) {
                showError("This parameter requires a whole number.");
                return;
            }
            value = rounded;
        }
        if (value < editingParameterSpec.hardMinimum ||
                value > editingParameterSpec.hardMaximum) {
            std::ostringstream message;
            message << std::setprecision(9) << "Safe manual range: "
                << editingParameterSpec.hardMinimum << " to "
                << editingParameterSpec.hardMaximum;
            if (!editingParameterSpec.unit.empty())
                message << ' ' << narrow(editingParameterSpec.unit);
            message << ". The slider keeps its narrower precision range.";
            showError(message.str());
            return;
        }
        auto& controller = MasterDspController::instance();
        controller.beginParameterGesture();
        const auto accepted = setParameterValue(editingParameter, value, true);
        controller.endParameterGesture();
        if (!accepted) {
            showError("This processor rejected the value.");
            return;
        }
        editingParameter.clear();
        editingParameterSpec = {};
        ShowWindow(parameterValue, SW_HIDE);
        ShowWindow(parameterSet, SW_HIDE);
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void updateDraggedParameter(POINT point)
    {
        if (draggedParameter.empty()) return;
        const auto hit = std::find_if(parameterHits.begin(), parameterHits.end(),
            [&](const auto& item) { return item.spec.key == draggedParameter; });
        if (hit == parameterHits.end()) return;
        const auto width = std::max(1L, hit->track.right - hit->track.left);
        const auto normalized = (point.x - hit->track.left) /
            static_cast<float>(width);
        const auto value = valueFromNormalized(hit->spec, normalized);
        if (std::fabs(value - draggedParameterValue) <
                std::max(hit->spec.step * 0.25f, 1.0e-6f)) return;
        draggedParameterValue = value;
        setParameterValue(draggedParameter, value);
        InvalidateRect(window, &parameterViewport, FALSE);
        if (hudGraphRect.right > hudGraphRect.left &&
                hudGraphRect.bottom > hudGraphRect.top)
            InvalidateRect(window, &hudGraphRect, FALSE);
    }

    void filterAddBrowser()
    {
        const auto length = GetWindowTextLengthW(addSearch);
        std::wstring query(static_cast<std::size_t>(length) + 1U, L'\0');
        const auto copied = GetWindowTextW(addSearch, query.data(), length + 1);
        query.resize(static_cast<std::size_t>(std::max(copied, 0)));
        std::transform(query.begin(), query.end(), query.begin(), towlower);
        SendMessageW(addList, LB_RESETCONTENT, 0, 0);
        for (std::size_t index = 0; index < addCatalog.size(); ++index) {
            auto haystack = widen(addCatalog[index].category + " " +
                addCatalog[index].displayName + " " + addCatalog[index].type);
            std::transform(haystack.begin(), haystack.end(), haystack.begin(), towlower);
            if (!query.empty() && haystack.find(query) == std::wstring::npos) continue;
            const auto label = widen(addCatalog[index].category + "  /  " +
                addCatalog[index].displayName);
            const auto row = SendMessageW(addList, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            if (row != LB_ERR && row != LB_ERRSPACE) {
                SendMessageW(addList, LB_SETITEMDATA, row,
                    static_cast<LPARAM>(index));
            }
        }
        if (SendMessageW(addList, LB_GETCOUNT, 0, 0) > 0)
            SendMessageW(addList, LB_SETCURSEL, 0, 0);
    }

    void showAddBrowser()
    {
        if (addBrowserVisible) {
            addBrowserVisible = false;
            ShowWindow(addSearch, SW_HIDE);
            ShowWindow(addList, SW_HIDE);
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        addBrowserVisible = true;
        SetWindowTextW(addSearch, L"");
        filterAddBrowser();
        ShowWindow(addSearch, SW_SHOW);
        ShowWindow(addList, SW_SHOW);
        SetFocus(addSearch);
        InvalidateRect(window, nullptr, FALSE);
    }

    void commitAddBrowser()
    {
        const auto row = SendMessageW(addList, LB_GETCURSEL, 0, 0);
        if (row == LB_ERR) return;
        const auto index = static_cast<std::size_t>(
            SendMessageW(addList, LB_GETITEMDATA, row, 0));
        if (index < addCatalog.size()) {
            const auto& entry = addCatalog[index];
            if (MasterDspController::instance().addModule(entry.type, entry.terminal)) {
                const auto state = MasterDspController::instance().snapshot();
                const auto& rack = state.selectedB ? state.rackB : state.rackA;
                const auto added = std::find_if(rack.modules.rbegin(),
                    rack.modules.rend(), [&](const auto& module) {
                        return module.type == entry.type;
                    });
                if (added != rack.modules.rend()) selected = added->uuid;
                resetModuleViewState();
                refreshSelection();
            }
        }
        addBrowserVisible = false;
        ShowWindow(addSearch, SW_HIDE);
        ShowWindow(addList, SW_HIDE);
        InvalidateRect(window, nullptr, FALSE);
    }

    void presetMenu()
    {
        const auto names = MasterDspController::instance().factoryPresetNames();
        HMENU menu = CreatePopupMenu();
        for (std::size_t index = 0; index < names.size(); ++index)
            AppendMenuW(menu, MF_STRING, 6000 + static_cast<UINT>(index),
                widen(names[index]).c_str());
        POINT point{presetRect.left, presetRect.bottom}; ClientToScreen(window, &point);
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD,
            point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command >= 6000 && static_cast<std::size_t>(command - 6000) < names.size()) {
            MasterDspController::instance().loadFactoryPreset(names[command - 6000]);
            selected.clear(); resetModuleViewState(); refreshSelection();
        }
    }

    bool inside(const RECT& rectangle, POINT point) const noexcept
    {
        return PtInRect(&rectangle, point) != FALSE;
    }

    void click(POINT point)
    {
        auto& controller = MasterDspController::instance();
        if (inside(powerRect, point)) controller.toggleEnabled();
        else if (inside(bypassRect, point)) controller.toggleBypass();
        else if (inside(presetRect, point)) presetMenu();
        else if (inside(abRect, point)) {
            const auto state = controller.snapshot();
            controller.selectRackB(!state.selectedB);
            resetModuleViewState(); refreshSelection();
        } else if (inside(copyAToBRect, point)) {
            controller.copyAToB();
            resetModuleViewState(); refreshSelection();
        } else if (inside(copyBToARect, point)) {
            controller.copyBToA();
            resetModuleViewState(); refreshSelection();
        } else if (inside(undoRect, point)) {
            controller.undo();
            resetModuleViewState(); refreshSelection();
        } else if (inside(redoRect, point)) {
            controller.redo();
            resetModuleViewState(); refreshSelection();
        } else if (inside(addRect, point)) showAddBrowser();
        else if (inside(detailsRect, point)) {
            showDetails = !showDetails;
            parameterScroll = 0;
        }
        else if (inside(bandPreviousRect, point)) {
            const auto state = controller.snapshot();
            const auto& rack = state.selectedB ? state.rackB : state.rackA;
            const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
                [&](const auto& item) { return item.uuid == selected; });
            if (module != rack.modules.end()) {
                const auto count = std::max(1, bandCountForModule(*module));
                selectedBand = (selectedBand + count - 1) % count;
                parameterScroll = 0;
            }
        }
        else if (inside(bandNextRect, point)) {
            const auto state = controller.snapshot();
            const auto& rack = state.selectedB ? state.rackB : state.rackA;
            const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
                [&](const auto& item) { return item.uuid == selected; });
            if (module != rack.modules.end()) {
                const auto count = std::max(1, bandCountForModule(*module));
                selectedBand = (selectedBand + 1) % count;
                parameterScroll = 0;
            }
        }
        else if (inside(bandAddRect, point)) addEqBand();
        else if (inside(bandDeleteRect, point)) deleteSelectedBand();
        else if (inside(bandChannelRect, point)) {
            selectedSideChannel = !selectedSideChannel;
            parameterScroll = 0;
        }
        else if (inside(bandNestedRect, point)) {
            selectedNestedBand = (selectedNestedBand + 1) % 8;
            parameterScroll = 0;
        }
        else if (inside(bandSortRect, point)) {
            sortEqBandsByFrequency = !sortEqBandsByFrequency;
            parameterScroll = 0;
        }
        else if (inside(importApoRect, point)) {
            const auto path = openPath(window,
                L"Equalizer APO text (*.txt)\0*.txt\0All files\0*.*\0", false);
            if (!path.empty()) {
                std::string error;
                if (!controller.importEqualizerApo(path, &error)) showError(error);
            }
        } else if (inside(importIrRect, point)) {
            const auto path = openPath(window,
                L"Impulse responses (*.wav;*.flac)\0*.wav;*.flac\0All files\0*.*\0", false);
            if (!path.empty()) {
                std::string error;
                if (!controller.importImpulseResponse(path, &error)) showError(error);
            }
        } else if (inside(importPresetRect, point)) {
            const auto path = openPath(window,
                L"GODEN DSP preset (*.goden-dsp-preset)\0*.goden-dsp-preset\0", false);
            if (!path.empty()) {
                std::string error;
                if (!controller.importPreset(path, &error)) showError(error);
            }
        } else if (inside(exportPresetRect, point)) {
            const auto path = openPath(window,
                L"GODEN DSP preset (*.goden-dsp-preset)\0*.goden-dsp-preset\0", true);
            if (!path.empty()) {
                std::string error;
                if (!controller.exportPreset(path, &error)) showError(error);
            }
        } else {
            for (const auto& card : cardHits) {
                if (inside(card.power, point)) {
                    const auto state = controller.snapshot();
                    const auto& rack = state.selectedB ? state.rackB : state.rackA;
                    const auto item = std::find_if(rack.modules.begin(), rack.modules.end(),
                        [&](const auto& value) { return value.uuid == card.uuid; });
                    if (item != rack.modules.end()) controller.setModuleBypassed(
                        card.uuid, !item->bypassed);
                    return;
                }
                if (inside(card.up, point)) { controller.moveModule(card.uuid, -1); return; }
                if (inside(card.down, point)) { controller.moveModule(card.uuid, 1); return; }
                if (inside(card.remove, point)) { controller.removeModule(card.uuid); return; }
                if (inside(card.body, point)) {
                    selected = card.uuid;
                    resetModuleViewState();
                    refreshSelection();
                    layoutChildren();
                    InvalidateRect(window, nullptr, FALSE); return;
                }
            }
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void applyJson()
    {
        if (selected.empty()) return;
        const auto length = GetWindowTextLengthW(jsonEdit);
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        const auto copied = GetWindowTextW(jsonEdit, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(std::max(copied, 0)));
        if (!MasterDspController::instance().setModuleParameters(
                selected, narrow(value))) {
            showError("Parameters must be a valid JSON object.");
        }
    }

    void showError(const std::string& error) const
    {
        MessageBoxW(window, widen(error).c_str(), L"GODENplayer Master DSP",
            MB_OK | MB_ICONERROR);
    }

    void drawLine(HDC dc, int x1, int y1, int x2, int y2,
        COLORREF lineColor, int width = 1) const
    {
        const auto pen = CreatePen(PS_SOLID, width, lineColor);
        const auto old = SelectObject(dc, pen);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void drawModuleHud(HDC dc, const RECT& bounds,
        const goden_audio::MasterDspModuleV1& module,
        const goden_audio::MasterDspCatalogEntryV1* catalog,
        const goden_audio::MasterDspStateV1& state,
        COLORREF accent, COLORREF secondary,
        COLORREF muted, COLORREF border)
    {
        gEqPreviewSampleRate = state.telemetry.sampleRate != 0
            ? static_cast<float>(state.telemetry.sampleRate) : 48000.0f;
        eqNodeHits.clear();
        bandPreviousRect = {}; bandNextRect = {}; bandAddRect = {};
        bandDeleteRect = {}; bandChannelRect = {}; bandNestedRect = {};
        bandSortRect = {}; hudGraphRect = {};
        fill(dc, bounds, RGB(7, 11, 12));
        frame(dc, bounds, border);
        const auto type = lowerAscii(module.type);
        const auto multiband = module.type.rfind("multiband.", 0) == 0 ||
            module.type == "saturation.multibandExciter";
        const auto quantizer = module.type == "utility.bitDepth" ||
            module.type == "utility.dither" ||
            module.type == "saturation.bitcrusher" ||
            module.type == "final.bitDepth";
        const auto visualization = visualizationProviderFor(module);
        std::wstring hudName = L"SIGNAL FLOW";
        if (quantizer) hudName = L"QUANTIZATION RESPONSE";
        else if (multiband) hudName = L"MULTIBAND CROSSOVER";
        else if (contains(type, "eq") || contains(type, "autoeq"))
            hudName = L"FREQUENCY RESPONSE";
        else if (contains(type, "spectral")) hudName = L"SPECTRAL SHAPING";
        else if (contains(type, "dynamics") || contains(type, "limiter") ||
                contains(type, "maximizer")) hudName = L"TRANSFER CURVE";
        else if (contains(type, "stereo") || contains(type, "crossfeed") ||
                contains(type, "monobass")) hudName = L"STEREO FIELD";
        else if (contains(type, "reverb") || contains(type, "delay") ||
                contains(type, "modulation")) hudName = L"TIME RESPONSE";
        else if (contains(type, "saturation") || contains(type, "clip") ||
                contains(type, "waveshaper")) hudName = L"HARMONIC CURVE";

        sonare::util::json::Value document;
        try {
            document = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson);
        } catch (...) {
            document = sonare::util::json::Value(sonare::util::json::Object{});
        }
        auto number = [&](const char* key, float fallback) {
            const auto* value = document.find(key);
            if (!value) return fallback;
            if (value->is_number()) return value->as_float();
            if (value->is_bool()) return value->as_bool() ? 1.0f : 0.0f;
            return fallback;
        };

        RECT title{bounds.left + 12, bounds.top + 5,
            bounds.right - 12, bounds.top + 28};
        text(dc, hudName, title, secondary);
        if (catalog) {
            RECT category{bounds.left + 12, bounds.top + 5,
                bounds.right - 12, bounds.top + 28};
            text(dc, widen(catalog->category + " · " + visualization.label),
                category, muted,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        const auto bandEditor = hasBandEditor(module.type);
        const auto graphicEq = module.type == "eq.graphic";
        // Labels live in dedicated gutters; nodes and drag math use only graph.
        RECT graph{bounds.left + 50, bounds.top + 31,
            bounds.right - 18,
            bounds.bottom - ((bandEditor || graphicEq) ? 48 : 35)};
        hudGraphRect = graph;
        const auto frequencyGrid = hudName == L"FREQUENCY RESPONSE" ||
            hudName == L"SPECTRAL SHAPING" ||
            hudName == L"MULTIBAND CROSSOVER";
        if (frequencyGrid) {
            if (draggedEqGainKey.empty() && draggedEqFrequencyKey.empty()) {
                auto magnitude = std::abs(number("preampDb", 0.0f));
                if (document.is_object()) {
                    for (const auto& [key, value] : document.as_object()) {
                        if (!value.is_number()) continue;
                        if (key.find("GainDb") != std::string::npos ||
                                key == "gainDb" || key == "tiltDb" ||
                                key == "rangeDb")
                            magnitude = std::max(magnitude,
                                std::abs(value.as_float()));
                    }
                }
                constexpr std::array<float, 7> domains{
                    6.0f, 12.0f, 18.0f, 24.0f, 36.0f, 48.0f, 72.0f};
                hudDbHalfRange = domains.back();
                for (const auto candidate : domains) {
                    if (magnitude <= candidate * 0.82f) {
                        hudDbHalfRange = candidate;
                        break;
                    }
                }
            }
            for (int tick = -3; tick <= 3; ++tick) {
                const auto db = hudDbHalfRange * static_cast<float>(tick) / 3.0f;
                const auto y = graph.top + static_cast<LONG>(
                    (hudDbHalfRange - db) / (hudDbHalfRange * 2.0f) *
                    (graph.bottom - graph.top));
                drawLine(dc, graph.left, y, graph.right, y,
                    tick == 0 ? RGB(39, 55, 56) : RGB(20, 30, 31));
                wchar_t tickText[16]{};
                std::swprintf(tickText, std::size(tickText),
                    db > 0.0f ? L"+%.0f" : L"%.0f", db);
                RECT label{bounds.left + 5, y - 9, graph.left - 5, y + 9};
                text(dc, tickText, label, muted,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            for (const auto frequency : {20, 50, 100, 200, 500, 1000, 2000, 5000,
                    10000, 20000}) {
                const auto x = graph.left + static_cast<LONG>(std::log(
                    static_cast<float>(frequency) / 20.0f) / std::log(1000.0f) *
                    (graph.right - graph.left));
                drawLine(dc, x, graph.top, x, graph.bottom,
                    frequency == 1000 ? RGB(35, 49, 50) : RGB(17, 26, 27));
                if (frequency == 20 || frequency == 100 || frequency == 1000 ||
                        frequency == 10000 || frequency == 20000) {
                    const auto labelText = frequency >= 1000
                        ? std::to_wstring(frequency / 1000) + L"k"
                        : std::to_wstring(frequency);
                    RECT label{x - 18, graph.bottom + 2, x + 19,
                        graph.bottom + 20};
                    text(dc, labelText, label, muted,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        } else if (hudName == L"TIME RESPONSE") {
            const auto center = (graph.top + graph.bottom) / 2;
            drawLine(dc, graph.left, center, graph.right, center, RGB(39, 55, 56));
            for (int second = 0; second <= 15; second += 3) {
                const auto x = graph.left + static_cast<LONG>(second / 15.0f *
                    (graph.right - graph.left));
                drawLine(dc, x, graph.top, x, graph.bottom,
                    second == 0 ? RGB(35, 49, 50) : RGB(17, 26, 27));
                RECT label{x - 20, graph.bottom - 18, x + 21, graph.bottom};
                text(dc, std::to_wstring(second) + L"s", label, muted,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            for (const auto fraction : {-0.5f, 0.5f}) {
                const auto y = center - static_cast<LONG>(fraction *
                    (graph.bottom - graph.top) * 0.5f);
                drawLine(dc, graph.left, y, graph.right, y, RGB(20, 30, 31));
            }
        } else if (hudName == L"QUANTIZATION RESPONSE") {
            const auto center = (graph.top + graph.bottom) / 2;
            for (int index = 0; index <= 8; ++index) {
                const auto x = graph.left + static_cast<LONG>(index / 8.0f *
                    (graph.right - graph.left));
                drawLine(dc, x, graph.top, x, graph.bottom, RGB(17, 26, 27));
            }
            for (const auto amplitude : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
                const auto y = center - static_cast<LONG>(amplitude *
                    (graph.bottom - graph.top) * 0.45f);
                drawLine(dc, graph.left, y, graph.right, y,
                    amplitude == 0.0f ? RGB(39, 55, 56) : RGB(20, 30, 31));
            }
            RECT low{graph.left + 3, graph.bottom - 18, graph.left + 48, graph.bottom};
            RECT high{graph.right - 88, graph.bottom - 18, graph.right - 3, graph.bottom};
            text(dc, L"REFERENCE", low, muted);
            text(dc, L"QUANTIZED", high, secondary,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            const auto centerX = (graph.left + graph.right) / 2;
            const auto centerY = (graph.top + graph.bottom) / 2;
            drawLine(dc, graph.left, centerY, graph.right, centerY, RGB(39, 55, 56));
            drawLine(dc, centerX, graph.top, centerX, graph.bottom, RGB(24, 35, 36));
            for (const auto fraction : {0.25f, 0.75f}) {
                const auto x = graph.left + static_cast<LONG>(fraction *
                    (graph.right - graph.left));
                drawLine(dc, x, graph.top, x, graph.bottom, RGB(17, 26, 27));
            }
        }

        if (multiband) {
            const auto count = bandCountForModule(module);
            std::vector<int> boundaries;
            boundaries.push_back(graph.left);
            for (int index = 0; index < count - 1; ++index) {
                const auto fallback = 200.0f * std::pow(4.0f,
                    static_cast<float>(index));
                const auto cutoff = number(("cutoff" + std::to_string(index) + "Hz").c_str(),
                    fallback);
                const auto x = graph.left + static_cast<LONG>(std::clamp(
                    std::log(std::max(cutoff, 20.0f) / 20.0f) / std::log(1000.0f),
                    0.0f, 1.0f) * (graph.right - graph.left));
                boundaries.push_back(x);
                drawLine(dc, x, graph.top, x, graph.bottom, secondary, 2);
                RECT label{x - 34, graph.top + 4, x + 35, graph.top + 23};
                text(dc, formatParameterValue(parameterSpec("frequencyHz", module.type),
                    cutoff, true), label, secondary,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            boundaries.push_back(graph.right);
            const auto band = std::clamp(selectedBand, 0, count - 1);
            RECT selectedStrip{boundaries[static_cast<std::size_t>(band)],
                graph.bottom - 5, boundaries[static_cast<std::size_t>(band + 1)],
                graph.bottom};
            fill(dc, selectedStrip, accent);
            for (int index = 0; index < count; ++index) {
                RECT label{boundaries[static_cast<std::size_t>(index)],
                    graph.bottom - 28, boundaries[static_cast<std::size_t>(index + 1)],
                    graph.bottom - 7};
                text(dc, L"B" + std::to_wstring(index + 1), label,
                    index == band ? accent : muted,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        struct EqPreviewBand {
            int type = 0;
            float frequency = 1000.0f;
            float gain = 0.0f;
            float q = 0.707f;
        };
        std::vector<EqPreviewBand> previewBands;
        if (hudName == L"FREQUENCY RESPONSE" && !graphicEq) {
            const auto root = module.type == "eq.midSide"
                ? std::string(selectedSideChannel ? "sideBand" : "midBand")
                : std::string("band");
            const auto count = std::max(24, bandCountForType(module.type));
            for (int band = 0; band < count; ++band) {
                const auto prefix = root + std::to_string(band) + ".";
                auto active = false;
                for (const auto& [key, value] : document.as_object()) {
                    (void)value;
                    if (key.rfind(prefix, 0) == 0) { active = true; break; }
                }
                if (!active || number((prefix + "enabled").c_str(), 1.0f) < 0.5f ||
                        number((prefix + "bypassed").c_str(), 0.0f) >= 0.5f) continue;
                EqPreviewBand preview;
                preview.type = static_cast<int>(std::lround(
                    number((prefix + "type").c_str(), 0.0f)));
                preview.frequency = number((prefix + "frequencyHz").c_str(), 1000.0f);
                preview.gain = number((prefix +
                    (module.type == "eq.dynamic" ? "staticGainDb" : "gainDb")).c_str(),
                    0.0f);
                preview.q = std::max(0.1f, number((prefix + "q").c_str(), 0.707f));
                previewBands.push_back(preview);
            }
        }

        std::vector<POINT> points;
        std::vector<POINT> referencePoints;
        const auto graphWidth = std::max(1L, graph.right - graph.left);
        points.reserve(static_cast<std::size_t>(graphWidth));
        referencePoints.reserve(static_cast<std::size_t>(graphWidth));
        const auto curveCacheKey = module.uuid + '\n' + module.type + '\n' +
            module.parametersJson + '\n' + narrow(hudName) + '\n' +
            std::to_string(selectedBand) + ':' +
            std::to_string(selectedNestedBand) + ':' +
            std::to_string(selectedSideChannel) + ':' +
            std::to_string(sortEqBandsByFrequency) + ':' +
            std::to_string(hudDbHalfRange) + ':' +
            std::to_string(gEqPreviewSampleRate);
        const auto cacheHit = curveCacheKey == hudCurveCacheKey &&
            EqualRect(&graph, &hudCurveCacheRect) != FALSE &&
            (!hudCurveCache.empty() || !hudReferenceCache.empty());
        if (cacheHit) {
            points = hudCurveCache;
            referencePoints = hudReferenceCache;
        } else if (hudName == L"FREQUENCY RESPONSE") {
            for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                const auto position = static_cast<float>(pixel) /
                    static_cast<float>(std::max(1L, graphWidth - 1));
                const auto frequency = 20.0f * std::pow(1000.0f, position);
                float response = number("preampDb", 0.0f);
                if (graphicEq) {
                    const auto bandPosition = position * 30.0f;
                    const auto left = std::clamp(static_cast<int>(bandPosition), 0, 30);
                    const auto right = std::min(30, left + 1);
                    const auto fraction = bandPosition - left;
                    const auto leftKey = "band" + std::to_string(left) + "GainDb";
                    const auto rightKey = "band" + std::to_string(right) + "GainDb";
                    response += number(leftKey.c_str(), 0.0f) * (1.0f - fraction) +
                        number(rightKey.c_str(), 0.0f) * fraction;
                } else {
                    for (const auto& band : previewBands)
                        response += eqMagnitudeDb(band.type, frequency,
                            band.frequency, band.gain, band.q);
                    if (previewBands.empty()) {
                        if (module.type == "eq.tilt") {
                            const auto pivot = number("pivotHz", 1000.0f);
                            response += std::clamp(std::log2(frequency /
                                std::max(pivot, 20.0f)), -3.0f, 3.0f) *
                                number("tiltDb", 0.0f) / 3.0f;
                        } else if (module.type == "eq.shelving") {
                            if (number("lowEnabled", 1.0f) >= 0.5f)
                                response += eqMagnitudeDb(1, frequency,
                                    number("lowFrequencyHz", 120.0f),
                                    number("lowGainDb", 0.0f), number("lowQ", 0.707f));
                            if (number("highEnabled", 1.0f) >= 0.5f)
                                response += eqMagnitudeDb(2, frequency,
                                    number("highFrequencyHz", 8000.0f),
                                    number("highGainDb", 0.0f), number("highQ", 0.707f));
                        } else if (module.type == "eq.cutFilter") {
                            if (number("highPassEnabled", 0.0f) >= 0.5f)
                                response += eqMagnitudeDb(4, frequency,
                                    number("highPassFrequencyHz", 80.0f), 0.0f,
                                    number("highPassQ", 0.707f));
                            if (number("lowPassEnabled", 0.0f) >= 0.5f)
                                response += eqMagnitudeDb(3, frequency,
                                    number("lowPassFrequencyHz", 16000.0f), 0.0f,
                                    number("lowPassQ", 0.707f));
                        } else if (module.type == "eq.bandPass") {
                            if (number("bandPassEnabled", 0.0f) >= 0.5f)
                                response += eqMagnitudeDb(5, frequency,
                                    number("bandPassFrequencyHz", 1000.0f), 0.0f,
                                    number("bandPassQ", 0.707f));
                            if (number("notchEnabled", 0.0f) >= 0.5f)
                                response += eqMagnitudeDb(6, frequency,
                                    number("notchFrequencyHz", 1000.0f), 0.0f,
                                    number("notchQ", 1.0f));
                        } else if (module.type == "eq.apiStyle") {
                            response += eqMagnitudeDb(1, frequency,
                                number("lowFrequencyHz", 100.0f),
                                number("lowGainDb", 0.0f), 0.707f);
                            response += eqMagnitudeDb(0, frequency,
                                number("lowMidFrequencyHz", 400.0f),
                                number("lowMidGainDb", 0.0f), 1.0f);
                            response += eqMagnitudeDb(0, frequency,
                                number("highMidFrequencyHz", 3000.0f),
                                number("highMidGainDb", 0.0f), 1.0f);
                            response += eqMagnitudeDb(2, frequency,
                                number("highFrequencyHz", 10000.0f),
                                number("highGainDb", 0.0f), 0.707f);
                        }
                    }
                }
                const auto y = graph.top + static_cast<LONG>(
                    std::clamp(0.5f - response /
                        (hudDbHalfRange * 2.0f), 0.0f, 1.0f) *
                    (graph.bottom - graph.top));
                points.push_back({graph.left + pixel, y});
            }
        } else if (hudName == L"SPECTRAL SHAPING") {
            for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                const auto position = static_cast<float>(pixel) /
                    static_cast<float>(std::max(1L, graphWidth - 1));
                const auto frequency = 20.0f * std::pow(1000.0f, position);
                float response = 0.0f;
                if (module.type == "spectral.airBand")
                    response = eqMagnitudeDb(2, frequency,
                        number("shelfFrequencyHz", 8000.0f),
                        number("amount", 0.0f) * 12.0f, 0.707f);
                else if (module.type == "spectral.lowEndFocus")
                    response = eqMagnitudeDb(1, frequency,
                        number("cutoffHz", 120.0f),
                        number("subharmonicAmount", 0.0f) * 9.0f, 0.707f);
                else if (module.type == "spectral.presenceEnhancer")
                    response = eqMagnitudeDb(0, frequency,
                        number("centerFrequencyHz", 3000.0f),
                        number("amount", 0.0f) * 9.0f,
                        number("q", 1.0f));
                else {
                    const auto low = number("frequencyHz", 500.0f);
                    const auto high = number("highFrequencyHz", 8000.0f);
                    const auto center = std::sqrt(std::max(low * high, 1.0f));
                    response = eqMagnitudeDb(0, frequency, center,
                        number("amount", 0.0f) * number("rangeDb", 6.0f), 0.7f);
                }
                const auto y = graph.top + static_cast<LONG>(
                    std::clamp(0.5f - response /
                        (hudDbHalfRange * 2.0f), 0.0f, 1.0f) *
                    (graph.bottom - graph.top));
                points.push_back({graph.left + pixel, y});
            }
        } else if (hudName == L"TRANSFER CURVE") {
            auto threshold = number("thresholdDb", -18.0f);
            const auto limiter = contains(type, "limiter") ||
                contains(type, "maximizer");
            const auto ceilingBased = module.type ==
                    "dynamics.brickwallLimiter" ||
                module.type == "maximizer.truePeakLimiter" ||
                contains(type, "maximizer");
            if (ceilingBased) {
                if (const auto* ceilingDb = document.find("ceilingDb");
                        ceilingDb && ceilingDb->is_number())
                    threshold = ceilingDb->as_float();
                else if (const auto* ceiling = document.find("ceiling");
                        ceiling && ceiling->is_number())
                    threshold = 20.0f * std::log10(std::max(
                        ceiling->as_float(), 1.0e-6f));
                else threshold = -1.0f;
            }
            const auto ratio = limiter ? 1000.0f
                : std::max(1.0f, number("ratio", 2.0f));
            const auto makeup = number("makeupGainDb", 0.0f);
            const auto knee = std::max(0.0f, number("kneeDb", 0.0f));
            const auto range = std::max(0.0f, number("rangeDb", 60.0f));
            const auto expander = contains(type, "expander") ||
                contains(type, "gate");
            for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                const auto input = -60.0f + 60.0f * pixel /
                    static_cast<float>(std::max(1L, graphWidth - 1));
                auto output = input;
                if (expander && input < threshold)
                    output = std::max(input - (threshold - input) * (ratio - 1.0f),
                        input - range);
                else if (!expander && knee > 0.0f &&
                        input > threshold - knee * 0.5f &&
                        input < threshold + knee * 0.5f) {
                    const auto distance = input - threshold + knee * 0.5f;
                    output = input + (1.0f / ratio - 1.0f) *
                        distance * distance / (2.0f * knee);
                } else if (!expander && input > threshold)
                    output = threshold + (input - threshold) / ratio;
                output += makeup;
                const auto y = graph.bottom - static_cast<LONG>(
                    std::clamp((output + 60.0f) / 60.0f, 0.0f, 1.0f) *
                    (graph.bottom - graph.top));
                points.push_back({graph.left + pixel, y});
            }
        } else if (hudName == L"HARMONIC CURVE") {
            const auto linearDrive = std::max(0.001f, number("drive", 1.0f));
            const auto drive = std::pow(10.0f,
                number("driveDb", 20.0f * std::log10(linearDrive)) / 20.0f);
            const auto mix = std::clamp(number("mix", 1.0f), 0.0f, 1.0f);
            const auto bias = number("bias", number("biasV", 0.0f) * 0.15f);
            const auto output = std::pow(10.0f,
                number("outputGainDb", number("outputDb", 0.0f)) / 20.0f);
            const auto hardClip = contains(type, "hardclip");
            const auto ceiling = std::clamp(number("ceiling", 1.0f), 0.01f, 1.0f);
            for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                const auto input = -1.0f + 2.0f * pixel /
                    static_cast<float>(std::max(1L, graphWidth - 1));
                const auto biased = input * drive + bias;
                const auto wet = hardClip
                    ? std::clamp(biased, -ceiling, ceiling)
                    : std::tanh(biased) - std::tanh(bias);
                const auto shaped = (input * (1.0f - mix) + wet * mix) * output;
                const auto y = graph.top + static_cast<LONG>((1.0f -
                    std::clamp((shaped + 1.0f) * 0.5f, 0.0f, 1.0f)) *
                    (graph.bottom - graph.top));
                points.push_back({graph.left + pixel, y});
            }
        } else if (hudName == L"QUANTIZATION RESPONSE") {
            const auto bitKey = (module.type == "saturation.bitcrusher" ||
                module.type == "final.bitDepth") ? "bitDepth" : "bits";
            const auto defaultBits = module.type == "saturation.bitcrusher"
                ? 12.0f : 24.0f;
            const auto bits = std::clamp(static_cast<int>(std::lround(
                number(bitKey, defaultBits))), 1, 32);
            const auto heldSamples = module.type == "saturation.bitcrusher"
                ? std::clamp(static_cast<int>(std::lround(
                    number("downsampleFactor", 1.0f))), 1, 64)
                : 1;
            const auto mix = module.type == "saturation.bitcrusher"
                ? std::clamp(number("mix", 1.0f), 0.0f, 1.0f) : 1.0f;
            const auto levels = std::max(1.0,
                std::ldexp(1.0, std::min(bits - 1, 30)) - 1.0);
            const auto ditherType = module.type == "utility.dither"
                ? std::clamp(static_cast<int>(std::lround(
                    number("type", 2.0f))), 0, 3)
                : (module.type == "final.bitDepth"
                    ? std::clamp(static_cast<int>(std::lround(
                        number("ditherType", 2.0f))), 0, 3) : 0);
            std::uint32_t rng = 0x47504453U;
            double shapingError = 0.0;
            constexpr double twoPi = 6.28318530717958647692;
            for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                const auto position = pixel /
                    static_cast<double>(std::max(1L, graphWidth - 1));
                const auto input = std::sin(position * twoPi * 2.0) * 0.82;
                const auto heldPixel = pixel - pixel % heldSamples;
                const auto heldPosition = heldPixel /
                    static_cast<double>(std::max(1L, graphWidth - 1));
                const auto heldInput = std::sin(heldPosition * twoPi * 2.0) * 0.82;
                auto noise = 0.0;
                if (ditherType != 0) {
                    rng = rng * 1664525U + 1013904223U;
                    const auto a = static_cast<double>(rng >> 8U) / 16777216.0;
                    if (ditherType == 1) noise = a - 0.5;
                    else {
                        rng = rng * 1664525U + 1013904223U;
                        const auto b = static_cast<double>(rng >> 8U) / 16777216.0;
                        noise = a - b;
                    }
                }
                auto shaped = heldInput;
                if (ditherType == 3) shaped += shapingError * 0.85;
                const auto quantized = std::clamp(
                    std::round(shaped * levels + noise) / levels,
                    -1.0, 1.0 - 1.0 / levels);
                shapingError = ditherType == 3 ? shaped - quantized : 0.0;
                const auto output = input * (1.0 - mix) + quantized * mix;
                const auto referenceY = (graph.top + graph.bottom) / 2 -
                    static_cast<LONG>(input * (graph.bottom - graph.top) * 0.45);
                const auto outputY = (graph.top + graph.bottom) / 2 -
                    static_cast<LONG>(output * (graph.bottom - graph.top) * 0.45);
                referencePoints.push_back({graph.left + pixel, referenceY});
                points.push_back({graph.left + pixel, outputY});
            }
        } else if (hudName == L"TIME RESPONSE") {
            const auto isReverb = contains(type, "reverb") ||
                module.type == "effects.acoustic.roomMorph";
            const auto isDelay = contains(type, "delay");
            constexpr float previewSeconds = 15.0f;
            constexpr float t60Drop = 6.90775527898f;
            if (isReverb) {
                auto decaySeconds = 1.5f;
                if (const auto* explicitDecay = document.find("decaySec");
                        explicitDecay && explicitDecay->is_number()) {
                    decaySeconds = explicitDecay->as_float();
                } else if (module.type == "effects.reverb.plate" ||
                        module.type == "effects.reverb.dattorro") {
                    constexpr float tankLoopSeconds = 21589.0f / 29761.0f;
                    const auto decay = std::clamp(number("decay", 0.5f),
                        0.0f, 0.98f);
                    decaySeconds = decay > 0.0001f
                        ? t60Drop * tankLoopSeconds /
                            (-4.0f * std::log(decay))
                        : 0.05f;
                } else if (module.type == "effects.reverb.fdn") {
                    decaySeconds = number("decay", 0.55f) * 10.0f;
                } else if (module.type == "effects.reverb.velvet") {
                    decaySeconds = number("reverbTimeS", 1.5f) *
                        (0.5f + std::clamp(number("decay", 0.45f), 0.0f, 1.0f));
                } else if (module.type == "effects.reverb.room" ||
                        module.type == "effects.acoustic.roomMorph") {
                    decaySeconds = number("maxSeconds", 2.0f);
                }
                decaySeconds = std::clamp(decaySeconds, 0.05f, 60.0f);
                const auto preDelaySeconds = std::clamp(
                    number("preDelayMs", 0.0f) / 1000.0f, 0.0f, 2.0f);
                const auto wetDefault = module.type == "effects.reverb.velvet"
                    ? 0.3f : 0.35f;
                const auto wet = std::clamp(number("dryWet", wetDefault), 0.0f, 1.0f);
                const auto damping = std::clamp(number("damping",
                    number("hfDamping", 0.5f)), 0.0f, 1.0f);
                const auto density = std::clamp(number("densityHz", 2000.0f),
                    100.0f, 3000.0f);
                const auto oscillationHz = 7.0f + density / 180.0f - damping * 7.0f;
                for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                    const auto position = pixel /
                        static_cast<float>(std::max(1L, graphWidth - 1));
                    const auto time = position * previewSeconds;
                    auto wave = 0.0f;
                    if (time >= preDelaySeconds) {
                        const auto tailTime = time - preDelaySeconds;
                        const auto envelope = std::exp(-t60Drop * tailTime /
                            decaySeconds);
                        const auto diffuse = std::sin(tailTime * oscillationHz *
                            6.28318530718f) * 0.72f +
                            std::sin(tailTime * oscillationHz * 2.137f *
                                6.28318530718f) * 0.28f;
                        wave = diffuse * envelope * wet;
                    }
                    const auto y = (graph.top + graph.bottom) / 2 -
                        static_cast<LONG>(wave * (graph.bottom - graph.top) * 0.42f);
                    points.push_back({graph.left + pixel, y});
                }
            } else if (isDelay) {
                auto delayMs = (number("delayTimeLMs", 250.0f) +
                    number("delayTimeRMs", 375.0f)) * 0.5f;
                if (module.type == "utility.channelDelay")
                    delayMs = number("delayUs", 0.0f) / 1000.0f;
                delayMs = std::max(1.0f, delayMs);
                const auto delaySeconds = delayMs / 1000.0f;
                const auto feedback = std::clamp(number("feedback", 0.25f),
                    -0.95f, 0.95f);
                const auto wet = std::clamp(number("dryWet", 0.25f), 0.0f, 1.0f);
                for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                    const auto position = pixel /
                        static_cast<float>(std::max(1L, graphWidth - 1));
                    const auto time = position * previewSeconds;
                    const auto echoIndex = std::max(1,
                        static_cast<int>(std::lround(time / delaySeconds)));
                    const auto echoTime = echoIndex * delaySeconds;
                    const auto distance = std::abs(time - echoTime);
                    const auto pulse = std::exp(-distance * distance /
                        std::max(0.00002f, delaySeconds * delaySeconds * 0.0015f));
                    const auto wave = pulse * std::pow(std::abs(feedback),
                        static_cast<float>(echoIndex - 1)) * wet *
                        ((feedback < 0.0f && (echoIndex & 1) == 0) ? -1.0f : 1.0f);
                    const auto y = (graph.top + graph.bottom) / 2 -
                        static_cast<LONG>(wave * (graph.bottom - graph.top) * 0.42f);
                    points.push_back({graph.left + pixel, y});
                }
            } else {
                const auto rate = std::max(0.01f, number("rateHz",
                    number("rateSlowHz", number("carrierHz", 0.8f))));
                const auto depth = std::clamp(number("depthMs",
                    number("depthSlowMs", std::abs(number("semitones", 6.0f)))),
                    0.0f, 50.0f);
                const auto wet = std::clamp(number("dryWet", 0.5f), 0.0f, 1.0f);
                const auto amplitude = std::clamp(depth / 12.0f, 0.08f, 1.0f) * wet;
                for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                    const auto position = pixel /
                        static_cast<float>(std::max(1L, graphWidth - 1));
                    const auto time = position * previewSeconds;
                    const auto wave = std::sin(time * rate * 6.28318530718f) *
                        amplitude;
                    const auto y = (graph.top + graph.bottom) / 2 -
                        static_cast<LONG>(wave * (graph.bottom - graph.top) * 0.42f);
                    points.push_back({graph.left + pixel, y});
                }
            }
        } else if (hudName == L"MULTIBAND CROSSOVER") {
            // The crossover map and selected-band strip are retained above.
        } else if (hudName == L"STEREO FIELD") {
            const auto centerX = (graph.left + graph.right) / 2;
            const auto centerY = (graph.top + graph.bottom) / 2;
            drawLine(dc, centerX, graph.top, centerX, graph.bottom, muted);
            drawLine(dc, graph.left, centerY, graph.right, centerY, muted);
            const auto width = std::clamp(number("width", 1.0f), 0.0f, 2.0f);
            const auto balance = std::clamp(number("balance", 0.0f), -1.0f, 1.0f);
            const auto pen = CreatePen(PS_SOLID, 2, accent);
            const auto old = SelectObject(dc, pen);
            const auto radiusX = static_cast<int>((graph.right - graph.left) *
                (0.12f + 0.32f * width));
            const auto radiusY = static_cast<int>((graph.bottom - graph.top) * 0.38f);
            const auto oldFill = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Ellipse(dc, centerX - radiusX + static_cast<int>(balance * 24),
                centerY - radiusY,
                centerX + radiusX + static_cast<int>(balance * 24),
                centerY + radiusY);
            SelectObject(dc, oldFill);
            SelectObject(dc, old); DeleteObject(pen);
        } else if (visualization.classification !=
                goden_audio::ProcessorVisualizationClass::none) {
            const auto gain = number("gainDb", 0.0f);
            for (LONG pixel = 0; pixel < graphWidth; ++pixel) {
                const auto phase = pixel / static_cast<float>(std::max(1L, graphWidth - 1));
                const auto wave = std::sin(phase * 25.1327f) *
                    std::pow(10.0f, gain / 20.0f) * 0.35f;
                const auto y = (graph.top + graph.bottom) / 2 -
                    static_cast<LONG>(wave * (graph.bottom - graph.top));
                points.push_back({graph.left + pixel, y});
            }
        } else {
            RECT notice{graph.left + 8, graph.top + 8,
                graph.right - 8, graph.bottom - 8};
            text(dc, L"This processor has no truthful visual response model.",
                notice, muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (!cacheHit) {
            hudCurveCacheKey = curveCacheKey;
            hudCurveCacheRect = graph;
            hudCurveCache = points;
            hudReferenceCache = referencePoints;
        }
        if (referencePoints.size() > 1) {
            const auto referencePen = CreatePen(PS_SOLID, 1, muted);
            const auto oldReference = SelectObject(dc, referencePen);
            Polyline(dc, referencePoints.data(),
                static_cast<int>(referencePoints.size()));
            SelectObject(dc, oldReference);
            DeleteObject(referencePen);
        }
        if (points.size() > 1) {
            const auto shadowPen = CreatePen(PS_SOLID, 4, RGB(25, 42, 43));
            const auto old = SelectObject(dc, shadowPen);
            Polyline(dc, points.data(), static_cast<int>(points.size()));
            SelectObject(dc, old); DeleteObject(shadowPen);
            const auto curvePen = CreatePen(PS_SOLID, 2, accent);
            const auto oldCurve = SelectObject(dc, curvePen);
            Polyline(dc, points.data(), static_cast<int>(points.size()));
            SelectObject(dc, oldCurve); DeleteObject(curvePen);
        }

        if (graphicEq) {
            for (int band = 0; band < 31; ++band) {
                const auto key = "band" + std::to_string(band) + "GainDb";
                const auto gain = number(key.c_str(), 0.0f);
                const auto x = graph.left + static_cast<LONG>(band / 30.0f *
                    (graph.right - graph.left));
                const auto y = graph.top + static_cast<LONG>(
                    std::clamp(0.5f - gain /
                        (hudDbHalfRange * 2.0f), 0.0f, 1.0f) *
                    (graph.bottom - graph.top));
                EqNodeHit hit;
                hit.body = RECT{x - 5, y - 8, x + 6, y + 9};
                hit.band = band; hit.gainKey = key; hit.gainOnly = true;
                const auto brush = CreateSolidBrush(secondary);
                const auto oldBrush = SelectObject(dc, brush);
                Ellipse(dc, hit.body.left, hit.body.top, hit.body.right, hit.body.bottom);
                SelectObject(dc, oldBrush); DeleteObject(brush);
                eqNodeHits.push_back(std::move(hit));
            }
            RECT help{bounds.left + 12, bounds.bottom - 34,
                bounds.right - 12, bounds.bottom - 6};
            text(dc, L"Drag bands directly. Double-click a band to reset it.",
                help, muted);
            hudFooterRect = {};
        } else if (bandEditor) {
            const auto root = module.type == "eq.midSide"
                ? std::string(selectedSideChannel ? "sideBand" : "midBand")
                : std::string("band");
            const auto bands = bandCountForModule(module);
            if (!multiband) for (int band = 0; band < bands; ++band) {
                if (!bandIsActive(module, band)) continue;
                auto prefix = root + std::to_string(band) + ".";
                if (module.type == "multiband.dynamicEq")
                    prefix += "dyn" + std::to_string(selectedNestedBand) + ".";
                const auto frequencyKey = prefix + "frequencyHz";
                const auto gainKey = prefix +
                    (module.type == "eq.dynamic" ? "staticGainDb" : "gainDb");
                const auto frequency = number(frequencyKey.c_str(),
                    80.0f * std::pow(2.0f, static_cast<float>(band)));
                const auto gain = number(gainKey.c_str(), 0.0f);
                const auto x = graph.left + static_cast<LONG>(std::clamp(
                    std::log(std::max(frequency, 20.0f) / 20.0f) /
                        std::log(1000.0f), 0.0f, 1.0f) *
                    (graph.right - graph.left));
                const auto y = graph.top + static_cast<LONG>(
                    std::clamp(0.5f - gain /
                        (hudDbHalfRange * 2.0f), 0.0f, 1.0f) *
                    (graph.bottom - graph.top));
                EqNodeHit hit;
                hit.body = RECT{x - 8, y - 8, x + 9, y + 9};
                hit.band = band;
                hit.frequencyKey = frequencyKey;
                hit.gainKey = gainKey;
                const auto nodeColor = band == selectedBand ? accent : secondary;
                const auto brush = CreateSolidBrush(nodeColor);
                const auto oldBrush = SelectObject(dc, brush);
                Ellipse(dc, hit.body.left, hit.body.top, hit.body.right, hit.body.bottom);
                SelectObject(dc, oldBrush); DeleteObject(brush);
                RECT nodeLabel{hit.body.left - 8, hit.body.top - 18,
                    hit.body.right + 8, hit.body.top - 1};
                text(dc, std::to_wstring(band + 1), nodeLabel, nodeColor,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                eqNodeHits.push_back(std::move(hit));
            }

            const int stripTop = bounds.bottom - 34;
            bandPreviousRect = RECT{bounds.left + 12, stripTop,
                bounds.left + 42, bounds.bottom - 6};
            RECT bandLabel{bounds.left + 46, stripTop,
                bounds.left + 126, bounds.bottom - 6};
            bandNextRect = RECT{bounds.left + 130, stripTop,
                bounds.left + 160, bounds.bottom - 6};
            const auto active = bandIsActive(module, selectedBand);
            fill(dc, bandPreviousRect, RGB(15, 24, 25));
            fill(dc, bandNextRect, RGB(15, 24, 25));
            frame(dc, bandPreviousRect, border); frame(dc, bandNextRect, border);
            text(dc, L"<", bandPreviousRect, secondary,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            text(dc, L">", bandNextRect, secondary,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            text(dc, L"BAND " + std::to_wstring(selectedBand + 1) +
                (active ? L"" : L"  empty"), bandLabel,
                active ? accent : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            const auto fixedBands = module.type.rfind("multiband.", 0) == 0 ||
                module.type == "saturation.multibandExciter";
            if (!fixedBands) {
                bandAddRect = RECT{bounds.left + 170, stripTop,
                    bounds.left + 244, bounds.bottom - 6};
                bandDeleteRect = RECT{bounds.left + 250, stripTop,
                    bounds.left + 330, bounds.bottom - 6};
                fill(dc, bandAddRect, RGB(18, 31, 31));
                fill(dc, bandDeleteRect, RGB(27, 20, 24));
                frame(dc, bandAddRect, border); frame(dc, bandDeleteRect, border);
                text(dc, L"+ BAND", bandAddRect, accent,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                text(dc, L"REMOVE", bandDeleteRect,
                    active ? RGB(220, 112, 150) : muted,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            if (module.type == "eq.midSide") {
                bandChannelRect = RECT{bounds.right - 92, stripTop,
                    bounds.right - 12, bounds.bottom - 6};
                fill(dc, bandChannelRect, RGB(18, 31, 31));
                frame(dc, bandChannelRect, border);
                text(dc, selectedSideChannel ? L"SIDE" : L"MID",
                    bandChannelRect, accent,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            if (module.type == "multiband.dynamicEq") {
                bandNestedRect = RECT{bounds.right - 104, stripTop,
                    bounds.right - 12, bounds.bottom - 6};
                fill(dc, bandNestedRect, RGB(18, 31, 31));
                frame(dc, bandNestedRect, border);
                text(dc, L"DYN " + std::to_wstring(selectedNestedBand + 1) + L" / 8",
                    bandNestedRect, accent,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            hudFooterRect = {};
        } else {
            RECT footer{bounds.left + 12, bounds.bottom - 26,
                bounds.right - 12, bounds.bottom - 4};
            hudFooterRect = footer;
            drawMeterFooter(dc, state.telemetry, muted);
        }
    }

    void drawMeterFooter(HDC dc,
        const goden_audio::MasterDspTelemetryV1& telemetry, COLORREF muted)
    {
        if (IsRectEmpty(&hudFooterRect)) return;
        fill(dc, hudFooterRect, RGB(7, 11, 12));
        const auto status = L"IN " + oneDecimal(amplitudeDb(std::max(
                telemetry.inputPeak[0], telemetry.inputPeak[1]))) +
            L" dB   OUT " + oneDecimal(amplitudeDb(std::max(
                telemetry.outputPeak[0], telemetry.outputPeak[1]))) +
            L" dB   GR " + oneDecimal(telemetry.gainReductionDb) + L" dB";
        text(dc, status, hudFooterRect, muted);
    }

    float autoEqPeakBoostDb(
        const goden_audio::MasterDspModuleV1& module,
        const sonare::util::json::Object& object)
    {
        if (module.parametersJson == autoEqHeadroomJson)
            return autoEqPeakBoostCache;

        struct Band {
            int type = 0;
            float frequency = 1000.0f;
            float gain = 0.0f;
            float q = 0.707f;
        };
        auto number = [&](const std::string& key, float fallback) {
            const auto found = object.find(key);
            if (found == object.end()) return fallback;
            if (found->second.is_number()) return found->second.as_float();
            if (found->second.is_bool()) return found->second.as_bool()
                ? 1.0f : 0.0f;
            return fallback;
        };
        std::vector<Band> active;
        active.reserve(24);
        for (int index = 0; index < 128; ++index) {
            const auto prefix = "band" + std::to_string(index) + ".";
            if (object.find(prefix + "frequencyHz") == object.end() &&
                    object.find(prefix + "gainDb") == object.end() &&
                    object.find(prefix + "type") == object.end()) continue;
            if (number(prefix + "enabled", 1.0f) < 0.5f ||
                    number(prefix + "bypassed", 0.0f) >= 0.5f) continue;
            active.push_back({
                static_cast<int>(std::lround(number(prefix + "type", 0.0f))),
                number(prefix + "frequencyHz", 1000.0f),
                number(prefix + "gainDb", 0.0f),
                std::max(0.1f, number(prefix + "q", 0.707f)),
            });
        }

        auto peak = 0.0f;
        for (int point = 0; point < 192; ++point) {
            const auto position = static_cast<float>(point) / 191.0f;
            const auto frequency = 20.0f * std::pow(1000.0f, position);
            auto response = 0.0f;
            for (const auto& band : active)
                response += eqMagnitudeDb(band.type, frequency,
                    band.frequency, band.gain, band.q);
            peak = std::max(peak, response);
        }
        autoEqHeadroomJson = module.parametersJson;
        autoEqPeakBoostCache = peak;
        return peak;
    }

    void drawEqBandTable(HDC dc, const RECT& viewport,
        const goden_audio::MasterDspModuleV1& module,
        COLORREF accent, COLORREF secondary, COLORREF bright,
        COLORREF muted, COLORREF panel, COLORREF border)
    {
        parameterViewport = viewport;
        parameterHits.clear();
        parameterScrollRail = {};
        parameterScrollThumb = {};
        sonare::util::json::Object object;
        try {
            object = sonare::util::json::parse_strict(
                module.parametersJson.empty() ? "{}" : module.parametersJson).as_object();
        } catch (...) {
        }
        auto scalar = [&](const std::string& key, float fallback, bool& explicitValue) {
            explicitValue = false;
            const auto found = object.find(key);
            if (found == object.end()) return fallback;
            if (found->second.is_number()) {
                explicitValue = true; return found->second.as_float();
            }
            if (found->second.is_bool()) {
                explicitValue = true; return found->second.as_bool() ? 1.0f : 0.0f;
            }
            return fallback;
        };
        std::vector<int> bands;
        const auto bandCount = bandCountForModule(module);
        for (int band = 0; band < bandCount; ++band)
            if (bandIsActive(module, band)) bands.push_back(band);
        auto prefixFor = [&](int band) {
            if (module.type == "eq.midSide")
                return std::string(selectedSideChannel ? "sideBand" : "midBand") +
                    std::to_string(band) + ".";
            return std::string("band") + std::to_string(band) + ".";
        };
        if (sortEqBandsByFrequency) {
            std::stable_sort(bands.begin(), bands.end(), [&](int left, int right) {
                bool leftExplicit = false, rightExplicit = false;
                const auto leftFrequency = scalar(prefixFor(left) + "frequencyHz",
                    1000.0f, leftExplicit);
                const auto rightFrequency = scalar(prefixFor(right) + "frequencyHz",
                    1000.0f, rightExplicit);
                return leftFrequency < rightFrequency;
            });
        }

        const auto dynamicBand = module.type == "eq.dynamic";
        const auto autoEq = module.type == "autoeq.equalizerApo";
        const auto fullBand = !dynamicBand;
        const int preampHeight = autoEq ? 46 : 0;
        const int tableTop = viewport.top + preampHeight;
        const int headerHeight = 27;
        const int rowHeight = 38;
        const int footerHeight = 24;
        const int contentHeight = static_cast<int>(bands.size()) * rowHeight;
        const int available = std::max(1, static_cast<int>(viewport.bottom - tableTop) -
            headerHeight - footerHeight);
        parameterScrollMaximum = std::max(0, contentHeight - available);
        parameterScroll = std::clamp(parameterScroll, 0, parameterScrollMaximum);

        const int width = static_cast<int>(viewport.right - viewport.left);
        const int onWidth = fullBand ? 42 : 30;
        const int idWidth = 34;
        const int typeWidth = fullBand ? std::clamp(width / 5, 82, 118) : 0;
        const int remaining = std::max(180, width - onWidth - idWidth - typeWidth);
        const int frequencyWidth = fullBand ? remaining * 38 / 100 : remaining * 52 / 100;
        const int gainWidth = fullBand ? remaining * 34 / 100 : remaining - frequencyWidth;
        const int qWidth = fullBand ? std::max(0,
            remaining - frequencyWidth - gainWidth) : 0;
        if (autoEq) {
            bool preampExplicit = false;
            const auto preampSpec = parameterSpec("preampDb", module.type);
            const auto preamp = scalar(
                "preampDb", preampSpec.defaultValue, preampExplicit);
            RECT row{viewport.left, viewport.top,
                viewport.right, tableTop - 4};
            fill(dc, row, RGB(11, 18, 19));
            frame(dc, row, border);
            RECT label{row.left + 10, row.top + 3,
                row.left + 135, row.bottom - 3};
            text(dc, L"MANUAL PREAMP", label, secondary);

            ParameterHit hit;
            hit.spec = preampSpec;
            hit.currentValue = preamp;
            hit.explicitValue = preampExplicit;
            hit.body = row;
            hit.value = RECT{row.left + 137, row.top + 6,
                row.left + 244, row.bottom - 6};
            hit.track = RECT{row.left + 256,
                (row.top + row.bottom) / 2 - 3,
                std::min(row.left + 410, row.right - 12),
                (row.top + row.bottom) / 2 + 3};
            frame(dc, hit.value, border);
            text(dc, formatParameterValue(hit.spec, preamp,
                preampExplicit), hit.value, bright,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (hit.track.right > hit.track.left) {
                fill(dc, hit.track, RGB(28, 38, 39));
                RECT amount = hit.track;
                amount.right = amount.left + static_cast<LONG>(normalizedValue(
                    hit.spec, preamp) * (hit.track.right - hit.track.left));
                fill(dc, amount, accent);
            }
            parameterHits.push_back(std::move(hit));

            const auto peakBoost = autoEqPeakBoostDb(module, object);
            const auto potentialOver = std::max(0.0f, preamp + peakBoost);
            RECT status{std::min(row.left + 424, row.right), row.top + 3,
                row.right - 10, row.bottom - 3};
            if (status.right > status.left + 70) {
                if (potentialOver > 0.05f) {
                    const auto message = L"POTENTIAL +" + oneDecimal(potentialOver) +
                        L" dB  |  SUGGEST <= " + oneDecimal(-peakBoost) + L" dB";
                    text(dc, message, status, RGB(239, 94, 145),
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    text(dc, L"FIXED GAIN  |  HEADROOM OK", status, muted,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }

        int x = viewport.left;
        auto headerCell = [&](int cellWidth, const wchar_t* label) {
            if (cellWidth <= 0) return;
            RECT cell{x, tableTop, x + cellWidth, tableTop + headerHeight};
            text(dc, label, cell, muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += cellWidth;
        };
        headerCell(onWidth, fullBand ? L"ON" : L"");
        headerCell(idWidth, L"#");
        headerCell(typeWidth, L"TYPE");
        headerCell(frequencyWidth, L"FREQUENCY");
        headerCell(gainWidth, L"GAIN");
        headerCell(qWidth, fullBand ? L"Q" : L"");
        drawLine(dc, viewport.left, tableTop + headerHeight - 1,
            viewport.right, tableTop + headerHeight - 1, border);

        const auto saved = SaveDC(dc);
        IntersectClipRect(dc, viewport.left, tableTop + headerHeight,
            viewport.right, viewport.bottom - footerHeight);
        for (std::size_t row = 0; row < bands.size(); ++row) {
            const auto band = bands[row];
            const auto prefix = prefixFor(band);
            const auto y = tableTop + headerHeight +
                static_cast<int>(row) * rowHeight - parameterScroll;
            RECT rowRect{viewport.left, y, viewport.right, y + rowHeight};
            if (rowRect.bottom <= tableTop + headerHeight ||
                    rowRect.top >= viewport.bottom - footerHeight) continue;
            fill(dc, rowRect, band == selectedBand ? RGB(18, 37, 36)
                                                   : (row % 2 ? RGB(11, 17, 18) : panel));
            if (band == selectedBand) {
                RECT marker{rowRect.left, rowRect.top, rowRect.left + 3, rowRect.bottom};
                fill(dc, marker, accent);
            }
            drawLine(dc, rowRect.left, rowRect.bottom - 1,
                rowRect.right, rowRect.bottom - 1, RGB(24, 32, 33));
            int cellX = viewport.left;
            auto addCell = [&](int cellWidth, const std::string& key,
                               bool interactive, const wchar_t* fallback = L"") {
                if (cellWidth <= 0) return;
                RECT cell{cellX + 3, y + 4, cellX + cellWidth - 3, y + rowHeight - 4};
                cellX += cellWidth;
                if (!interactive) {
                    text(dc, fallback, cell, muted,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    return;
                }
                ParameterHit hit;
                hit.spec = parameterSpec(key, module.type);
                hit.currentValue = scalar(key, hit.spec.defaultValue, hit.explicitValue);
                hit.band = band;
                hit.body = cell;
                hit.value = hit.spec.choices.empty() ?
                    RECT{cell.left + 3, cell.top, cell.right - 3, cell.bottom - 6} : RECT{};
                hit.track = hit.spec.toggle || !hit.spec.choices.empty() ? cell :
                    RECT{cell.left + 5, cell.bottom - 5, cell.right - 5, cell.bottom - 2};
                if (hit.spec.toggle) {
                    const auto enabled = hit.currentValue >= 0.5f;
                    RECT switchBody{cell.left + (cell.right - cell.left - 28) / 2,
                        cell.top + 6, cell.left + (cell.right - cell.left - 28) / 2 + 28,
                        cell.bottom - 6};
                    fill(dc, switchBody, enabled ? RGB(25, 65, 61) : RGB(29, 37, 38));
                    frame(dc, switchBody, enabled ? accent : border);
                    RECT light = enabled ? RECT{switchBody.right - 9, switchBody.top + 5,
                        switchBody.right - 4, switchBody.bottom - 5}
                        : RECT{switchBody.left + 4, switchBody.top + 5,
                            switchBody.left + 9, switchBody.bottom - 5};
                    fill(dc, light, enabled ? accent : muted);
                } else {
                    frame(dc, cell, band == selectedBand ? secondary : border);
                    text(dc, formatParameterValue(hit.spec, hit.currentValue,
                        hit.explicitValue), cell,
                        band == selectedBand ? bright : secondary,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    if (hit.spec.choices.empty()) {
                        RECT amount = hit.track;
                        amount.right = amount.left + static_cast<LONG>(normalizedValue(
                            hit.spec, hit.currentValue) * (hit.track.right - hit.track.left));
                        fill(dc, hit.track, RGB(28, 38, 39));
                        fill(dc, amount, accent);
                    }
                }
                parameterHits.push_back(std::move(hit));
            };
            addCell(onWidth, prefix + "enabled", fullBand);
            RECT id{cellX, y, cellX + idWidth, y + rowHeight};
            text(dc, std::to_wstring(band + 1), id,
                band == selectedBand ? accent : muted,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            cellX += idWidth;
            addCell(typeWidth, prefix + "type", fullBand);
            addCell(frequencyWidth, prefix + "frequencyHz", true);
            addCell(gainWidth, prefix +
                (dynamicBand ? "staticGainDb" : "gainDb"), true);
            addCell(qWidth, prefix + "q", fullBand);
        }
        RestoreDC(dc, saved);

        RECT footer{viewport.left, viewport.bottom - footerHeight,
            viewport.right, viewport.bottom};
        if (bands.empty()) {
            RECT empty{viewport.left, tableTop + headerHeight,
                viewport.right, viewport.bottom - footerHeight};
            text(dc, L"No active bands. Double-click the graph or choose + BAND.",
                empty, muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const auto countText = std::to_wstring(bands.size()) + L" / " +
            std::to_wstring(bandCount) + L" bands";
        text(dc, countText, footer, muted);
        bandSortRect = RECT{footer.right - 104, footer.top + 1,
            footer.right, footer.bottom - 1};
        fill(dc, bandSortRect, sortEqBandsByFrequency ? RGB(25, 52, 50) : panel);
        frame(dc, bandSortRect, sortEqBandsByFrequency ? accent : border);
        text(dc, sortEqBandsByFrequency ? L"SORTED" : L"SORT FREQ",
            bandSortRect, sortEqBandsByFrequency ? accent : secondary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (parameterScrollMaximum > 0) {
            parameterScrollRail = RECT{viewport.right - 6,
                tableTop + headerHeight, viewport.right,
                viewport.bottom - footerHeight};
            fill(dc, parameterScrollRail, RGB(24, 32, 33));
            const auto viewHeight = std::max(1L,
                parameterScrollRail.bottom - parameterScrollRail.top);
            const auto thumbHeight = std::max(28L,
                viewHeight * viewHeight /
                    std::max<LONG>(contentHeight, viewHeight));
            const auto travel = viewHeight - thumbHeight;
            const auto thumbTop = parameterScrollRail.top + travel *
                parameterScroll / std::max(1, parameterScrollMaximum);
            parameterScrollThumb = RECT{parameterScrollRail.left - 3, thumbTop,
                parameterScrollRail.right, thumbTop + thumbHeight};
            fill(dc, parameterScrollThumb, secondary);
        }
    }

    void drawParameterEditor(HDC dc, const RECT& viewport,
        const goden_audio::MasterDspModuleV1& module,
        const goden_audio::MasterDspCatalogEntryV1* catalog,
        COLORREF accent, COLORREF secondary, COLORREF bright,
        COLORREF muted, COLORREF panel, COLORREF border)
    {
        if (hasEqBandTable(module.type) && !showDetails) {
            drawEqBandTable(dc, viewport, module, accent, secondary, bright,
                muted, panel, border);
            return;
        }
        parameterViewport = viewport;
        parameterHits.clear();
        parameterScrollRail = {};
        parameterScrollThumb = {};
        const auto keys = parameterKeys(module, catalog);
        const auto columns = (viewport.right - viewport.left) >= 560 ? 3 : 2;
        const int gap = 8;
        const int cardHeight = 70;
        const int cardWidth = std::max(120,
            static_cast<int>(viewport.right - viewport.left -
                gap * (columns - 1)) / columns);
        const int rows = static_cast<int>((keys.size() + columns - 1) / columns);
        parameterScrollMaximum = std::max(0,
            rows * (cardHeight + gap) -
                static_cast<int>(viewport.bottom - viewport.top));
        parameterScroll = std::clamp(parameterScroll, 0, parameterScrollMaximum);
        const auto saved = SaveDC(dc);
        IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            ParameterHit hit;
            const int column = static_cast<int>(index % columns);
            const int row = static_cast<int>(index / columns);
            const int x = viewport.left + column * (cardWidth + gap);
            const int y = viewport.top + row * (cardHeight + gap) - parameterScroll;
            hit.body = RECT{x, y, x + cardWidth, y + cardHeight};
            if (hit.body.bottom < viewport.top || hit.body.top > viewport.bottom) continue;
            hit.spec = parameterSpec(keys[index], module.type);
            hit.currentValue = readParameterValue(module, hit.spec, hit.explicitValue);
            fill(dc, hit.body, panel); frame(dc, hit.body, border);
            RECT label{hit.body.left + 10, hit.body.top + 5,
                hit.body.right - 8, hit.body.top + 27};
            text(dc, hit.spec.label, label, bright);
            hit.value = RECT{hit.body.left + 8, hit.body.top + 25,
                hit.body.right - 8, hit.body.top + 47};
            text(dc, formatParameterValue(hit.spec, hit.currentValue,
                hit.explicitValue), hit.value, hit.explicitValue ? secondary : muted,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            hit.track = RECT{hit.body.left + 10, hit.body.bottom - 16,
                hit.body.right - 10, hit.body.bottom - 10};
            if (hit.spec.toggle) {
                RECT toggleTrack{hit.body.right - 50, hit.body.top + 8,
                    hit.body.right - 10, hit.body.top + 24};
                fill(dc, toggleTrack, hit.currentValue >= 0.5f
                    ? RGB(26, 65, 62) : RGB(31, 39, 40));
                frame(dc, toggleTrack, hit.currentValue >= 0.5f ? accent : border);
                const auto knobX = hit.currentValue >= 0.5f
                    ? toggleTrack.right - 12 : toggleTrack.left + 4;
                RECT knob{knobX, toggleTrack.top + 4, knobX + 8, toggleTrack.bottom - 4};
                fill(dc, knob, hit.currentValue >= 0.5f ? accent : muted);
                hit.track = hit.body;
            } else {
                fill(dc, hit.track, RGB(27, 37, 38));
                const auto normalized = normalizedValue(hit.spec, hit.currentValue);
                RECT active = hit.track;
                active.right = active.left + static_cast<LONG>(normalized *
                    (hit.track.right - hit.track.left));
                fill(dc, active, accent);
                const auto defaultX = hit.track.left + static_cast<LONG>(
                    normalizedValue(hit.spec, hit.spec.defaultValue) *
                    (hit.track.right - hit.track.left));
                drawLine(dc, defaultX, hit.track.top - 2, defaultX,
                    hit.track.bottom + 2, muted);
            }
            parameterHits.push_back(std::move(hit));
        }
        if (keys.empty()) {
            RECT empty = viewport;
            text(dc, L"This module has no scalar controls.", empty, muted,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RestoreDC(dc, saved);
        if (parameterScrollMaximum > 0) {
            parameterScrollRail = RECT{viewport.right - 4, viewport.top,
                viewport.right, viewport.bottom};
            fill(dc, parameterScrollRail, RGB(24, 32, 33));
            const auto viewHeight = static_cast<int>(viewport.bottom - viewport.top);
            const auto contentHeight = viewHeight + parameterScrollMaximum;
            const auto thumbHeight = std::max(24, viewHeight * viewHeight / contentHeight);
            const auto travel = viewHeight - thumbHeight;
            const auto thumbTop = viewport.top + (parameterScrollMaximum == 0 ? 0
                : travel * parameterScroll / parameterScrollMaximum);
            parameterScrollThumb = RECT{parameterScrollRail.left - 3, thumbTop,
                parameterScrollRail.right, thumbTop + thumbHeight};
            fill(dc, parameterScrollThumb, secondary);
        }
    }

    void paint()
    {
        const auto theme = ThemeService::instance().snapshot();
        const auto telemetry = MasterDspController::instance().telemetrySnapshot();
        if (!paintStateValid || paintedThemeGeneration != theme.generation ||
            paintState.telemetry.sampleRate != telemetry.sampleRate) {
            InvalidateRect(window, nullptr, FALSE);
        }
        const bool metersOnly = paintStateValid &&
            isDspMeterDamageOnly(window, hudFooterRect);
        TelemetrySpan paintSpan(TelemetrySurface::graphics,
            metersOnly ? 211U : 210U);
        if (!metersOnly) {
            paintState = MasterDspController::instance().snapshot();
            paintStateValid = true;
            paintedThemeGeneration = theme.generation;
        } else {
            paintState.telemetry = telemetry;
        }
        const auto& state = paintState;
        PAINTSTRUCT paintStruct{};
        const auto dc = BeginPaint(window, &paintStruct);
        RECT client{}; GetClientRect(window, &client);
        fill(dc, client, RGB(8, 12, 13));
        SelectObject(dc, font);
        const auto accent = color(theme.accent.red, theme.accent.green, theme.accent.blue);
        const auto secondary = color(theme.secondary.red, theme.secondary.green, theme.secondary.blue);
        const auto bright = RGB(224, 234, 235);
        const auto muted = RGB(118, 132, 134);
        const auto panel = RGB(14, 20, 21);
        const auto border = RGB(38, 50, 51);

        RECT toolbar{0, 0, client.right, 64}; fill(dc, toolbar, RGB(10, 15, 16));
        auto button = [&](RECT& rectangle, int left, int width, const std::wstring& label,
            bool active = false) {
            rectangle = RECT{left, 14, left + width, 50};
            fill(dc, rectangle, active ? RGB(27, 48, 47) : panel);
            frame(dc, rectangle, border);
            text(dc, label, rectangle, active ? accent : bright,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        button(powerRect, 14, 76, state.enabled ? L"POWER" : L"OFF", state.enabled);
        button(bypassRect, 96, 80, L"BYPASS", state.bypassed);
        button(presetRect, 182, 160, widen(state.presetName));
        button(abRect, 348, 54, state.selectedB ? L"B" : L"A", state.selectedB);
        button(copyAToBRect, 408, 54, L"A > B");
        button(copyBToARect, 468, 54, L"B > A");
        button(undoRect, 528, 56, L"UNDO");
        button(redoRect, 590, 56, L"REDO");
        if (client.right >= 1000) {
            RECT meter{client.right - 340, 8, client.right - 14, 56};
            const auto latencyMs = state.telemetry.sampleRate != 0
                ? 1000.0f * state.telemetry.latencySamples /
                    state.telemetry.sampleRate : 0.0f;
            const auto status = L"IN " + oneDecimal(amplitudeDb(
                    std::max(state.telemetry.inputPeak[0], state.telemetry.inputPeak[1]))) +
                L"  OUT " + oneDecimal(amplitudeDb(
                    std::max(state.telemetry.outputPeak[0], state.telemetry.outputPeak[1]))) +
                L"  TP " + oneDecimal(state.telemetry.truePeakDbtp) +
                L"  " + oneDecimal(state.telemetry.integratedLufs) + L" LUFS  " +
                oneDecimal(latencyMs) + L" ms  " +
                oneDecimal(state.telemetry.callbackCpuPercent) + L"% DSP";
            text(dc, status, meter, secondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        if (metersOnly) {
            drawMeterFooter(dc, state.telemetry, muted);
            EndPaint(window, &paintStruct);
            return;
        }
        hudFooterRect = {};

        const bool wide = client.right >= 840;
        const int rackRight = wide ? 330 : client.right - 18;
        RECT rackPanel{14, 78, rackRight, wide ? client.bottom - 14 : client.bottom / 2 - 8};
        fill(dc, rackPanel, RGB(9, 14, 15)); frame(dc, rackPanel, border);
        SelectObject(dc, heading);
        RECT title{26, 86, rackRight - 20, 116}; text(dc, L"MASTER RACK", title, bright);
        SelectObject(dc, font);
        addRect = RECT{rackRight - 102, 84, rackRight - 26, 116};
        fill(dc, addRect, RGB(18, 31, 31)); text(dc, L"+ ADD", addRect, accent,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const auto& rack = state.selectedB ? state.rackB : state.rackA;
        cardHits.clear();
        rackViewport = RECT{24, 124, rackRight - 19, rackPanel.bottom - 70};
        const auto rackViewHeight = std::max(1L,
            rackViewport.bottom - rackViewport.top);
        const auto rackContentHeight = static_cast<int>(rack.modules.size()) * 62;
        rackScrollMaximum = std::max(0, rackContentHeight -
            static_cast<int>(rackViewHeight));
        rackScroll = std::clamp(rackScroll, 0, rackScrollMaximum);
        const auto rackSaved = SaveDC(dc);
        IntersectClipRect(dc, rackViewport.left, rackViewport.top,
            rackViewport.right, rackViewport.bottom);
        int y = 126 - rackScroll;
        for (const auto& module : rack.modules) {
            if (y + 56 <= rackViewport.top) { y += 62; continue; }
            if (y >= rackViewport.bottom) break;
            CardHit hit;
            hit.body = RECT{24, y, rackRight - 24, y + 56};
            hit.drag = RECT{26, y, 38, y + 56};
            hit.power = RECT{42, y + 13, 74, y + 43};
            hit.up = RECT{rackRight - 110, y + 13, rackRight - 88, y + 43};
            hit.down = RECT{rackRight - 84, y + 13, rackRight - 62, y + 43};
            hit.remove = RECT{rackRight - 56, y + 13, rackRight - 30, y + 43};
            hit.uuid = module.uuid;
            fill(dc, hit.body, module.uuid == selected ? RGB(24, 35, 36) : panel);
            frame(dc, hit.body, module.terminal ? secondary : border);
            if (!module.terminal) {
                for (int row = 0; row < 3; ++row) {
                    RECT leftDot{hit.drag.left + 2, y + 19 + row * 6,
                        hit.drag.left + 4, y + 21 + row * 6};
                    RECT rightDot{hit.drag.left + 7, y + 19 + row * 6,
                        hit.drag.left + 9, y + 21 + row * 6};
                    fill(dc, leftDot, muted); fill(dc, rightDot, muted);
                }
            }
            fill(dc, hit.power, module.bypassed ? RGB(25, 32, 33) : RGB(24, 55, 53));
            frame(dc, hit.power, module.bypassed ? border : accent);
            text(dc, module.bypassed ? L"OFF" : L"ON", hit.power,
                module.bypassed ? muted : accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            const auto catalogItem = std::find_if(addCatalog.begin(), addCatalog.end(),
                [&](const auto& entry) { return entry.type == module.type; });
            const auto moduleName = catalogItem != addCatalog.end()
                ? catalogItem->displayName : module.type;
            RECT name{82, y + 5, rackRight - 116, y + 31};
            text(dc, widen(moduleName), name, module.bypassed ? muted : bright);
            RECT summary{82, y + 27, rackRight - 116, y + 51};
            std::wstring summaryText = module.terminal ? L"Terminal / " : L"";
            try {
                const auto values = sonare::util::json::parse_strict(
                    module.parametersJson.empty() ? "{}" : module.parametersJson);
                int shown = 0;
                for (const auto& [key, value] : values.as_object()) {
                    if ((!value.is_number() && !value.is_bool()) || shown >= 2) continue;
                    const auto spec = parameterSpec(key, module.type);
                    const auto scalar = value.is_bool()
                        ? (value.as_bool() ? 1.0f : 0.0f) : value.as_float();
                    if (shown++ != 0) summaryText += L"  /  ";
                    summaryText += spec.label + L" " +
                        formatParameterValue(spec, scalar, true);
                }
            } catch (...) {
            }
            if (summaryText.empty()) summaryText = L"Default settings";
            text(dc, summaryText, summary, muted);
            POINT upTriangle[3]{{(hit.up.left + hit.up.right) / 2, hit.up.top + 7},
                {hit.up.left + 5, hit.up.bottom - 7}, {hit.up.right - 5, hit.up.bottom - 7}};
            POINT downTriangle[3]{{hit.down.left + 5, hit.down.top + 7},
                {hit.down.right - 5, hit.down.top + 7},
                {(hit.down.left + hit.down.right) / 2, hit.down.bottom - 7}};
            const auto arrowBrush = CreateSolidBrush(muted);
            const auto oldBrush = SelectObject(dc, arrowBrush);
            Polygon(dc, upTriangle, 3); Polygon(dc, downTriangle, 3);
            SelectObject(dc, oldBrush); DeleteObject(arrowBrush);
            drawLine(dc, hit.remove.left + 7, hit.remove.top + 8,
                hit.remove.right - 7, hit.remove.bottom - 8, RGB(220, 112, 150), 2);
            drawLine(dc, hit.remove.right - 7, hit.remove.top + 8,
                hit.remove.left + 7, hit.remove.bottom - 8, RGB(220, 112, 150), 2);
            cardHits.push_back(std::move(hit));
            y += 62;
        }
        RestoreDC(dc, rackSaved);
        rackScrollRail = {};
        rackScrollThumb = {};
        if (rackScrollMaximum > 0) {
            rackScrollRail = RECT{rackRight - 20, rackViewport.top,
                rackRight - 15, rackViewport.bottom};
            fill(dc, rackScrollRail, RGB(22, 30, 31));
            const auto thumbHeight = std::max(28L,
                rackViewHeight * rackViewHeight /
                    std::max<LONG>(rackContentHeight, rackViewHeight));
            const auto travel = rackViewHeight - thumbHeight;
            const auto thumbTop = rackViewport.top + travel * rackScroll /
                std::max(1, rackScrollMaximum);
            rackScrollThumb = RECT{rackScrollRail.left - 3, thumbTop,
                rackScrollRail.right + 1, thumbTop + thumbHeight};
            fill(dc, rackScrollThumb, secondary);
        }

        importApoRect = RECT{24, rackPanel.bottom - 58, 92, rackPanel.bottom - 24};
        importIrRect = RECT{98, rackPanel.bottom - 58, 154, rackPanel.bottom - 24};
        importPresetRect = RECT{160, rackPanel.bottom - 58, 226, rackPanel.bottom - 24};
        exportPresetRect = RECT{232, rackPanel.bottom - 58, rackRight - 24, rackPanel.bottom - 24};
        for (auto* rectangle : {&importApoRect, &importIrRect, &importPresetRect, &exportPresetRect})
            fill(dc, *rectangle, panel);
        text(dc, L"AutoEQ", importApoRect, secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"IR", importIrRect, secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"Import", importPresetRect, secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"Export", exportPresetRect, secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const int editorLeft = wide ? 342 : 18;
        const int titleTop = wide ? 78 : client.bottom / 2 - 2;
        RECT editorTitle{editorLeft, titleTop,
            client.right - 18, titleTop + 38};
        SelectObject(dc, heading);
        const auto selectedModule = std::find_if(rack.modules.begin(), rack.modules.end(),
            [&](const auto& module) { return module.uuid == selected; });
        const auto selectedCatalog = selectedModule == rack.modules.end()
            ? addCatalog.end() : std::find_if(addCatalog.begin(), addCatalog.end(),
                [&](const auto& entry) { return entry.type == selectedModule->type; });
        const auto editorHeading = selected.empty() ? L"SELECT A MODULE" :
            (selectedCatalog != addCatalog.end()
                ? widen(selectedCatalog->displayName) : L"MODULE PARAMETERS");
        text(dc, editorHeading, editorTitle, bright);
        SelectObject(dc, font);
        if (!state.faultText.empty()) {
            RECT fault{editorLeft, titleTop + 22, client.right - 18, titleTop + 44};
            text(dc, widen(state.faultText), fault, RGB(230, 104, 151));
        }
        detailsRect = RECT{client.right - 154, client.bottom - 42,
            client.right - 18, client.bottom - 12};
        fill(dc, detailsRect, showDetails ? RGB(27, 48, 47) : panel);
        frame(dc, detailsRect, border);
        const auto detailsLabel = showDetails
            ? (selectedModule != rack.modules.end() && hasEqBandTable(selectedModule->type)
                ? L"BAND TABLE" : L"FEWER CONTROLS")
            : L"MORE CONTROLS";
        text(dc, detailsLabel,
            detailsRect, showDetails ? accent : secondary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (selectedModule != rack.modules.end()) {
            const auto* catalogPointer = selectedCatalog == addCatalog.end()
                ? nullptr : &*selectedCatalog;
            const int hudTop = titleTop + 42;
            const int hudHeight = wide ? 184 : 126;
            RECT hud{editorLeft, hudTop, client.right - 18, hudTop + hudHeight};
            drawModuleHud(dc, hud, *selectedModule, catalogPointer, state,
                accent, secondary, muted, border);
            RECT controls{editorLeft, hud.bottom + 10,
                client.right - 18, client.bottom - 50};
            drawParameterEditor(dc, controls, *selectedModule, catalogPointer,
                accent, secondary, bright, muted, panel, border);
        } else {
            parameterHits.clear();
            RECT empty{editorLeft, titleTop + 40, client.right - 18, client.bottom - 50};
            text(dc, L"Add or select a processor to open its visual controls.",
                empty, muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(window, &paintStruct);
    }

    void syncMeterDemand()
    {
        const bool visible = !IsIconic(window) &&
            DisplayEnvironmentService::instance().presentationVisible(window);
        if (visible == meterDemand) return;
        meterDemand = visible;
        MasterDspController::instance().setTelemetryDemand(visible);
        if (visible) SetTimer(window, kMeterTimer, 33, nullptr);
        else KillTimer(window, kMeterTimer);
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        static const auto visibilityMessage = RegisterWindowMessageW(
            L"Goden.RootPresentationVisibility.V1");
        if (message == visibilityMessage) {
            syncMeterDemand();
            if (meterDemand) InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            DisplayEnvironmentService::instance().registerPersistentRoot(window);
            createChildren(); layoutChildren(); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            applyDpiAwareMinimum(window, info, 640, 480);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            recreateFonts();
            layoutChildren();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            syncMeterDemand();
            if (wParam == SIZE_MINIMIZED) return 0;
            layoutChildren(); InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_SHOWWINDOW:
            syncMeterDemand();
            return 0;
        case WM_WINDOWPOSCHANGED:
            DisplayEnvironmentService::instance().notifyWindowChanged(window);
            syncMeterDemand();
            break;
        case WM_TIMER:
            if (wParam == kMeterTimer) {
                syncMeterDemand();
                if (!meterDemand) return 0;
                RECT toolbar{0, 0, 10000, 64};
                InvalidateRect(window, &toolbar, FALSE);
                if (hudFooterRect.right > hudFooterRect.left)
                    InvalidateRect(window, &hudFooterRect, FALSE);
                return 0;
            }
            break;
        case WM_LBUTTONUP: {
            if (draggingParameterScroll) {
                draggingParameterScroll = false;
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            if (draggingRackScroll) {
                draggingRackScroll = false;
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            if (!draggedEqGainKey.empty() || !draggedEqFrequencyKey.empty()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                updateDraggedEqPoint(point);
                MasterDspController::instance().endParameterGesture();
                draggedEqFrequencyKey.clear();
                draggedEqGainKey.clear();
                draggedEqGainOnly = false;
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            if (!draggedParameter.empty()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                updateDraggedParameter(point);
                MasterDspController::instance().endParameterGesture();
                draggedParameter.clear();
                if (GetCapture() == window) ReleaseCapture();
                InvalidateRect(window, &parameterViewport, FALSE);
                return 0;
            }
            if (!draggedUuid.empty()) {
                const auto distance = dragCurrentY - dragAnchorY;
                const auto delta = distance >= 0
                    ? (distance + 31) / 62 : (distance - 31) / 62;
                if (delta != 0)
                    MasterDspController::instance().moveModule(draggedUuid, delta);
                draggedUuid.clear();
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            click(point); return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (inside(parameterScrollThumb, point)) {
                draggingParameterScroll = true;
                parameterScrollDragAnchor = point.y;
                parameterScrollDragOrigin = parameterScroll;
                SetCapture(window);
                return 0;
            }
            if (inside(parameterScrollRail, point)) {
                const auto page = std::max(48L,
                    (parameterViewport.bottom - parameterViewport.top) * 9 / 10);
                parameterScroll = std::clamp(parameterScroll +
                    (point.y < parameterScrollThumb.top ? -static_cast<int>(page)
                                                        : static_cast<int>(page)),
                    0, parameterScrollMaximum);
                InvalidateRect(window, &parameterViewport, FALSE);
                return 0;
            }
            if (inside(rackScrollThumb, point)) {
                draggingRackScroll = true;
                rackScrollDragAnchor = point.y;
                rackScrollDragOrigin = rackScroll;
                SetCapture(window);
                return 0;
            }
            if (inside(rackScrollRail, point)) {
                const auto page = std::max(62L,
                    (rackViewport.bottom - rackViewport.top) * 9 / 10);
                rackScroll = std::clamp(rackScroll +
                    (point.y < rackScrollThumb.top ? -static_cast<int>(page)
                                                   : static_cast<int>(page)),
                    0, rackScrollMaximum);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            for (const auto& node : eqNodeHits) {
                if (!inside(node.body, point)) continue;
                selectedBand = node.band;
                // The presentation domain is immutable for the whole gesture;
                // otherwise a gain crossing a threshold makes the node jump.
                draggedEqHalfRange = hudDbHalfRange;
                draggedEqFrequencyKey = node.frequencyKey;
                draggedEqGainKey = node.gainKey;
                draggedEqGainOnly = node.gainOnly;
                MasterDspController::instance().beginParameterGesture();
                SetCapture(window);
                updateDraggedEqPoint(point);
                return 0;
            }
            for (const auto& parameter : parameterHits) {
                if (parameter.band >= 0 && !inside(parameter.body, point)) continue;
                if (parameter.band >= 0) selectedBand = parameter.band;
                // Toggles are direct actions, never numeric-edit targets.  In
                // the compact EQ table their value rectangle overlaps the ON
                // cell; handling value first leaked the legacy native "Set"
                // editor into the checkbox and laid it out outside the narrow
                // column.
                if (parameter.spec.toggle && inside(parameter.body, point)) {
                    editingParameter.clear();
                    editingParameterSpec = {};
                    ShowWindow(parameterValue, SW_HIDE);
                    ShowWindow(parameterSet, SW_HIDE);
                    auto& controller = MasterDspController::instance();
                    controller.beginParameterGesture();
                    setParameterValue(parameter.spec.key,
                        parameter.currentValue >= 0.5f ? 0.0f : 1.0f);
                    controller.endParameterGesture();
                    InvalidateRect(window, &parameterViewport, FALSE);
                    return 0;
                }
                if (!parameter.spec.choices.empty() && inside(parameter.body, point)) {
                    const auto count = static_cast<int>(parameter.spec.choices.size());
                    auto current = std::clamp(static_cast<int>(std::lround(
                        parameter.currentValue)), 0, std::max(0, count - 1));
                    if (parameter.spec.choiceValues.size() ==
                            parameter.spec.choices.size()) {
                        float bestDistance = std::numeric_limits<float>::max();
                        for (std::size_t candidate = 0;
                                candidate < parameter.spec.choiceValues.size();
                                ++candidate) {
                            const auto distance = std::abs(
                                parameter.spec.choiceValues[candidate] -
                                parameter.currentValue);
                            if (distance < bestDistance) {
                                bestDistance = distance;
                                current = static_cast<int>(candidate);
                            }
                        }
                    }
                    const auto next = (current + 1) % std::max(1, count);
                    const auto nextValue = parameter.spec.choiceValues.size() ==
                            parameter.spec.choices.size()
                        ? parameter.spec.choiceValues[static_cast<std::size_t>(next)]
                        : static_cast<float>(next);
                    auto& controller = MasterDspController::instance();
                    controller.beginParameterGesture();
                    setParameterValue(parameter.spec.key, nextValue);
                    controller.endParameterGesture();
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (inside(parameter.value, point)) {
                    beginDirectEdit(parameter);
                    return 0;
                }
                if (!inside(parameter.track, point)) continue;
                editingParameter.clear();
                editingParameterSpec = {};
                ShowWindow(parameterValue, SW_HIDE);
                ShowWindow(parameterSet, SW_HIDE);
                draggedParameter = parameter.spec.key;
                draggedParameterValue = parameter.currentValue;
                MasterDspController::instance().beginParameterGesture();
                SetCapture(window);
                updateDraggedParameter(point);
                return 0;
            }
            for (const auto& card : cardHits) {
                if (!inside(card.drag, point)) continue;
                draggedUuid = card.uuid;
                dragAnchorY = point.y;
                dragCurrentY = point.y;
                SetCapture(window);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (draggingParameterScroll) {
                const auto viewHeight = std::max(1L,
                    parameterScrollRail.bottom - parameterScrollRail.top);
                const auto thumbHeight = std::max(1L,
                    parameterScrollThumb.bottom - parameterScrollThumb.top);
                const auto travel = std::max(1L, viewHeight - thumbHeight);
                parameterScroll = std::clamp(parameterScrollDragOrigin +
                    (GET_Y_LPARAM(lParam) - parameterScrollDragAnchor) *
                        parameterScrollMaximum / static_cast<int>(travel),
                    0, parameterScrollMaximum);
                InvalidateRect(window, &parameterViewport, FALSE);
                return 0;
            }
            if (draggingRackScroll) {
                const auto viewHeight = std::max(1L,
                    rackViewport.bottom - rackViewport.top);
                const auto thumbHeight = std::max(1L,
                    rackScrollThumb.bottom - rackScrollThumb.top);
                const auto travel = std::max(1L, viewHeight - thumbHeight);
                rackScroll = std::clamp(rackScrollDragOrigin +
                    (GET_Y_LPARAM(lParam) - rackScrollDragAnchor) *
                        rackScrollMaximum / static_cast<int>(travel),
                    0, rackScrollMaximum);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (!draggedEqGainKey.empty() || !draggedEqFrequencyKey.empty()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                updateDraggedEqPoint(point);
                return 0;
            }
            if (!draggedParameter.empty()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                updateDraggedParameter(point);
                return 0;
            }
            if (!draggedUuid.empty()) {
                dragCurrentY = GET_Y_LPARAM(lParam);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            draggingRackScroll = false;
            draggingParameterScroll = false;
            if (!draggedEqGainKey.empty() || !draggedEqFrequencyKey.empty()) {
                MasterDspController::instance().endParameterGesture();
                draggedEqFrequencyKey.clear();
                draggedEqGainKey.clear();
                draggedEqGainOnly = false;
            }
            if (!draggedParameter.empty()) {
                MasterDspController::instance().endParameterGesture();
                draggedParameter.clear();
            }
            draggedUuid.clear();
            break;
        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (const auto& node : eqNodeHits) {
                if (!inside(node.body, point) || node.gainKey.empty()) continue;
                selectedBand = node.band;
                auto& controller = MasterDspController::instance();
                controller.beginParameterGesture();
                setParameterValue(node.gainKey, 0.0f);
                controller.endParameterGesture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (inside(hudGraphRect, point)) {
                const auto state = MasterDspController::instance().snapshot();
                const auto& rack = state.selectedB ? state.rackB : state.rackA;
                const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
                    [&](const auto& item) { return item.uuid == selected; });
                if (module != rack.modules.end() && hasEqBandTable(module->type) &&
                        module->type != "eq.dynamic") {
                    addEqBand(&point);
                    return 0;
                }
            }
            for (const auto& parameter : parameterHits) {
                if (!inside(parameter.body, point)) continue;
                auto& controller = MasterDspController::instance();
                controller.beginParameterGesture();
                setParameterValue(parameter.spec.key, parameter.spec.defaultValue);
                controller.endParameterGesture();
                InvalidateRect(window, &parameterViewport, FALSE);
                return 0;
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
            for (const auto& node : eqNodeHits) {
                if (!inside(node.body, point)) continue;
                selectedBand = node.band;
                if (!node.frequencyKey.empty()) {
                    const auto dot = node.frequencyKey.rfind("frequencyHz");
                    const auto qKey = dot == std::string::npos ? std::string{}
                        : node.frequencyKey.substr(0, dot) + "q";
                    if (!qKey.empty()) {
                        const auto state = MasterDspController::instance().snapshot();
                        const auto& rack = state.selectedB ? state.rackB : state.rackA;
                        const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
                            [&](const auto& item) { return item.uuid == selected; });
                        if (module != rack.modules.end()) {
                            const auto spec = parameterSpec(qKey, module->type);
                            bool explicitValue = false;
                            const auto current = readParameterValue(*module, spec, explicitValue);
                            const auto fine = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
                            const auto factor = fine ? 1.025f : 1.12f;
                            auto& controller = MasterDspController::instance();
                            controller.beginParameterGesture();
                            setParameterValue(qKey, delta > 0 ? current * factor : current / factor);
                            controller.endParameterGesture();
                            InvalidateRect(window, nullptr, FALSE);
                            return 0;
                        }
                    }
                }
            }
            for (const auto& parameter : parameterHits) {
                if (!inside(parameter.track, point) || parameter.spec.toggle) continue;
                const auto fine = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
                auto step = parameter.spec.step * (fine ? 0.1f : 1.0f);
                if (parameter.spec.logarithmic)
                    step = std::max(step, parameter.currentValue * (fine ? 0.005f : 0.025f));
                auto& controller = MasterDspController::instance();
                controller.beginParameterGesture();
                setParameterValue(parameter.spec.key, parameter.currentValue +
                    (delta > 0 ? step : -step));
                controller.endParameterGesture();
                InvalidateRect(window, &parameterViewport, FALSE);
                return 0;
            }
            if (inside(parameterViewport, point) && parameterScrollMaximum > 0) {
                parameterScroll = std::clamp(parameterScroll -
                    delta / WHEEL_DELTA * 78, 0, parameterScrollMaximum);
                InvalidateRect(window, &parameterViewport, FALSE);
                return 0;
            }
            if (inside(rackViewport, point) && rackScrollMaximum > 0) {
                rackScroll = std::clamp(rackScroll -
                    (delta > 0 ? 62 : -62), 0, rackScrollMaximum);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (const auto& node : eqNodeHits) {
                if (!inside(node.body, point)) continue;
                selectedBand = node.band;
                const auto state = MasterDspController::instance().snapshot();
                const auto& rack = state.selectedB ? state.rackB : state.rackA;
                const auto module = std::find_if(rack.modules.begin(), rack.modules.end(),
                    [&](const auto& item) { return item.uuid == selected; });
                if (module == rack.modules.end()) return 0;
                HMENU menu = CreatePopupMenu();
                const std::array<const wchar_t*, 9> names{L"Peak", L"Low shelf",
                    L"High shelf", L"Low pass", L"High pass", L"Band pass",
                    L"Notch", L"Tilt shelf", L"Flat tilt"};
                for (std::size_t index = 0; index < names.size(); ++index)
                    AppendMenuW(menu, MF_STRING, 8100 + static_cast<UINT>(index),
                        names[index]);
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 8190, L"Toggle band bypass");
                AppendMenuW(menu, MF_STRING, 8191, L"Remove band");
                POINT screen = point; ClientToScreen(window, &screen);
                const auto command = TrackPopupMenu(menu, TPM_RETURNCMD,
                    screen.x, screen.y, 0, window, nullptr);
                DestroyMenu(menu);
                const auto prefix = node.frequencyKey.empty() ? selectedBandPrefix(module->type)
                    : node.frequencyKey.substr(0, node.frequencyKey.rfind("frequencyHz"));
                auto& controller = MasterDspController::instance();
                if (command >= 8100 && command < 8109) {
                    controller.beginParameterGesture();
                    setParameterValue(prefix + "type", static_cast<float>(command - 8100));
                    controller.endParameterGesture();
                } else if (command == 8190) {
                    const auto spec = parameterSpec(prefix + "bypassed", module->type);
                    bool explicitValue = false;
                    const auto current = readParameterValue(*module, spec, explicitValue);
                    controller.beginParameterGesture();
                    setParameterValue(spec.key, current >= 0.5f ? 0.0f : 1.0f);
                    controller.endParameterGesture();
                } else if (command == 8191) {
                    deleteSelectedBand();
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN: {
            const auto control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (control && (wParam == 'Z' || wParam == 'Y')) {
                if (wParam == 'Z') MasterDspController::instance().undo();
                else MasterDspController::instance().redo();
                refreshSelection();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_INSERT) {
                addEqBand();
                return 0;
            }
            if (wParam == VK_DELETE && GetFocus() != parameterValue) {
                deleteSelectedBand();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                addBrowserVisible = false;
                ShowWindow(addSearch, SW_HIDE); ShowWindow(addList, SW_HIDE);
                editingParameter.clear();
                editingParameterSpec = {};
                ShowWindow(parameterValue, SW_HIDE); ShowWindow(parameterSet, SW_HIDE);
                if (!draggedParameter.empty() || !draggedEqGainKey.empty() ||
                        !draggedEqFrequencyKey.empty())
                    MasterDspController::instance().endParameterGesture();
                draggedParameter.clear(); draggedEqGainKey.clear();
                draggedEqFrequencyKey.clear(); draggingRackScroll = false;
                draggingParameterScroll = false;
                if (GetCapture() == window) ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kApplyId && HIWORD(wParam) == BN_CLICKED) {
                applyJson(); return 0;
            }
            if (LOWORD(wParam) == kAddSearchId && HIWORD(wParam) == EN_CHANGE) {
                filterAddBrowser(); return 0;
            }
            if (LOWORD(wParam) == kAddListId && HIWORD(wParam) == LBN_DBLCLK) {
                commitAddBrowser(); return 0;
            }
            if (LOWORD(wParam) == kParameterSetId && HIWORD(wParam) == BN_CLICKED) {
                finishDirectEdit(); return 0;
            }
            break;
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(216, 228, 229));
            SetBkColor(dc, RGB(12, 18, 19));
            return reinterpret_cast<LRESULT>(editBrush);
        }
        case WM_CTLCOLORBTN: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(216, 228, 229));
            SetBkColor(dc, RGB(20, 29, 30));
            return reinterpret_cast<LRESULT>(buttonBrush);
        }
        case WM_CTLCOLORLISTBOX: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(216, 228, 229));
            SetBkColor(dc, RGB(12, 18, 19));
            return reinterpret_cast<LRESULT>(editBrush);
        }
        case kRefreshMessage:
            paintStateValid = false;
            (void)UiInvalidationBus::instance().consume(
                UiInvalidationSurface::masterDsp, window);
            refreshSelection(); InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_PAINT: paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE:
            if (!draggedParameter.empty() || !draggedEqGainKey.empty() ||
                    !draggedEqFrequencyKey.empty())
                MasterDspController::instance().endParameterGesture();
            draggedParameter.clear(); draggedEqGainKey.clear();
            draggedEqFrequencyKey.clear(); draggingRackScroll = false;
            draggingParameterScroll = false;
            if (GetCapture() == window) ReleaseCapture();
            KillTimer(window, kMeterTimer);
            meterDemand = false;
            MasterDspController::instance().setTelemetryDemand(false);
            ShowWindow(window, SW_HIDE); return 0;
        case WM_DESTROY:
            paintStateValid = false;
            meterDemand = false;
            DisplayEnvironmentService::instance().unregisterPersistentRoot(window);
            KillTimer(window, kMeterTimer);
            MasterDspController::instance().setTelemetryDemand(false);
            UiInvalidationBus::instance().unregisterWindow(
                UiInvalidationSurface::masterDsp, window);
            if (font) DeleteObject(font); font = nullptr;
            if (heading) DeleteObject(heading); heading = nullptr;
            if (editBrush) DeleteObject(editBrush); editBrush = nullptr;
            if (buttonBrush) DeleteObject(buttonBrush); buttonBrush = nullptr;
            if (parameterValue) RemovePropW(parameterValue, L"Goden.MasterDsp.Owner");
            parameterEditOriginal = nullptr;
            window = nullptr; return 0;
        default: break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

MasterDspWindow::MasterDspWindow() : impl_(std::make_unique<Impl>())
{
    impl_->owner = this;
}
MasterDspWindow::~MasterDspWindow() { close(); }

MasterDspWindow& MasterDspWindow::instance()
{
    static MasterDspWindow window;
    return window;
}

void MasterDspWindow::show(HWND owner) { impl_->show(owner); }

void MasterDspWindow::close()
{
    if (impl_ && impl_->window && IsWindow(impl_->window)) {
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
}

} // namespace nekorox::component
