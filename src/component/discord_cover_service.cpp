#include "presence_cover_policy.h"
#include "qa_hooks.h"
#include "discord_cover_service.h"
#include "discord_http_io.h"
#include "rollout_flags.h"
#include "text_util.h"

#include <winhttp.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

// Relay images are completely decoded by WIC before publication and stay
// below the relay's explicit 512 px / 1 MiB contract.
constexpr std::uint32_t kCoverEdge = 512;
constexpr std::uint64_t kCoverEncodingFingerprint =
    0x474f44454e800003ULL; // GODEN, 512 px, encoder contract v3
constexpr std::size_t kMaximumEncodedBytes = 1024U * 1024U;
constexpr std::size_t kMaximumResponseBytes = 2048U;
constexpr std::uintmax_t kMaximumCacheFileBytes = 256U * 1024U;
constexpr std::size_t kMaximumCacheEntries = 512U;
constexpr std::string_view kCacheHeaderV1 =
    "goden.discord-cover-cache.v1";
constexpr std::string_view kCacheHeaderV2 =
    "goden.discord-cover-cache.v2";
// Optional comment records keep the v2 URL rows readable by older builds.
// A cache has at most one record per URL, within its existing 2*entry read cap.
constexpr std::string_view kPngEncodingRecord = "#png-encoding-v1\t";
constexpr std::array<WICPngFilterOption, 5> kPngEncodings{
    WICPngFilterUnspecified, WICPngFilterSub, WICPngFilterUp,
    WICPngFilterPaeth, WICPngFilterNone};

constexpr std::array<std::uint8_t, 8> kPngSignature{
    137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};

bool hasPngSignature(std::string_view image) noexcept
{
    return image.size() >= kPngSignature.size() &&
        std::memcmp(image.data(), kPngSignature.data(), kPngSignature.size()) == 0;
}

// Validate the complete stored representation, not just its signature. A
// successful HTTP response can itself contain a truncated PNG. This checks
// framing and CRCs; it is deliberately not an image decoder.
bool completePngStructure(std::string_view image) noexcept
{
    if (!hasPngSignature(image) || image.size() > kMaximumEncodedBytes) return false;
    const auto word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value = (value << 8U) | static_cast<unsigned char>(image[offset + i]);
        return value;
    };
    static constexpr auto crcTable = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < table.size(); ++i) {
            auto value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
            table[i] = value;
        }
        return table;
    }();
    bool header = false, data = false;
    std::size_t offset = kPngSignature.size();
    while (image.size() - offset >= 12U) {
        const auto length = word(offset);
        if (length > image.size() - offset - 12U) return false;
        const auto type = image.substr(offset + 4U, 4U);
        if (!header && (type != "IHDR" || length != 13U)) return false;
        std::uint32_t crc = 0xffffffffU;
        for (std::size_t i = offset + 4U; i < offset + 8U + length; ++i)
            crc = crcTable[(crc ^ static_cast<unsigned char>(image[i])) & 0xffU] ^ (crc >> 8U);
        if ((crc ^ 0xffffffffU) != word(offset + 8U + length)) return false;
        if (type == "IHDR") {
            if (header || length != 13U) return false;
            const auto width = word(offset + 8U), height = word(offset + 12U);
            if (!width || !height || width > kCoverEdge || height > kCoverEdge) return false;
            header = true;
        } else if (type == "IDAT") {
            data = true;
        } else if (type == "IEND") {
            return header && data && length == 0U && offset + 12U == image.size();
        }
        offset += static_cast<std::size_t>(length) + 12U;
    }
    return false;
}

bool qaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle()
    {
        if (value != nullptr) WinHttpCloseHandle(value);
    }
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

std::string trimAscii(std::string value)
{
    const auto useful = [](unsigned char character) {
        return character > 0x20U;
    };
    value.erase(value.begin(),
        std::find_if(value.begin(), value.end(), useful));
    value.erase(std::find_if(value.rbegin(), value.rend(), useful).base(),
        value.end());
    return value;
}

bool startsWith(std::string_view value, std::string_view prefix) noexcept
{
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

std::string percentEncode(std::string_view value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3U);
    for (const auto raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        const auto unreserved = std::isalnum(character) != 0 ||
            character == '-' || character == '_' || character == '.' ||
            character == '~';
        if (unreserved) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(digits[character >> 4U]);
            result.push_back(digits[character & 0x0fU]);
        }
    }
    return result;
}

std::string discordDeliveryUrl(
    const std::string& origin, bool qaOverride)
{
    if (qaOverride) return origin;
    // Catbox is a durable origin, but Discord's media proxy intermittently
    // refuses or times out fetching it directly. wsrv.nl is a documented,
    // Cloudflare-backed image cache intended for this exact origin-to-CDN
    // use. Pre-warming this immutable URL before SET_ACTIVITY also removes
    // Catbox propagation races from Discord's one-shot fetch.
    auto result = std::string("https://wsrv.nl/?url=") +
        percentEncode(origin) + "&output=png&maxage=1y";
    return result.size() <= 512U ? result : std::string{};
}

