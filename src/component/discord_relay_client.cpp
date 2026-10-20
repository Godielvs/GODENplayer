#include "presence_cover_policy.h"
#include "qa_hooks.h"
#include "discord_relay_client.h"
#include "discord_http_io.h"
#include "presence_deployment.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace nekorox::component {
namespace {

using namespace std::chrono_literals;
constexpr std::size_t kMaximumSessionResponse = 16U * 1024U;
constexpr std::size_t kMaximumCoverBytes = 1024U * 1024U;
constexpr std::size_t kMaximumAssets = 4U;
constexpr auto kConnectionWait = 1500ms;
constexpr auto kRequestDeadline = 5s;

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value != nullptr) WinHttpCloseHandle(value); }
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

std::wstring widen(std::string_view value)
{
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        return {};
    }
    return result;
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

std::optional<ParsedUrl> parseUrl(std::string value, bool websocket)
{
    if (value.size()>2048U || value.find_first_of("?#\r\n")!=std::string::npos) return std::nullopt;
    if (websocket) {
        if (value.starts_with("wss://")) value.replace(0, 3, "https");
        else if (value.starts_with("ws://")) value.replace(0, 2, "http");
        else return std::nullopt;
    }
    auto wide = widen(value);
    if (wide.empty()) return std::nullopt;
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.data(), static_cast<DWORD>(wide.size()), 0,
            &components)) return std::nullopt;
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
            components.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
    if (components.dwUserNameLength || components.dwPasswordLength) return std::nullopt;
    ParsedUrl result;
    result.host.assign(components.lpszHostName, components.dwHostNameLength);
    result.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        result.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (result.path.empty()) result.path = L"/";
    result.port = components.nPort;
    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopback = result.host==L"127.0.0.1" || result.host==L"localhost" || result.host==L"[::1]";
    if (!result.secure && !(qa::hooksAllowed() && loopback)) return std::nullopt;
    return result;
}

std::optional<std::string> jsonString(
    std::string_view json, std::string_view key)
{
    const auto marker = '"' + std::string(key) + "\":";
    auto offset = json.find(marker);
    if (offset == std::string_view::npos) return std::nullopt;
    offset += marker.size();
    if (offset >= json.size() || json[offset] != '"') return std::nullopt;
    ++offset;
    std::string result;
    bool escaped = false;
    for (; offset < json.size(); ++offset) {
        const auto character = json[offset];
        if (escaped) {
            if (character == '"' || character == '\\' || character == '/') {
                result.push_back(character);
            } else {
                return std::nullopt;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return result;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            return std::nullopt;
        } else {
            result.push_back(character);
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> jsonInteger(
    std::string_view json, std::string_view key)
{
    const auto marker = '"' + std::string(key) + "\":";
    auto offset = json.find(marker);
    if (offset == std::string_view::npos) return std::nullopt;
    offset += marker.size();
    while (offset < json.size() && json[offset] == ' ') ++offset;
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(
        json.data() + offset, json.data() + json.size(), value);
    if (error != std::errc{} || end == json.data() + offset) return std::nullopt;
    return value;
}

std::string hexadecimal(std::span<const std::uint8_t> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> decodeBase64Url(std::string value)
{
    const auto decode = [](char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '-' || value == '+') return 62;
        if (value == '_' || value == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> result;
    result.reserve(value.size() * 3U / 4U);
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const auto character : value) {
        if (character == '=') break;
        const auto decoded = decode(character);
        if (decoded < 0) return std::nullopt;
        accumulator = (accumulator << 6U) | static_cast<unsigned>(decoded);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            result.push_back(static_cast<std::uint8_t>(accumulator >> bits));
            accumulator &= (1U << bits) - 1U;
        }
    }
    if (result.size() != 32U) return std::nullopt;
    return result;
}

std::optional<std::array<std::uint8_t, 32>> hmacSha256(
    std::span<const std::uint8_t> secret, std::string_view message)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD returned = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> digest{};
    auto cleanup = [&] {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &returned, 0) < 0) {
        cleanup();
        return std::nullopt;
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            const_cast<PUCHAR>(secret.data()),
            static_cast<ULONG>(secret.size()), 0) < 0 ||
        BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
            static_cast<ULONG>(message.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(),
            static_cast<ULONG>(digest.size()), 0) < 0) {
        cleanup();
        return std::nullopt;
    }
    cleanup();
    return digest;
}

std::string randomHex(std::size_t bytes)
{
    std::vector<std::uint8_t> random(bytes);
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    return hexadecimal(random);
}

std::int64_t unixMilliseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint64_t steadyMilliseconds() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now()
            .time_since_epoch()).count());
}

} // namespace

