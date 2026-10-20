#include "../src/component/frame_notification.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using nekorox::component::FrameNotification;
constexpr UINT notice = WM_APP + 17;
constexpr UINT stopNotice = WM_APP + 18;
struct State {
    FrameNotification* notification = nullptr;
    unsigned frames = 0;
    WPARAM generation = 0;
    bool cycling = false, posted = false, stopped = false;
    std::atomic<ULONGLONG> sent{0};
    ULONGLONG received = 0;
    unsigned paints = 0;
    RECT paintedRegion{};
};
LRESULT CALLBACK targetProc(HWND window, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    const auto state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state && msg == notice) {
        ++state->frames; state->generation = w;
        if (state->cycling && !state->stopped) {
            if (state->posted) { Sleep(2); PostMessageW(window, notice, w + 1, 0); }
            else state->notification->request(w + 1);
        }
        return 0;
    }
    if (state && msg == stopNotice) {state->stopped = true; return 0;}
    if (state && msg == WM_KEYDOWN && w == VK_F24) {state->received = GetTickCount64(); return 0;}
    if (state && msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        ++state->paints;
        state->paintedRegion = paint.rcPaint;
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, msg, w, l);
}
void pump()
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {TranslateMessage(&msg); DispatchMessageW(&msg);}
}
void check(bool value, const char* text) {if (!value) throw std::runtime_error(text);}
int wmain(int argc, wchar_t** argv)
{
    try {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = targetProc;
        wc.lpszClassName = L"GodenFrameNotificationTestV1";
        check(RegisterClassW(&wc) != 0, "register target");
        State state;
        const auto window = CreateWindowExW(0, wc.lpszClassName, L"Frame/input qualification",
            WS_OVERLAPPEDWINDOW, 80, 80, 380, 140, nullptr, nullptr, wc.hInstance, &state);
        check(window != nullptr, "create target");
        ShowWindow(window, SW_SHOWNOACTIVATE);
        FrameNotification notification;
        state.notification = &notification;
        check(notification.attach(window, notice), "attach paint notification");
        check(notification.request(7) && notification.request(8), "request coalesced notices");
        check(state.frames == 0, "producer synchronously dispatched a frame");
        pump(); check(state.frames == 1 && state.generation == 8, "internal paint failed/coalescing failed");
        check(notification.request(9), "request canceled notice"); notification.cancel();
        pump(); check(state.frames == 1, "canceled notice reached target");
        std::thread producer([&] {for (unsigned i = 0; i < 100; ++i) check(notification.request(100 + i), "worker request");});
        producer.join(); pump(); check(state.frames == 2 && state.generation == 199, "worker notification failed");
        check(notification.request(200), "request before detach"); notification.reset();
        pump(); check(state.frames == 2, "detached notice reached target");
        check(notification.attach(window, notice), "reattach");
        const RECT liveRegion{40, 20, 80, 60};
        notification.setDamageProvider([](HWND, void* context, RECT& region) noexcept {
            region = *static_cast<const RECT*>(context);
            return true;
        }, const_cast<RECT*>(&liveRegion));
        const auto beforePartial = state.paints;
        check(notification.request(201), "request retained live region");
        pump();
        check(state.paints == beforePartial + 1 && EqualRect(&state.paintedRegion, &liveRegion),
            "live scope tick repainted the complete header");
        RECT client{}; GetClientRect(window, &client);
        InvalidateRect(window, nullptr, FALSE);
        check(notification.request(202), "request with full semantic damage");
        pump();
        check(EqualRect(&state.paintedRegion, &client), "live-region tick lost full semantic damage");
        notification.setDamageProvider(nullptr, nullptr);
        std::cout << "FRAME_NOTIFICATION lifecycle/coalescing/cross-thread/internal-paint PASS\n";
        if (argc > 1 && std::wstring_view(argv[1]) == L"--physical") {
            ShowWindow(window, SW_SHOW);
            const auto foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            const auto attached = foregroundThread != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
            SetForegroundWindow(window); SetFocus(window);
            if (attached) AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
            pump();
            check(GetForegroundWindow() == window, "test window is not foreground");
            for (const auto posted : {true, false}) {
                state.frames = 0; state.received = 0; state.sent = 0; state.stopped = false;
                state.cycling = true; state.posted = posted;
                std::thread input([&] {
                    Sleep(60);
                    if (GetForegroundWindow() == window) {
                        INPUT key{}; key.type = INPUT_KEYBOARD; key.ki.wVk = VK_F24;
                        auto up = key; up.ki.dwFlags = KEYEVENTF_KEYUP;
                        INPUT keys[]{key, up}; state.sent.store(GetTickCount64());
                        if (SendInput(2, keys, sizeof(INPUT)) != 2) state.sent.store(0);
                    }
                    Sleep(550); PostMessageW(window, stopNotice, 0, 0);
                });
                if (posted) PostMessageW(window, notice, 1, 0); else notification.request(1);
                MSG msg{};
                while (!state.stopped && GetMessageW(&msg, nullptr, 0, 0) > 0) {TranslateMessage(&msg); DispatchMessageW(&msg);}
                state.cycling = false; notification.cancel(); input.join(); pump();
                check(state.sent != 0 && state.received >= state.sent, "hardware input was not delivered");
                const auto latency = state.received - state.sent.load();
                std::cout << "INPUT_PRIORITY " << (posted ? "posted-baseline" : "paint-candidate")
                    << " latency_ms=" << latency << " frames=" << state.frames << '\n';
                check(posted ? latency >= 400 : latency < 100, "input priority contract failed");
            }
        }
        notification.reset(); DestroyWindow(window); pump();
        return 0;
    } catch (const std::exception& e) {std::cerr << "FAIL " << e.what() << '\n'; return 1;}
}
