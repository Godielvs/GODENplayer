#include "art_cache.h"
#include "coalesced_resource_cache.h"
#include "a4/a4_material_cache.h"

#include "nekorox/artwork_resampler.h"
#include "nekorox/ambient_features.h"
#include "text_util.h"
#include "rollout_flags.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kMaximumTransientAttempts = 3;
constexpr std::array kBusyRetryDelays{
    std::chrono::milliseconds(100),
    std::chrono::milliseconds(250),
    std::chrono::milliseconds(600)};
constexpr std::array kTransientRetryDelays{
    std::chrono::milliseconds(500),
    std::chrono::milliseconds(1000),
    std::chrono::milliseconds(2000)};
constexpr std::size_t kMaximumEncodedBytes = 32U * 1024U * 1024U;
constexpr std::uint64_t kMaximumSourcePixels = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kLooseFolderAudioLimit = 64;
constexpr std::size_t kLooseFolderImageLimit = 32;
std::mutex g_albumArtLoaderMutex;

struct TemporaryArtworkBudget {
    std::mutex mutex;
    std::size_t bytes = 0;
    std::size_t peakBytes = 0;
    std::uint64_t rejected = 0;
};

TemporaryArtworkBudget g_temporaryArtworkBudget;
struct ArtworkBudgetBusy {};

struct ArtworkLoadCounters {
    std::atomic<std::uint64_t> loadRequests{0};
    std::atomic<std::uint64_t> localPaths{0};
    std::atomic<std::uint64_t> sidecarCandidates{0};
    std::atomic<std::uint64_t> fileReads{0};
    std::atomic<std::uint64_t> decodeAttempts{0};
    std::atomic<std::uint64_t> decodeSuccesses{0};
    std::atomic<std::uint64_t> resampleAttempts{0};
    std::atomic<std::uint64_t> resampleMemoHits{0};
};

ArtworkLoadCounters g_artworkLoadCounters;

// Weak reuse across aliases: the broker owns the resident byte budget.
using DecodedArtworkKey = std::tuple<std::uint64_t, UINT, UINT, ArtAnalysisFlags>;
std::mutex g_decodedArtworkMutex;
std::map<DecodedArtworkKey, std::weak_ptr<ArtPixels>> g_decodedArtwork;
using ArtworkFlightKey = std::tuple<std::uint64_t, std::size_t, std::uint32_t, ArtAnalysisFlags>;
CoalescedResourceCache<ArtworkFlightKey, ArtPixels> g_artworkFlights;
// Weak entries never own resident bytes outside the broker budget. Source
// dimensions and material revision distinguish different decode/profile paths.
using FittedArtworkKey = std::tuple<std::uint64_t, UINT, UINT, UINT, UINT, std::uint64_t>;
std::mutex g_fittedArtworkMutex;
std::map<FittedArtworkKey, std::weak_ptr<ArtPixels>> g_fittedArtwork;

class TemporaryArtworkReservation final {
public:
    TemporaryArtworkReservation() = default;
    ~TemporaryArtworkReservation() { release(); }
    TemporaryArtworkReservation(const TemporaryArtworkReservation&) = delete;
    TemporaryArtworkReservation& operator=(
        const TemporaryArtworkReservation&) = delete;

    bool add(std::size_t bytes) noexcept
    {
        if (bytes == 0) return true;
        std::lock_guard lock(g_temporaryArtworkBudget.mutex);
        if (!artworkBudgetCanAdd(g_temporaryArtworkBudget.bytes, bytes,
                kArtworkTemporaryBudgetBytes)) {
            ++g_temporaryArtworkBudget.rejected;
            return false;
        }
        g_temporaryArtworkBudget.bytes += bytes;
        held_ += bytes;
        g_temporaryArtworkBudget.peakBytes = std::max(
            g_temporaryArtworkBudget.peakBytes,
            g_temporaryArtworkBudget.bytes);
        return true;
    }

private:
    void release() noexcept
    {
        if (held_ == 0) return;
        std::lock_guard lock(g_temporaryArtworkBudget.mutex);
        g_temporaryArtworkBudget.bytes -= std::min(
            g_temporaryArtworkBudget.bytes, held_);
        held_ = 0;
    }

    std::size_t held_ = 0;
};

constexpr std::array<std::wstring_view, 6> kArtworkExtensions{
    L".jpg", L".jpeg", L".png", L".webp", L".bmp", L".gif"};
constexpr std::array<std::wstring_view, 11> kAudioExtensions{
    L".flac", L".mp3", L".wav", L".m4a", L".aac", L".ogg",
    L".opus", L".wma", L".ape", L".wv", L".mka"};

class EmbeddedOnlyAlbumArtConfig : public album_art_manager_config {
public:
    bool get_external_pattern(
        pfc::string_base& output,
        const GUID&) override
    {
        output.reset();
        return false;
    }

    bool use_embedded_pictures() override { return true; }
    bool use_fallbacks() override { return false; }
};

struct TextLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
    {
        return CompareStringOrdinal(
                   left.data(), static_cast<int>(left.size()),
                   right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

bool equalText(std::wstring_view left, std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
        CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

template <std::size_t Size>
bool hasExtension(
    const std::filesystem::path& path,
    const std::array<std::wstring_view, Size>& extensions) noexcept
{
    const auto extension = path.extension().wstring();
    return std::any_of(extensions.begin(), extensions.end(),
        [&](std::wstring_view candidate) {
            return equalText(extension, candidate);
        });
}

std::filesystem::path localTrackPath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        // foobar paths are virtual filesystem URLs, not necessarily a literal
        // "file://" prefix followed by a Windows path. Let the SDK resolve the
        // active filesystem implementation; hand-parsing rejects valid local
        // handles such as the canonical file:// URL returned by foobar 2.x.
        const auto native = filesystem::g_get_native_path(track->get_path());
        if (native.is_empty()) return {};
        return std::filesystem::path(utf8ToWide(native.c_str()));
    } catch (...) {
        return {};
    }
}

std::wstring metadataValue(
    const metadb_handle_ptr& track,
    const char* field)
{
    if (!track.is_valid()) return {};
    const auto container = track->get_info_ref();
    if (!container.is_valid()) return {};
    const auto& info = container->info();
    const auto count = info.meta_get_count_by_name(field);
    for (t_size index = 0; index < count; ++index) {
        const auto* value = info.meta_get(field, index);
        if (value == nullptr) continue;
        auto converted = trimText(utf8ToWide(value));
        if (!converted.empty()) return converted;
    }
    return {};
}

bool validFileStem(std::wstring_view value) noexcept
{
    if (value.empty() || value.back() == L'.' || value.back() == L' ') {
        return false;
    }
    constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    return value.find_first_of(invalid) == std::wstring_view::npos;
}

void appendUniqueStem(
    std::vector<std::wstring>& stems,
    std::wstring value)
{
    value = trimText(std::move(value));
    if (!validFileStem(value)) return;
    const auto exists = std::any_of(stems.begin(), stems.end(),
        [&](const std::wstring& candidate) {
            return equalText(candidate, value);
        });
    if (!exists) stems.push_back(std::move(value));
}

std::vector<std::wstring> specificArtworkStems(
    const metadb_handle_ptr& track,
    const std::filesystem::path& path)
{
    std::vector<std::wstring> stems;
    appendUniqueStem(stems, path.stem().wstring());

    const auto artist = metadataValue(track, "artist");
    const auto title = metadataValue(track, "title");
    const auto albumArtist = metadataValue(track, "album artist");
    const auto album = metadataValue(track, "album");
    if (!artist.empty() && !title.empty()) {
        appendUniqueStem(stems, artist + L" - " + title);
    }
    if (!albumArtist.empty() && !album.empty()) {
        appendUniqueStem(stems, albumArtist + L" - " + album);
    } else if (!artist.empty() && !album.empty()) {
        appendUniqueStem(stems, artist + L" - " + album);
    }
    appendUniqueStem(stems, album);
    return stems;
}

bool allowGenericFolderArtwork(const std::filesystem::path& directory, abort_callback* abort)
{
    if (directory.empty() || directory == directory.root_path()) return false;

    std::size_t audioFiles = 0;
    std::size_t imageFiles = 0;
    std::size_t entries = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
            !error && iterator != end; iterator.increment(error)) {
        if (abort) abort->check();
        // Generic folder names are a fallback, never a reason to occupy a
        // cover worker indefinitely in a download root with unrelated files.
        if (++entries > 4096) return false;
        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        const auto& path = iterator->path();
        if (hasExtension(path, kAudioExtensions)) {
            if (++audioFiles > kLooseFolderAudioLimit) return false;
        } else if (hasExtension(path, kArtworkExtensions)) {
            if (++imageFiles > kLooseFolderImageLimit) return false;
        }
    }
    return !error;
}

