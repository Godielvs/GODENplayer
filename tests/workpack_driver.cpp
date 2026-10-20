#include "workpack_driver.h"
#include "nekorox/qa_contracts.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct ScheduledWheel {
    std::uint64_t offsetUs = 0;
    int delta = 0;
};

std::atomic<bool> stopRequested{false};

BOOL WINAPI controlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        stopRequested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

bool parseUnsigned(std::wstring_view value, std::uint64_t& result)
{
    if (value.empty()) return false;
    std::uint64_t parsed = 0;
    for (const auto ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (parsed > (UINT64_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    result = parsed;
    return true;
}

bool parseSigned(std::wstring_view value, int& result)
{
    if (value.empty()) return false;
    bool negative = value.front() == L'-';
    if (negative) value.remove_prefix(1);
    std::uint64_t magnitude = 0;
    if (!parseUnsigned(value, magnitude) || magnitude > 32767U) return false;
    result = static_cast<int>(magnitude) * (negative ? -1 : 1);
    return result != 0;
}

std::vector<std::wstring_view> split(
    std::wstring_view value, wchar_t delimiter)
{
    std::vector<std::wstring_view> pieces;
    while (true) {
        const auto position = value.find(delimiter);
        pieces.push_back(value.substr(0, position));
        if (position == std::wstring_view::npos) break;
        value.remove_prefix(position + 1U);
    }
    return pieces;
}

bool belongsToProcess(HWND window, DWORD expectedProcessId)
{
    if (!IsWindow(window)) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == expectedProcessId;
}

std::uint64_t qpcNow()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(std::max<LONGLONG>(0, value.QuadPart));
}

void waitUntil(std::uint64_t deadline, std::uint64_t frequency)
{
    // Sleep() granularity is the whole point of a burst schedule, so the last
    // stretch is spun and the bulk uses a high-resolution waitable timer when
    // the platform offers one.
    static const HANDLE timer = [] {
        auto handle = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (handle == nullptr) handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        return handle;
    }();
    for (;;) {
        if (stopRequested.load(std::memory_order_acquire)) return;
        const auto now = qpcNow();
        if (now >= deadline) return;
        const auto remainingUs = (deadline - now) * 1000000ULL /
            std::max<std::uint64_t>(1U, frequency);
        if (remainingUs <= 200U) {
            YieldProcessor();
            continue;
        }
        if (remainingUs <= 1500U || timer == nullptr) {
            Sleep(0);
            continue;
        }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((remainingUs - 400U) * 10ULL);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer, 20) != WAIT_OBJECT_0) {
            Sleep(0);
        }
    }
}

bool targetPoint(HWND target, POINT& point)
{
    RECT client{};
    if (!GetClientRect(target, &client)) return false;
    point.x = client.left + (client.right - client.left) * 3 / 4;
    point.y = client.top + (client.bottom - client.top) / 2;
    return ClientToScreen(target, &point) != FALSE;
}

// Album TRACKS and LYRICS occupy disjoint columns, so a schedule must be able
// to aim at one of them. The optional fifth header field carries an absolute
// target in client DIP; without it the historical 3/4-width, 1/2-height point
// is preserved exactly.
bool targetPointDip(HWND target, int xDip, int yDip, POINT& point)
{
    RECT client{};
    if (!GetClientRect(target, &client)) return false;
    const auto dpi = static_cast<double>(
        std::max<UINT>(1U, GetDpiForWindow(target)));
    point.x = client.left + static_cast<LONG>(
        std::lround(static_cast<double>(xDip) * dpi / 96.0));
    point.y = client.top + static_cast<LONG>(
        std::lround(static_cast<double>(yDip) * dpi / 96.0));
    return ClientToScreen(target, &point) != FALSE;
}