bool validReturnedUrl(const std::string& value, bool qaOverride) noexcept
{
    if (value.empty() || value.size() > 512U ||
        std::any_of(value.begin(), value.end(), [](unsigned char character) {
            return character <= 0x20U || character == 0x7fU;
        })) {
        return false;
    }
    const std::wstring wide(value.begin(), value.end());
    URL_COMPONENTS components{sizeof(components)};
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
            &components) || components.lpszHostName == nullptr ||
            components.dwHostNameLength == 0 ||
            components.lpszUrlPath == nullptr ||
            components.dwUrlPathLength == 0 ||
            components.dwUserNameLength != 0 ||
            components.dwPasswordLength != 0) {
        return false;
    }
    const auto secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const auto qaHttp = qaOverride &&
        components.nScheme == INTERNET_SCHEME_HTTP;
    if (qaHttp) return true;
    if (!secure) return false;
    std::string host;
    host.reserve(components.dwHostNameLength);
    for (DWORD index = 0; index < components.dwHostNameLength; ++index) {
        const auto character = components.lpszHostName[index];
        if (character > 0x7f) return false;
        host.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(character))));
    }
    // Production cache entries are public image assets. Restricting them to
    // the providers Goden actually emits prevents a modified profile from
    // turning Presence into an arbitrary Discord-side fetch primitive. The
    // litter host remains accepted only to migrate valid V1 cache entries.
    return host == "files.catbox.moe" || host == "litter.catbox.moe";
}

std::wstring uploadEndpoint(bool& qaOverride)
{
    qaOverride = false;
    std::array<wchar_t, 2048> value{};
    const auto length = qaHooksAllowed()
        ? GetEnvironmentVariableW(L"NEKOROX_QA_COVER_UPLOAD_URL", value.data(),
            static_cast<DWORD>(value.size()))
        : 0U;
    if (length != 0 && length < value.size()) {
        qaOverride = true;
        return std::wstring(value.data(), length);
    }
    // Litterbox returns one-hour URLs and has repeatedly taken 15-20 seconds
    // to answer in the field. Catbox's permanent endpoint gives the same
    // opt-in artwork a stable URL which Discord's media proxy can reuse.
    return L"https://catbox.moe/user/api.php";
}

std::vector<std::uint8_t> encodePng(
    const ArtPixels& pixels, std::uint32_t edge,
    WICPngFilterOption filter = WICPngFilterUnspecified)
{
    if (pixels.width == 0 || pixels.height == 0 || pixels.bgra.empty() ||
        pixels.width > std::numeric_limits<UINT>::max() / 4U ||
        pixels.bgra.size() < static_cast<std::size_t>(pixels.width) *
            pixels.height * 4U) {
        return {};
    }

    // WIC interfaces below must be released before the matching COM teardown,
    // including on early returns from the encoder. The cover worker need not
    // have initialized COM before this standalone operation.
    struct Apartment {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(initialized)) CoUninitialize(); }
    };
    Apartment apartment;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    const auto stride = pixels.width * 4U;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))) ||
        FAILED(factory->CreateBitmapFromMemory(
            pixels.width, pixels.height, GUID_WICPixelFormat32bppPBGRA,
            stride, static_cast<UINT>(pixels.bgra.size()),
            const_cast<BYTE*>(pixels.bgra.data()),
            bitmap.ReleaseAndGetAddressOf()))) {
        return {};
    }

    const auto square = (std::min)(pixels.width, pixels.height);
    const WICRect cropRect{
        static_cast<INT>((pixels.width - square) / 2U),
        static_cast<INT>((pixels.height - square) / 2U),
        static_cast<INT>(square), static_cast<INT>(square)};
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapClipper(
            clipper.ReleaseAndGetAddressOf())) ||
        FAILED(clipper->Initialize(bitmap.Get(), &cropRect)) ||
        FAILED(factory->CreateBitmapScaler(
            scaler.ReleaseAndGetAddressOf())) ||
        FAILED(scaler->Initialize(clipper.Get(), edge, edge,
            WICBitmapInterpolationModeFant))) {
        return {};
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(
            converter.ReleaseAndGetAddressOf())) ||
        FAILED(converter->Initialize(scaler.Get(),
            GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return {};
    }

    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE,
            stream.ReleaseAndGetAddressOf())) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
            encoder.ReleaseAndGetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
            properties.ReleaseAndGetAddressOf()))) {
        return {};
    }
    if (filter != WICPngFilterUnspecified) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        VARIANT value{};
        value.vt = VT_UI1;
        value.bVal = static_cast<BYTE>(filter);
        if (FAILED(properties->Write(1, &option, &value))) return {};
    }
    if (FAILED(frame->Initialize(properties.Get())) ||
        FAILED(frame->SetSize(edge, edge))) {
        return {};
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(converter.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return {};
    }

    STATSTG state{};
    LARGE_INTEGER beginning{};
    if (FAILED(stream->Stat(&state, STATFLAG_NONAME)) ||
        state.cbSize.QuadPart <= 0 ||
        state.cbSize.QuadPart >
            static_cast<ULONGLONG>(kMaximumEncodedBytes) ||
        FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) {
        return {};
    }

    std::vector<std::uint8_t> result(
        static_cast<std::size_t>(state.cbSize.QuadPart));
    ULONG read = 0;
    if (FAILED(stream->Read(result.data(), static_cast<ULONG>(result.size()),
            &read)) || read != result.size()) {
        result.clear();
    }
    return result;
}

std::string hexadecimal(std::uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::uint32_t elapsedMilliseconds(
    std::chrono::steady_clock::time_point started) noexcept
{
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return static_cast<std::uint32_t>((std::clamp)(
        value, std::int64_t{0},
        static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())));
}

std::uint64_t encodedContentHash(std::uint64_t sourceHash) noexcept
{
    auto value = sourceHash;
    value ^= kCoverEncodingFingerprint + 0x9e3779b97f4a7c15ULL +
        (value << 6U) + (value >> 2U);
    return value == 0 ? kCoverEncodingFingerprint : value;
}

void append(std::vector<std::uint8_t>& output, std::string_view text)
{
    output.insert(output.end(), text.begin(), text.end());
}

