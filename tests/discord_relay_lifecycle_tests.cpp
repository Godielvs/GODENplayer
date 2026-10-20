// Standalone, fail-closed WinHTTP transport fixture. Includes the complete
// production relay; no winhttp.lib, real endpoint or artwork is used.
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace allocation_fault {
thread_local bool relayThread = false;
std::atomic<bool> enabled{false};
std::atomic<unsigned> failures{0};
std::condition_variable* changed = nullptr;
}
void* operator new(std::size_t size)
{
    if (allocation_fault::relayThread && allocation_fault::enabled.load()) {
        ++allocation_fault::failures;
        if (allocation_fault::changed) allocation_fault::changed->notify_all();
        throw std::bad_alloc{};
    }
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
BOOL WINAPI fake_SetThreadPriority(HANDLE thread, int priority)
{
    allocation_fault::relayThread = true;
    return ::SetThreadPriority(thread, priority);
}
#define SetThreadPriority fake_SetThreadPriority

#define DECLARE_FAKE(name) decltype(name) fake_##name
DECLARE_FAKE(WinHttpOpen);
DECLARE_FAKE(WinHttpConnect);
DECLARE_FAKE(WinHttpOpenRequest);
DECLARE_FAKE(WinHttpCrackUrl);
DECLARE_FAKE(WinHttpSetTimeouts);
DECLARE_FAKE(WinHttpSetOption);
DECLARE_FAKE(WinHttpAddRequestHeaders);
DECLARE_FAKE(WinHttpSetStatusCallback);
DECLARE_FAKE(WinHttpSendRequest);
DECLARE_FAKE(WinHttpReceiveResponse);
DECLARE_FAKE(WinHttpQueryHeaders);
DECLARE_FAKE(WinHttpReadData);
DECLARE_FAKE(WinHttpCloseHandle);
DECLARE_FAKE(WinHttpWebSocketCompleteUpgrade);
DECLARE_FAKE(WinHttpWebSocketReceive);
DECLARE_FAKE(WinHttpWebSocketSend);
DECLARE_FAKE(GetEnvironmentVariableW);
#undef DECLARE_FAKE
#define WinHttpOpen fake_WinHttpOpen
#define WinHttpConnect fake_WinHttpConnect
#define WinHttpOpenRequest fake_WinHttpOpenRequest
#define WinHttpCrackUrl fake_WinHttpCrackUrl
#define WinHttpSetTimeouts fake_WinHttpSetTimeouts
#define WinHttpSetOption fake_WinHttpSetOption
#define WinHttpAddRequestHeaders fake_WinHttpAddRequestHeaders
#define WinHttpSetStatusCallback fake_WinHttpSetStatusCallback
#define WinHttpSendRequest fake_WinHttpSendRequest
#define WinHttpReceiveResponse fake_WinHttpReceiveResponse
#define WinHttpQueryHeaders fake_WinHttpQueryHeaders
#define WinHttpReadData fake_WinHttpReadData
#define WinHttpCloseHandle fake_WinHttpCloseHandle
#define WinHttpWebSocketCompleteUpgrade fake_WinHttpWebSocketCompleteUpgrade
#define WinHttpWebSocketReceive fake_WinHttpWebSocketReceive
#define WinHttpWebSocketSend fake_WinHttpWebSocketSend
#define GetEnvironmentVariableW fake_GetEnvironmentVariableW
#include "../src/component/discord_relay_client.cpp"

namespace fixture {
using namespace std::chrono_literals;
using Client = nekorox::component::DiscordRelayClient;
using Status = nekorox::component::DiscordRelayStatus;
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::fflush(stderr); std::abort(); }
}
enum class Kind { session, connection, request, socket };
enum class Pending { none, sendHeaders, httpRead, socketRead, socketWrite };
struct Handle {
    Kind kind{};
    DWORD accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
    Handle* parent = nullptr;
    std::thread::id owner{};
    WINHTTP_STATUS_CALLBACK callback = nullptr;
    DWORD_PTR context = 0;
    DWORD callbackFlags = 0;
    bool get = false;
    bool redirectsDisabled = false;
    bool authorized = false;
    bool closing = false;
    bool closed = false;
    unsigned issuing = 0;
    unsigned closes = 0;
    Pending pending{};
    void* buffer = nullptr;
    DWORD capacity = 0;
    std::size_t offset = 0;
    DWORD coverStatus = 200;
    bool coverLengthKnown = true;
    std::optional<DWORD> coverDeclaredLength;
    std::optional<std::size_t> coverReadFailureAt;
    std::string coverBody;
    std::string requestBody;
    std::wstring host,path;
};
std::mutex mutex;
std::condition_variable changed;
std::vector<std::unique_ptr<Handle>> handles;
std::thread::id ui = std::this_thread::get_id();
constexpr std::string_view credential = "qa-fixture-credential-01234567890123456789";
bool holdClose = false, holdSend = false, holdHttpRead = false;
bool holdUpgradeClose = false;
bool coverTransport = false, holdHttpSend = false;
bool coverQaOverride = true, holdCoverGet = false;
bool requireCoverGetProxy = false;
bool requireCoverReadProxy = false;
unsigned coverUploads = 0, coverGets = 0;
struct CoverReply {
    DWORD status; std::string body; bool lengthKnown = true;
    std::optional<DWORD> declaredLength;
    std::optional<std::size_t> readFailureAt;
    std::wstring host;
};
std::string validCoverPng() {
    // Generated 1x1 RGB fixture, including valid chunk CRCs and terminal IEND.
    constexpr unsigned char png[]{137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,
        0,0,0,1,0,0,0,1,8,2,0,0,0,144,119,83,222,0,0,0,12,73,68,65,84,120,156,
        99,96,104,112,0,0,1,68,0,193,17,110,151,59,0,0,0,0,73,69,78,68,174,66,96,130};
    return {reinterpret_cast<const char*>(png),sizeof(png)};
}
std::deque<CoverReply> coverReplies;
bool failRequestCallback = false, failSocketContext = false, holdSocketClose = false;
std::size_t readChunk = 4096;
std::string coverResponse = "http://127.0.0.1:1/test.png\n";
bool blockSendApi = false, releaseSendApi = false, enteredSendApi = false;
unsigned opens = 0, sends = 0;
std::string sessionResponse()
{
    const auto expires = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch() + std::chrono::hours(12)).count();
    return "{\"sessionId\":\"0123456789abcdef0123456789abcdef\","
    "\"secret\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
    "\"socketUrl\":\"ws://127.0.0.1:1/v1/sessions/0123456789abcdef0123456789abcdef/socket\","
    "\"coverBaseUrl\":\"http://127.0.0.1:1/v1/c/0123456789abcdef0123456789abcdef\",\"expiresAt\":" +
        std::to_string(expires) + "}";
}
std::string response = sessionResponse();
Handle* cast(HINTERNET value) { check(value != nullptr, "null handle"); return static_cast<Handle*>(value); }
void checkOwner(Handle* h)
{
    check(h->owner == std::this_thread::get_id() && h->owner != ui, "WinHTTP issued from wrong thread/UI");
    check(!h->closing && !h->closed, "API reused a closing/closed handle");
}
Handle* make(Kind kind, Handle* parent = nullptr)
{
    check(std::this_thread::get_id() != ui, "network handle opened on UI");
    auto h = std::make_unique<Handle>();
    h->kind = kind; h->parent = parent; h->owner = std::this_thread::get_id();
    auto* out = h.get(); handles.push_back(std::move(h)); changed.notify_all(); return out;
}
struct Issue {
    Handle* h;
    explicit Issue(Handle* value) : h(value) {
        std::lock_guard lock(mutex); checkOwner(h);
        check(h->issuing == 0 && h->pending == Pending::none, "overlapping WinHTTP issue");
        ++h->issuing;
    }
    ~Issue() { std::lock_guard lock(mutex); --h->issuing; changed.notify_all(); }
};
void callback(Handle* h, DWORD status, void* info = nullptr, DWORD length = 0)
{
    WINHTTP_STATUS_CALLBACK cb; DWORD_PTR context;
    { std::lock_guard lock(mutex); cb=h->callback; context=h->context; }
    check(cb && context, "callback/context missing");
    cb(h, context, status, info, length);
}
void finishClose(Handle* h)
{
    bool notify = false;
    {
        std::lock_guard lock(mutex);
        check(h->closing && !h->closed && h->issuing == 0, "invalid final close");
        h->closed = true; h->pending = Pending::none;
        notify = h->callback && h->context;
    }
    if (notify) callback(h, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING);
    changed.notify_all();
}
template<class Pred> void wait(Pred&& pred, const char* failure)
{
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 8s, std::forward<Pred>(pred)), failure);
}
Handle* pending(Pending p)
{
    for (auto& h : handles) if (!h->closing && h->pending == p && h->issuing == 0) return h.get();
    return nullptr;
}
Handle* waitPending(Pending p)
{
    Handle* h = nullptr; wait([&] { h=pending(p); return h!=nullptr; }, "pending operation missing"); return h;
}
void reset()
{
    std::lock_guard lock(mutex);
    for (auto& h : handles) check(h->closed && h->closes == 1, "handle leak/double close");
    handles.clear(); holdClose=holdSend=holdHttpRead=holdUpgradeClose=blockSendApi=releaseSendApi=enteredSendApi=false;
    opens=sends=0;
    coverTransport=holdHttpSend=false;readChunk=4096;coverResponse="http://127.0.0.1:1/test.png\n";
    coverQaOverride=true;holdCoverGet=false;coverUploads=coverGets=0;coverReplies.clear();
    requireCoverGetProxy=false;
    requireCoverReadProxy=false;
    failRequestCallback=failSocketContext=holdSocketClose=false;
    response=sessionResponse();
    nekorox::component::PresenceCoverConfiguration policy;
    policy.provider=nekorox::component::PresenceCoverProvider::privateRelay;
    policy.disclosureAccepted=true;
    policy.relayEndpoint="http://127.0.0.1:1";
    policy.relayCredential=credential;
    nekorox::component::publishPresenceCoverConfiguration(std::move(policy));
}
void releaseClosing()
{
    std::vector<Handle*> closing;
    {
        std::lock_guard lock(mutex); holdClose=false; holdUpgradeClose=false;holdSocketClose=false;
        for (auto& h : handles) if (h->closing && !h->closed) closing.push_back(h.get());
    }
    for (auto* h : closing) finishClose(h);
}
void feed(Handle* h, const std::string& text, bool malformed = false)
{
    DWORD bytes = 0;
    {
        std::lock_guard lock(mutex);
        check(h->pending == Pending::socketRead && h->issuing == 0, "no pending socket buffer");
        check(text.size() <= h->capacity, "fixture receive overflow");
        std::memcpy(h->buffer, text.data(), text.size()); bytes=static_cast<DWORD>(text.size());
        h->pending=Pending::none;
    }
    WINHTTP_WEB_SOCKET_STATUS info{bytes, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE};
    callback(h, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, &info, malformed ? 1 : sizeof(info));
}
std::vector<std::uint8_t> png() { return {137,80,78,71,13,10,26,10,0x12,0x34,0x56,0x78}; }
void checkCover(Client& c, std::uint64_t ticket)
{
    auto cover=c.registerCover(png(), {}, ticket);
    check(cover.succeeded() && cover.assetId.size()==32 && cover.url.ends_with(".png"), "signed cover admission failed");
}
void testDisabled()
{
    reset(); Client c;
    check(!c.registerCover(png(), {}, c.admissionTicket()).succeeded(), "disabled resolver restarted relay");
    check(opens==0, "disabled relay opened network"); c.stop();
    std::puts("PASS disabled registration cannot start network");
}
void testSendAndReenable()
{
    reset(); Client c; c.start(); auto old=c.admissionTicket();
    auto* socket=waitPending(Pending::socketRead);
    auto cover=c.registerCover(png(), {}, old); check(cover.succeeded(), "register failed");
    {
        std::lock_guard lock(mutex); holdSend=true; holdClose=true;
        check(socket->context != socket->parent->context, "upgrade shared request/socket context");
        check(socket->parent->closing, "upgrade request not retired");
    }
    feed(socket, "{\"op\":\"fetch\",\"request\":\"11111111111111111111111111111111\",\"asset\":\""+cover.assetId+"\"}");
    auto* send=waitPending(Pending::socketWrite);
    auto started=std::chrono::steady_clock::now(); c.disable();
    check(std::chrono::steady_clock::now()-started < 500ms, "disable waited for network");
    wait([&]{return send->closing;}, "cancel did not close pending send");
    const auto count=opens;
    c.start(); check(c.admissionTicket()!=old, "new policy reused ticket");
    check(!c.registerCover(png(), {}, old).succeeded(), "stale ticket admitted after off/on");
    {
        std::lock_guard lock(mutex);
        check(opens==count && !send->closed, "restart overtook old callback drain");
        check(send->capacity==32+png().size(), "wrong send payload length");
        const auto* bytes=static_cast<const std::uint8_t*>(send->buffer);
        auto expected=png(); check(std::equal(expected.begin(), expected.end(), bytes+32), "pending send buffer died before final callback");
    }
    releaseClosing(); auto* newer=waitPending(Pending::socketRead);
    check(newer!=socket, "old socket reused after revoke"); checkCover(c,c.admissionTicket());
    c.stop(); reset(); std::puts("PASS send lifetime, final callback drain, off/on and stale tickets");
}
void testCancelInsideIssue()
{
    reset(); {std::lock_guard lock(mutex); blockSendApi=true;}
    Client c; c.start(); auto ticket=c.admissionTicket();
    wait([]{return enteredSendApi;}, "HTTP issue barrier missing");
    c.disable(); c.start();
    {
        std::lock_guard lock(mutex);
        for(auto& h:handles) check(!h->closing, "handle closed before issuing API returned");
        blockSendApi=false; releaseSendApi=true; changed.notify_all();
    }
    waitPending(Pending::socketRead);
    check(!c.registerCover(png(), {}, ticket).succeeded(), "old HTTP response revived ticket");
    checkCover(c,c.admissionTicket()); c.stop(); reset();
    std::puts("PASS cancellation during issuing API and late inline completion");
}
void testStopRestartDrain()
{
    reset(); Client c; c.start(); auto* socket=waitPending(Pending::socketRead);
    {std::lock_guard lock(mutex); holdClose=true;}
    auto stop=std::async(std::launch::async,[&]{c.stop();});
    wait([&]{return socket->closing;}, "stop failed to cancel socket");
    c.start(); check(c.admissionTicket()==0, "admission opened during synchronous drain");
    check(stop.wait_for(0ms)==std::future_status::timeout, "stop returned before final close");
    releaseClosing(); stop.get();
    check(waitPending(Pending::socketRead)!=socket, "start during drain was lost");
    checkCover(c,c.admissionTicket()); c.stop(); reset();
    std::puts("PASS synchronous cleanup and deferred restart");
}
void testMalformedCompletion()
{
    reset(); Client c; c.start(); auto* socket=waitPending(Pending::socketRead);
    feed(socket,"{}",true); wait([&]{return socket->closed;}, "malformed callback did not close");
    c.stop(); reset(); std::puts("PASS malformed websocket completion rejected");
}
void testUpgradeCloseAndTwoStops()
{
    reset(); {std::lock_guard lock(mutex);holdUpgradeClose=true;}
    Client c; c.start(); auto* socket=waitPending(Pending::socketRead);
    auto* request=socket->parent;
    c.disable();
    wait([&]{return socket->closed;}, "delayed upgrade-request callback prevented socket cancellation");
    {
        std::lock_guard lock(mutex);
        check(request->closing && !request->closed, "upgrade callback was not delayed");
    }
    auto first=std::async(std::launch::async,[&]{c.stop();});
    auto second=std::async(std::launch::async,[&]{c.stop();});
    check(first.wait_for(10ms)==std::future_status::timeout &&
        second.wait_for(10ms)==std::future_status::timeout, "stop returned with a live callback context");
    releaseClosing();first.get();second.get();reset();
    std::puts("PASS delayed request closing cannot block socket cancellation; both stops drain");
}
void testRegistrationWaitRevoked()
{
    reset(); {std::lock_guard lock(mutex);holdHttpRead=true;holdClose=true;}
    Client c; c.start(); auto* request=waitPending(Pending::httpRead);
    auto ticket=c.admissionTicket();
    auto cover=std::async(std::launch::async,[&]{return c.registerCover(png(),{},ticket);});
    c.disable();c.start();
    check(cover.wait_for(200ms)==std::future_status::ready, "registration waited despite revoked ticket");
    check(!cover.get().succeeded(), "revoked waiting registration succeeded");
    wait([&]{return request->closing;},"revoked HTTP request not closed");
    {std::lock_guard lock(mutex);holdHttpRead=false;}
    releaseClosing();waitPending(Pending::socketRead);checkCover(c,c.admissionTicket());
    c.stop();reset();std::puts("PASS waiting resolver revoked across off/on");
}
void testDeadline()
{
    reset(); {std::lock_guard lock(mutex); holdHttpRead=true;}
    Client c; auto start=std::chrono::steady_clock::now(); c.start();
    auto* request=waitPending(Pending::httpRead);
    unsigned reads=0;
    for(;;) {
        std::unique_lock lock(mutex);
        if(changed.wait_for(lock, 350ms, [&]{return request->closing;})) break;
        check(request->pending==Pending::httpRead && request->issuing==0, "drip read state lost");
        static_cast<char*>(request->buffer)[0]=' '; request->pending=Pending::none;
        auto* data=request->buffer; lock.unlock();
        callback(request,WINHTTP_CALLBACK_STATUS_READ_COMPLETE,data,1); ++reads;
        wait([&]{return request->closing || (request->pending==Pending::httpRead && !request->issuing);}, "read continuation lost");
    }
    auto elapsed=std::chrono::steady_clock::now()-start;
    c.disable(); c.stop();
    check(reads>=8 && elapsed>=4500ms && elapsed<6500ms, "HTTP total deadline was reset by progress");
    reset(); std::printf("PASS total HTTP deadline despite %u progressing reads (%.1f ms)\n", reads,
        std::chrono::duration<double,std::milli>(elapsed).count());
}
void testAllocationBackoff()
{
    reset();
    allocation_fault::changed=&changed;allocation_fault::failures=0;allocation_fault::enabled=true;
    Client c;c.start();
    wait([]{return allocation_fault::failures.load()!=0;},"allocation fault was not reached");
    {
        std::unique_lock lock(mutex);
        check(!changed.wait_for(lock,200ms,[]{return allocation_fault::failures.load()>1;}),
            "allocation failure spun without retry backoff");
    }
    check(c.trace().status==Status::degraded,"allocation failure lost policy identity/status");
    const auto start=std::chrono::steady_clock::now();c.disable();c.stop();
    check(std::chrono::steady_clock::now()-start<200ms,"allocation-failure retry ignored cancellation");
    allocation_fault::enabled=false;allocation_fault::changed=nullptr;
    reset();std::puts("PASS allocation failure backs off and cancels without IO");
}
void testPartialAttachment()
{
    reset();failRequestCallback=true;
    {
        Client c;c.start();
        wait([]{for(auto& h:handles)if(h->kind==Kind::request&&h->closed)return true;return false;},"request callback setup failed to close handle");
        c.stop();
    }
    reset();failSocketContext=true;holdSocketClose=true;
    {
        Client c;c.start();Handle* socket=nullptr;
        wait([&]{for(auto& h:handles)if(h->kind==Kind::socket&&h->closing){socket=h.get();return true;}return false;},"socket partial attachment did not retire handle");
        c.disable();
        {std::lock_guard lock(mutex);check(socket->callback&&socket->context&&!socket->closed,"partial socket lost inherited callback context");}
        releaseClosing();c.stop();
    }
    reset();std::puts("PASS partial callback/context attachment closes once and drains inherited context");
}
void testInvalidSessions()
{
    for(const auto& invalid : {std::string("different-origin"),std::string("expired"),std::string("invalid-secret")}) {
        reset();
        if(invalid=="different-origin") response.replace(response.find("ws://127.0.0.1:1/"),17,"ws://elsewhere.invalid/");
        else if(invalid=="expired") response.replace(response.find("\"expiresAt\":")+12,std::string::npos,"1}");
        else response.replace(response.find("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),43,"bad");
        Client c;c.start();
        wait([] {for(auto& h:handles)if(h->kind==Kind::request&&h->closed)return true;return false;},"invalid session request did not drain");
        check(!c.trace().error.empty(),"invalid session did not report diagnostic");
        c.stop();
        for(auto& h:handles)check(h->kind!=Kind::socket,"invalid session opened websocket");
    }
    reset();std::puts("PASS different-origin, expired and invalid-secret sessions rejected before websocket");
}
} // namespace fixture

DWORD WINAPI fake_GetEnvironmentVariableW(LPCWSTR key, LPWSTR out, DWORD size)
{
    std::wstring value;
    if(std::wstring_view(key)==L"GODEN_PRESENCE_RELAY_URL") value=L"http://127.0.0.1:1";
    else if(std::wstring_view(key)==L"NEKOROX_QA_HOOKS" ||
            std::wstring_view(key)==L"NEKOROX_QA_PROFILE") value=L"1";
    else if(fixture::coverTransport && std::wstring_view(key)==L"NEKOROX_QA_COVER_UPLOAD_URL") {
        if (!fixture::coverQaOverride) return 0;
        value=L"http://127.0.0.1:1/upload";
    }
    else fixture::check(false,"unexpected environment access");
    if(size<=value.size()) return static_cast<DWORD>(value.size()+1);
    std::copy(value.begin(),value.end(),out);out[value.size()]=0;return static_cast<DWORD>(value.size());
}
HINTERNET WINAPI fake_WinHttpOpen(LPCWSTR,DWORD access,LPCWSTR,LPCWSTR,DWORD flags)
{
    std::lock_guard lock(fixture::mutex); fixture::check(flags&WINHTTP_FLAG_ASYNC,"sync WinHTTP session");
    ++fixture::opens; auto* h=fixture::make(fixture::Kind::session);h->accessType=access;return h;
}
HINTERNET WINAPI fake_WinHttpConnect(HINTERNET parent,LPCWSTR host,INTERNET_PORT port,DWORD)
{
    std::lock_guard lock(fixture::mutex);
    const auto hostname=std::wstring_view(host);
    const bool simulatedCover=fixture::coverTransport && port==443 &&
        (hostname==L"catbox.moe" || hostname==L"files.catbox.moe" || hostname==L"wsrv.nl" || hostname==L"litter.catbox.moe");
    fixture::check((hostname==L"127.0.0.1"&&port==1)||simulatedCover,"non-fixture endpoint");
    auto* h=fixture::cast(parent); fixture::checkOwner(h);
    auto* connected=fixture::make(fixture::Kind::connection,h);connected->host=hostname;return connected;
}
HINTERNET WINAPI fake_WinHttpOpenRequest(HINTERNET parent,LPCWSTR verb,LPCWSTR path,LPCWSTR,LPCWSTR,LPCWSTR*,DWORD)
{
    std::lock_guard lock(fixture::mutex); auto* p=fixture::cast(parent); fixture::checkOwner(p);
    auto* h=fixture::make(fixture::Kind::request,p); h->get=std::wstring_view(verb)==L"GET";
    h->host=p->host;h->path=path;
    if(fixture::coverTransport && h->get) {
        ++fixture::coverGets;
        h->coverBody=fixture::validCoverPng();
        if(!fixture::coverReplies.empty() && (fixture::coverReplies.front().host.empty() ||
                fixture::coverReplies.front().host==h->host)) {
            h->coverStatus=fixture::coverReplies.front().status;
            h->coverLengthKnown=fixture::coverReplies.front().lengthKnown;
            h->coverDeclaredLength=fixture::coverReplies.front().declaredLength;
            h->coverReadFailureAt=fixture::coverReplies.front().readFailureAt;
            h->coverBody=std::move(fixture::coverReplies.front().body);
            fixture::coverReplies.pop_front();
        }
    }
    if(!fixture::coverTransport)fixture::check(std::wstring_view(path)==(h->get?
        L"/v1/sessions/0123456789abcdef0123456789abcdef/socket":L"/v1/sessions"),"unexpected relay request path");
    return h;
}
BOOL WINAPI fake_WinHttpCrackUrl(LPCWSTR url,DWORD length,DWORD,LPURL_COMPONENTS parts)
{
    const std::wstring_view value(url,length?length:std::wcslen(url));
    std::wstring_view prefix=L"http://127.0.0.1:1";
    std::size_t hostStart=7,hostLength=9;
    parts->nScheme=INTERNET_SCHEME_HTTP; parts->nPort=1;
    if(!value.starts_with(prefix) && fixture::coverTransport) {
        constexpr std::array<std::wstring_view,4> hosts{L"catbox.moe",L"files.catbox.moe",L"wsrv.nl",L"litter.catbox.moe"};
        hostStart=8;bool found=false;
        for(const auto host:hosts) {
            if(value.starts_with(L"https://") && value.substr(hostStart).starts_with(host) &&
                    value.size()>hostStart+host.size() && value[hostStart+host.size()]==L'/') {
                hostLength=host.size();prefix=value.substr(0,hostStart+hostLength);found=true;break;
            }
        }
        if(!found) {SetLastError(ERROR_WINHTTP_INVALID_URL);return FALSE;}
        parts->nScheme=INTERNET_SCHEME_HTTPS;parts->nPort=443;
    } else if(!value.starts_with(prefix)) {SetLastError(ERROR_WINHTTP_INVALID_URL);return FALSE;}
    parts->lpszHostName=const_cast<LPWSTR>(url+hostStart); parts->dwHostNameLength=static_cast<DWORD>(hostLength);
    parts->lpszUrlPath=const_cast<LPWSTR>(url+prefix.size()); parts->dwUrlPathLength=static_cast<DWORD>(value.size()-prefix.size());
    parts->dwExtraInfoLength=0;parts->dwUserNameLength=parts->dwPasswordLength=0; return TRUE;
}
BOOL WINAPI fake_WinHttpSetTimeouts(HINTERNET h,int,int,int,int)
{std::lock_guard lock(fixture::mutex);fixture::checkOwner(fixture::cast(h));return TRUE;}
BOOL WINAPI fake_WinHttpSetOption(HINTERNET value,DWORD option,LPVOID data,DWORD length)
{
    std::lock_guard lock(fixture::mutex);auto* h=fixture::cast(value);fixture::checkOwner(h);
    if(fixture::failSocketContext && h->kind==fixture::Kind::socket && option==WINHTTP_OPTION_CONTEXT_VALUE){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    if(option==WINHTTP_OPTION_CONTEXT_VALUE){fixture::check(length==sizeof(DWORD_PTR),"context size");h->context=*static_cast<DWORD_PTR*>(data);}
    if(option==WINHTTP_OPTION_DISABLE_FEATURE){fixture::check(length==sizeof(DWORD),"redirect option size");h->redirectsDisabled=(*static_cast<DWORD*>(data)&WINHTTP_DISABLE_REDIRECTS)!=0;}
    return TRUE;
}
WINHTTP_STATUS_CALLBACK WINAPI fake_WinHttpSetStatusCallback(HINTERNET value,WINHTTP_STATUS_CALLBACK cb,DWORD flags,DWORD_PTR)
{
    std::lock_guard lock(fixture::mutex);auto* h=fixture::cast(value);fixture::checkOwner(h);
    if(fixture::failRequestCallback && h->kind==fixture::Kind::request){SetLastError(ERROR_INVALID_PARAMETER);return WINHTTP_INVALID_STATUS_CALLBACK;}
    auto old=h->callback;h->callback=cb;h->callbackFlags=flags;return old;
}
BOOL WINAPI fake_WinHttpAddRequestHeaders(HINTERNET value,LPCWSTR headers,DWORD,DWORD)
{std::lock_guard lock(fixture::mutex);auto* h=fixture::cast(value);fixture::checkOwner(h);
 if(!fixture::coverTransport){fixture::check(std::wstring_view(headers)==L"Authorization: Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n","websocket bearer missing");h->authorized=true;}return TRUE;}
BOOL WINAPI fake_WinHttpSendRequest(HINTERNET value,LPCWSTR headers,DWORD,LPVOID data,DWORD size,DWORD,DWORD_PTR context)
{
    auto* h=fixture::cast(value);fixture::Issue issue(h);
    {
        std::unique_lock lock(fixture::mutex); h->context=context; ++fixture::sends;
        if(fixture::coverTransport && h->get && fixture::requireCoverGetProxy &&
                h->parent->parent->accessType==WINHTTP_ACCESS_TYPE_NO_PROXY) {
            SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);return FALSE;
        }
        if(fixture::coverTransport && !h->get) {
            fixture::check(h->redirectsDisabled,"cover upload redirect protection missing");
            ++fixture::coverUploads;
        }
        if(fixture::coverTransport && data && size) h->requestBody.assign(static_cast<const char*>(data),size);
        if(!fixture::coverTransport){
            if(!h->get){const auto expected="Authorization: Bearer "+std::string(fixture::credential)+"\r\n";
                const std::wstring wide(expected.begin(),expected.end());
                h->authorized=headers&&std::wstring_view(headers).find(wide)!=std::wstring_view::npos;}
            fixture::check(h->authorized&&h->redirectsDisabled,"relay credential/redirect protection missing");
        }
        if(fixture::holdHttpSend || (fixture::holdCoverGet && h->get)){h->buffer=data;h->capacity=size;h->pending=fixture::Pending::sendHeaders;SetLastError(ERROR_IO_PENDING);return FALSE;}
        if(fixture::blockSendApi){fixture::enteredSendApi=true;fixture::changed.notify_all();
            fixture::check(fixture::changed.wait_for(lock,std::chrono::seconds(8),[]{return fixture::releaseSendApi;}),"blocked issue was not released");}
    }
    fixture::callback(h,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);return TRUE;
}
BOOL WINAPI fake_WinHttpReceiveResponse(HINTERNET value,LPVOID)
{auto* h=fixture::cast(value);fixture::Issue issue(h);fixture::callback(h,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);return TRUE;}
BOOL WINAPI fake_WinHttpQueryHeaders(HINTERNET value,DWORD query,LPCWSTR,LPVOID out,LPDWORD,LPDWORD)
{
    auto* h=fixture::cast(value);fixture::Issue issue(h);
    if ((query & ~WINHTTP_QUERY_FLAG_NUMBER) == WINHTTP_QUERY_CONTENT_LENGTH) {
        if (!fixture::coverTransport || !h->coverLengthKnown) {
            SetLastError(ERROR_WINHTTP_HEADER_NOT_FOUND);return FALSE;
        }
        *static_cast<DWORD*>(out)=h->coverDeclaredLength.value_or(static_cast<DWORD>(h->coverBody.size()));return TRUE;
    }
    *static_cast<DWORD*>(out)=fixture::coverTransport?h->coverStatus:(h->get?101:201);return TRUE;
}
BOOL WINAPI fake_WinHttpReadData(HINTERNET value,LPVOID buffer,DWORD size,LPDWORD read)
{
    auto* h=fixture::cast(value);fixture::Issue issue(h);fixture::check(!read,"async HTTP received synchronous byte-count pointer");
    if(h->coverReadFailureAt && h->offset>=*h->coverReadFailureAt) {
        SetLastError(ERROR_WINHTTP_TIMEOUT);return FALSE;
    }
    if(fixture::coverTransport && h->get && fixture::requireCoverReadProxy &&
            h->parent->parent->accessType==WINHTTP_ACCESS_TYPE_NO_PROXY) {
        SetLastError(ERROR_WINHTTP_TIMEOUT);return FALSE;
    }
    DWORD copied=0;
    {
        std::lock_guard lock(fixture::mutex);h->buffer=buffer;h->capacity=size;
        if(fixture::holdHttpRead){h->pending=fixture::Pending::httpRead;SetLastError(ERROR_IO_PENDING);return FALSE;}
        const auto& response=fixture::coverTransport?(h->get?h->coverBody:fixture::coverResponse):fixture::response;
        copied=static_cast<DWORD>((std::min)({response.size()-h->offset,static_cast<std::size_t>(size),fixture::readChunk}));
        std::memcpy(buffer,response.data()+h->offset,copied);h->offset+=copied;
    }
    fixture::callback(h,WINHTTP_CALLBACK_STATUS_READ_COMPLETE,buffer,copied);return TRUE;
}
BOOL WINAPI fake_WinHttpCloseHandle(HINTERNET value)
{
    auto* h=fixture::cast(value);bool hold=false;
    {
        std::lock_guard lock(fixture::mutex);fixture::checkOwner(h);fixture::check(h->issuing==0,"close raced issuing API");
        for(auto& child:fixture::handles) if(child->parent==h && child->kind!=fixture::Kind::socket)
            fixture::check(child->closed,"parent closed before child drained");
        ++h->closes;h->closing=true;
        hold=h->callback && (fixture::holdClose || (fixture::holdUpgradeClose && h->kind==fixture::Kind::request && h->get) ||
            (fixture::holdSocketClose && h->kind==fixture::Kind::socket));
        fixture::changed.notify_all();
    }
    if(!hold) fixture::finishClose(h);return TRUE;
}
HINTERNET WINAPI fake_WinHttpWebSocketCompleteUpgrade(HINTERNET value,DWORD_PTR context)
{
    auto* request=fixture::cast(value);fixture::Issue issue(request);
    std::lock_guard lock(fixture::mutex);auto* h=fixture::make(fixture::Kind::socket,request);
    h->callback=request->callback;h->callbackFlags=request->callbackFlags;h->context=context;return h;
}
DWORD WINAPI fake_WinHttpWebSocketReceive(HINTERNET value,PVOID data,DWORD size,DWORD* read,WINHTTP_WEB_SOCKET_BUFFER_TYPE* type)
{
    auto* h=fixture::cast(value);fixture::Issue issue(h);fixture::check(!read&&!type,"async socket received synchronous out pointers");
    std::lock_guard lock(fixture::mutex);h->pending=fixture::Pending::socketRead;h->buffer=data;h->capacity=size;return ERROR_SUCCESS;
}
DWORD WINAPI fake_WinHttpWebSocketSend(HINTERNET value,WINHTTP_WEB_SOCKET_BUFFER_TYPE type,PVOID data,DWORD size)
{
    auto* h=fixture::cast(value);fixture::Issue issue(h);
    {
        std::lock_guard lock(fixture::mutex);h->buffer=data;h->capacity=size;
        if(fixture::holdSend){h->pending=fixture::Pending::socketWrite;return ERROR_SUCCESS;}
    }
    WINHTTP_WEB_SOCKET_STATUS status{size,type};fixture::callback(h,WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,&status,sizeof(status));return ERROR_SUCCESS;
}
#ifndef GODEN_RELAY_FIXTURE_NO_MAIN
int main()
{
    fixture::testDisabled(); fixture::testSendAndReenable(); fixture::testCancelInsideIssue();
    fixture::testStopRestartDrain(); fixture::testMalformedCompletion(); fixture::testUpgradeCloseAndTwoStops();
    fixture::testRegistrationWaitRevoked(); fixture::testDeadline();fixture::testAllocationBackoff();fixture::testPartialAttachment();fixture::testInvalidSessions();
    std::puts("All relay lifecycle tests PASS (fake transport, full production client).");
}
#endif