bool parsePoint(std::wstring_view value, int& xDip, int& yDip)
{
    const auto pair = split(value, L':');
    if (pair.size() != 2U) return false;
    int x = 0;
    int y = 0;
    if (!parseSigned(pair[0], x) || !parseSigned(pair[1], y)) return false;
    xDip = x;
    yDip = y;
    return true;
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())));
    return values[std::clamp<std::size_t>(rank, 1U, values.size()) - 1U];
}

} // namespace

int runWorkpackWheelDriver(
    HWND target,
    HWND topLevel,
    DWORD expectedProcessId,
    std::wstring_view encodedSchedule)
{
    const auto fields = split(encodedSchedule, L'|');
    if ((fields.size() != 4U && fields.size() != 5U) ||
        (fields[0] != L"handler-message" && fields[0] != L"send-input" &&
         fields[0] != L"post-message")) {
        std::wcerr << L"Invalid workpack schedule header\n";
        return 8;
    }
    int aimXDip = 0;
    int aimYDip = 0;
    const auto aimAtPoint = fields.size() == 5U;
    if (aimAtPoint && !parsePoint(fields[4], aimXDip, aimYDip)) {
        std::wcerr << L"Invalid workpack target point\n";
        return 8;
    }
    std::uint64_t totalUs = 0;
    std::uint64_t loopUs = 0;
    if (!parseUnsigned(fields[1], totalUs) ||
        !parseUnsigned(fields[2], loopUs) || totalUs == 0 || loopUs == 0) {
        std::wcerr << L"Invalid workpack schedule duration\n";
        return 8;
    }
    std::vector<ScheduledWheel> events;
    for (const auto encoded : split(fields[3], L',')) {
        const auto pair = split(encoded, L':');
        ScheduledWheel event;
        if (pair.size() != 2U || !parseUnsigned(pair[0], event.offsetUs) ||
            !parseSigned(pair[1], event.delta) || event.offsetUs >= loopUs) {
            std::wcerr << L"Invalid workpack wheel event\n";
            return 8;
        }
        events.push_back(event);
    }
    if (events.empty() || !std::is_sorted(events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.offsetUs < right.offsetUs;
            })) {
        std::wcerr << L"Workpack schedule must contain sorted events\n";
        return 8;
    }

    LARGE_INTEGER rawFrequency{};
    if (!QueryPerformanceFrequency(&rawFrequency) || rawFrequency.QuadPart <= 0) {
        std::wcerr << L"High-resolution clock is unavailable\n";
        return 8;
    }
    const auto frequency = static_cast<std::uint64_t>(rawFrequency.QuadPart);
    if (!belongsToProcess(target, expectedProcessId) ||
        !belongsToProcess(topLevel, expectedProcessId) ||
        !IsChild(topLevel, target)) {
        std::wcerr << L"Target ownership changed before schedule start\n";
        return 7;
    }

    stopRequested.store(false, std::memory_order_release);
    SetConsoleCtrlHandler(controlHandler, TRUE);
    const auto started = qpcNow();
    std::vector<double> latenessUs;
    std::vector<double> handlerUs;
    std::map<std::wstring, unsigned> routingCounts;
    std::vector<std::wstring> routingOrder;
    std::uint64_t scheduled = 0;
    std::uint64_t delivered = 0;
    std::wstring abortReason = L"none";

    for (std::uint64_t loopBaseUs = 0; loopBaseUs < totalUs;
         loopBaseUs += loopUs) {
        for (const auto& event : events) {
            const auto absoluteUs = loopBaseUs + event.offsetUs;
            if (absoluteUs >= totalUs) break;
            ++scheduled;
            const auto deadline = started + absoluteUs * frequency / 1000000ULL;
            waitUntil(deadline, frequency);
            if (stopRequested.load(std::memory_order_acquire)) {
                abortReason = L"control-request";
                break;
            }
            if (!belongsToProcess(target, expectedProcessId) ||
                !belongsToProcess(topLevel, expectedProcessId) ||
                !IsChild(topLevel, target)) {
                abortReason = L"target-changed";
                stopRequested.store(true, std::memory_order_release);
                break;
            }
            const auto injected = qpcNow();
            latenessUs.push_back(static_cast<double>(
                injected >= deadline ? injected - deadline : 0U) * 1000000.0 /
                static_cast<double>(frequency));
            POINT point{};
            if (!(aimAtPoint
                    ? targetPointDip(target, aimXDip, aimYDip, point)
                    : targetPoint(target, point))) {
                abortReason = L"target-point-failed";
                stopRequested.store(true, std::memory_order_release);
                break;
            }
            bool accepted = false;
            if (fields[0] == L"handler-message") {
                DWORD_PTR ignored = 0;
                accepted = SendMessageTimeoutW(target, WM_MOUSEWHEEL,
                    MAKEWPARAM(0, static_cast<WORD>(event.delta)),
                    MAKELPARAM(point.x, point.y),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) != FALSE;
            } else if (fields[0] == L"post-message") {
                // Real wheel hardware posts; a sent message is dispatched ahead
                // of the queue and can never expose handler starvation. Posting
                // reproduces a burst that must queue behind whatever the UI
                // thread is already doing.
                accepted = PostMessageW(target, WM_MOUSEWHEEL,
                    MAKEWPARAM(0, static_cast<WORD>(event.delta)),
                    MAKELPARAM(point.x, point.y)) != FALSE;
            } else {
                if (GetForegroundWindow() != topLevel) {
                    abortReason = L"foreground-changed";
                    stopRequested.store(true, std::memory_order_release);
                    break;
                }
                // Move with SetCursorPos first: it is synchronous, so the
                // cursor is guaranteed to be over the target before the wheel
                // is injected. The injected absolute move that follows is what
                // produces the WM_MOUSEMOVE stream a real pointer generates,
                // which panels use to take the focus wheel routing follows.
                SetCursorPos(point.x, point.y);
                RECT desktop{0, 0,
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
                INPUT move{};
                move.type = INPUT_MOUSE;
                move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                move.mi.dx = static_cast<LONG>((static_cast<double>(point.x) -
                    desktop.left) * 65535.0 / std::max<LONG>(1, desktop.right));
                move.mi.dy = static_cast<LONG>((static_cast<double>(point.y) -
                    desktop.top) * 65535.0 / std::max<LONG>(1, desktop.bottom));
                SendInput(1, &move, sizeof(move));
                POINT cursor{};
                GetCursorPos(&cursor);
                const auto hit = WindowFromPoint(cursor);
                if (hit != target && !IsChild(target, hit)) {
                    abortReason = L"cursor-left-target";
                    stopRequested.store(true, std::memory_order_release);
                    break;
                }
                const auto className = [](HWND window) {
                    wchar_t name[128]{};
                    if (window == nullptr) return std::wstring(L"null");
                    GetClassNameW(window, name,
                        static_cast<int>(std::size(name)));
                    return std::wstring(name);
                };
                // Record where this event will land without attaching to the
                // target thread: attaching per event perturbs the focus it is
                // meant to observe.
                const auto routing = className(GetFocus()) + L'/' +
                    className(GetForegroundWindow());
                if (routingCounts.find(routing) == routingCounts.end()) {
                    routingOrder.push_back(routing);
                }
                ++routingCounts[routing];
                INPUT input{};
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                input.mi.mouseData = static_cast<DWORD>(event.delta);
                accepted = SendInput(1, &input, sizeof(input)) == 1U;
            }
            const auto completed = qpcNow();
            handlerUs.push_back(static_cast<double>(completed - injected) *
                1000000.0 / static_cast<double>(frequency));
            if (!accepted) {
                abortReason = L"delivery-failed";
                stopRequested.store(true, std::memory_order_release);
                break;
            }
            ++delivered;
        }
        if (stopRequested.load(std::memory_order_acquire)) break;
    }
    const auto finished = qpcNow();
    SetConsoleCtrlHandler(controlHandler, FALSE);
    const auto maximum = [](const std::vector<double>& values) {
        return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    };
    std::wcout << std::fixed << std::setprecision(3)
        << L"WORKPACK_WHEEL_DRIVER mode=" << fields[0]
        << L" scheduled=" << scheduled
        << L" delivered=" << delivered
        << L" duration_ms=" << static_cast<double>(finished - started) *
            1000.0 / static_cast<double>(frequency)
        << L" lateness_p50_us=" << percentile(latenessUs, 0.50)
        << L" lateness_p95_us=" << percentile(latenessUs, 0.95)
        << L" lateness_max_us=" << maximum(latenessUs)
        << L" handler_p50_us=" << percentile(handlerUs, 0.50)
        << L" handler_p95_us=" << percentile(handlerUs, 0.95)
        << L" handler_max_us=" << maximum(handlerUs)
        << L" abort=" << abortReason;
    for (const auto& routing : routingOrder) {
        std::wcout << L" focus/active=" << routing << L"x"
            << routingCounts[routing];
    }
    std::wcout << L'\n';
    return delivered == scheduled && abortReason == L"none" ? 0 : 7;
}