std::vector<std::uint8_t> multipartBody(
    std::string_view boundary,
    std::uint64_t contentHash,
    const std::vector<std::uint8_t>& png)
{
    std::vector<std::uint8_t> result;
    result.reserve(png.size() + 1024U);
    const auto field = [&](std::string_view name, std::string_view value) {
        append(result, "--"); append(result, boundary);
        append(result, "\r\nContent-Disposition: form-data; name=\"");
        append(result, name); append(result, "\"\r\n\r\n");
        append(result, value); append(result, "\r\n");
    };
    field("reqtype", "fileupload");
    append(result, "--"); append(result, boundary);
    append(result,
        "\r\nContent-Disposition: form-data; name=\"fileToUpload\"; "
        "filename=\"goden-cover-");
    append(result, hexadecimal(contentHash));
    append(result, ".png\"\r\nContent-Type: image/png\r\n\r\n");
    result.insert(result.end(), png.begin(), png.end());
    append(result, "\r\n--"); append(result, boundary); append(result, "--\r\n");
    return result;
}

} // namespace

DiscordCoverService::DiscordCoverService()
{
    try {
        const auto native = filesystem::g_get_native_path(
            core_api::get_profile_path());
        cachePath_ = std::filesystem::path(utf8ToWide(native.c_str())) /
            L"nekorox" / L"discord-cover-cache-v1.tsv";
    } catch (...) {
        cachePath_.clear();
    }
}

void DiscordCoverService::startRelay()
{
    if (presenceCoverConfiguration()->provider == PresenceCoverProvider::privateRelay &&
            presenceCoverConfiguration()->allowsCovers() &&
            rolloutFlagEnabled(RolloutFlag::presenceV3) &&
            rolloutFlagEnabled(RolloutFlag::presenceEphemeralRelayV1) &&
            DiscordRelayClient::configured()) {
        relay_.start();
    }
}

void DiscordCoverService::disableRelay() noexcept
{
    relay_.disable();
}