std::uint64_t encodedContentHash(
    const std::uint8_t* bytes,
    std::size_t byteCount, abort_callback* abort)
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset;
    for (std::size_t index = 0; index < byteCount; ++index) {
        if ((index & 65535U) == 0 && abort) abort->check();
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

std::size_t residentPixelBytes(const ArtPixels& pixels) noexcept
{
    return pixels.bgra.size();
}

std::shared_ptr<ArtPixels> decodeBytes(
    const std::uint8_t* bytes,
    std::size_t byteCount,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    bool encodedBytesAlreadyReserved = false,
    abort_callback* abort = nullptr)
{
    if (abort) abort->check();
    if (bytes == nullptr || byteCount == 0 ||
        byteCount > kMaximumEncodedBytes ||
        byteCount > std::numeric_limits<DWORD>::max()) {
        return {};
    }
    g_artworkLoadCounters.decodeAttempts.fetch_add(1, std::memory_order_relaxed);

    TemporaryArtworkReservation reservation;
    if (!encodedBytesAlreadyReserved && !reservation.add(byteCount)) throw ArtworkBudgetBusy{};

    const auto contentHash = encodedContentHash(bytes, byteCount, abort);
    auto flight = g_artworkFlights.acquire(
        ArtworkFlightKey{contentHash, byteCount, decodeEdge, analysis},
        [abort] { if (abort) abort->check(); });
    if (flight.cached()) return flight.cached();

    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    struct Apartment {
        bool owned = false;
        ~Apartment() { if (owned) CoUninitialize(); }
    } apartment{uninitialize};

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) {
        return {};
    }

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateStream(stream.ReleaseAndGetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(bytes), static_cast<DWORD>(byteCount))) ||
        FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
            decoder.ReleaseAndGetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()))) {
        return {};
    }
    if (abort) abort->check();

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0 ||
        static_cast<std::uint64_t>(sourceWidth) * sourceHeight >
            kMaximumSourcePixels) {
        return {};
    }

    const auto scale = std::min(
        1.0,
        static_cast<double>(std::max<std::uint32_t>(1, decodeEdge)) /
            static_cast<double>(std::max(sourceWidth, sourceHeight)));
    const auto width = std::max<UINT>(1, static_cast<UINT>(std::round(sourceWidth * scale)));
    const auto height = std::max<UINT>(1, static_cast<UINT>(std::round(sourceHeight * scale)));
    const auto decodedBytes = artworkBgraBytes(width, height);
    if (decodedBytes == 0) return {};
    // Detect requests that cannot fit by themselves before the expensive
    // decode. Contention is retryable; an intrinsically oversized job is not.
    const auto paletteScaleForBudget = std::min(1.0,256.0/std::max(width,height));
    const auto paletteBytesForBudget = artworkBgraBytes(
        std::max(1U,static_cast<UINT>(std::round(width*paletteScaleForBudget))),
        std::max(1U,static_cast<UINT>(std::round(height*paletteScaleForBudget))));
    if (!artworkDecodeFitsBudget(byteCount,decodedBytes,paletteBytesForBudget)) return {};
    const DecodedArtworkKey decodedKey{contentHash, width, height, analysis};
    {
        std::lock_guard lock(g_decodedArtworkMutex);
        if (const auto found = g_decodedArtwork.find(decodedKey); found != g_decodedArtwork.end()) {
            if (auto reused = found->second.lock()) {
                flight.publish(reused);
                return reused;
            }
        }
    }
    // Admit the complete decode before CopyPixels. A competing job must not
    // make us discard an image already decoded when the palette copy starts.
    // The intrinsic fit check above also bounds this sum against overflow.
    if (!reservation.add(decodedBytes * 2U + paletteBytesForBudget))
        throw ArtworkBudgetBusy{};

    ComPtr<IWICBitmapSource> originalSource;
    if (FAILED(frame.As(&originalSource))) {
        return {};
    }
    ComPtr<IWICBitmapSource> source = originalSource;
    ComPtr<IWICBitmapScaler> scaler;
    if (width != sourceWidth || height != sourceHeight) {
        if (FAILED(factory->CreateBitmapScaler(scaler.ReleaseAndGetAddressOf())) ||
            FAILED(scaler->Initialize(
                source.Get(), width, height, WICBitmapInterpolationModeFant))) {
            return {};
        }
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
        FAILED(converter->Initialize(
            source.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        return {};
    }

    auto result = std::make_shared<ArtPixels>();
    result->width = width;
    result->height = height;
    result->contentHash = contentHash;
    const auto stride = width * 4U;
    result->bgra.resize(static_cast<std::size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(result->bgra.size()),
            result->bgra.data()))) {
        result.reset();
    }
    if (abort) abort->check();
    if (result) {
        if (abort) abort->check();
        constexpr UINT kPaletteEdge = 256U;
        const auto paletteScale = std::min(1.0,
            static_cast<double>(kPaletteEdge) /
                static_cast<double>(std::max(width, height)));
        const auto paletteWidth = std::max<UINT>(1U,
            static_cast<UINT>(std::round(width * paletteScale)));
        const auto paletteHeight = std::max<UINT>(1U,
            static_cast<UINT>(std::round(height * paletteScale)));
        const auto paletteBytes = artworkBgraBytes(paletteWidth, paletteHeight);
        std::vector<std::uint8_t> palettePixels;
        ComPtr<IWICBitmap> decodedBitmap;
        auto paletteSourceReady = SUCCEEDED(factory->CreateBitmapFromMemory(width, height,
            GUID_WICPixelFormat32bppPBGRA, stride, static_cast<UINT>(result->bgra.size()),
            result->bgra.data(), decodedBitmap.ReleaseAndGetAddressOf()));
        ComPtr<IWICBitmapSource> paletteSource = decodedBitmap;
        ComPtr<IWICBitmapScaler> paletteScaler;
        ComPtr<IWICFormatConverter> paletteConverter;
        if (paletteSourceReady && (paletteWidth != width || paletteHeight != height)) {
            paletteSourceReady = SUCCEEDED(factory->CreateBitmapScaler(
                paletteScaler.ReleaseAndGetAddressOf())) &&
                SUCCEEDED(paletteScaler->Initialize(decodedBitmap.Get(),
                    paletteWidth, paletteHeight,
                    WICBitmapInterpolationModeFant));
            if (paletteSourceReady) paletteSource = paletteScaler;
        }
        if (paletteSourceReady && paletteBytes != 0 &&
            SUCCEEDED(factory->CreateFormatConverter(
                paletteConverter.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(paletteConverter->Initialize(paletteSource.Get(),
                GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            palettePixels.resize(paletteBytes);
            if (FAILED(paletteConverter->CopyPixels(nullptr,
                    paletteWidth * 4U, static_cast<UINT>(palettePixels.size()),
                    palettePixels.data()))) {
                palettePixels.clear();
            }
        }
        if (!palettePixels.empty()) {
            result->palette = extractArtPalette(palettePixels,
                paletteWidth, paletteHeight, paletteWidth * 4U);
        } else {
            result->palette = extractArtPalette(
                result->bgra, result->width, result->height, result->width * 4U);
        }
        result->material = !palettePixels.empty()
            ? a4::materialFor(palettePixels, paletteWidth, paletteHeight, result->contentHash,
                analysis == ArtAnalysisFlags::alloyV2)
            : a4::materialFor(result->bgra, result->width, result->height, result->contentHash,
                analysis == ArtAnalysisFlags::alloyV2);
        result->palettePrepared = true;
    }
    if (abort) abort->check();
    if (result) {
        g_artworkLoadCounters.decodeSuccesses.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(g_decodedArtworkMutex);
        if (g_decodedArtwork.size() >= kArtworkMaximumEntries * 2) {
            std::erase_if(g_decodedArtwork, [](const auto& entry) { return entry.second.expired(); });
            if (g_decodedArtwork.size() >= kArtworkMaximumEntries * 2) g_decodedArtwork.erase(g_decodedArtwork.begin());
        }
        g_decodedArtwork[decodedKey] = result;
    }
    flight.publish(result);
    return result;
}

std::shared_ptr<ArtPixels> decodeArt(
    const album_art_data_ptr& data,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort = nullptr)
{
    if (!data.is_valid()) return {};
    return decodeBytes(
        static_cast<const std::uint8_t*>(data->get_ptr()),
        data->get_size(),
        decodeEdge, analysis, false, abort);
}

std::shared_ptr<ArtPixels> loadArtFile(
    const std::filesystem::path& path,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort = nullptr);

std::shared_ptr<ArtPixels> loadSpecificSidecarArt(
    const metadb_handle_ptr& track,
    const std::filesystem::path& path,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort = nullptr)
{
    if (abort) abort->check();
    const auto directory = path.parent_path();
    if (directory.empty()) return {};
    for (const auto& stem : specificArtworkStems(track, path)) {
        for (const auto extension : kArtworkExtensions) {
            if (abort) abort->check();
            auto candidate = directory / (stem + std::wstring(extension));
            std::error_code error;
            if (!std::filesystem::is_regular_file(candidate, error)) continue;
            g_artworkLoadCounters.sidecarCandidates.fetch_add(
                1, std::memory_order_relaxed);
            if (auto pixels = loadArtFile(candidate, decodeEdge, analysis, abort)) return pixels;
        }
    }
    return {};
}

std::shared_ptr<ArtPixels> loadEmbeddedArt(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort = nullptr)
{
    album_art_data_ptr data;
    try {
        abort_callback_dummy neverAbort;
        auto& sdkAbort = abort ? *abort : static_cast<abort_callback&>(neverAbort);
        sdkAbort.check();
        pfc::list_t<GUID> identifiers;
        identifiers.add_item(album_art_ids::cover_front);
        album_art_manager_config::ptr config =
            new service_impl_t<EmbeddedOnlyAlbumArtConfig>();
        {
            std::unique_lock<std::mutex> lock(g_albumArtLoaderMutex,
                std::defer_lock);
            if (abort) {
                while (!lock.try_lock()) { sdkAbort.check(); Sleep(5); }
            } else lock.lock();
            const auto extractor = album_art_manager_v3::get()->open_v3(
                pfc::list_single_ref_t<metadb_handle_ptr>(track),
                identifiers, config, sdkAbort);
            data = extractor->query(album_art_ids::cover_front, sdkAbort);
        }
    } catch (const exception_album_art_not_found&) {
    } catch (const exception_io&) {
    } catch (...) {
    }
    if (abort) abort->check();
    return decodeArt(data, decodeEdge, analysis, abort);
}

std::shared_ptr<ArtPixels> loadConfiguredArt(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort = nullptr)
{
    album_art_data_ptr data;
    try {
        abort_callback_dummy neverAbort;
        auto& sdkAbort = abort ? *abort : static_cast<abort_callback&>(neverAbort);
        sdkAbort.check();
        pfc::list_t<GUID> identifiers;
        identifiers.add_item(album_art_ids::cover_front);
        {
            std::unique_lock<std::mutex> lock(g_albumArtLoaderMutex,
                std::defer_lock);
            if (abort) {
                while (!lock.try_lock()) { sdkAbort.check(); Sleep(5); }
            } else lock.lock();
            const auto extractor = album_art_manager_v2::get()->open(
                pfc::list_single_ref_t<metadb_handle_ptr>(track), identifiers, sdkAbort);
            data = extractor->query(album_art_ids::cover_front, sdkAbort);
        }
    } catch (const exception_album_art_not_found&) {
    } catch (const exception_io&) {
    } catch (...) {
    }
    if (abort) abort->check();
    return decodeArt(data, decodeEdge, analysis, abort);
}

std::shared_ptr<ArtPixels> loadArt(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort = nullptr)
{
    if (abort) abort->check();
    g_artworkLoadCounters.loadRequests.fetch_add(1, std::memory_order_relaxed);
    // Only the foobar extractor is serialized. Directory discovery, WIC
    // validation and resampling remain outside the SDK lock.
    if (auto embedded = loadEmbeddedArt(track, decodeEdge, analysis, abort)) return embedded;

    if (abort) abort->check();
    const auto path = localTrackPath(track);
    if (path.empty()) return {};
    g_artworkLoadCounters.localPaths.fetch_add(1, std::memory_order_relaxed);
    if (auto sidecar = loadSpecificSidecarArt(track, path, decodeEdge,
            analysis, abort)) {
        return sidecar;
    }

    // Generic names such as Front.jpg are meaningful in a real album folder,
    // but not in download roots that mix hundreds of unrelated media files.
    if (abort) abort->check();
    if (!allowGenericFolderArtwork(path.parent_path(), abort)) return {};
    return loadConfiguredArt(track, decodeEdge, analysis, abort);
}

std::shared_ptr<ArtPixels> loadArtFile(
    const std::filesystem::path& path,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis,
    abort_callback* abort)
{
    if (abort) abort->check();
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumEncodedBytes ||
        size > std::numeric_limits<DWORD>::max()) return {};
    TemporaryArtworkReservation encodedReservation;
    if (!encodedReservation.add(static_cast<std::size_t>(size))) throw ArtworkBudgetBusy{};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (abort) {
        for (std::size_t offset = 0; offset < bytes.size();) {
            abort->check();
            const auto count = (std::min)(bytes.size() - offset,
                static_cast<std::size_t>(256U * 1024U));
            stream.read(reinterpret_cast<char*>(bytes.data() + offset),
                static_cast<std::streamsize>(count));
            if (!stream) return {};
            offset += count;
        }
    } else {
        stream.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream) return {};
    }
    g_artworkLoadCounters.fileReads.fetch_add(1, std::memory_order_relaxed);
    return decodeBytes(bytes.data(), bytes.size(), decodeEdge, analysis,
        true, abort);
}

} // namespace