DiscordRelayClient::DiscordRelayClient() = default;
DiscordRelayClient::~DiscordRelayClient() { stop(); }

std::string DiscordRelayClient::configuredEndpoint()
{
    const auto configuration=presenceCoverConfiguration();
    if(configuration->provider==PresenceCoverProvider::privateRelay &&
            configuration->allowsCovers() && validEndpoint(configuration->relayEndpoint))
        return configuration->relayEndpoint;
    if constexpr (qa::compiled) {
        if (qa::hooksAllowed()) {
            std::array<wchar_t,2048> value{};
            const auto size=GetEnvironmentVariableW(L"GODEN_PRESENCE_RELAY_URL",value.data(),static_cast<DWORD>(value.size()));
            if(size && size<value.size()) {
                const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),size,nullptr,0,nullptr,nullptr);
                if(count<=0)return {};
                std::string endpoint(count,'\0');
                WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),size,endpoint.data(),count,nullptr,nullptr);
                while(!endpoint.empty()&&endpoint.back()=='/')endpoint.pop_back();
                if(validEndpoint(endpoint))return endpoint;
            }
        }
    }
    return {};
}

bool DiscordRelayClient::validEndpoint(const std::string& endpoint)
{
    const auto parsed=parseUrl(endpoint,false);
    return parsed && parsed->path==L"/";
}

bool DiscordRelayClient::configured()
{
    return !configuredEndpoint().empty();
}

void DiscordRelayClient::setStateChangedCallback(std::function<void()> callback)
{
    std::lock_guard lock(mutex_);
    stateChanged_=std::move(callback);
}

void DiscordRelayClient::start()
{
    std::lock_guard lock(mutex_);
    if (started_) return;
    endpoint_ = configuredEndpoint();
    if (endpoint_.empty()) {
        trace_.status = DiscordRelayStatus::disabled;
        return;
    }
    started_ = true;
    ++admissionGeneration_;
    trace_.status = DiscordRelayStatus::connecting;
    // An old episode owns its closing callbacks until fully drained. If it
    // is still exiting, it will observe this new policy before deciding to
    // finish. A synchronous cleanup owns restart while syncDraining_ is set.
    if (!syncDraining_ && workerFinished_) launchWorkerLocked();
    changed_.notify_all();
}

void DiscordRelayClient::launchWorkerLocked()
{
    // workerFinished_ is published only after all worker-local handles and
    // callbacks have drained, with no subsequent access to mutex_.
    if (worker_.joinable()) worker_.join();
    workerFinished_ = false;
    try {
        worker_ = std::jthread([this](std::stop_token token) { workerMain(token); });
    } catch (...) {
        workerFinished_ = true;
        started_ = false;
        trace_.status = DiscordRelayStatus::degraded;
    }
}

bool DiscordRelayClient::admittedLocked(std::uint64_t ticket) const noexcept
{
    return started_ && !syncDraining_ && ticket != 0 && ticket == admissionGeneration_;
}

void DiscordRelayClient::revokeLocked() noexcept
{
    started_ = false;
    ++admissionGeneration_;
    assets_.clear();
    sessionId_.clear();
    secret_.clear();
    socketUrl_.clear();
    coverBaseUrl_.clear();
    expiresAtMilliseconds_ = 0;
    trace_.status = DiscordRelayStatus::disabled;
    trace_.delivery = DiscordRelayDelivery::none;
    trace_.assetId.clear();
    trace_.error.clear();
    trace_.deliveryMilliseconds = 0;
    trace_.deliveredAtMilliseconds = 0;
    changed_.notify_all();
}

void DiscordRelayClient::disable() noexcept
{
    std::stop_source cancel(std::nostopstate);
    {
        std::lock_guard lock(mutex_);
        revokeLocked();
        cancel = connectionStop_;
    }
    cancel.request_stop();
}

std::uint64_t DiscordRelayClient::admissionTicket() const noexcept
{
    std::lock_guard lock(mutex_);
    return admittedLocked(admissionGeneration_) ? admissionGeneration_ : 0;
}

