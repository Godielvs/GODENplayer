#include "media_binding_recovery.h"
#include "nekorox/track_identity.h"
#include "../../vendor/libsonare/src/util/json.h"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace nekorox::component {
namespace {
namespace json = sonare::util::json;
constexpr std::size_t maxJournalBytes = 16U * 1024U * 1024U;
constexpr std::size_t maxScanBytes = 64U * 1024U * 1024U;
constexpr std::size_t maxEntries = 4096;

struct ReadLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit ReadLease(const std::filesystem::path& path) : handle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(handle, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))) {
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
        }
    }
    ~ReadLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
    ReadLease(const ReadLease&) = delete;
};

bool noReparse(const std::filesystem::path& path) {
    if (!path.is_absolute()) return false;
    for (auto part = path; !part.empty();) {
        const auto attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        const auto parent = part.parent_path();
        if (parent == part) break;
        part = parent;
    }
    return true;
}

bool regularMediaLeaf(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

std::string readBounded(const std::filesystem::path& path, std::size_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (size > limit) throw std::runtime_error("Recovery record exceeds limit");
    std::ifstream stream(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Recovery record changed or could not be read");
    return text;
}

bool hexDigest(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string stringField(const json::Value& value, const char* name) {
    const auto* field = value.find(name);
    if (!field || !field->is_string()) throw std::runtime_error("Missing recovery field");
    return field->as_string();
}

std::filesystem::path nativePath(const std::string& text) {
    if (text.empty() || text.size() > 32768 || text.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid recovery path");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid recovery UTF-8");
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), wide.data(), count);
    // Keep native drive/UNC paths supported by the physical engine. Reject
    // device namespaces, URLs, alternate streams and relative paths.
    const bool drive = wide.size() >= 4 && wide[1] == L':' && wide[2] == L'\\' &&
        ((wide[0] >= L'a' && wide[0] <= L'z') || (wide[0] >= L'A' && wide[0] <= L'Z')) &&
        wide.find(L':', 2) == std::wstring::npos;
    const bool unc = wide.starts_with(L"\\\\") && !wide.starts_with(L"\\\\?\\") &&
        !wide.starts_with(L"\\\\.\\") && wide.find(L':') == std::wstring::npos;
    if ((!drive && !unc) || !std::filesystem::path(wide).is_absolute())
        throw std::runtime_error("Invalid native recovery path");
    return std::filesystem::path(wide).lexically_normal();
}

std::filesystem::path receiptPath(const std::filesystem::path& journal) {
    return journal.wstring() + L".bound";
}

bool confirmed(const MediaBindingRecoveryItem& item) {
    try {
        const auto receipt = receiptPath(item.journalPath);
        return noReparse(receipt) && readBounded(receipt, 64) == item.journalSha256;
    } catch (...) { return false; }
}

struct Candidate {
    MediaBindingRecoveryItem item;
    std::filesystem::path destination;
    std::string expectedHash;
};
} // namespace

bool freezeMediaBindingRevisions(const std::filesystem::path& path,
    std::vector<MediaRevisionBinding>& revisions, std::stop_token stop,
    const std::string& expectedSha256)
{
    try {
        if (revisions.empty() || revisions.size() > 65536 || !regularMediaLeaf(path)) return false;
        ReadLease lease(path);
        if (!lease) return false;
        std::string hash;
        if (!expectedSha256.empty() &&
                (!mediaFileSha256(path, hash, stop) || hash != expectedSha256)) return false;
        const auto size = std::filesystem::file_size(path);
        const auto modified = std::filesystem::last_write_time(path);
        for (auto& binding : revisions) {
            if (stop.stop_requested()) return false;
            auto& current = binding.current;
            const auto identity = makeTrackId({path, current.subsong, size,
                static_cast<std::uint64_t>(modified.time_since_epoch().count())});
            current.trackId = identity.value;
            current.path = identity.normalizedPathUtf8;
            current.partialFingerprint = identity.partialFingerprint;
            current.fileSize = size;
            current.modifiedTicks = static_cast<std::uint64_t>(modified.time_since_epoch().count());
        }
        return size == std::filesystem::file_size(path) &&
            modified == std::filesystem::last_write_time(path);
    } catch (...) { return false; }
}