std::wstring artworkTrackIdentity(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    auto identity = utf8ToWide(track->get_path());
    identity.push_back(L'\x1f');
    identity += std::to_wstring(track->get_subsong_index());
    return identity;
}

std::shared_ptr<const ArtPixels> loadResolvedArtworkBlocking(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge)
{
    if (!track.is_valid()) return {};
    return loadArt(track, (std::max)(1U, decodeEdge), ArtAnalysisFlags::legacy);
}

std::shared_ptr<const ArtPixels> loadResolvedArtworkBlocking(
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge,
    abort_callback& abort)
{
    if (!track.is_valid()) return {};
    try {
        return loadArt(track, (std::max)(1U, decodeEdge),
            ArtAnalysisFlags::legacy, &abort);
    } catch (const exception_aborted&) {
        return {};
    } catch (const ArtworkBudgetBusy&) {
        return {};
    }
}

struct ArtCache::State {
    explicit State(UINT message) : refreshMessage(message) {}
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> ownerEpoch{1};
    // Guarded by the broker mutex. Only main-purpose subscriptions expire on
    // a track switch; row and prefetch consumers retain their own interest.
    std::wstring mainIdentity;
    std::uint64_t mainEpoch = 1;
    UINT refreshMessage = 0;
};

namespace {

constexpr std::uint32_t kArtworkMasterEdge = 720;

class ArtworkBroker final {
public:
    inline static std::atomic<ArtworkBroker*> live{nullptr};
    static ArtworkBroker& instance()
    {
        static ArtworkBroker broker;
        return broker;
    }