DiscordCoverResult DiscordCoverService::resolve(
    const metadb_handle_ptr& track,
    std::uint64_t trackKey,
    std::stop_token stopToken, std::optional<std::uint64_t> admission)
{
    DiscordCoverResult result;
    if (!presenceCoverConfiguration()->allowsCovers()) {
        result.error="cover provider is disabled or unconfigured"; return result;
    }
    const auto requestGeneration = admission.value_or(admissionTicket());
    if (cancelled(stopToken, requestGeneration) || !track.is_valid() ||
            trackKey == 0) {
        result.error = "cancelled";
        return result;
    }

    // A transient network failure must not serialize another artwork decode
    // and WIC encode behind the same request. Prepared bytes are owned solely
    // by the cover worker and remain available for its scheduled retry.
    bool replaceOrigin = false;
    std::optional<std::uint64_t> rejectedHash;
    if (prepared_.trackKey == trackKey && prepared_.contentHash != 0 &&
            !prepared_.png.empty()) {
        const auto cached = cache_.find(prepared_.contentHash);
        const bool retryReplacement = presenceCoverConfiguration()->provider == PresenceCoverProvider::catbox &&
            !prepared_.rejectedOrigin.empty() &&
            cached != cache_.end() && cached->second.url == prepared_.rejectedOrigin &&
            std::chrono::system_clock::now() - cached->second.lastUsed >= 10min;
        result = retryReplacement
            ? upload(prepared_.contentHash, prepared_.png, stopToken, requestGeneration,
                prepared_.pngEncoding)
            : publishPrepared(prepared_.contentHash, prepared_.png, stopToken, requestGeneration,
                prepared_.pngEncoding);
        if (!result.retryWithNewEncoding) {
            if (result.succeeded() && result.source != DiscordCoverResult::Source::relay) prepared_ = {};
            return result;
        }
        replaceOrigin = true;
        rejectedHash = prepared_.contentHash;
        prepared_ = {};
    }

    const auto artworkStarted = std::chrono::steady_clock::now();
    auto artworkAbort = std::make_shared<abort_callback_impl>();
    std::stop_callback stopArtwork(stopToken, [artworkAbort] {
        artworkAbort->abort();
    });
    {
        std::lock_guard lock(artworkAbortMutex_);
        activeArtworkAbort_ = artworkAbort;
    }
    if (cancelled(stopToken, requestGeneration)) artworkAbort->abort();
    const auto pixels = loadResolvedArtworkBlocking(track, 1024, *artworkAbort);
    {
        std::lock_guard lock(artworkAbortMutex_);
        if (activeArtworkAbort_ == artworkAbort) activeArtworkAbort_.reset();
    }
    result.artworkMilliseconds = elapsedMilliseconds(artworkStarted);
    if (cancelled(stopToken, requestGeneration)) {
        result.error = "cancelled";
        return result;
    }
    if (!pixels || pixels->contentHash == 0) {
        result.error = "no local artwork";
        return result;
    }
    const auto legacyContentHash = pixels->contentHash;
    // New uploads include the public encoding contract in their identity.
    // Existing permanent 256 px objects remain visually correct, however, so
    // accept their legacy source hash as a migration fast path rather than
    // re-uploading every cover immediately after upgrade.
    result.contentHash = encodedContentHash(legacyContentHash);
    // Artwork can change while a track retains its key. Rejection of the
    // previous image never authorizes replacement of a different cached one.
    if (rejectedHash && *rejectedHash != result.contentHash) replaceOrigin = false;
    const auto relayEnabled = presenceCoverConfiguration()->provider ==
        PresenceCoverProvider::privateRelay;
    if (relayEnabled) replaceOrigin = false;
    if (!relayEnabled) {
        loadCache();
        pruneCache();
    }
    auto found = cache_.find(result.contentHash);
    if (found == cache_.end()) found = cache_.find(legacyContentHash);
    if (!relayEnabled && found != cache_.end()) {
        const auto qaCache = qaHooksAllowed() &&
            startsWith(found->second.url, "http://127.0.0.1:");
        const auto delivery = discordDeliveryUrl(
            found->second.url, qaCache);
        if (delivery.empty()) {
            result.error = "cover delivery URL was invalid";
            return result;
        }
        const auto verificationStarted = std::chrono::steady_clock::now();
        const auto cachedState = replaceOrigin || verifiedDelivery_.contains(found->first)
            ? CachedCoverState::readable
            : checkCachedCover(found->second, delivery, qaCache, stopToken, requestGeneration);
        if (replaceOrigin || cachedState != CachedCoverState::readable) {
            result.verificationMilliseconds = elapsedMilliseconds(
                verificationStarted);
            if (replaceOrigin || cachedState == CachedCoverState::replaceOrigin) {
                // Retain the rejected URL until replacement bytes exist. A
                // failed encode must not make a retry forget the bad object.
                replaceOrigin = true;
            } else {
                result.error = cancelled(stopToken, requestGeneration)
                    ? "cancelled" : "cover CDN cache was not readable";
                return result;
            }
        } else {
            verifiedDelivery_.insert(found->first);
            found->second.lastUsed = std::chrono::system_clock::now();
            result.url = delivery;
            result.verificationMilliseconds = elapsedMilliseconds(verificationStarted);
            result.source = DiscordCoverResult::Source::cache;
            return result;
        }
    }

    const auto encodeStarted = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> png;
    std::uint32_t encodedEdge = 0;
    for (const auto edge : {kCoverEdge, 384U, 256U}) {
        png = encodePng(*pixels, edge);
        if (!png.empty()) { encodedEdge = edge; break; }
    }
    unsigned pngEncoding = 1;
    if (replaceOrigin && !png.empty()) {
        // Older recovery tried Sub/Up/Paeth without recording its choice.
        // Exclude all of them for untracked entries, then try None once.
        const auto previous = found == cache_.end() || found->second.pngEncoding == 0
            ? 4U : found->second.pngEncoding;
        std::vector<std::vector<std::uint8_t>> rejected;
        rejected.push_back(std::move(png));
        for (unsigned i = 2; i <= previous && i <= kPngEncodings.size(); ++i) {
            rejected.push_back(encodePng(*pixels, encodedEdge, kPngEncodings[i - 1U]));
        }
        png.clear();
        for (unsigned i = previous + 1U; i <= kPngEncodings.size(); ++i) {
            auto candidate = encodePng(*pixels, encodedEdge, kPngEncodings[i - 1U]);
            if (!candidate.empty() && std::find(rejected.begin(), rejected.end(), candidate) == rejected.end()) {
                png = std::move(candidate); pngEncoding = i; break;
            }
        }
        // Identical filter results cannot escape deduplication. Do not lower
        // resolution or cycle back through known rejected representations.
        if (png.empty()) {
            result.error = "cover storage recovery limit reached";
            return result;
        }
    }
    result.encodeMilliseconds = elapsedMilliseconds(encodeStarted);
    if (cancelled(stopToken, requestGeneration)) {
        result.error = "cancelled";
        return result;
    }
    if (png.empty()) {
        result.error = "cover PNG encode failed";
        return result;
    }
    if (replaceOrigin && found != cache_.end()) {
        verifiedDelivery_.erase(found->first);
        verifiedDelivery_.erase(result.contentHash);
    }
    result.encodedBytes = png.size();
    prepared_.trackKey = trackKey;
    prepared_.contentHash = result.contentHash;
    prepared_.png = std::move(png);
    prepared_.rejectedOrigin = replaceOrigin && found != cache_.end() ? found->second.url : std::string{};
    prepared_.pngEncoding = pngEncoding;
    // Keep rejection evidence on disk until upload() saves its acknowledged
    // origin. Cancellation/transport failure must not restore ordinary bytes
    // on restart. Retained replacement bytes also avoid repeated WIC work.
    auto uploaded = replaceOrigin
        ? upload(prepared_.contentHash, prepared_.png, stopToken, requestGeneration,
            prepared_.pngEncoding)
        : publishPrepared(prepared_.contentHash, prepared_.png, stopToken, requestGeneration,
            prepared_.pngEncoding);
    uploaded.artworkMilliseconds = result.artworkMilliseconds;
    uploaded.encodeMilliseconds = result.encodeMilliseconds;
    if (uploaded.succeeded() && uploaded.source != DiscordCoverResult::Source::relay) prepared_ = {};
    return uploaded;
}