void DiscordRelayClient::stop() noexcept
{
    std::jthread worker;
    std::stop_source cancel(std::nostopstate);
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return !syncDraining_; });
        revokeLocked();
        syncDraining_ = true;
        worker = std::move(worker_);
        cancel = connectionStop_;
    }
    worker.request_stop();
    cancel.request_stop();
    if (worker.joinable()) worker.join();
    {
        std::lock_guard lock(mutex_);
        syncDraining_ = false;
        workerFinished_ = true;
        // A start arriving during the join is a new policy, not permission
        // for the old worker to begin another connection while draining.
        if (started_) launchWorkerLocked();
        changed_.notify_all();
    }
}

void DiscordRelayClient::clear() noexcept
{
    std::lock_guard lock(mutex_);
    assets_.clear();
    trace_.delivery = DiscordRelayDelivery::none;
    trace_.assetId.clear();
    trace_.deliveryMilliseconds = 0;
}

DiscordRelayCover DiscordRelayClient::registerCover(
    std::vector<std::uint8_t> png, std::stop_token stopToken, std::uint64_t ticket)
{
    DiscordRelayCover result;
    if (png.empty() || png.size() > kMaximumCoverBytes) {
        result.error = "relay cover exceeds the validated size limit";
        return result;
    }
    std::unique_lock lock(mutex_);
    const auto ready = [&] {
        return stopToken.stop_requested() ||
            trace_.status == DiscordRelayStatus::connected || !admittedLocked(ticket);
    };
    changed_.wait_for(lock, stopToken, kConnectionWait, ready);
    if (stopToken.stop_requested() || !admittedLocked(ticket)) {
        result.error = "cancelled";
        return result;
    }
    if (trace_.status != DiscordRelayStatus::connected ||
            coverBaseUrl_.empty() || secret_.empty() || sessionId_.empty()) {
        result.error = trace_.error.empty()
            ? "cover relay is not connected" : trace_.error;
        return result;
    }
    const auto sessionId = sessionId_;
    const auto sessionSecret = secret_;
    const auto coverBase = coverBaseUrl_;
    const auto sessionExpiry = expiresAtMilliseconds_;
    if (sessionExpiry <= unixMilliseconds() + 30000) {
        // Replace the session before a short-lived cover can repeatedly renew
        // against an expiring session. This cancels only the current episode.
        connectionStop_.request_stop();
        result.error = "cover relay session is renewing";
        return result;
    }
    const auto connectionGeneration = trace_.connectionGeneration;
    lock.unlock();
    const auto assetId = randomHex(16);
    const auto secret = decodeBase64Url(sessionSecret);
    if (assetId.empty() || !secret) {
        result.error = "cover relay token generation failed";
        return result;
    }
    // The session can remain connected for hours, but an individual public
    // artwork URL is intentionally useful for only one minute. After that the
    // Worker serves the code-native fallback and can no longer ask this client
    // for the exact cover.
    const auto expiresAt = (std::min)(sessionExpiry,
        unixMilliseconds() + std::chrono::duration_cast<std::chrono::milliseconds>(
            60s).count());
    const auto message = sessionId + ':' + assetId + ':' +
        std::to_string(expiresAt);
    const auto digest = hmacSha256(*secret, message);
    if (!digest) {
        result.error = "cover relay signature failed";
        return result;
    }
    lock.lock();
    if (stopToken.stop_requested() || !admittedLocked(ticket) ||
            trace_.status != DiscordRelayStatus::connected ||
            trace_.connectionGeneration != connectionGeneration ||
            expiresAt <= unixMilliseconds()) {
        result.error = "cancelled";
        return result;
    }
    while (assets_.size() >= kMaximumAssets) {
        const auto oldest = std::min_element(assets_.begin(), assets_.end(),
            [](const auto& left, const auto& right) {
                return left.second.inserted < right.second.inserted;
            });
        if (oldest == assets_.end()) break;
        assets_.erase(oldest);
    }
    assets_[assetId] = Asset{std::move(png), std::chrono::steady_clock::now()};
    trace_.delivery = DiscordRelayDelivery::none;
    trace_.assetId = assetId;
    trace_.deliveryMilliseconds = 0;
    trace_.deliveredAtMilliseconds = 0;
    result.assetId = assetId;
    result.expiresAtMilliseconds=expiresAt;
    result.connectionGeneration=connectionGeneration;
    result.url = coverBase + '/' + assetId + '/' +
        std::to_string(expiresAt) + '/' +
        hexadecimal(std::span(digest->data(), 16U)) + ".png";
    return result;
}