    ArtworkBroker()
    {
        for (auto& worker : workers_) {
            worker = std::thread([this] { workerMain(); });
        }
        live.store(this,std::memory_order_release);
    }

    ~ArtworkBroker()
    {
        live.store(nullptr,std::memory_order_release);
        shutdownAbort_.abort();
        {std::lock_guard lock(mutex_);for(auto& pair:entries_)if(pair.second.cancellation)pair.second.cancellation->abort();}
        {
            std::lock_guard lock(queueMutex_);
            stopping_ = true;
            queue_.clear();
        }
        queueChanged_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }

    ArtCacheStatus status(const std::wstring& key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = entries_.find(key);
        if (iterator == entries_.end()) return ArtCacheStatus::absent;
        switch (iterator->second.status) {
        case Status::loading: return ArtCacheStatus::loading;
        case Status::ready: return ArtCacheStatus::ready;
        case Status::missing: return ArtCacheStatus::missing;
        case Status::retrying: return ArtCacheStatus::missing;
        }
        return ArtCacheStatus::absent;
    }

    void selectMain(const std::shared_ptr<ArtCache::State>& state, std::wstring identity)
    {
        std::lock_guard lock(mutex_);
        if (!state || state->mainIdentity == identity) return;
        state->mainIdentity = std::move(identity);
        ++state->mainEpoch;
        for (auto& pair : entries_) {
            auto& entry = pair.second;
            if (entry.status != Status::ready && entry.cancellation && !hasSubscriber(entry)) {
                entry.cancellation->abort();
                entry.cancellation.reset();
                entry.generation = ++generation_;
                entry.status = Status::missing;
                entry.attempts = 1;
                entry.busyRetries = 0;
                entry.retryAfter = {};
                entry.subscribers.clear();
            }
        }
    }

    std::shared_ptr<const ArtPixels> request(
        const std::wstring& key,
        const std::shared_ptr<ArtCache::State>& subscriber,
        std::function<std::shared_ptr<ArtPixels>(abort_callback&)> loader,
        ArtRequestPriority priority)
    {
        std::uint64_t generation = 0;
        std::shared_ptr<abort_callback_impl> cancellation;
        std::chrono::steady_clock::time_point notBefore{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto iterator = entries_.find(key);
            if (iterator != entries_.end()) {
                auto& entry = iterator->second;
                entry.lastUse = ++useCounter_;
                if (entry.status == Status::ready) return entry.pixels;
                addSubscriber(entry, subscriber, priority);
                if ((entry.status == Status::loading || entry.status == Status::retrying) &&
                    entry.cancellation && !entry.cancellation->is_aborting()) {
                    // A speculative album prefetch must never leave a later
                    // visible click waiting behind the rest of the prefetch
                    // queue. Promote the already queued task in place.
                    if (priority < entry.priority) {
                        entry.priority = priority;
                        reprioritizeQueued(key, entry.generation, priority);
                    }
                    return {};
                }
                if (entry.status == Status::missing &&
                    entry.attempts >= kMaximumTransientAttempts) {
                    return {};
                }
                // A cancelled task's event cannot be reset safely while SDK
                // code is waiting on it. Reattachment gets a new generation.
                const auto wasMissing = entry.status == Status::missing;
                notBefore = wasMissing ? entry.retryAfter :
                    std::chrono::steady_clock::time_point{};
                entry.status = notBefore > std::chrono::steady_clock::now()
                    ? Status::retrying : Status::loading;
                entry.priority = priority;
                if (wasMissing) ++entry.attempts;
                entry.busyRetries = 0;
                entry.generation = ++generation_;
                entry.cancellation=std::make_shared<abort_callback_impl>();
                cancellation=entry.cancellation;
                generation = entry.generation;
            } else {
                evictEntriesLocked(0);
                if (entries_.size() >= kArtworkMaximumEntries) {
                    ++rejectedEntries_;
                    return {};
                }
                Entry entry;
                entry.status = Status::loading;
                entry.priority = priority;
                entry.lastUse = ++useCounter_;
                entry.generation = ++generation_;
                entry.cancellation=std::make_shared<abort_callback_impl>();
                cancellation=entry.cancellation;
                addSubscriber(entry, subscriber, priority);
                generation = entry.generation;
                entries_.emplace(key, std::move(entry));
            }
        }

        auto enqueueResult = enqueue(
            {key, generation, priority, std::move(loader),std::move(cancellation),notBefore});
        if (enqueueResult.displaced) {
            displacedPrefetchRequests_.fetch_add(1, std::memory_order_relaxed);
            complete(enqueueResult.displaced->key,
                enqueueResult.displaced->generation, {},true);
        }
        if (!enqueueResult.accepted) {
            complete(key, generation, {},true);
        }
        return {};
    }

