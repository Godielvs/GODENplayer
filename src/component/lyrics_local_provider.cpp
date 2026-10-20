#include "lyrics_local_provider.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <windows.h>

namespace nekorox::component {
struct LyricsLocalProvider::Index {
    std::mutex mutex;
    std::filesystem::path profile;
    std::vector<std::filesystem::path> paths;
    std::array<HANDLE,3> watches{INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
    std::chrono::steady_clock::time_point scanned{};
    std::uint64_t builds=0;
    ~Index(){for(auto h:watches)if(h!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(h);}
};
LyricsLocalProvider::LyricsLocalProvider():index_(std::make_shared<Index>()){}
std::uint64_t LyricsLocalProvider::indexBuilds() const noexcept {
    std::lock_guard lock(index_->mutex);return index_->builds;
}
namespace {

constexpr std::uintmax_t kMaximumLocalLyricsBytes = 512U * 1024U;

std::wstring utf8ToWideLocal(std::string_view value)
{
    if (value.empty()) return {};
    const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) != required)
        return {};
    return result;
}

std::string wideToUtf8Local(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr) != required) return {};
    return result;
}

bool equalOrdinal(std::wstring_view left, std::wstring_view right) noexcept
{
    return left.size() == right.size() && CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::string> readUtf8(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumLocalLyricsBytes) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::nullopt;
    }
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf) bytes.erase(0, 3);
    if (bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data(), static_cast<int>(bytes.size()), nullptr, 0) <= 0) {
        return std::nullopt;
    }
    return bytes;
}

bool looksSynchronized(std::string_view text) noexcept
{
    for (std::size_t index = 0; index + 4 < text.size(); ++index) {
        if (text[index] == '[' && text[index + 1] >= '0' &&
            text[index + 1] <= '9' && text.find(':', index + 2) !=
                std::string_view::npos) return true;
    }
    return false;
}

std::string staticTextFromLrc(std::string_view body)
{
    std::string result;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find('\n', start);
        auto line = body.substr(start, end == std::string_view::npos
            ? body.size() - start : end - start);
        std::size_t cursor = 0;
        while (cursor < line.size() && line[cursor] == '[') {
            const auto close = line.find(']', cursor + 1U);
            if (close == std::string_view::npos) break;
            cursor = close + 1U;
        }
        line.remove_prefix(std::min(cursor, line.size()));
        if (!line.empty()) {
            if (!result.empty()) result.push_back('\n');
            result.append(line);
        }
        if (end == std::string_view::npos) break;
        start = end + 1U;
    }
    return result;
}

LyricsCandidateV2 localCandidate(const LyricsRequestContext& request,
    std::string body, std::string recordId, std::string display,
    bool permitTiming)
{
    LyricsCandidateV2 candidate;
    candidate.source = {"local", std::move(recordId), "file-revision-v1"};
    candidate.title = request.metadata.title;
    candidate.artist = request.metadata.artist;
    candidate.album = request.metadata.album;
    candidate.durationSeconds = request.metadata.durationSeconds;
    if (permitTiming && looksSynchronized(body)) {
        candidate.syncedLyrics = std::move(body);
    } else {
        candidate.plainLyrics = looksSynchronized(body)
            ? staticTextFromLrc(body) : std::move(body);
    }
    candidate.sourceDisplayName = std::move(display);
    candidate.attribution = "Local lyrics";
    return candidate;
}

void appendPathCandidates(std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& base, std::uint32_t subsong)
{
    if (base.empty()) return;
    const auto parent = base.parent_path();
    const auto stem = base.stem().wstring();
    if (subsong != 0) {
        for (const auto* extension : {L".lrc", L".txt"}) {
            paths.push_back(parent / (stem + L" [subsong " +
                std::to_wstring(subsong) + L"]" + extension));
            paths.push_back(parent / (stem + L"." +
                std::to_wstring(subsong) + extension));
        }
    }
    auto lrc = base;
    lrc.replace_extension(L".lrc");
    paths.push_back(std::move(lrc));
    auto text = base;
    text.replace_extension(L".txt");
    paths.push_back(std::move(text));
}

std::vector<std::filesystem::path> scanIndexedPaths(
    const LyricsRequestContext& request, std::stop_token stop)
{
    std::vector<std::filesystem::path> matches;
    const std::array roots{
        request.profilePath / L"lyrics-v2",
        request.profilePath / L"openlyrics",
        request.profilePath / L"lyrics"};
    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) continue;
        std::filesystem::recursive_directory_iterator iterator(root,
            std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            if(stop.stop_requested())return {};
            const auto path = iterator->path();
            if (iterator->is_regular_file(error) &&
                (equalOrdinal(path.extension().wstring(), L".lrc") ||
                 equalOrdinal(path.extension().wstring(), L".txt"))) {
                matches.push_back(path);
            }
            iterator.increment(error);
        }
    }
    return matches;
}

} // namespace

