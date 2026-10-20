"""Extract the production motion cfg/query/notification path for a native test.

Only cfg, SDK notifications and read-only SPI queries are doubled. No host,
personal profile, actual settings broadcast, input or OS preference is changed.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def generate(output):
    source = (ROOT / 'src/component/ui_preferences.cpp').read_text(encoding='utf-8-sig')
    functions = []
    for signature in ('MotionMode effectiveMotion', 'void initializeMotionPreferences',
                      'MotionMode motionMode', 'MotionPreference motionPreference',
                      'void setMotionPreference', 'void setMotionMode',
                      'void refreshSystemMotionPreference'):
        found = re.findall(r'^' + signature + r'\([^\n]*\)[^\n]*\n\{[\s\S]*?^\}', source, re.M)
        if len(found) != 1:
            raise ValueError('Production extraction changed: ' + signature)
        functions.extend(found)
    chrome = (ROOT / 'src/component/chrome.cpp').read_text(encoding='utf-8-sig')
    wake = re.findall(r'^void publishRootDisplayEnvironment\([^\n]*\)[^\n]*\n\{[\s\S]*?^\}', chrome, re.M)
    if len(wake) != 1:
        raise ValueError('Monitor wake extraction changed')
    listen = (ROOT / 'src/component/listen_window.cpp').read_text(encoding='utf-8-sig')
    listen_wake = re.findall(r'^void ListenWindow::refreshMonitorIdentity\([^\n]*\)[^\n]*\n\{[\s\S]*?^\}', listen, re.M)
    if len(listen_wake) != 1:
        raise ValueError('Listen monitor wake extraction changed')
    position = listen.split('case WM_WINDOWPOSCHANGED:', 1)[1].split('break;', 1)[0]
    if position.index('refreshMonitorIdentity(window);') > position.index('updateVisibilityLifecycle('):
        raise ValueError('Listen clock observes monitor change too late')
    for event in ('case WM_CREATE:', 'case WM_DESTROY:'):
        if 'monitorIdentity_.reset();' not in listen.split(event, 1)[1].split('return', 1)[0]:
            raise ValueError('Listen monitor identity outlives HWND')
    display = (ROOT / 'src/component/display_environment.cpp').read_text(encoding='utf-8-sig')
    setting = display.split('case WM_SETTINGCHANGE:', 1)[1].split('break;', 1)[0]
    if 'refreshSystemMotionPreference();' not in setting or 'MonitorTextRendering::settingsChanged();' not in setting:
        raise ValueError('System preference event is no longer wired')
    if 'static_cast<unsigned>(MotionPreference::followWindows)' not in source.split('cfg_uint g_motionMode(', 1)[1].split(';', 1)[0]:
        raise ValueError('Fresh profile no longer follows Windows')
    # Integration guard: updating the target alone is insufficient if a scene
    # still reuses text rasterized with old parameters. Native build separately
    # checks these call sites; WIC below validates the shared raster policy.
    for name, begin, end, invalidation in (
        ('panel_d2d.cpp', 'bool PanelD2D::ensure(HWND', 'bool PanelD2D::ensureBackend', 'partialBackingValid_ = false;'),
        ('workspace_panel.cpp', 'bool ensureDeviceResources()', 'bool ensureDeviceBackendResources()', 'libraryStaticCacheValid_ = false;'),
        ('listen_visual_renderer.cpp', 'bool ListenVisualRenderer::ensureResources(', 'bool ListenVisualRenderer::createCompositionResources(', 'liveStaticBitmap_.Reset();'),
    ):
        renderer = (ROOT / 'src/component' / name).read_text(encoding='utf-8-sig')
        section = renderer.split(begin, 1)[1].split(end, 1)[0]
        if not re.search(r'if \(textRendering_\.apply\([\s\S]*?\)\) \{\s*[\s\S]*?' + re.escape(invalidation), section):
            raise ValueError('Retained text no longer invalidated: ' + name)
    prefix = r'''
namespace motion_fixture {
unsigned g_motionMode = 0, g_listenEffectsMode = 0, notifications = 0, nativeNotifications = 0, refreshes = 0, queries = 0;
bool animations = true, querySuccess = true;
std::atomic<MotionMode> g_runtimeMotionMode{MotionMode::normal};
std::atomic<MotionPreference> g_runtimeMotionPreference{MotionPreference::followWindows};
enum class ListenEffectsMode { softLight, noEffects };
enum class ListenEffectsFallback { none, disabled };
std::atomic<ListenEffectsMode> g_runtimeListenEffectsMode{ListenEffectsMode::softLight};
std::atomic<ListenEffectsFallback> g_listenEffectsFallback{ListenEffectsFallback::none};
ListenEffectsMode sanitizedListenEffects(unsigned value) { return value == 1 ? ListenEffectsMode::noEffects : ListenEffectsMode::softLight; }
void notifyWorkspacePlaybackChanged() { ++notifications; }
void notifyNativePanelsPlaybackChanged() { ++nativeNotifications; }
struct ListenWindow {
    static ListenWindow& instance() { static ListenWindow value; return value; }
    void requestRefresh() { ++refreshes; }
    WindowMonitorIdentity monitorIdentity_;
    unsigned rendererChanges = 0, revisions = 0, frames = 0;
    struct Renderer { unsigned& changes; void displayEnvironmentChanged() { ++changes; } } renderer_{rendererChanges};
    struct Revision { unsigned& changes; void invalidate() { ++changes; } } presentationRevision_{revisions};
    struct Clock { unsigned& changes; void requestImmediate() { ++changes; } } frameClock_{frames};
    void refreshMonitorIdentity(HWND window) noexcept;
};
BOOL WINAPI queryMotion(UINT action, UINT, PVOID value, UINT) {
    require(action == SPI_GETCLIENTAREAANIMATION, "Motion path made unexpected SPI query");
    ++queries; *static_cast<BOOL*>(value) = animations; return querySuccess;
}
HMONITOR g_rootDisplayMonitor = nullptr, fixtureMonitor = nullptr;
unsigned childNotifications = 0;
HMONITOR queryMonitor(HWND, DWORD) { return fixtureMonitor; }
BOOL CALLBACK notifyDisplayEnvironmentChild(HWND, LPARAM) { ++childNotifications; return TRUE; }
UINT fixtureDpi = 96;
UINT queryDpi(HWND) { return fixtureDpi; }
unsigned listenInvalidations = 0;
BOOL invalidateListen(HWND, const RECT*, BOOL) { ++listenInvalidations; return TRUE; }
#define MonitorFromWindow queryMonitor
#define GetDpiForWindow queryDpi
#define InvalidateRect invalidateListen
#define SystemParametersInfoW queryMotion
'''
    tests = r'''
#undef SystemParametersInfoW
#undef MonitorFromWindow
#undef GetDpiForWindow
#undef InvalidateRect
void run() {
    static_assert(static_cast<unsigned>(MotionPreference::normal) == 0);
    static_assert(static_cast<unsigned>(MotionPreference::reduced) == 1);
    static_assert(static_cast<unsigned>(MotionPreference::off) == 2);
    static_assert(static_cast<unsigned>(MotionPreference::followWindows) == 3);
    for (unsigned stored = 0; stored < 5; ++stored)
        for (bool enabled : {false, true}) for (bool success : {false, true}) {
            g_motionMode = stored; animations = enabled; querySuccess = success;
            const auto before = notifications + nativeNotifications + refreshes;
            initializeMotionPreferences();
            require(g_motionMode == stored && notifications + nativeNotifications + refreshes == before,
                "Rehydration rewrote saved cfg or opened/refreshed windows");
            const auto preference = sanitizeMotionPreference(stored);
            const auto expected = stored < 3 ? static_cast<MotionMode>(stored) :
                (success && !enabled ? MotionMode::reduced : MotionMode::normal);
            require(motionPreference() == preference && motionMode() == expected,
                "Rehydrated mode is incorrect");
            const auto nativeBefore = nativeNotifications;
            refreshSystemMotionPreference();
            require(g_motionMode == stored && motionMode() == expected,
                "OS event changed a stored choice");
            require(nativeNotifications == nativeBefore + 1,
                "OS event did not notify native panels");
        }
    for (unsigned stored = 0; stored < 4; ++stored) {
        animations = false; querySuccess = true;
        const auto nativeBefore = nativeNotifications;
        setMotionPreference(static_cast<MotionPreference>(stored));
        require(nativeNotifications == nativeBefore + 1,
            "Motion setter did not notify native panels");
        require(g_motionMode == stored && static_cast<unsigned>(motionPreference()) == stored,
            "Setter stored the effective rather than selected preference");
        animations = true; refreshSystemMotionPreference();
        require(g_motionMode == stored && motionMode() == (stored == 3 ? MotionMode::normal : static_cast<MotionMode>(stored)),
            "Re-enable animations lost explicit selection");
    }
    const auto before = queries;
    setMotionMode(MotionMode::off); refreshSystemMotionPreference();
    require(queries == before && motionMode() == MotionMode::off, "Explicit mode queried or followed OS");
    const auto parent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Own hidden monitor fixture",
        WS_POPUP, 0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(parent != nullptr, "Hidden root fixture failed");
    const auto child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
        0, 0, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(child != nullptr && !IsWindowVisible(parent), "Hidden child fixture failed");
    fixtureMonitor = reinterpret_cast<HMONITOR>(1);
    auto nativeBefore = nativeNotifications;
    publishRootDisplayEnvironment(parent);
    require(nativeNotifications == nativeBefore + 1 && childNotifications == 1,
        "Monitor move did not wake paused native surfaces");
    for (unsigned index = 0; index < 10000; ++index) publishRootDisplayEnvironment(parent);
    require(nativeNotifications == nativeBefore + 1 && childNotifications == 1,
        "Same-monitor drag repeatedly invalidated native surfaces");
    fixtureMonitor = reinterpret_cast<HMONITOR>(2);
    publishRootDisplayEnvironment(parent);
    require(nativeNotifications == nativeBefore + 2 && childNotifications == 2,
        "Monitor change with unchanged DPI did not wake surfaces");
    publishRootDisplayEnvironment(parent, true);
    require(nativeNotifications == nativeBefore + 3 && childNotifications == 3,
        "Forced display refresh did not wake surfaces");
    ListenWindow listen;
    listen.monitorIdentity_.update(fixtureMonitor, fixtureDpi);
    for (unsigned index = 0; index < 10000; ++index) listen.refreshMonitorIdentity(parent);
    require(listen.frames == 0 && listenInvalidations == 0, "Listen woke on same-monitor drag");
    fixtureMonitor = reinterpret_cast<HMONITOR>(3);
    g_rootDisplayMonitor = fixtureMonitor; // shared WinEvent identity already updated
    listen.refreshMonitorIdentity(parent);
    require(listen.frames == 1 && listen.revisions == 1 && listen.rendererChanges == 1 &&
        listenInvalidations == 1, "Paused Listen lost same-DPI monitor wake to shared cache");
    listen.refreshMonitorIdentity(parent);
    require(listen.frames == 1, "Listen repeated unchanged monitor wake");
    fixtureDpi = 192;
    listen.refreshMonitorIdentity(parent);
    require(listen.frames == 2, "Listen missed DPI identity change");
    listen.monitorIdentity_.reset();
    listen.refreshMonitorIdentity(parent);
    require(listen.frames == 3, "Recreated Listen HWND reused stale identity");
    DestroyWindow(parent);
}
}
'''
    output.mkdir(parents=True, exist_ok=True)
    (output / 'motion-production.inc').write_text(prefix + '\n\n'.join(functions + wake + listen_wake) + tests, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    generate(parser.parse_args().output)
