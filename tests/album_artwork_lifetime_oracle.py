"""Extract the real shell lifecycle and two-worker artwork broker.

The broker, queue, epochs, cancellation and completion code run unchanged.
Only the SDK image loader and unrelated HWND/render operations are host seams.
Blocked loaders reproduce abandoned main/variant work without timing a decoder.
Native artwork/restore qualification is separate; this is not a CPU benchmark.
"""
from pathlib import Path
import hashlib
import json
import sys

repo = Path(__file__).resolve().parents[1]
out, shell_path, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
assert mode in ('baseline', 'candidate')
out.mkdir(parents=True, exist_ok=False)
shell = shell_path.read_text(encoding='utf-8-sig')
art = (repo/'src/component/art_cache.cpp').read_text(encoding='utf-8-sig')

def block(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

candidate = mode == 'candidate'
assert ('void setAlbumArtworkActive(bool active)' in shell) == candidate
broker = block(art, 'class ArtworkBroker final') + ';'
broker = broker.replace(block(art, '    ArtworkBrokerDiagnostics diagnostics() const noexcept'), '')
broker = broker.replace('private:', 'public:')  # Inspect epochs under its real mutex.
state = block(art, 'struct ArtCache::State') + ';'
constants = art[art.index('constexpr unsigned kMaximumTransientAttempts'):art.index('constexpr std::size_t kMaximumEncodedBytes')]
wrappers = '\n'.join(block(art, sig) for sig in (
    'ArtCache::ArtCache(UINT refreshMessage)', 'ArtCache::~ArtCache()',
    'void ArtCache::attach(HWND window)', 'void ArtCache::detach()'))
mode_body = block(shell, '    bool setMode(PlayerShellMode mode) noexcept')
helper = block(shell, '    void setAlbumArtworkActive(bool active)') if candidate else ''
show = shell[shell.index('        case WM_SHOWWINDOW:'):shell.index('        case WM_SETFOCUS:', shell.index('        case WM_SHOWWINDOW:'))]
root = block(shell, '        if (message == rootPresentationVisibilityMessage())')
paint = ''
if candidate:
    begin = shell.index('        const bool albumPresentationVisible =')
    paint = shell[begin:shell.index('        a4::trace("album.paint.begin"', begin)]
    assert 'if (!albumArtworkActive_) return {};' in block(shell, '    std::shared_ptr<const ArtPixels> albumArtwork(')
    assert '!albumArtworkActive_' in block(shell, '    void syncAlbumThemeIntent(bool resolveArtwork)')
    assert 'artCache_.attach(window_)' not in shell[shell.index('case WM_CREATE:'):shell.index('case WM_DESTROY:')]
    assert 'setAlbumArtworkActive(false)' in shell[shell.index('case WM_DESTROY:'):shell.index('case WM_SIZE:')]
policy = (repo/'src/component/artwork_broker_policy.h').read_bytes()
(out/'artwork_broker_policy.h').write_bytes(policy)

cpp = r'''
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "artwork_broker_policy.h"
using namespace nekorox::component;
using namespace std::chrono_literals;
constexpr bool candidate = __CANDIDATE__;
int checks = 0;
void check(bool ok, const char* why) { ++checks; if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
struct exception_aborted {};
using abort_callback_event = HANDLE;
struct abort_callback {
    virtual ~abort_callback() = default;
    virtual bool is_aborting() const = 0;
    virtual abort_callback_event get_abort_event() const = 0;
    void check() const { if (is_aborting()) throw exception_aborted{}; }
};
struct abort_callback_impl : abort_callback {
    std::atomic<bool> stopped{false}; HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~abort_callback_impl() { CloseHandle(event); }
    void abort() { stopped.store(true); SetEvent(event); }
    bool is_aborting() const override { return stopped.load(); }
    HANDLE get_abort_event() const override { return event; }
};
struct ArtworkBudgetBusy {};
bool qaArtworkBusyOnce() { return false; }
using TextLess = std::less<>;
struct Material { std::uint64_t contentRevision = 1; };
struct ArtPixels { std::uint64_t contentHash = 0; std::uint32_t width = 8, height = 8; std::vector<unsigned char> bgra = std::vector<unsigned char>(256); std::shared_ptr<Material> material; };
std::size_t residentPixelBytes(const ArtPixels& p) { return p.bgra.size(); }
enum class ArtCacheStatus { absent, loading, ready, missing };
enum class ArtRequestPriority : std::uint8_t { visibleMain, visibleRow, prefetch };
struct ArtCache {
    struct State; std::shared_ptr<State> state_;
    explicit ArtCache(UINT); ~ArtCache(); void attach(HWND); void detach();
    void retainPrefetch(const std::vector<std::wstring>& keys, unsigned);
};
__STATE__
__CONSTANTS__
__BROKER__
__WRAPPERS__
void ArtCache::retainPrefetch(const std::vector<std::wstring>& keys, unsigned) { ArtworkBroker::instance().retainPrefetch(state_, keys); }
enum class PlayerShellMode { normal, album };
std::atomic<PlayerShellMode> g_playerShellMode{PlayerShellMode::normal};
enum class TelemetrySurface { album };
struct TelemetrySpan { TelemetrySpan(TelemetrySurface, unsigned, unsigned) {} };
struct DisplayEnvironmentService {
    bool visible = true;
    static auto& instance() { static DisplayEnvironmentService x; return x; }
    bool presentationVisible(HWND) { return visible; }
};
constexpr UINT kAlbumLyricsWakeTimer = 10;
constexpr UINT rootPresentationVisibilityMessage() { return WM_APP + 10; }
struct Renderer { void suspendTextPreparation() {} void suspendCurated() {} void resize() {} int bindingKey() { return 0; } };
struct Controller { void shutdown() {} };
struct Clock { void stop() {} void setReadinessBinding(int) {} bool requestImmediate() { return true; } };
struct Scroll { void setPosition(double) {} };
struct Booklet { void closeImmediately() {} };
struct Child { bool is_valid() const { return false; } Child* operator->() { return this; } HWND get_wnd() { return nullptr; } };
struct Shell {
    HWND window_ = reinterpret_cast<HWND>(0x123);
    PlayerShellMode mode_ = PlayerShellMode::normal;
    ArtCache artCache_{WM_APP+11}; bool albumArtworkActive_ = false;
    Renderer albumRenderer_; Controller ambientController_; Clock frameClock_, ambientPumpClock_;
    Booklet booklet_; Scroll trackScroll_; Child workspace_; std::optional<int> trackPress_;
    bool a4RetryArmed_ = false, lyricsTimerArmed_ = false;
    unsigned long long preparationRetryQpc_ = 0, lyricsTimerDeadlineMs_ = 0, trackScrollSessionId_ = 0, artworkPrefetchSessionId_ = 0;
    unsigned artworkPrefetchEdge_ = 0; std::size_t artworkPrefetchCenter_ = -1; float trackScrollDip_ = 0;
    bool completeTopology() { return true; } void syncVisibility() {} void layoutChildren() {} void invalidatePresentation() {}
    void syncAlbumThemeIntent(bool) { if (candidate && DisplayEnvironmentService::instance().visible) check(artCache_.state_->window == window_, "Attach precedes synchronous theme resolution"); }
    Shell() { if (!candidate) artCache_.attach(window_); }
    __HELPER__
    __MODE__
    void onShow(bool shown) { WPARAM wParam = shown; switch(WM_SHOWWINDOW) { __SHOW__ } }
    int onRoot(bool shown) { UINT message = rootPresentationVisibilityMessage(); WPARAM wParam = shown; __ROOT__ return -1; }
    void paint() { __PAINT__ }
};
struct Gate {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Gate() {
        SetEvent(release);
        if (WaitForSingleObject(entered, 0) == WAIT_OBJECT_0 &&
            WaitForSingleObject(finished, 5000) != WAIT_OBJECT_0) std::exit(4);
        CloseHandle(entered); CloseHandle(release); CloseHandle(finished);
    }
    void wait() { check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "Real worker entered controlled loader"); }
    void open() { SetEvent(release); }
    std::shared_ptr<ArtPixels> load(abort_callback&, unsigned hash) {
        SetEvent(entered); if (WaitForSingleObject(release, 5000) != WAIT_OBJECT_0) std::exit(3);
        auto pixels = std::make_shared<ArtPixels>(); pixels->contentHash = hash;
        SetEvent(finished); return pixels;
    }
};
using Token = std::pair<std::uint64_t, std::shared_ptr<abort_callback_impl>>;
Token token(const std::wstring& key) { auto& b = ArtworkBroker::instance(); std::lock_guard lock(b.mutex_); auto& e = b.entries_.at(key); return {e.generation, e.cancellation}; }
auto request(Shell& s, const std::wstring& key, Gate& gate, unsigned hash, ArtRequestPriority p = ArtRequestPriority::visibleMain) {
    return ArtworkBroker::instance().request(key, s.artCache_.state_, [&gate, hash](abort_callback& a) { return gate.load(a, hash); }, p);
}
void ready(const std::wstring& key) {
    const auto deadline = std::chrono::steady_clock::now()+5s;
    while (ArtworkBroker::instance().status(key) != ArtCacheStatus::ready && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    check(ArtworkBroker::instance().status(key) == ArtCacheStatus::ready, "Artwork completes after controlled release");
}
int main() {
    auto& b = ArtworkBroker::instance();
    {
        Shell s; check(s.setMode(PlayerShellMode::album), "Enter Album");
        Gate master, variant, prefetch;
        request(s,L"exclusive-master",master,11); request(s,L"exclusive-variant",variant,12);
        request(s,L"exclusive-neighbour",prefetch,13,ArtRequestPriority::prefetch);
        master.wait(); variant.wait();
        const auto m=token(L"exclusive-master"), v=token(L"exclusive-variant"), n=token(L"exclusive-neighbour");
        check(s.setMode(PlayerShellMode::normal),"Leave Album");
        check(b.wanted(L"exclusive-master",m.first)==!candidate,"Only candidate revokes obsolete main cover");
        check(b.wanted(L"exclusive-variant",v.first)==!candidate,"Only candidate revokes obsolete variant");
        check(m.second->is_aborting()==candidate && v.second->is_aborting()==candidate,"Main/variant abort events reflect owner exit");
        check(!b.wanted(L"exclusive-neighbour",n.first) && n.second->is_aborting(),"Neighbour revoked in both versions");
        if(candidate) {
            s.setMode(PlayerShellMode::album); Gate fresh;
            request(s,L"exclusive-master",fresh,99);
            auto current=token(L"exclusive-master");
            check(current.first!=m.first && current.second!=m.second,"Rapid reentry obtains independent generation and cancellation event");
            master.open(); variant.open(); prefetch.open(); fresh.wait();
            // Occupy the second real worker: both old completions must have
            // returned before it can enter this sentinel loader.
            Gate sentinel; request(s,L"completion-sentinel",sentinel,102); sentinel.wait();
            check(b.status(L"exclusive-master") == ArtCacheStatus::loading,"Old completion cannot publish over new request");
            fresh.open(); sentinel.open(); ready(L"exclusive-master"); ready(L"completion-sentinel");
            auto resident=request(s,L"exclusive-master",fresh,100);
            check(resident && resident->contentHash==99,"New request publishes correct pixels");
            s.setMode(PlayerShellMode::normal); s.setMode(PlayerShellMode::album);
            check(request(s,L"exclusive-master",fresh,101)==resident,"Ready cache reused across exit/reentry without decode");
        } else { master.open(); variant.open(); prefetch.open(); ready(L"exclusive-master"); ready(L"exclusive-variant"); }
    }
    {
        Shell s, other; s.setMode(PlayerShellMode::album); other.setMode(PlayerShellMode::album);
        Gate shared; request(s,L"shared-master",shared,23); shared.wait();
        request(other,L"shared-master",shared,23,ArtRequestPriority::visibleRow);
        auto t=token(L"shared-master"); s.setMode(PlayerShellMode::normal);
        check(!t.second->is_aborting() && b.wanted(L"shared-master",t.first),"Other panel keeps shared decode alive");
        shared.open(); ready(L"shared-master");
        check(request(other,L"shared-master",shared,24)->contentHash==23,"Other owner receives unchanged pixels");
    }
    if(candidate) {
        Shell s; s.setMode(PlayerShellMode::album); auto& display=DisplayEnvironmentService::instance();
        const auto epoch=s.artCache_.state_->ownerEpoch.load(); s.paint(); s.paint(); s.onShow(true); s.paint();
        check(s.artCache_.state_->ownerEpoch==epoch,"Repeated visible paints/show do not renew owner epoch");
        for(int cycle=0;cycle<3;++cycle) {
            Gate hidden; auto key=L"hidden-main"+std::to_wstring(cycle);
            request(s,key,hidden,100+cycle); hidden.wait(); auto t=token(key);
            s.artworkPrefetchSessionId_=34; s.artworkPrefetchEdge_=700; s.artworkPrefetchCenter_=7;
            display.visible=false;
            if(cycle==0) s.onShow(false); else s.onRoot(false);
            check(t.second->is_aborting() && !b.wanted(key,t.first),"Hide/minimize suspends actual loading work");
            const auto detached=s.artCache_.state_->ownerEpoch.load();
            s.onRoot(true); s.onShow(true); s.paint();
            check(s.artCache_.state_->ownerEpoch==detached && !s.albumArtworkActive_,"Delayed show/restore cannot attach while still hidden");
            hidden.open(); display.visible=true; s.onShow(true); s.paint();
            check(s.albumArtworkActive_ && s.artCache_.state_->window==s.window_,"Paint resumes owner without playback events (paused restore)");
            check(s.artworkPrefetchSessionId_==0 && s.artworkPrefetchEdge_==0 && s.artworkPrefetchCenter_==std::size_t(-1),"Resume resets all neighbour request guards");
            Gate resumed; request(s,key,resumed,200+cycle); resumed.wait(); resumed.open(); ready(key);
            check(request(s,key,resumed,0)->contentHash==200+cycle,"Restored owner reacquires and receives artwork");
        }
        s.setMode(PlayerShellMode::normal); auto detached=s.artCache_.state_->ownerEpoch.load();
        s.onRoot(true); s.onShow(true); s.paint();
        check(!s.albumArtworkActive_ && s.artCache_.state_->ownerEpoch==detached,"Late visibility notifications do not attach Library owner");
        display.visible=false; s.setMode(PlayerShellMode::album);
        check(!s.albumArtworkActive_,"Mode change while hidden does not acquire artwork");
        display.visible=true; s.onRoot(false);
        check(s.albumArtworkActive_,"Stale hide notification reconciles current visible state");
    }
    std::cout << "{\"status\":\"PASS\",\"mode\":\"" << (candidate?"candidate":"baseline") << "\",\"checks\":" << checks << "}\n";
}
'''
replacements = dict(CANDIDATE='true' if candidate else 'false', STATE=state, CONSTANTS=constants,
                    BROKER=broker, WRAPPERS=wrappers, HELPER=helper, MODE=mode_body,
                    SHOW=show, ROOT=root, PAINT=paint)
for name, value in replacements.items():
    cpp = cpp.replace('__'+name+'__', value)
(out/'oracle.cpp').write_text(cpp, encoding='utf-8')
(out/'source.json').write_text(json.dumps(dict(mode=mode, shellSha256=hashlib.sha256(shell_path.read_bytes()).hexdigest(),
    artSha256=hashlib.sha256((repo/'src/component/art_cache.cpp').read_bytes()).hexdigest(),
    policySha256=hashlib.sha256(policy).hexdigest(),
    extractedHashes={key:hashlib.sha256(value.encode()).hexdigest() for key,value in replacements.items()},
    seams=['SDK image loader replaced by bounded event gates', 'Unrelated shell render/HWND/layout operations stubbed',
           'Broker diagnostic-only export omitted; private inspection made public; real two workers/queue/epochs/completion retained']),indent=2)+'\n')
print(out/'oracle.cpp')