LyricsProviderResult LyricsLocalProvider::lookup(
    const LyricsRequestContext& request, std::stop_token stop) const
{
    LyricsProviderResult result;
    if(stop.stop_requested()){result.outcome=LyricsProviderOutcome::cancelled;return result;}
    if (!request.embeddedSynced.empty()) {
        result.candidates.push_back(localCandidate(request,
            request.embeddedSynced, "embedded-synced", "Embedded tag", true));
    } else if (!request.embeddedPlain.empty()) {
        result.candidates.push_back(localCandidate(request,
            request.embeddedPlain, "embedded-plain", "Embedded tag", false));
    }
    if (!result.candidates.empty()) {
        result.outcome = LyricsProviderOutcome::candidates;
        return result;
    }
    std::vector<std::filesystem::path> paths;
    appendPathCandidates(paths, request.nativePath,
        request.metadata.recording.subsong);
    for (const auto& path : paths) {
        if(stop.stop_requested()){result.outcome=LyricsProviderOutcome::cancelled;return result;}
        const auto body = readUtf8(path);
        if (!body) continue;
        const auto genericForSubsong = request.metadata.recording.subsong != 0 &&
            path.stem() == request.nativePath.stem();
        result.candidates.push_back(localCandidate(request, *body,
            wideToUtf8Local(path.wstring()), "Sidecar", !genericForSubsong));
        break;
    }
    if (result.candidates.empty()) {
        std::vector<std::filesystem::path> indexed;
        if(!request.metadata.artist.empty()&&!request.metadata.title.empty()){
            std::lock_guard lock(index_->mutex);
            bool dirty=index_->profile!=request.profilePath||index_->scanned==std::chrono::steady_clock::time_point{}||
                request.reason==LyricsRequestReason::refresh;
            const std::array roots{request.profilePath/L"lyrics-v2",request.profilePath/L"openlyrics",request.profilePath/L"lyrics"};
            bool fallback=false;
            for(unsigned i=0;i<roots.size();++i){auto& watch=index_->watches[i];
                if(index_->profile!=request.profilePath&&watch!=INVALID_HANDLE_VALUE){FindCloseChangeNotification(watch);watch=INVALID_HANDLE_VALUE;}
                if(watch==INVALID_HANDLE_VALUE){watch=FindFirstChangeNotificationW(roots[i].c_str(),TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME);if(watch!=INVALID_HANDLE_VALUE)dirty=true;else fallback=true;}
                else if(WaitForSingleObject(watch,0)==WAIT_OBJECT_0){dirty=true;if(!FindNextChangeNotification(watch)){FindCloseChangeNotification(watch);watch=INVALID_HANDLE_VALUE;fallback=true;}}
            }
            if(fallback&&std::chrono::steady_clock::now()-index_->scanned>std::chrono::seconds(5))dirty=true;
            if(dirty){auto scanned=scanIndexedPaths(request,stop);
                if(stop.stop_requested()){result.outcome=LyricsProviderOutcome::cancelled;return result;}
                index_->paths=std::move(scanned);index_->profile=request.profilePath;
                index_->scanned=std::chrono::steady_clock::now();++index_->builds;}
            const auto wanted=utf8ToWideLocal(request.metadata.artist+" - "+request.metadata.title);
            for(const auto& path:index_->paths){if(stop.stop_requested()){result.outcome=LyricsProviderOutcome::cancelled;return result;}
                if(equalOrdinal(path.stem().wstring(),wanted)){indexed.push_back(path);if(indexed.size()>1)break;}}
        }
        // A global match is accepted only when artist+title names exactly one
        // file. There is deliberately no title-only fallback.
        if (indexed.size() == 1U) {
            if (const auto body = readUtf8(indexed.front())) {
                result.candidates.push_back(localCandidate(request, *body,
                    wideToUtf8Local(indexed.front().wstring()), "OpenLyrics file", true));
            }
        } else if (indexed.size() > 1U) {
            result.reason = "ambiguous-local-files";
        }
    }
    result.outcome = result.candidates.empty()
        ? LyricsProviderOutcome::notFound
        : LyricsProviderOutcome::candidates;
    return result;
}

} // namespace nekorox::component