DiscordCoverResult DiscordCoverService::publishPrepared(
    std::uint64_t contentHash,
    const std::vector<std::uint8_t>& png,
    std::stop_token stopToken,
    std::uint64_t cancellationGeneration,
    unsigned pngEncoding)
{
    const auto policy = presenceCoverConfiguration();
    if (!policy->allowsCovers()) {
        DiscordCoverResult denied; denied.error="cover provider is disabled or unconfigured"; return denied;
    }
    if (policy->provider == PresenceCoverProvider::privateRelay) {
        DiscordCoverResult result;
        if (!rolloutFlagEnabled(RolloutFlag::presenceV3) ||
                !rolloutFlagEnabled(RolloutFlag::presenceEphemeralRelayV1) ||
                !DiscordRelayClient::configured()) {
            result.error="private relay is unavailable"; return result;
        }
        result.contentHash = contentHash;
        result.encodedBytes = png.size();
        const auto started = std::chrono::steady_clock::now();
        // Capture relay policy before checking the resolver generation. An
        // off/on between those checks must not admit a cancelled old cover.
        const auto ticket = relay_.admissionTicket();
        if (cancelled(stopToken, cancellationGeneration)) {
            result.error = "cancelled";
            return result;
        }
        auto registered = relay_.registerCover(png, stopToken, ticket);
        result.uploadMilliseconds = elapsedMilliseconds(started);
        if (!registered.succeeded()) {
            result.error = registered.error.empty()
                ? "cover relay registration failed" : registered.error;
            return result;
        }
        result.url = std::move(registered.url);
        result.token = std::move(registered.assetId);
        result.source = DiscordCoverResult::Source::relay;
        result.expiresAtMilliseconds=registered.expiresAtMilliseconds;
        result.connectionGeneration=registered.connectionGeneration;
        return result;
    }
    // A retained PNG retry must consult the same durable origin cache as the
    // ordinary resolver. Delivery failure never justifies another upload POST.
    loadCache(); pruneCache();
    const auto cached=cache_.find(contentHash);
    if(cached!=cache_.end()) {
        DiscordCoverResult result; result.contentHash=contentHash;
        const bool fixture=qaHooksAllowed() && startsWith(cached->second.url,"http://127.0.0.1:");
        const auto delivery=discordDeliveryUrl(cached->second.url,fixture);
        if(delivery.empty()) {result.error="cover delivery URL was invalid";return result;}
        const auto cachedState=verifiedDelivery_.contains(contentHash)?CachedCoverState::readable:
            checkCachedCover(cached->second,delivery,fixture,stopToken,cancellationGeneration);
        if(cachedState!=CachedCoverState::readable) {
            if(cachedState==CachedCoverState::replaceOrigin) {
                result.retryWithNewEncoding=true;
                result.error="cover origin needs new encoding";
                return result;
            } else {
                result.error=cancelled(stopToken,cancellationGeneration)?"cancelled":"cover CDN cache was not readable";
                return result;
            }
        } else {
            verifiedDelivery_.insert(contentHash);
            cached->second.lastUsed=std::chrono::system_clock::now();
            result.url=delivery;result.source=DiscordCoverResult::Source::cache;
            return result;
        }
    }
    return upload(contentHash, png, stopToken, cancellationGeneration, pngEncoding);
}