    ArtworkBrokerDiagnostics diagnostics() const noexcept
    {
        ArtworkBrokerDiagnostics result;
        {
            std::lock_guard lock(mutex_);
            result.readyBytes = cachedBytes_;
            result.entryCount = entries_.size();
            result.rejectedEntries = rejectedEntries_;
            result.displacedPrefetchRequests =
                displacedPrefetchRequests_.load(std::memory_order_relaxed);
        }
        {
            std::lock_guard lock(queueMutex_);
            result.queuedRequests = queue_.size();
        }
        {
            std::lock_guard lock(g_temporaryArtworkBudget.mutex);
            result.temporaryBytes = g_temporaryArtworkBudget.bytes;
            result.peakTemporaryBytes = g_temporaryArtworkBudget.peakBytes;
            result.rejectedTemporaryReservations =
                g_temporaryArtworkBudget.rejected;
        }
        result.loadRequests = g_artworkLoadCounters.loadRequests.load(
            std::memory_order_relaxed);
        result.localPaths = g_artworkLoadCounters.localPaths.load(
            std::memory_order_relaxed);
        result.sidecarCandidates = g_artworkLoadCounters.sidecarCandidates.load(
            std::memory_order_relaxed);
        result.fileReads = g_artworkLoadCounters.fileReads.load(
            std::memory_order_relaxed);
        result.decodeAttempts = g_artworkLoadCounters.decodeAttempts.load(
            std::memory_order_relaxed);
        result.decodeSuccesses = g_artworkLoadCounters.decodeSuccesses.load(
            std::memory_order_relaxed);
        result.resampleAttempts = g_artworkLoadCounters.resampleAttempts.load(std::memory_order_relaxed);
        result.resampleMemoHits = g_artworkLoadCounters.resampleMemoHits.load(std::memory_order_relaxed);
        result.busyRetryRequeues = busyRetryRequeues_.load(std::memory_order_relaxed);
        result.failedNotices = failedNotices_.load(std::memory_order_relaxed);
        return result;
    }

    void invalidatePrefix(std::wstring_view prefix)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            const auto key = std::wstring_view(iterator->first);
            const auto separator = key.find(L'#');
            const auto identity = separator == std::wstring_view::npos
                ? key : key.substr(separator + 1U);
            if (identity.starts_with(prefix)) {
                if(iterator->second.cancellation)iterator->second.cancellation->abort();
                releaseContentLocked(iterator->second);
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void cancelAbandonedRequests()
    {
        std::lock_guard lock(mutex_);
        for(auto& pair:entries_){auto& entry=pair.second;
            if(entry.cancellation&&!hasSubscriber(entry)) {
                entry.cancellation->abort();
                entry.cancellation.reset();
                entry.generation=++generation_;
                entry.status=Status::missing;
                entry.attempts=1;
                entry.busyRetries=0;
                entry.retryAfter={};
                entry.subscribers.clear();
            }}
    }

    void retainPrefetch(const std::shared_ptr<ArtCache::State>& state,
        const std::vector<std::wstring>& keys)
    {
        retainRequests(state, keys, ArtRequestPriority::prefetch);
    }

    void retainRequests(const std::shared_ptr<ArtCache::State>& state,
        const std::vector<std::wstring>& keys, ArtRequestPriority purpose)
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, entry] : entries_) {
            if (entry.status == Status::ready ||
                std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
            const auto removed = std::erase_if(entry.subscribers, [&](const auto& subscriber) {
                return subscriber.purpose == purpose &&
                    subscriber.state.lock() == state;
            });
            // The same decode may still belong to a visible row/main cover,
            // or to another cache owner. Only a wholly abandoned job stops.
            if (entry.cancellation && !hasSubscriber(entry)) {
                entry.cancellation->abort();
                entry.cancellation.reset();
                entry.generation = ++generation_;
                entry.status = Status::missing;
                entry.attempts = 1;
                entry.busyRetries = 0;
                entry.retryAfter = {};
                entry.subscribers.clear();
            } else if (removed != 0 && entry.cancellation) {
                if (const auto priority = effectivePriority(entry);
                    priority && *priority != entry.priority) {
                    entry.priority = *priority;
                    reprioritizeQueued(key, entry.generation, *priority);
                }
            }
        }
    }

private:
    enum class Status { loading, ready, missing, retrying };

    struct ContentKey {
        std::uint64_t hash = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t materialRevision = 0;

        bool operator<(const ContentKey& other) const noexcept
        {
            if (hash != other.hash) return hash < other.hash;
            if (width != other.width) return width < other.width;
            if (height != other.height) return height < other.height;
            return materialRevision < other.materialRevision;
        }
    };

    struct Entry {
        struct Subscriber {
            std::weak_ptr<ArtCache::State> state;
            std::uint64_t ownerEpoch = 0;
            std::uint64_t mainEpoch = 0;
            ArtRequestPriority purpose = ArtRequestPriority::visibleRow;
        };
        Status status = Status::loading;
        ArtRequestPriority priority = ArtRequestPriority::visibleRow;
        std::shared_ptr<ArtPixels> pixels;
        std::vector<Subscriber> subscribers;
        std::uint64_t lastUse = 0;
        std::uint64_t generation = 0;
        std::size_t bytes = 0;
        std::optional<ContentKey> contentKey;
        std::shared_ptr<abort_callback_impl> cancellation;
        unsigned attempts = 1;
        unsigned busyRetries = 0;
        std::chrono::steady_clock::time_point retryAfter{};
    };

    struct Task {
        std::wstring key;
        std::uint64_t generation = 0;
        ArtRequestPriority priority = ArtRequestPriority::visibleRow;
        std::function<std::shared_ptr<ArtPixels>(abort_callback&)> loader;
        std::shared_ptr<abort_callback_impl> cancellation;
        std::chrono::steady_clock::time_point notBefore{};
    };

    struct ContentAllocation {
        std::weak_ptr<ArtPixels> pixels;
        std::size_t bytes = 0;
        std::size_t references = 0;
    };

    void releaseContentLocked(Entry& entry) noexcept
    {
        if (!entry.contentKey) return;
        const auto allocation = content_.find(*entry.contentKey);
        if (allocation != content_.end()) {
            if (allocation->second.references > 0) {
                --allocation->second.references;
            }
            if (allocation->second.references == 0) {
                cachedBytes_ -= std::min(
                    cachedBytes_, allocation->second.bytes);
                content_.erase(allocation);
            }
        }
        entry.contentKey.reset();
        entry.bytes = 0;
    }

    static void addSubscriber(
        Entry& entry,
        const std::shared_ptr<ArtCache::State>& subscriber,
        ArtRequestPriority purpose)
    {
        if (!subscriber) return;
        const auto exists = std::find_if(
            entry.subscribers.begin(), entry.subscribers.end(),
            [&](const auto& value) {
                const auto current = value.state.lock();
                return current && current.get() == subscriber.get() && value.purpose == purpose;
            });
        const auto epoch=subscriber->ownerEpoch.load(std::memory_order_acquire);
        if (exists==entry.subscribers.end()) {
            entry.subscribers.push_back({subscriber,epoch,subscriber->mainEpoch,purpose});
        } else {
            exists->ownerEpoch=epoch;
            exists->mainEpoch=subscriber->mainEpoch;
        }
    }

    struct EnqueueResult {
        bool accepted = false;
        std::optional<Task> displaced;
    };

    EnqueueResult enqueue(Task task)
    {
        EnqueueResult result;
        {
            std::lock_guard lock(queueMutex_);
            if (stopping_) return result;
            // Cancelled requests must not occupy all slots ahead of the newest
            // cover. Entries remain retryable under a fresh cancellation token.
            std::erase_if(queue_, [](const Task& queued) {
                return queued.cancellation && queued.cancellation->is_aborting();
            });
            if (queue_.size() >= kArtworkMaximumPendingRequests) {
                const auto worst = std::prev(queue_.end());
                if (!artworkPriorityDisplaces(
                        static_cast<std::uint8_t>(worst->priority),
                        static_cast<std::uint8_t>(task.priority))) {
                    return result;
                }
                result.displaced = std::move(*worst);
                queue_.erase(worst);
            }
            const auto position = std::find_if(queue_.begin(), queue_.end(),
                [&](const Task& queued) {
                    return static_cast<unsigned>(queued.priority) >
                        static_cast<unsigned>(task.priority);
                });
            queue_.insert(position, std::move(task));
            result.accepted = true;
        }
        queueChanged_.notify_all();
        return result;
    }