DiscordRelayTrace DiscordRelayClient::trace() const
{
    std::lock_guard lock(mutex_);
    return trace_;
}

void DiscordRelayClient::setError(std::uint64_t ticket, std::string value)
{
    std::lock_guard lock(mutex_);
    if (!admittedLocked(ticket)) return;
    trace_.status = DiscordRelayStatus::degraded;
    trace_.error = std::move(value);
    changed_.notify_all();
}

void DiscordRelayClient::workerMain(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    unsigned failure = 0;
    for (;;) {
        std::uint64_t ticket = 0;
        std::stop_token episode;
        try {
            std::string endpoint;
            {
                std::lock_guard lock(mutex_);
                if (stopToken.stop_requested() || syncDraining_ || !started_) {
                    // All previous iteration locals have been destroyed.
                    // No client access after this lock releases on return.
                    workerFinished_ = true;
                    changed_.notify_all();
                    return;
                }
                ticket = admissionGeneration_;
                // Establish policy identity before allocations: a failure
                // still belongs to this episode and observes retry backoff.
                endpoint = endpoint_;
                connectionStop_ = std::stop_source{};
                episode = connectionStop_.get_token();
                assets_.clear();
                sessionId_.clear();
                secret_.clear();
                socketUrl_.clear();
                coverBaseUrl_.clear();
                trace_.status = DiscordRelayStatus::connecting;
                trace_.error.clear();
                ++trace_.connectionGeneration;
            if(stateChanged_) stateChanged_();
            }
            if (createSession(episode, ticket, endpoint) && connectSocket(episode, ticket)) {
                failure = 0;
            } else if (!episode.stop_requested()) {
                ++failure;
            }
        } catch (...) {
            // Allocation/network setup failure must not terminate the host.
            // Stack unwinding already drained every outstanding async handle.
            std::lock_guard lock(mutex_);
            if (admittedLocked(ticket)) {
                trace_.status = DiscordRelayStatus::degraded;
                trace_.error.clear();
            }
            ++failure;
        }
        if (stopToken.stop_requested()) break;
        const auto delay = failure < 2U ? 500ms :
            failure < 5U ? 2s : 10s;
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, episode, delay, [&] {
            return stopToken.stop_requested() || !admittedLocked(ticket);
        });
    }
    std::lock_guard lock(mutex_);
    workerFinished_ = true;
    changed_.notify_all();
}