int dumpPerformanceEvents(DWORD expectedProcessId, std::size_t maximumRecords)
{
    const auto mappingName = std::wstring(
        nekorox::kPerformanceEventMappingPrefixV2) +
        std::to_wstring(expectedProcessId);
    const auto mapping = OpenFileMappingW(
        FILE_MAP_READ, FALSE, mappingName.c_str());
    if (mapping == nullptr) {
        std::wcerr << L"Performance event ring is unavailable\n";
        return 7;
    }
    const auto bytes = sizeof(nekorox::PerformanceEventRingHeaderV2) +
        sizeof(nekorox::PerformanceEventV2) *
            nekorox::kPerformanceEventCapacityV2;
    const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes);
    if (view == nullptr) {
        CloseHandle(mapping);
        std::wcerr << L"Performance event ring could not be mapped\n";
        return 7;
    }
    const auto* header = static_cast<
        const nekorox::PerformanceEventRingHeaderV2*>(view);
    if (header->magic != nekorox::kPerformanceEventMagicV2 ||
        header->version != 2U ||
        header->recordBytes != sizeof(nekorox::PerformanceEventV2) ||
        header->capacity != nekorox::kPerformanceEventCapacityV2 ||
        header->ownerPid != expectedProcessId) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        std::wcerr << L"Performance event ring ABI mismatch\n";
        return 8;
    }
    const auto* records = reinterpret_cast<const nekorox::PerformanceEventV2*>(
        static_cast<const std::byte*>(view) + sizeof(*header));
    const auto latest = header->latestSequence;
    const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(
        latest, nekorox::kPerformanceEventCapacityV2));
    const auto requested = std::min(maximumRecords, available);
    const auto first = latest >= requested ? latest - requested + 1U : 1U;
    std::size_t emitted = 0;
    std::size_t overwritten = 0;
    for (auto sequence = first; sequence <= latest && sequence != 0; ++sequence) {
        const auto record = records[
            sequence % nekorox::kPerformanceEventCapacityV2];
        if (record.sequence != sequence) {
            ++overwritten;
            continue;
        }
        std::wcout << L"EVENT sequence=" << record.sequence
            << L" qpc=" << record.qpc
            << L" tid=" << record.threadId
            << L" kind=" << static_cast<unsigned>(record.kind)
            << L" surface=" << record.surface
            << L" a0=" << record.argument0
            << L" a1=" << record.argument1
            << L" a2=" << record.argument2
            << L" a3=" << record.argument3
            << L" duration_qpc=" << record.durationQpc << L'\n';
        ++emitted;
    }
    std::wcout << L"PERFORMANCE_EVENTS owner_pid=" << header->ownerPid
        << L" latest=" << latest
        << L" emitted=" << emitted
        << L" overwritten=" << overwritten
        << L" qpc_frequency=" << header->qpcFrequency << L'\n';
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 0;
}