MediaBindingRecoveryScan scanMediaBindingRecovery(
    const std::filesystem::path& directory, std::stop_token stop)
{
    MediaBindingRecoveryScan scan;
    std::vector<Candidate> candidates;
    std::map<std::wstring, std::size_t> pathCounts;
    std::size_t entries = 0, bytes = 0;
    try {
        if (!std::filesystem::exists(directory)) return scan;
        if (!noReparse(directory)) { scan.limited = true; return scan; }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (stop.stop_requested() || ++entries > maxEntries) { scan.limited = true; break; }
            if (entry.path().extension() != L".json") continue;
            try {
                if (!noReparse(entry.path()) || !entry.is_regular_file()) { scan.limited = true; continue; }
                ReadLease journalLease(entry.path());
                if (!journalLease) { scan.limited = true; continue; }
                const auto size = entry.file_size();
                if (size > maxJournalBytes || size > maxScanBytes - bytes) { scan.limited = true; break; }
                bytes += static_cast<std::size_t>(size);
                const auto text = readBounded(entry.path(), maxJournalBytes);
                const auto doc = json::parse_strict(text, 12);
                if (stringField(doc, "schema") != "goden.media-operation.v2" ||
                        stringField(doc, "mode") != "InPlace") continue;
                const auto* files = doc.find("files");
                if (!files || !files->is_array()) throw std::runtime_error("Missing files");
                // Production emits one item journal per attempted physical edit.
                // Legacy journals without bindings are not guessed from metadata.
                bool hasBindings = false;
                for (const auto& file : files->as_array()) {
                    const auto* bindings = file.find("revisionBindings");
                    if (bindings && !bindings->is_array()) throw std::runtime_error("Invalid revision array");
                    hasBindings |= bindings && bindings->is_array() && !bindings->as_array().empty();
                }
                if (!hasBindings) continue;
                if (files->size() != 1) throw std::runtime_error("Ambiguous multi-file recovery journal");
                Candidate candidate;
                candidate.item.journalPath = entry.path();
                if (!mediaFileSha256(entry.path(), candidate.item.journalSha256, stop))
                    throw std::runtime_error("Unreadable journal");
                if (confirmed(candidate.item)) continue;
                const auto& file = (*files)[0];
                candidate.destination = nativePath(stringField(file, "destination"));
                if (normalizeWindowsPath(nativePath(stringField(file, "source"))) !=
                        normalizeWindowsPath(candidate.destination)) throw std::runtime_error("Not in-place");
                candidate.expectedHash = stringField(file, "sha256");
                if (!hexDigest(candidate.expectedHash)) throw std::runtime_error("Invalid hash");
                std::set<std::uint32_t> subsongs;
                const auto& bindings = file["revisionBindings"].as_array();
                if (bindings.empty() || bindings.size() > 65536) throw std::runtime_error("Invalid subsongs");
                for (const auto& value : bindings) {
                    MediaRevisionBinding binding;
                    binding.previousRevisionTrackId = stringField(value, "previousRevisionTrackId");
                    if (!binding.previousRevisionTrackId.starts_with("v1:") ||
                            !hexDigest(std::string_view(binding.previousRevisionTrackId).substr(3)))
                        throw std::runtime_error("Invalid previous revision");
                    const auto* sub = value.find("subsong");
                    if (!sub || !sub->is_number() || !std::isfinite(sub->as_number()) ||
                            sub->as_number() < 0 || sub->as_number() > UINT32_MAX ||
                            std::floor(sub->as_number()) != sub->as_number())
                        throw std::runtime_error("Invalid subsong");
                    binding.current.subsong = static_cast<std::uint32_t>(sub->as_number());
                    if (!subsongs.insert(binding.current.subsong).second) throw std::runtime_error("Duplicate subsong");
                    candidate.item.revisions.push_back(std::move(binding));
                }
                ++pathCounts[normalizeWindowsPath(candidate.destination)];
                candidates.push_back(std::move(candidate));
            } catch (...) { scan.limited = true; }
        }
        // A partially scanned directory cannot prove that another pending edit
        // does not precede a candidate. Fail closed rather than merge a chain.
        for (auto& candidate : candidates) {
            if (scan.limited || pathCounts[normalizeWindowsPath(candidate.destination)] != 1 ||
                    stop.stop_requested()) {
                scan.blockedPaths.push_back(candidate.destination); continue;
            }
            ReadLease lease(candidate.destination);
            std::string hash;
            if (!regularMediaLeaf(candidate.destination) || !lease ||
                    !mediaFileSha256(candidate.destination, hash, stop) ||
                    !freezeMediaBindingRevisions(candidate.destination, candidate.item.revisions, stop)) {
                scan.blockedPaths.push_back(candidate.destination); continue;
            }
            if (hash != candidate.expectedHash && !std::all_of(candidate.item.revisions.begin(),
                    candidate.item.revisions.end(), [](const auto& binding) {
                        return binding.current.trackId == binding.previousRevisionTrackId;
                    })) {
                scan.blockedPaths.push_back(candidate.destination); continue;
            }
            scan.pending.push_back(std::move(candidate.item));
        }
    } catch (...) { scan.limited = true; scan.pending.clear(); }
    return scan;
}

bool acknowledgeMediaBindingRecovery(const MediaBindingRecoveryItem& item, std::stop_token stop)
{
    try {
        if (stop.stop_requested() || !hexDigest(item.journalSha256) || !noReparse(item.journalPath)) return false;
        ReadLease journalLease(item.journalPath);
        std::string actual;
        if (!journalLease || !mediaFileSha256(item.journalPath, actual, stop) || actual != item.journalSha256)
            return false;
        const auto receipt = receiptPath(item.journalPath);
        const auto temporary = receipt.wstring() + L".staging-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64());
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool okay = WriteFile(handle, actual.data(), static_cast<DWORD>(actual.size()), &written, nullptr) &&
            written == actual.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        const bool committed = okay && !stop.stop_requested() && MoveFileExW(temporary.c_str(), receipt.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!committed) DeleteFileW(temporary.c_str());
        return committed;
    } catch (...) { return false; }
}
} // namespace nekorox::component