bool DiscordRelayClient::createSession(std::stop_token stopToken,
    std::uint64_t ticket, const std::string& endpoint)
{
    if (stopToken.stop_requested()) return false;
    const auto deadline = std::chrono::steady_clock::now() + kRequestDeadline;
    const auto parsed = parseUrl(endpoint + "/v1/sessions", false);
    if (!parsed) {
        setError(ticket, "invalid cover relay endpoint");
        return false;
    }
    auto credential=presenceCoverConfiguration()->relayCredential;
    if constexpr (qa::compiled) {
        if(credential.empty() && qa::hooksAllowed())
            credential="qa-transport-fixture-credential-not-for-production";
    }
    if(credential.size()<32U || credential.size()>256U) {
        setError(ticket,"private relay credential is not configured"); return false;
    }
    const auto headers=widen("Content-Type: application/json\r\nAuthorization: Bearer "+credential+"\r\n");
    SecureZeroMemory(credential.data(),credential.size());
    InternetHandle session(WinHttpOpen(L"GODENplayer/1 PresenceRelay",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session.value) {
        setError(ticket, "cover relay network initialization failed");
        return false;
    }
    WinHttpSetTimeouts(session.value, 2000, 2000, 3000, 3000);
    InternetHandle connection(WinHttpConnect(session.value,
        parsed->host.c_str(), parsed->port, 0));
    const auto flags = parsed->secure ? WINHTTP_FLAG_SECURE : 0U;
    AsyncIo request;
    if (!request.attach(connection.value ? WinHttpOpenRequest(
        connection.value, L"POST", parsed->path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr) ||
        !([&] { DWORD disabled=WINHTTP_DISABLE_REDIRECTS;
            return WinHttpSetOption(request.handle(),WINHTTP_OPTION_DISABLE_FEATURE,
                &disabled,sizeof(disabled))!=FALSE; }()) ||
        !request.perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stopToken, deadline, [&] {
            return AsyncIo::accepted(WinHttpSendRequest(request.handle(),
                headers.c_str(), static_cast<DWORD>(headers.size()),
                nullptr, 0, 0, request.contextValue()));
        }) || !request.perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stopToken, deadline, [&] {
            return AsyncIo::accepted(WinHttpReceiveResponse(request.handle(), nullptr));
        })) {
        setError(ticket, "cover relay session request failed");
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX) || status != 201U) {
        setError(ticket, "cover relay rejected the session");
        return false;
    }
    std::string response;
    if (!request.readResponse(response, stopToken, deadline, kMaximumSessionResponse)) {
        setError(ticket, "cover relay returned an invalid session response");
        return false;
    }
    const auto sessionId = jsonString(response, "sessionId");
    const auto secret = jsonString(response, "secret");
    const auto socketUrl = jsonString(response, "socketUrl");
    const auto coverBase = jsonString(response, "coverBaseUrl");
    const auto expires = jsonInteger(response, "expiresAt");
    if (!sessionId || !secret || !socketUrl || !coverBase || !expires ||
            sessionId->size() != 32U ||
            !std::all_of(sessionId->begin(),sessionId->end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}) ||
            !decodeBase64Url(*secret) || *expires <= unixMilliseconds() ||
            *expires > unixMilliseconds()+13LL*60*60*1000) {
        setError(ticket, "cover relay session response is incomplete");
        return false;
    }
    auto expectedSocket=endpoint;
    expectedSocket.replace(0,endpoint.starts_with("https://")?5U:4U,
        endpoint.starts_with("https://")?"wss":"ws");
    expectedSocket+="/v1/sessions/"+*sessionId+"/socket";
    if(*socketUrl!=expectedSocket || *coverBase!=endpoint+"/v1/c/"+*sessionId ||
            !parseUrl(*socketUrl,true) || !parseUrl(*coverBase,false)) {
        setError(ticket,"cover relay returned a different origin or path");return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopToken.stop_requested() || !admittedLocked(ticket)) return false;
        sessionId_ = *sessionId;
        secret_ = *secret;
        socketUrl_ = *socketUrl;
        coverBaseUrl_ = *coverBase;
        expiresAtMilliseconds_ = *expires;
    }
    return true;
}

bool DiscordRelayClient::connectSocket(std::stop_token stopToken, std::uint64_t ticket)
{
    std::string socketUrl;
    std::string secret;
    {
        std::lock_guard lock(mutex_);
        if (stopToken.stop_requested() || !admittedLocked(ticket)) return false;
        socketUrl = socketUrl_;
        secret = secret_;
    }
    const auto parsed = parseUrl(socketUrl, true);
    if (!parsed) {
        setError(ticket, "invalid cover relay socket URL");
        return false;
    }
    InternetHandle session(WinHttpOpen(L"GODENplayer/1 PresenceRelay",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session.value) return false;
    WinHttpSetTimeouts(session.value, 2000, 2000, 5000, 5000);
    InternetHandle connection(WinHttpConnect(session.value,
        parsed->host.c_str(), parsed->port, 0));
    const auto flags = parsed->secure ? WINHTTP_FLAG_SECURE : 0U;
    const auto deadline = std::chrono::steady_clock::now() + kRequestDeadline;
    AsyncIo request;
    if (!request.attach(connection.value ? WinHttpOpenRequest(
        connection.value, L"GET", parsed->path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr) ||
        !WinHttpSetOption(request.handle(),
            WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;
    const auto authorization = widen("Authorization: Bearer " + secret + "\r\n");
    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request.handle(), WINHTTP_OPTION_DISABLE_FEATURE,
            &disabledFeatures, sizeof(disabledFeatures))) return false;
    if (!WinHttpAddRequestHeaders(request.handle(), authorization.c_str(),
            static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ||
        !request.perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stopToken, deadline, [&] {
            return AsyncIo::accepted(WinHttpSendRequest(request.handle(),
                WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, request.contextValue()));
        }) || !request.perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stopToken, deadline, [&] {
            return AsyncIo::accepted(WinHttpReceiveResponse(request.handle(), nullptr));
        })) {
        setError(ticket, "cover relay socket handshake failed");
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX) || status != 101U || stopToken.stop_requested()) return false;
    AsyncIo socket(true);
    if (!socket.attach(WinHttpWebSocketCompleteUpgrade(request.handle(),
            socket.contextValue()), true)) {
        setError(ticket, "cover relay socket upgrade failed");
        return false;
    }
    // Retire the upgrade request without blocking cancellation of the live
    // socket on its final callback. Locals drain socket, then request, before
    // their parents; the two callback contexts remain independent throughout.
    request.beginClose();
    {
        std::lock_guard lock(mutex_);
        if (stopToken.stop_requested() || !admittedLocked(ticket)) return false;
        trace_.status = DiscordRelayStatus::connected;
        if(stateChanged_) stateChanged_();
        trace_.error.clear();
        changed_.notify_all();
    }
    receiveSocket(socket, stopToken, ticket);
    {
        std::lock_guard lock(mutex_);
        if (admittedLocked(ticket) && !stopToken.stop_requested()) {
            trace_.status = DiscordRelayStatus::degraded;
            trace_.error = "cover relay disconnected";
        }
        changed_.notify_all();
    }
    return !stopToken.stop_requested();
}

