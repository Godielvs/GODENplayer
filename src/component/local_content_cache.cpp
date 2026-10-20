#include "local_content_cache.h"

#include "text_util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

namespace nekorox::component {
namespace {

constexpr std::size_t kMaximumEntries = 64;
constexpr std::size_t kMaximumTextBytes = 512 * 1024;
std::atomic<std::uint64_t> gContentRevision{1};

struct TextLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
    {
        return CompareStringOrdinal(
                   left.data(), static_cast<int>(left.size()),
                   right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

std::wstring folded(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool isImage(const std::filesystem::path& path)
{
    const auto extension = folded(path.extension().wstring());
    return extension == L".jpg" || extension == L".jpeg" ||
        extension == L".png" || extension == L".webp" || extension == L".bmp";
}

std::wstring readText(const std::filesystem::path& path)
{
    return readLocalTextFile(path, kMaximumTextBytes);
}

struct ContentIndex {
    std::map<std::wstring, std::filesystem::path, TextLess> biographies;
    std::map<std::wstring, std::filesystem::path, TextLess> reviews;
    std::map<std::wstring, std::vector<std::filesystem::path>, TextLess> artistImages;
};

void indexTextDirectory(
    const std::filesystem::path& root,
    std::map<std::wstring, std::filesystem::path, TextLess>& output)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) &&
                folded(iterator->path().extension().wstring()) == L".txt") {
            output.try_emplace(iterator->path().stem().wstring(), iterator->path());
        }
        iterator.increment(error);
    }
}

std::wstring artistFromImage(const std::filesystem::path& path)
{
    auto stem = path.stem().wstring();
    const auto separator = stem.find_last_of(L'_');
    if (separator != std::wstring::npos && stem.size() - separator - 1U == 32U) {
        const auto hash = std::wstring_view(stem).substr(separator + 1U);
        if (std::all_of(hash.begin(), hash.end(), [](wchar_t character) {
                return std::iswxdigit(static_cast<wint_t>(character)) != 0;
            })) {
            stem.resize(separator);
            return stem;
        }
    }
    const auto parent = path.parent_path().filename().wstring();
    if (parent.size() > 1U && compareText(parent, L"art_img") != 0) return parent;
    return stem;
}

void indexArtistDirectory(
    const std::filesystem::path& root,
    std::map<std::wstring, std::vector<std::filesystem::path>, TextLess>& output)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && isImage(iterator->path())) {
            const auto artist = artistFromImage(iterator->path());
            if (!artist.empty()) {
                auto& paths = output[artist];
                const auto fileName = folded(iterator->path().filename().wstring());
                const auto duplicate = std::any_of(paths.begin(), paths.end(),
                    [&](const auto& candidate) {
                        return folded(candidate.filename().wstring()) == fileName;
                    });
                if (!duplicate) paths.push_back(iterator->path());
            }
        }
        iterator.increment(error);
    }
}

std::shared_ptr<ContentIndex> buildContentIndex(const std::filesystem::path& profile)
{
    auto result = std::make_shared<ContentIndex>();
    const std::array roots{
        profile / L"nekorox" / L"assets" / L"legacy-yttm",
        profile / L"yttm",
    };
    for (const auto& root : roots) {
        indexTextDirectory(root / L"biography", result->biographies);
        indexTextDirectory(root / L"review", result->reviews);
        indexArtistDirectory(root / L"art_img", result->artistImages);
    }
    for (auto& [artist, images] : result->artistImages) {
        if (images.size() > 24U) images.resize(24U);
    }
    return result;
}

std::filesystem::path nativeTrackPath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        const auto native = filesystem::g_get_native_path(track->get_path());
        return utf8ToWide(native.c_str());
    } catch (...) {
        return {};
    }
}

std::shared_ptr<LocalTrackContent> loadContent(
    const std::filesystem::path& profile,
    const ContentIndex& index,
    const std::filesystem::path& trackPath,
    const std::wstring& artist,
    const std::wstring& album,
    const std::wstring& title)
{
    auto result = std::make_shared<LocalTrackContent>();
    if (const auto found = index.biographies.find(artist);
            found != index.biographies.end()) {
        result->biography = readText(found->second);
    }
    if (const auto found = index.reviews.find(artist + L" - " + album);
            found != index.reviews.end()) {
        result->review = readText(found->second);
    }
    if (const auto found = index.artistImages.find(artist);
            found != index.artistImages.end()) {
        result->artistImages = found->second;
    }
    (void)profile;
    (void)trackPath;
    (void)title;
    return result;
}

} // namespace