DiscordCoverResult DiscordCoverService::upload(
    std::uint64_t contentHash,
    const std::vector<std::uint8_t>& png,
    std::stop_token stopToken,
    std::uint64_t cancellationGeneration,
    unsigned pngEncoding)
{
    DiscordCoverResult result;
    result.contentHash = contentHash;
    result.encodedBytes = png.size();
    const auto policy = presenceCoverConfiguration();
    if (policy->provider != PresenceCoverProvider::catbox || !policy->allowsCovers()) {
        result.error = "public cover provider is disabled or unconfigured";
        return result;
    }
    const auto uploadStarted = std::chrono::steady_clock::now();
    if (cancelled(stopToken, cancellationGeneration)) {
        result.error = "cancelled";
        return result;
    }
    bool qaOverride = false;
    const auto endpoint = uploadEndpoint(qaOverride);
    URL_COMPONENTS components{sizeof(components)};
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(endpoint.c_str(), 0, 0, &components) ||
        components.lpszHostName == nullptr || components.dwHostNameLength == 0) {
        result.error = "invalid cover upload endpoint";
        return result;
    }

    const std::wstring host(components.lpszHostName,
        components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath,
        components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    InternetHandle session;
    InternetHandle connection;
    const auto accessType = preferAutomaticProxy_
        ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
        : WINHTTP_ACCESS_TYPE_NO_PROXY;
    session.value = WinHttpOpen(L"Goden-foobar2000/4.0",
        accessType, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (session.value == nullptr) {
        result.error = "WinHTTP initialization failed";
        return result;
    }
    // Consumer networks overwhelmingly connect directly. Avoid a cold WPAD /
    // PAC discovery on every cover; a bounded automatic-proxy fallback is
    // handled by the cover worker after a direct failure.
    WinHttpSetTimeouts(session.value, 1500, 2000, 3000, 5000);
    connection.value = WinHttpConnect(session.value, host.c_str(),
        components.nPort, 0);
    if (connection.value == nullptr ||
            cancelled(stopToken, cancellationGeneration)) {
        if (!qaOverride && !preferAutomaticProxy_ &&
                !cancelled(stopToken, cancellationGeneration)) {
            preferAutomaticProxy_ = true;
        }
        result.error = cancelled(stopToken, cancellationGeneration)
            ? "cancelled" : "cover host connection failed";
        return result;
    }

    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE : 0;
    std::stop_source networkStop;
    {
        std::lock_guard lock(artworkAbortMutex_);
        if (cancelled(stopToken, cancellationGeneration)) {
            result.error = "cancelled";
            return result;
        }
        activeNetworkStop_ = networkStop;
    }
    std::stop_callback cancelNetwork(stopToken, [networkStop]() mutable {
        networkStop.request_stop();
    });
    const auto requestToken = networkStop.get_token();
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    // Declare send bytes before the handle: every exit, including exceptions,
    // drains the final callback before the body can be destroyed.
    const auto boundary = "----------------goden" + hexadecimal(contentHash);
    auto body = multipartBody(boundary, contentHash, png);
    const auto headers = L"Content-Type: multipart/form-data; boundary=" +
        std::wstring(boundary.begin(), boundary.end());
    DiscordHttpIo request;
    std::string response;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS;
    const bool uploadSucceeded =
        request.attach(WinHttpOpenRequest(connection.value, L"POST",
            path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) &&
        // An opted-in upload belongs only to the selected provider. Never
        // forward its body to a redirect target (including another origin).
        WinHttpSetOption(request.handle(), WINHTTP_OPTION_DISABLE_FEATURE,
            &disabledFeatures, sizeof(disabledFeatures)) &&
        body.size() <= std::numeric_limits<DWORD>::max() &&
        request.perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, requestToken, deadline, [&] {
            return DiscordHttpIo::accepted(WinHttpSendRequest(request.handle(), headers.c_str(),
                static_cast<DWORD>(-1), body.data(), static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()), request.contextValue()));
        }) && request.perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, requestToken, deadline, [&] {
            return DiscordHttpIo::accepted(WinHttpReceiveResponse(request.handle(), nullptr));
        }) && WinHttpQueryHeaders(request.handle(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
        status >= 200U && status < 300U &&
        request.readResponse(response, requestToken, deadline, kMaximumResponseBytes);
    request.close();
    if (cancelled(stopToken, cancellationGeneration)) {
        result.error = "cancelled";
        return result;
    }
    if (!uploadSucceeded) {
        if (!qaOverride && !preferAutomaticProxy_) {
            // Retry through the system proxy only after the fast direct route
            // has actually failed. The prepared PNG is retained, so no
            // artwork decode or encode is repeated.
            preferAutomaticProxy_ = true;
        }
        result.error = "cover upload failed";
        return result;
    }

    response = trimAscii(std::move(response));
    if (response.empty()) {
        result.error = "cover host returned an empty upload response";
        return result;
    }
    if (!validReturnedUrl(response, qaOverride)) {
        result.error = "cover host returned an invalid URL";
        return result;
    }
    const auto originUrl = response;
    const auto uploadedAt=std::chrono::system_clock::now();
    cache_[contentHash]={originUrl,std::chrono::system_clock::time_point::max(),uploadedAt,pngEncoding};
    pruneCache();
    saveCache();
    const auto delivery = discordDeliveryUrl(originUrl, qaOverride);
    if (delivery.empty()) {
        result.error = "cover delivery URL was invalid";
        return result;
    }
    // Verify the stored origin as well as the CDN. A proxy can decode a
    // truncated source into a complete but visibly damaged image, hiding the
    // original corruption. Never publish that merely because the proxy works.
    const auto verificationStarted = std::chrono::steady_clock::now();
    auto backoff = 100ms;
    const auto verificationDeadline = verificationStarted + 4s;
    bool verified = false;
    bool originVerified = false;
    while (!cancelled(stopToken, cancellationGeneration) &&
            std::chrono::steady_clock::now() < verificationDeadline) {
        if (!originVerified) originVerified = verifyRemotePng(originUrl, qaOverride,
            stopToken, cancellationGeneration, nullptr, nullptr, verificationDeadline);
        if (originVerified && (delivery == originUrl || verifyRemotePng(delivery, qaOverride, stopToken,
                cancellationGeneration, nullptr, nullptr, verificationDeadline))) {
            verified = true;
            break;
        }
        const auto remaining = verificationDeadline -
            std::chrono::steady_clock::now();
        if (remaining <= 0ms) break;
        const auto delay = (std::min)(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
            backoff);
        const auto until = std::chrono::steady_clock::now() + delay;
        while (!cancelled(stopToken, cancellationGeneration) &&
                std::chrono::steady_clock::now() < until) {
            Sleep(20);
        }
        backoff = (std::min)(800ms, backoff * 2);
    }
    result.verificationMilliseconds = elapsedMilliseconds(
        verificationStarted);
    if (cancelled(stopToken, cancellationGeneration)) {
        result.error = "cancelled";
        return result;
    }
    if (!verified) {
        result.error = originVerified ? "cover CDN cache was not readable after upload"
            : "cover origin was not complete after upload";
        return result;
    }
    result.url = delivery;
    verifiedDelivery_.insert(contentHash);
    result.source = DiscordCoverResult::Source::upload;
    result.uploadMilliseconds = elapsedMilliseconds(uploadStarted);
    const auto now = std::chrono::system_clock::now();
    cache_[contentHash] = {originUrl,
        std::chrono::system_clock::time_point::max(), now, pngEncoding};
    pruneCache();
    saveCache();
    return result;
}

bool DiscordCoverService::verifyRemotePng(
    const std::string& url, bool qaOverride, std::stop_token stopToken,
    std::uint64_t cancellationGeneration, unsigned* httpStatus, bool* brokenImage,
    std::chrono::steady_clock::time_point totalDeadline)
{
    unsigned status = 0;
    bool transportFailed = false;
    auto verified = verifyRemotePngOnce(url, qaOverride, stopToken,
        cancellationGeneration, &status, &transportFailed, brokenImage, totalDeadline);
    // Cached covers may be the very first request after a restart: there is
    // no POST failure to select the system proxy. Retry only transport failures
    // once, at the same URL, with the same cancellation and per-read deadline.
    // HTTP failures (including 404/429) retain their existing cache policy.
    if (!verified && transportFailed && !qaOverride && !preferAutomaticProxy_ &&
            !cancelled(stopToken, cancellationGeneration) &&
            std::chrono::steady_clock::now() < totalDeadline) {
        preferAutomaticProxy_ = true;
        verified = verifyRemotePngOnce(url, qaOverride, stopToken,
            cancellationGeneration, &status, &transportFailed, brokenImage, totalDeadline);
    }
    if (httpStatus) *httpStatus = status;
    return verified;
}

bool DiscordCoverService::verifyRemotePngOnce(
    const std::string& url,
    bool qaOverride,
    std::stop_token stopToken,
    std::uint64_t cancellationGeneration,
    unsigned* httpStatus, bool* transportFailed, bool* brokenImage,
    std::chrono::steady_clock::time_point totalDeadline)
{
    *transportFailed = true;
    if (httpStatus) *httpStatus = 0;
    if (brokenImage) *brokenImage = false;
    if (cancelled(stopToken, cancellationGeneration)) return false;
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS components{sizeof(components)};
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
            &components) || components.lpszHostName == nullptr ||
            components.dwHostNameLength == 0) {
        return false;
    }
    const std::wstring host(components.lpszHostName,
        components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath,
        components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    InternetHandle session;
    InternetHandle connection;
    const auto accessType = !qaOverride && preferAutomaticProxy_
        ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
        : WINHTTP_ACCESS_TYPE_NO_PROXY;
    session.value = WinHttpOpen(L"Goden-foobar2000/4.0",
        accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC);
    if (session.value == nullptr) return false;
    WinHttpSetTimeouts(session.value, 750, 1000, 1250, 1500);
    connection.value = WinHttpConnect(session.value, host.c_str(),
        components.nPort, 0);
    if (connection.value == nullptr ||
            cancelled(stopToken, cancellationGeneration)) {
        return false;
    }

    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE : 0;
    std::stop_source networkStop;
    {
        std::lock_guard lock(artworkAbortMutex_);
        if (cancelled(stopToken, cancellationGeneration)) return false;
        activeNetworkStop_ = networkStop;
    }
    std::stop_callback cancelNetwork(stopToken, [networkStop]() mutable {
        networkStop.request_stop();
    });
    const auto requestToken = networkStop.get_token();
    const auto deadline = (std::min)(totalDeadline, std::chrono::steady_clock::now() + 1500ms);
    DiscordHttpIo request;
    if (!request.attach(WinHttpOpenRequest(connection.value, L"GET",
            path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) ||
        !request.perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, requestToken, deadline, [&] {
            return DiscordHttpIo::accepted(WinHttpSendRequest(request.handle(),
                WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, request.contextValue()));
        }) || !request.perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, requestToken, deadline, [&] {
            return DiscordHttpIo::accepted(WinHttpReceiveResponse(request.handle(), nullptr));
        })) return false;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) return false;
    if (httpStatus) *httpStatus = status;
    if (status < 200U || status >= 300U) {
        *transportFailed = false;
        return false;
    }
    DWORD contentLength = 0;
    DWORD contentLengthSize = sizeof(contentLength);
    const bool lengthKnown =
        WinHttpQueryHeaders(request.handle(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX);
    if (lengthKnown && contentLength > kMaximumEncodedBytes) {
        *transportFailed = false;
        return false;
    }
    std::string image;
    if (!request.readResponse(image, requestToken, deadline, kMaximumEncodedBytes)) return false;
    request.close();
    *transportFailed = false;
    if (cancelled(stopToken, cancellationGeneration) ||
            std::chrono::steady_clock::now() >= deadline ||
            (lengthKnown && contentLength != image.size())) return false;
    const auto complete = completePngStructure(image);
    if (cancelled(stopToken, cancellationGeneration) ||
            std::chrono::steady_clock::now() >= deadline) return false;
    // Only a complete, exact-length HTTP 200 containing an empty object or
    // structurally broken PNG can authorize repair. Unknown-length EOF, HTML,
    // timeouts and short network reads preserve the existing cache entry.
    if (brokenImage) *brokenImage = status == 200U && lengthKnown && !complete &&
        (image.empty() || hasPngSignature(image));
    return complete;
}