void DiscordRelayClient::receiveSocket(AsyncIo& socket,
    std::stop_token stopToken, std::uint64_t ticket)
{
    std::string textMessage;
    while (!stopToken.stop_requested()) {
        if (!socket.perform(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, stopToken,
                AsyncIo::Deadline::max(), [&] {
                    return WinHttpWebSocketReceive(socket.handle(), socket.buffer.data(),
                        static_cast<DWORD>(socket.buffer.size()), nullptr, nullptr);
                })) break;
        const auto read = socket.bytes();
        const auto type = socket.type();
        if (read > socket.buffer.size() || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) continue;
        if (textMessage.size() + read > 16U * 1024U) {
            textMessage.clear();
            continue;
        }
        textMessage.append(reinterpret_cast<const char*>(socket.buffer.data()), read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
        const auto operation = jsonString(textMessage, "op");
        if (operation && *operation == "fetch") {
            const auto requestId = jsonString(textMessage, "request");
            const auto assetId = jsonString(textMessage, "asset");
            std::vector<std::uint8_t> payload;
            if (requestId && assetId && requestId->size() == 32U) {
                std::lock_guard lock(mutex_);
                if (!admittedLocked(ticket) || stopToken.stop_requested()) return;
                const auto found = assets_.find(*assetId);
                if (found != assets_.end()) {
                    payload.reserve(32U + found->second.png.size());
                    payload.insert(payload.end(), requestId->begin(), requestId->end());
                    payload.insert(payload.end(), found->second.png.begin(), found->second.png.end());
                }
            }
            // The policy lock above grants admission to this delivery. A
            // concurrent revoke cancels it; bytes already sent cannot be
            // recalled. No new delivery can acquire an old-generation permit.
            if (!payload.empty() && !socket.perform(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,
                    stopToken, std::chrono::steady_clock::now() + kRequestDeadline, [&] {
                        return WinHttpWebSocketSend(socket.handle(),
                            WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                            payload.data(), static_cast<DWORD>(payload.size()));
                    })) {
                break;
            }
        } else if (operation && *operation == "delivered") {
            const auto assetId = jsonString(textMessage, "asset");
            const auto kind = jsonString(textMessage, "kind");
            const auto latency = jsonInteger(textMessage, "latencyMs");
            if (assetId && kind) {
                std::lock_guard lock(mutex_);
                if (admittedLocked(ticket) && !stopToken.stop_requested() && trace_.assetId == *assetId) {
                    trace_.delivery = *kind == "exact"
                        ? DiscordRelayDelivery::exact
                        : DiscordRelayDelivery::fallback;
                    trace_.deliveryMilliseconds = latency
                        ? static_cast<std::uint32_t>((std::clamp)(
                            *latency, std::int64_t{0}, std::int64_t{10'000})) : 0U;
                    trace_.deliveredAtMilliseconds = steadyMilliseconds();
                    ++trace_.deliveryGeneration;
                    changed_.notify_all();
                }
            }
        }
        textMessage.clear();
    }
}

} // namespace nekorox::component