    void reprioritizeQueued(
        const std::wstring& key,
        std::uint64_t generation,
        ArtRequestPriority priority)
    {
        std::lock_guard lock(queueMutex_);
        const auto queued = std::find_if(queue_.begin(), queue_.end(),
            [&](const Task& task) {
                return task.key == key && task.generation == generation;
            });
        if (queued == queue_.end() || priority == queued->priority) return;
        auto task = std::move(*queued);
        queue_.erase(queued);
        task.priority = priority;
        const auto position = std::find_if(queue_.begin(), queue_.end(),
            [&](const Task& existing) {
                return static_cast<unsigned>(existing.priority) >
                    static_cast<unsigned>(task.priority);
            });
        queue_.insert(position, std::move(task));
    }

    void workerMain() noexcept
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            Task task;
            {
                std::unique_lock lock(queueMutex_);
                for (;;) {
                    if (stopping_) return;
                    const auto now = std::chrono::steady_clock::now();
                    const auto ready = std::find_if(queue_.begin(), queue_.end(),
                        [&](const Task& item) { return item.notBefore <= now; });
                    if (ready != queue_.end()) {
                        task = std::move(*ready);
                        queue_.erase(ready);
                        break;
                    }
                    if (queue_.empty()) queueChanged_.wait(lock);
                    else {
                        const auto first = std::min_element(queue_.begin(), queue_.end(),
                            [](const Task& a, const Task& b) { return a.notBefore < b.notBefore; });
                        const auto deadline = first->notBefore;
                        queueChanged_.wait_until(lock, deadline);
                    }
                }
            }
            if (!wanted(task.key, task.generation)) {
                complete(task.key, task.generation, {},true);
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                const auto entry=entries_.find(task.key);
                if(entry!=entries_.end()&&entry->second.generation==task.generation)
                    entry->second.status=Status::loading;
            }
            std::shared_ptr<ArtPixels> pixels;
            struct RequestAbort final : abort_callback {
                ArtworkBroker& broker; const Task& task;
                RequestAbort(ArtworkBroker& b, const Task& t) : broker(b), task(t) {}
                bool is_aborting() const override {
                    if(broker.shutdownAbort_.is_aborting() || !broker.wanted(task.key, task.generation))task.cancellation->abort();
                    return task.cancellation->is_aborting();
                }
                abort_callback_event get_abort_event() const override {
                    return task.cancellation->get_abort_event();
                }
            } abort{*this, task};
            bool retryable=false;
            try {
                abort.check();
                if (busyFaultPending_.exchange(false,std::memory_order_relaxed))
                    throw ArtworkBudgetBusy{};
                pixels = task.loader(abort);
                abort.check();
            }
            catch(const ArtworkBudgetBusy&){retryable=true;}
            catch(const exception_aborted&){retryable=true;}
            catch (...) {}
            complete(task.key, task.generation, std::move(pixels),retryable,&task);
        }
    }

    bool wanted(const std::wstring& key, std::uint64_t generation) const
    {
        std::lock_guard lock(mutex_);
        const auto iterator = entries_.find(key);
        if (iterator == entries_.end() ||
            iterator->second.generation != generation) return false;
        return hasSubscriber(iterator->second);
    }
    static bool hasSubscriber(const Entry& entry)
    {
        return effectivePriority(entry).has_value();
    }

    static std::optional<ArtRequestPriority> effectivePriority(const Entry& entry)
    {
        std::optional<ArtRequestPriority> priority;
        for (const auto& value : entry.subscribers) {
            const auto subscriber = value.state.lock();
            if (subscriber && subscriber->alive.load(std::memory_order_acquire) &&
                subscriber->ownerEpoch.load(std::memory_order_acquire) == value.ownerEpoch &&
                (value.purpose != ArtRequestPriority::visibleMain ||
                    subscriber->mainEpoch == value.mainEpoch)) {
                if (!priority || value.purpose < *priority) priority = value.purpose;
                if (*priority == ArtRequestPriority::visibleMain) break;
            }
        }
        return priority;
    }

    void complete(
        const std::wstring& key,
        std::uint64_t generation,
        std::shared_ptr<ArtPixels> pixels,bool retryable=false,const Task* retryTask=nullptr)
    {
        std::vector<std::shared_ptr<ArtCache::State>> subscribers;
        std::optional<Task> retry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto iterator = entries_.find(key);
            if (iterator == entries_.end() ||
                iterator->second.generation != generation) return;
            auto& entry = iterator->second;
            if (!pixels && retryable && retryTask && hasSubscriber(entry) &&
                entry.busyRetries < kBusyRetryDelays.size()) {
                const auto delay = kBusyRetryDelays[entry.busyRetries++];
                entry.generation = ++generation_;
                entry.cancellation = std::make_shared<abort_callback_impl>();
                retry = Task{key,entry.generation,entry.priority,retryTask->loader,
                    entry.cancellation,std::chrono::steady_clock::now()+delay};
                busyRetryRequeues_.fetch_add(1,std::memory_order_relaxed);
            }
            if (!retry) {
            if (pixels && pixels->contentHash != 0) {
                const ContentKey contentKey{
                    pixels->contentHash,
                    pixels->width,
                    pixels->height,
                    pixels->material ? pixels->material->contentRevision : 0U,
                };
                const auto duplicate = content_.find(contentKey);
                if (duplicate != content_.end()) {
                    if (auto shared = duplicate->second.pixels.lock()) {
                        pixels = std::move(shared);
                        ++duplicate->second.references;
                    } else {
                        cachedBytes_ -= std::min(
                            cachedBytes_, duplicate->second.bytes);
                        duplicate->second = {
                            pixels, residentPixelBytes(*pixels), 1U};
                        cachedBytes_ += residentPixelBytes(*pixels);
                    }
                } else {
                    content_.emplace(contentKey, ContentAllocation{
                        pixels, residentPixelBytes(*pixels), 1U});
                    cachedBytes_ += residentPixelBytes(*pixels);
                }
                entry.contentKey = contentKey;
            }
            entry.pixels = std::move(pixels);
            entry.status = entry.pixels ? Status::ready : Status::missing;
            entry.bytes = entry.pixels ? residentPixelBytes(*entry.pixels) : 0;
            entry.cancellation.reset();
            if(entry.pixels){entry.attempts=1;entry.busyRetries=0;}
            if (!entry.pixels) {
                if(retryable&&entry.attempts>1)--entry.attempts;
                const auto retry = std::min<std::size_t>(
                    entry.attempts - 1U, kTransientRetryDelays.size() - 1U);
                entry.retryAfter = std::chrono::steady_clock::now() +
                    (retryable?(entry.busyRetries>=kBusyRetryDelays.size()?
                        std::chrono::milliseconds(2000):std::chrono::milliseconds(100)):
                        kTransientRetryDelays[retry]);
            }
            for (const auto& value : entry.subscribers) {
                if (auto subscriber = value.state.lock();
                    subscriber && subscriber->alive.load(
                        std::memory_order_acquire) &&
                    subscriber->ownerEpoch.load(std::memory_order_acquire) ==
                        value.ownerEpoch &&
                    (value.purpose != ArtRequestPriority::visibleMain ||
                        subscriber->mainEpoch == value.mainEpoch)) {
                    subscribers.push_back(std::move(subscriber));
                }
            }
            entry.subscribers.clear();
            evictEntriesLocked(0);
            }
        }
        if (retry) {
            const auto keyCopy=retry->key;const auto retryGeneration=retry->generation;
            auto result=enqueue(std::move(*retry));
            if(result.displaced)complete(result.displaced->key,result.displaced->generation,{},true);
            if(!result.accepted)complete(keyCopy,retryGeneration,{},true);
            return;
        }
        for (const auto& subscriber : subscribers) {
            const auto window = subscriber->window.load(std::memory_order_acquire);
            if (window != nullptr && IsWindow(window)) {
                if (!PostMessageW(window, subscriber->refreshMessage, 0, 0))
                    failedNotices_.fetch_add(1,std::memory_order_relaxed);
            }
        }
    }

    void evictEntriesLocked(std::size_t incomingBytes)
    {
        for (auto iterator = content_.begin(); iterator != content_.end();) {
            if (iterator->second.references == 0 ||
                iterator->second.pixels.expired()) {
                cachedBytes_ -= std::min(
                    cachedBytes_, iterator->second.bytes);
                iterator = content_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        while (entries_.size() >= kArtworkMaximumEntries ||
            cachedBytes_ + incomingBytes > kArtworkCpuBudgetBytes) {
            auto oldest = entries_.end();
            for (auto candidate = entries_.begin(); candidate != entries_.end();
                 ++candidate) {
                if (candidate->second.status == Status::loading ||
                    candidate->second.status == Status::retrying ||
                    !candidate->second.subscribers.empty()) continue;
                if (oldest == entries_.end() ||
                    candidate->second.lastUse < oldest->second.lastUse) {
                    oldest = candidate;
                }
            }
            if (oldest == entries_.end()) break;
            releaseContentLocked(oldest->second);
            entries_.erase(oldest);
        }
    }

    mutable std::mutex mutex_;
    std::map<std::wstring, Entry, TextLess> entries_;
    std::map<ContentKey, ContentAllocation> content_;
    std::uint64_t useCounter_ = 0;
    std::uint64_t generation_ = 0;
    std::size_t cachedBytes_ = 0;
    std::uint64_t rejectedEntries_ = 0;
    std::atomic<std::uint64_t> displacedPrefetchRequests_{0};
    std::atomic<std::uint64_t> busyRetryRequeues_{0},failedNotices_{0};
    std::atomic<bool> busyFaultPending_{qaArtworkBusyOnce()};
    mutable std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<Task> queue_;
    bool stopping_ = false;
    abort_callback_impl shutdownAbort_;
    std::array<std::thread, 2> workers_;
};

std::uint32_t brokerEdge(std::uint32_t requested) noexcept
{
    return std::max(kArtworkMasterEdge, requested);
}

std::wstring trackBrokerKey(
    const std::wstring& key,
    std::uint32_t requested,
    ArtAnalysisFlags analysis)
{
    return L"track#" + key + L"#master#" +
        std::to_wstring(brokerEdge(requested)) + L"#analysis#" +
        std::to_wstring(static_cast<unsigned>(analysis));
}

std::wstring fileBrokerKey(
    const std::wstring& key,
    const std::filesystem::path& path,
    std::uint32_t requested,
    ArtAnalysisFlags analysis)
{
    return L"file#" + key + L"#" + path.wstring() + L"#master#" +
        std::to_wstring(brokerEdge(requested)) + L"#analysis#" +
        std::to_wstring(static_cast<unsigned>(analysis));
}

std::wstring artworkVariantKey(
    std::wstring masterKey,
    std::uint64_t contentHash,
    std::uint32_t targetEdge)
{
    masterKey += L"#variant#linear-lanczos-v1#";
    masterKey += std::to_wstring(targetEdge);
    masterKey += L"#";
    masterKey += std::to_wstring(contentHash);
    return masterKey;
}

std::shared_ptr<ArtPixels> resampleVariant(
    const std::shared_ptr<const ArtPixels>& master,
    std::uint32_t targetEdge,abort_callback& abort)
{
    if (!master || master->width == 0 || master->height == 0 ||
        master->bgra.empty()) {
        return {};
    }
    const auto sourceEdge = std::max(master->width, master->height);
    targetEdge = std::min(targetEdge, sourceEdge);
    const auto scale = static_cast<double>(targetEdge) /
        static_cast<double>(sourceEdge);
    const auto width = std::max<std::uint32_t>(
        1,
        static_cast<std::uint32_t>(std::lround(master->width * scale)));
    const auto height = std::max<std::uint32_t>(
        1,
        static_cast<std::uint32_t>(std::lround(master->height * scale)));
    const FittedArtworkKey memoKey{master->contentHash, master->width,
        master->height, width, height,
        master->material ? master->material->contentRevision : 0U};
    if (master->contentHash != 0) {
        std::lock_guard lock(g_fittedArtworkMutex);
        const auto found = g_fittedArtwork.find(memoKey);
        if (found != g_fittedArtwork.end()) {
            if (auto result = found->second.lock()) {
                g_artworkLoadCounters.resampleMemoHits.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
        }
    }
    // Include linear source, area/reference result, reconstruction and its
    // intermediate storage. Exact variants must obey the same temporary budget
    // as decoding; the already-ready master remains displayable on contention.
    TemporaryArtworkReservation reservation;
    const std::uint64_t temporary=16ULL*(std::uint64_t(master->width)*master->height+
        2ULL*width*height+std::uint64_t(width)*master->height)+
        4ULL*width*height+128ULL*(master->width+master->height+width+height);
    const auto withPrefilter=temporary+(std::max(width,height)<=96?
        16ULL*(2ULL*width*master->height+4ULL*width*height):0ULL);
    if(withPrefilter>std::numeric_limits<std::size_t>::max())return {};
    if(!reservation.add(static_cast<std::size_t>(withPrefilter)))throw ArtworkBudgetBusy{};
    g_artworkLoadCounters.resampleAttempts.fetch_add(1, std::memory_order_relaxed);
    auto image = resampleArtworkPbgraV1(
        master->bgra,
        master->width,
        master->height,
        width,
        height,[&abort]{abort.check();});
    if (image.width == 0 || image.height == 0 || image.bgra.empty()) {
        return {};
    }
    auto result = std::make_shared<ArtPixels>();
    result->width = image.width;
    result->height = image.height;
    result->contentHash = master->contentHash;
    result->bgra = std::move(image.bgra);
    result->palette = master->palette;
    result->material = master->material;
    result->palettePrepared = master->palettePrepared;
    if (master->contentHash != 0) {
        std::lock_guard lock(g_fittedArtworkMutex);
        if (g_fittedArtwork.size() >= kArtworkMaximumEntries * 2) {
            std::erase_if(g_fittedArtwork, [](const auto& pair) { return pair.second.expired(); });
            if (g_fittedArtwork.size() >= kArtworkMaximumEntries * 2) g_fittedArtwork.erase(g_fittedArtwork.begin());
        }
        g_fittedArtwork[memoKey] = result;
    }
    return result;
}

} // namespace

ArtworkBrokerDiagnostics artworkBrokerDiagnostics() noexcept
{
    return ArtworkBroker::instance().diagnostics();
}

ArtCache::ArtCache(UINT refreshMessage)
    : state_(std::make_shared<State>(refreshMessage))
{
}

ArtCache::~ArtCache()
{
    state_->ownerEpoch.fetch_add(1, std::memory_order_acq_rel);
    state_->alive.store(false, std::memory_order_release);
    state_->window.store(nullptr, std::memory_order_release);
    if(auto* broker=ArtworkBroker::live.load(std::memory_order_acquire))broker->cancelAbandonedRequests();
}

void ArtCache::attach(HWND window)
{
    state_->ownerEpoch.fetch_add(1, std::memory_order_acq_rel);
    state_->window.store(window, std::memory_order_release);
    if(auto* broker=ArtworkBroker::live.load(std::memory_order_acquire))broker->cancelAbandonedRequests();
}

void ArtCache::detach()
{
    state_->ownerEpoch.fetch_add(1, std::memory_order_acq_rel);
    state_->window.store(nullptr, std::memory_order_release);
    if(auto* broker=ArtworkBroker::live.load(std::memory_order_acquire))broker->cancelAbandonedRequests();
}

void ArtCache::invalidatePrefix(std::wstring_view keyPrefix)
{
    if (keyPrefix.empty()) return;
    ArtworkBroker::instance().invalidatePrefix(keyPrefix);
}

void ArtCache::retainPrefetch(const std::vector<std::wstring>& identities,
    std::uint32_t decodeEdge, ArtAnalysisFlags analysis)
{
    auto* broker = ArtworkBroker::live.load(std::memory_order_acquire);
    if (!broker) return;
    std::vector<std::wstring> keys;
    keys.reserve(identities.size());
    for (const auto& identity : identities)
        keys.push_back(trackBrokerKey(identity, decodeEdge, analysis));
    broker->retainPrefetch(state_, keys);
}

void ArtCache::beginVisibleRequests()
{
    visibleRequests_.clear();
    collectingVisibleRequests_ = true;
}

void ArtCache::endVisibleRequests()
{
    if (!collectingVisibleRequests_) return;
    collectingVisibleRequests_ = false;
    if (visibleRequests_ == retainedVisibleRequests_) return;
    if (auto* broker = ArtworkBroker::live.load(std::memory_order_acquire))
        broker->retainRequests(state_, visibleRequests_, ArtRequestPriority::visibleRow);
    retainedVisibleRequests_ = visibleRequests_;
}

void ArtCache::recordVisibleRequest(const std::wstring& key, ArtRequestPriority priority)
{
    if (collectingVisibleRequests_ && priority == ArtRequestPriority::visibleRow &&
        std::find(visibleRequests_.begin(), visibleRequests_.end(), key) == visibleRequests_.end())
        visibleRequests_.push_back(key);
}

ArtCacheStatus ArtCache::status(
    const std::wstring& key,
    std::uint32_t decodeEdge,
    ArtAnalysisFlags analysis) const
{
    if (key.empty()) return ArtCacheStatus::absent;
    return ArtworkBroker::instance().status(
        trackBrokerKey(key, decodeEdge, analysis));
}

void ArtCache::invalidateTrack(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return;
    const auto path = utf8ToWide(track->get_path());
    if (path.empty()) return;
    // Delimit the physical path so similarly named files remain cached.
    ArtworkBroker::instance().invalidatePrefix(path + L'\x1f');
}

std::shared_ptr<const ArtPixels> ArtCache::getOrRequest(
    const std::wstring& key,
    const metadb_handle_ptr& track,
    std::uint32_t decodeEdge,
    ArtRequestPriority priority,
    ArtAnalysisFlags analysis)
{
    if (key.empty() || !track.is_valid()) return {};
    const auto edge = brokerEdge(decodeEdge);
    if (priority == ArtRequestPriority::visibleMain) {
        ArtworkBroker::instance().selectMain(state_, artworkTrackIdentity(track));
    }
    const auto requestKey = trackBrokerKey(key, decodeEdge, analysis);
    recordVisibleRequest(requestKey, priority);
    return ArtworkBroker::instance().request(
        requestKey, state_,
        [track, edge, analysis](abort_callback& abort) { return loadArt(track, edge, analysis, &abort); }, priority);
}

std::shared_ptr<const ArtPixels> ArtCache::getOrRequestVariant(
    const std::wstring& key,
    const metadb_handle_ptr& track,
    std::uint32_t targetPhysicalEdge,
    bool exactTarget,
    ArtRequestPriority priority,
    ArtAnalysisFlags analysis)
{
    if (key.empty() || !track.is_valid() || targetPhysicalEdge == 0) return {};
    const auto edge = exactTarget
        ? targetPhysicalEdge
        : quantizeArtworkPhysicalEdgeV1(targetPhysicalEdge);
    const auto master = getOrRequest(key, track, edge, priority, analysis);
    if (!master) return {};
    const auto sourceEdge = std::max(master->width, master->height);
    if (sourceEdge <= edge) return master;
    const auto masterKey = trackBrokerKey(key, edge, analysis);
    const auto variantKey = artworkVariantKey(masterKey, master->contentHash, edge);
    recordVisibleRequest(variantKey, priority);
    const auto variant = ArtworkBroker::instance().request(
        variantKey, state_,
        [master, edge](abort_callback& abort) { abort.check(); return resampleVariant(master, edge,abort); }, priority);
    return variant ? variant : master;
}

std::shared_ptr<const ArtPixels> ArtCache::getOrRequestFile(
    const std::wstring& key,
    const std::filesystem::path& path,
    std::uint32_t decodeEdge,
    ArtRequestPriority priority,
    ArtAnalysisFlags analysis)
{
    if (key.empty() || path.empty()) return {};
    const auto edge = brokerEdge(decodeEdge);
    const auto requestKey = fileBrokerKey(key, path, decodeEdge, analysis);
    recordVisibleRequest(requestKey, priority);
    return ArtworkBroker::instance().request(
        requestKey, state_,
        [path, edge, analysis](abort_callback& abort) { return loadArtFile(path, edge, analysis, &abort); }, priority);
}

std::shared_ptr<const ArtPixels> ArtCache::getOrRequestFileVariant(
    const std::wstring& key,
    const std::filesystem::path& path,
    std::uint32_t targetPhysicalEdge,
    ArtRequestPriority priority,
    ArtAnalysisFlags analysis)
{
    if (key.empty() || path.empty() || targetPhysicalEdge == 0) return {};
    const auto edge = quantizeArtworkPhysicalEdgeV1(targetPhysicalEdge);
    const auto master = getOrRequestFile(key, path, edge, priority, analysis);
    if (!master) return {};
    const auto sourceEdge = std::max(master->width, master->height);
    if (sourceEdge <= edge) return master;
    const auto masterKey = fileBrokerKey(key, path, edge, analysis);
    const auto variantKey = artworkVariantKey(masterKey, master->contentHash, edge);
    recordVisibleRequest(variantKey, priority);
    const auto variant = ArtworkBroker::instance().request(
        variantKey, state_,
        [master, edge](abort_callback& abort) { abort.check(); return resampleVariant(master, edge,abort); }, priority);
    return variant ? variant : master;
}

} // namespace nekorox::component