DiscordCoverService::CachedCoverState DiscordCoverService::checkCachedCover(
    const CachedUrl& cached, const std::string& delivery, bool qaOverride,
    std::stop_token stopToken, std::uint64_t cancellationGeneration)
{
    // A healthy CDN response is insufficient: it may conceal missing source
    // pixels. Validate the exact origin once, then pre-warm the delivery URL.
    // The grace period limits POST repair, never origin validation itself.
    unsigned status = 0;
    bool brokenImage = false;
    if (!verifyRemotePng(cached.url, qaOverride, stopToken, cancellationGeneration,
            &status, &brokenImage)) {
        if (!cancelled(stopToken, cancellationGeneration) &&
                std::chrono::system_clock::now() - cached.lastUsed >= 10min &&
                (status == 404U || status == 410U || brokenImage))
            return CachedCoverState::replaceOrigin;
        return CachedCoverState::unavailable;
    }
    return (delivery == cached.url || verifyRemotePng(delivery, qaOverride, stopToken, cancellationGeneration))
        ? CachedCoverState::readable : CachedCoverState::unavailable;
}

void DiscordCoverService::cancel() noexcept
{
    cancellationGeneration_.fetch_add(1, std::memory_order_acq_rel);
    std::shared_ptr<abort_callback_impl> artworkAbort;
    std::stop_source networkStop(std::nostopstate);
    {
        std::lock_guard lock(artworkAbortMutex_);
        artworkAbort = activeArtworkAbort_;
        networkStop = activeNetworkStop_;
    }
    if (artworkAbort) artworkAbort->abort();
    networkStop.request_stop();

}

