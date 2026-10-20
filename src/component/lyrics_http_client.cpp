#include "lyrics_http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <windows.h>
#include <winhttp.h>

namespace nekorox::component {
namespace {

constexpr wchar_t kLrclibHost[] = L"lrclib.net";
constexpr std::size_t kMaximumBodyBytes = 2U * 1024U * 1024U;

class InternetHandle final {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { close(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const noexcept { return value_.load(std::memory_order_acquire); }
    void close() noexcept
    {
        if (const auto value = value_.exchange(nullptr,
                std::memory_order_acq_rel)) WinHttpCloseHandle(value);
    }
private:
    std::atomic<HINTERNET> value_{nullptr};
};

std::string winError(const char* operation)
{
    return std::string(operation) + " failed (" +
        std::to_string(GetLastError()) + ")";
}

std::chrono::seconds retryAfter(HINTERNET request)
{
    wchar_t value[128]{};
    DWORD bytes = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER,
            WINHTTP_HEADER_NAME_BY_INDEX, value, &bytes,
            WINHTTP_NO_HEADER_INDEX)) return std::chrono::seconds(60);
    wchar_t* end = nullptr;
    const auto delta = std::wcstoll(value, &end, 10);
    if (end != value && *end == L'\0' && delta >= 0) {
        return std::chrono::seconds(std::clamp<std::int64_t>(delta, 1, 86400));
    }
    SYSTEMTIME target{};
    FILETIME targetFile{};
    FILETIME nowFile{};
    GetSystemTimeAsFileTime(&nowFile);
    if (WinHttpTimeToSystemTime(value, &target) &&
        SystemTimeToFileTime(&target, &targetFile)) {
        ULARGE_INTEGER wanted{}, now{};
        wanted.LowPart = targetFile.dwLowDateTime;
        wanted.HighPart = targetFile.dwHighDateTime;
        now.LowPart = nowFile.dwLowDateTime;
        now.HighPart = nowFile.dwHighDateTime;
        if (wanted.QuadPart > now.QuadPart) {
            const auto seconds = (wanted.QuadPart - now.QuadPart) / 10000000ULL;
            return std::chrono::seconds(std::clamp<std::uint64_t>(seconds, 1, 86400));
        }
    }
    return std::chrono::seconds(60);
}

} // namespace

LyricsHttpResponse LyricsHttpClient::getLrclib(
    std::wstring_view pathAndQuery,
    std::stop_token stop) const
{
    LyricsHttpResponse response;
    if (stop.stop_requested()) {
        response.cancelled = true;
        return response;
    }
    if (pathAndQuery.empty() || pathAndQuery.front() != L'/' ||
        pathAndQuery.find(L"://") != std::wstring_view::npos) {
        response.error = "lyrics request path is invalid";
        return response;
    }
    InternetHandle session(WinHttpOpen(L"GODENplayer/5 Lyrics",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        response.error = winError("WinHttpOpen");
        return response;
    }
    WinHttpSetTimeouts(session.get(), 3000, 3000, 10000, 10000);
    InternetHandle connection(WinHttpConnect(session.get(), kLrclibHost,
        INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.get()) {
        response.error = winError("WinHttpConnect");
        return response;
    }
    const std::wstring path(pathAndQuery);
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.get()) {
        response.error = winError("WinHttpOpenRequest");
        return response;
    }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    (void)WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
        &disabled, sizeof(disabled));
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP |
        WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    (void)WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION,
        &decompression, sizeof(decompression));
    const wchar_t headers[] = L"Accept: application/json\r\n";
    std::stop_callback cancel(stop, [&request] { request.close(); });
    auto* raw = request.get();
    if (!raw || !WinHttpSendRequest(raw, headers, -1L,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        stop.stop_requested()) {
        response.cancelled = stop.stop_requested();
        if (!response.cancelled) response.error = winError("WinHttpSendRequest");
        return response;
    }
    raw = request.get();
    if (!raw || !WinHttpReceiveResponse(raw, nullptr) ||
        stop.stop_requested()) {
        response.cancelled = stop.stop_requested();
        if (!response.cancelled) response.error = winError("WinHttpReceiveResponse");
        return response;
    }
    raw = request.get();
    DWORD bytes = sizeof(response.status);
    if (!raw || !WinHttpQueryHeaders(raw,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &bytes,
            WINHTTP_NO_HEADER_INDEX)) {
        response.error = winError("WinHttpQueryHeaders");
        return response;
    }
    if (response.status == 429) response.retryAfter = retryAfter(raw);
    while (!stop.stop_requested()) {
        DWORD available = 0;
        raw = request.get();
        if (!raw || !WinHttpQueryDataAvailable(raw, &available)) {
            response.error = winError("WinHttpQueryDataAvailable");
            return response;
        }
        if (available == 0) break;
        if (available > kMaximumBodyBytes - response.body.size()) {
            response.body.clear();
            response.error = "lyrics response exceeds 2 MiB";
            return response;
        }
        const auto original = response.body.size();
        response.body.resize(original + available);
        DWORD read = 0;
        raw = request.get();
        if (!raw || !WinHttpReadData(raw, response.body.data() + original,
                available, &read)) {
            response.body.clear();
            response.error = winError("WinHttpReadData");
            return response;
        }
        response.body.resize(original + read);
    }
    response.cancelled = stop.stop_requested();
    if (response.cancelled) response.body.clear();
    return response;
}

} // namespace nekorox::component