struct LocalContentCache::State {
    enum class Status { loading, ready };
    struct Entry {
        Status status = Status::loading;
        std::shared_ptr<LocalTrackContent> content;
        std::uint64_t lastUse = 0;
    };

    explicit State(UINT message) : refreshMessage(message) {}
    std::mutex mutex;
    std::once_flag indexOnce;
    std::shared_ptr<ContentIndex> contentIndex;
    std::map<std::wstring, Entry, TextLess> entries;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> alive{true};
    std::uint64_t useCounter = 0;
    UINT refreshMessage = 0;
};

LocalContentCache::LocalContentCache(UINT refreshMessage)
    : state_(std::make_shared<State>(refreshMessage))
{
}

LocalContentCache::~LocalContentCache()
{
    state_->alive.store(false, std::memory_order_release);
    state_->window.store(nullptr, std::memory_order_release);
}

void LocalContentCache::attach(HWND window)
{
    state_->window.store(window, std::memory_order_release);
}

void LocalContentCache::detach()
{
    state_->window.store(nullptr, std::memory_order_release);
}

std::shared_ptr<const LocalTrackContent> LocalContentCache::getOrRequest(
    const metadb_handle_ptr& track,
    const std::wstring& artist,
    const std::wstring& album,
    const std::wstring& title)
{
    if (!track.is_valid()) return {};
    const auto key = artist + L"\x1f" + album + L"\x1f" + title + L"\x1f" +
        utf8ToWide(track->get_path());
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->entries.find(key);
        if (iterator != state_->entries.end()) {
            iterator->second.lastUse = ++state_->useCounter;
            return iterator->second.status == State::Status::ready
                ? iterator->second.content
                : std::shared_ptr<const LocalTrackContent>{};
        }
        if (state_->entries.size() >= kMaximumEntries) {
            auto oldest = state_->entries.end();
            for (auto candidate = state_->entries.begin();
                 candidate != state_->entries.end(); ++candidate) {
                if (candidate->second.status == State::Status::loading) continue;
                if (oldest == state_->entries.end() ||
                    candidate->second.lastUse < oldest->second.lastUse) {
                    oldest = candidate;
                }
            }
            if (oldest != state_->entries.end()) state_->entries.erase(oldest);
            else return {}; // All slots are in flight. Retry after a completion notice.
        }
        state_->entries.emplace(key, State::Entry{
            State::Status::loading, {}, ++state_->useCounter});
    }

    std::filesystem::path profile;
    try {
        const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
        profile = utf8ToWide(native.c_str());
    } catch (...) {
    }
    const auto trackPath = nativeTrackPath(track);
    const std::weak_ptr<State> weakState = state_;
    fb2k::splitTask([weakState, key, profile = std::move(profile), trackPath,
                        artist, album, title] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto state = weakState.lock();
        if (!state || !state->alive.load(std::memory_order_acquire)) return;
        std::call_once(state->indexOnce, [&] {
            state->contentIndex = buildContentIndex(profile);
        });
        const auto index = state->contentIndex;
        if (!index) {
            std::lock_guard lock(state->mutex);state->entries.erase(key);return;
        }
        auto content = loadContent(
            profile, *index, trackPath, artist, album, title);
        content->revision = gContentRevision.fetch_add(
            1, std::memory_order_acq_rel);
        fb2k::inMainThread([weakState, key, content = std::move(content)] {
            const auto state = weakState.lock();
            if (!state || !state->alive.load(std::memory_order_acquire)) return;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto iterator = state->entries.find(key);
                if (iterator == state->entries.end()) return;
                iterator->second.content = content;
                iterator->second.status = State::Status::ready;
            }
            const auto window = state->window.load(std::memory_order_acquire);
            if (window != nullptr && IsWindow(window)) {
                PostMessageW(window, state->refreshMessage, 0, 0);
            }
        });
    });
    return {};
}

} // namespace nekorox::component