void DiscordCoverService::clear() noexcept
{
    relay_.stop();
    cache_.clear();
    verifiedDelivery_.clear();
    prepared_ = {};
    // The persisted entries are public temporary URLs with their original
    // expiry. Keeping them avoids uploading the same cover again after a
    // player restart; no network or worker remains active while Presence is
    // disabled. Reload lazily on the next explicit opt-in.
    cacheLoaded_ = false;
}

DiscordRelayTrace DiscordCoverService::relayTrace() const
{
    return relay_.trace();
}

bool DiscordCoverService::cancelled(
    std::stop_token stopToken,
    std::uint64_t cancellationGeneration) const noexcept
{
    return stopToken.stop_requested() ||
        cancellationGeneration_.load(std::memory_order_acquire) !=
            cancellationGeneration;
}

void DiscordCoverService::pruneCache()
{
    const auto now = std::chrono::system_clock::now();
    std::erase_if(cache_, [&](const auto& entry) {
        if (entry.second.expires > now) return false;
        verifiedDelivery_.erase(entry.first);
        return true;
    });
    while (cache_.size() > kMaximumCacheEntries) {
        const auto oldest = std::min_element(cache_.begin(), cache_.end(),
            [](const auto& left, const auto& right) {
                return left.second.lastUsed < right.second.lastUsed;
            });
        if (oldest == cache_.end()) break;
        verifiedDelivery_.erase(oldest->first);
        cache_.erase(oldest);
    }
}

void DiscordCoverService::loadCache()
{
    if (cacheLoaded_) return;
    cacheLoaded_ = true;
    if (cachePath_.empty()) return;

    std::error_code error;
    const auto bytes = std::filesystem::file_size(cachePath_, error);
    if (error || bytes == 0 || bytes > kMaximumCacheFileBytes) return;
    std::ifstream input(cachePath_, std::ios::binary);
    if (!input) return;
    std::string line;
    if (!std::getline(input, line)) return;
    const auto header = trimAscii(line);
    const auto v1 = header == kCacheHeaderV1;
    const auto v2 = header == kCacheHeaderV2;
    if (!v1 && !v2) return;

    const auto now = std::chrono::system_clock::now();
    for (std::size_t count = 0;
            count < kMaximumCacheEntries * 2U && std::getline(input, line);
            ++count) {
        if (startsWith(line, kPngEncodingRecord)) {
            const auto separator = line.find('\t', kPngEncodingRecord.size());
            if (separator == std::string::npos) continue;
            try {
                const auto hashText = line.substr(kPngEncodingRecord.size(), separator - kPngEncodingRecord.size());
                const auto encodingText = trimAscii(line.substr(separator + 1U));
                std::size_t hashRead = 0, encodingRead = 0;
                const auto hash = std::stoull(hashText, &hashRead, 16);
                const auto encoding = std::stoul(encodingText, &encodingRead);
                const auto entry = cache_.find(hash);
                if (hashRead == hashText.size() && encodingRead == encodingText.size() &&
                        encoding >= 1 && encoding <= kPngEncodings.size() && entry != cache_.end()) {
                    entry->second.pngEncoding = static_cast<unsigned>(encoding);
                }
            } catch (...) {}
            continue;
        }
        const auto first = line.find('\t');
        const auto second = first == std::string::npos
            ? std::string::npos : line.find('\t', first + 1U);
        if (first == std::string::npos || second == std::string::npos) continue;
        try {
            const auto hash = std::stoull(line.substr(0, first), nullptr, 16);
            const auto expiresUnix = std::stoll(
                line.substr(first + 1U, second - first - 1U));
            auto lastUsed = now;
            auto urlOffset = second + 1U;
            if (v2) {
                const auto third = line.find('\t', urlOffset);
                if (third == std::string::npos) continue;
                const auto lastUsedUnix = std::stoll(
                    line.substr(urlOffset, third - urlOffset));
                lastUsed = std::chrono::system_clock::time_point(
                    std::chrono::seconds(lastUsedUnix));
                urlOffset = third + 1U;
            }
            auto url = trimAscii(line.substr(urlOffset));
            const auto expires = expiresUnix == 0
                ? std::chrono::system_clock::time_point::max()
                : std::chrono::system_clock::time_point(
                    std::chrono::seconds(expiresUnix));
            if (hash != 0 && expires > now && lastUsed <= now + 24h &&
                    validReturnedUrl(url, false)) {
                cache_[hash] = {std::move(url), expires, lastUsed};
            }
        } catch (...) {
        }
    }
    pruneCache();
}

void DiscordCoverService::saveCache() const
{
    if (cachePath_.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(cachePath_.parent_path(), error);
    if (error) return;
    auto temporary = cachePath_;
    temporary += L".tmp";
    {
        std::ofstream output(temporary,
            std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << kCacheHeaderV2 << '\n';
        for (const auto& [hash, cached] : cache_) {
            const auto expires = cached.expires ==
                    std::chrono::system_clock::time_point::max()
                ? 0
                : std::chrono::duration_cast<std::chrono::seconds>(
                    cached.expires.time_since_epoch()).count();
            const auto lastUsed = std::chrono::duration_cast<
                std::chrono::seconds>(cached.lastUsed.time_since_epoch()).count();
            output << hexadecimal(hash) << '\t' << expires << '\t'
                   << lastUsed << '\t'
                   << cached.url << '\n';
            if (cached.pngEncoding >= 1 && cached.pngEncoding <= kPngEncodings.size()) {
                output << kPngEncodingRecord << hexadecimal(hash) << '\t'
                       << cached.pngEncoding << '\n';
            }
        }
        output.flush();
        if (!output) return;
    }
    if (!MoveFileExW(temporary.c_str(), cachePath_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
    }
}

} // namespace nekorox::component
