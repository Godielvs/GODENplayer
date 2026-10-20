#include "media_file_operations.h"
#include "media_binding_recovery.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cwctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace nekorox::component {
namespace {

std::wstring folded(std::filesystem::path value)
{
    auto text = value.lexically_normal().wstring();
    std::transform(text.begin(), text.end(), text.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

bool samePath(const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    return folded(left) == folded(right);
}

bool within(const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    const auto rootText = folded(root);
    const auto candidateText = folded(candidate);
    if (candidateText == rootText) return true;
    if (candidateText.size() <= rootText.size() ||
        candidateText.compare(0, rootText.size(), rootText) != 0) return false;
    const auto separator = candidateText[rootText.size()];
    return separator == L'\\' || separator == L'/';
}

bool reservedDeviceName(std::wstring name)
{
    const auto dot = name.find(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    std::transform(name.begin(), name.end(), name.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (name == L"CON" || name == L"PRN" || name == L"AUX" ||
        name == L"NUL") return true;
    if (name.size() == 4 && (name.rfind(L"COM", 0) == 0 ||
            name.rfind(L"LPT", 0) == 0) &&
        name[3] >= L'1' && name[3] <= L'9') return true;
    return false;
}

std::wstring safeComponent(std::wstring value, std::wstring_view fallback)
{
    for (auto& c : value) {
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) !=
                std::wstring_view::npos) c = L'_';
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' '))
        value.pop_back();
    while (!value.empty() && value.front() == L' ') value.erase(value.begin());
    if (value.empty()) value.assign(fallback);
    if (reservedDeviceName(value)) value.insert(value.begin(), L'_');
    if (value.size() > 180) value.resize(180);
    while (!value.empty() && (value.back() == L'.' || value.back() == L' '))
        value.pop_back();
    return value;
}

bool validRelative(const std::filesystem::path& relative, std::wstring& reason)
{
    if (relative.empty() || relative.is_absolute() || relative.has_root_path()) {
        reason = L"Destination must be a non-empty relative path";
        return false;
    }
    for (const auto& component : relative) {
        const auto name = component.wstring();
        if (name.empty() || name == L"." || name == L"..") {
            reason = L"Destination contains a root escape";
            return false;
        }
        if (name.size() > 255 || name.back() == L'.' || name.back() == L' ' ||
            name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
            std::any_of(name.begin(), name.end(),
                [](wchar_t c) { return c < 32; }) ||
            reservedDeviceName(name)) {
            reason = L"Destination contains a Windows-invalid path component";
            return false;
        }
    }
    return true;
}

bool existingAncestryContained(const std::filesystem::path& root,
    const std::filesystem::path& destination)
{
    std::error_code error;
    const auto resolvedRoot = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    auto existing = destination.parent_path();
    while (!existing.empty() && !std::filesystem::exists(existing, error)) {
        if (error) return false;
        existing = existing.parent_path();
    }
    if (existing.empty()) return false;
    const auto resolvedExisting = std::filesystem::weakly_canonical(existing, error);
    return !error && within(resolvedRoot, resolvedExisting);
}

std::filesystem::path renamedCandidate(const std::filesystem::path& wanted,
    unsigned suffix)
{
    return wanted.parent_path() /
        (wanted.stem().wstring() + L" (" + std::to_wstring(suffix) + L")" +
            wanted.extension().wstring());
}

struct Revision {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};
};

bool revisionOf(const std::filesystem::path& path, Revision& output)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || status.type() != std::filesystem::file_type::regular) return false;
    output.size = std::filesystem::file_size(path, error);
    if (error) return false;
    output.writeTime = std::filesystem::last_write_time(path, error);
    return !error;
}

bool sameRevision(const Revision& left, const Revision& right) noexcept
{
    return left.size == right.size && left.writeTime == right.writeTime;
}

bool sha256(const std::filesystem::path& path, std::string& output,
    std::stop_token stop)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, ignored = 0;
    bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
        nullptr, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
            &ignored, 0) == 0;
    std::vector<std::uint8_t> object(objectBytes);
    std::array<std::uint8_t, 32> digest{};
    if (okay) okay = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
        nullptr, 0, 0) == 0;
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> buffer(256U * 1024U);
    okay = okay && static_cast<bool>(stream);
    while (okay && stream && !stop.stop_requested()) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0) okay = BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(buffer.data()),
            static_cast<ULONG>(count), 0) == 0;
    }
    if (stream.bad()) okay = false;
    okay = okay && !stop.stop_requested() &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!okay) return false;
    static constexpr char digits[] = "0123456789abcdef";
    output.clear();
    output.reserve(64);
    for (const auto value : digest) {
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return true;
}

bool makeDestinationParents(const std::filesystem::path& root,
    const std::filesystem::path& destination)
{
    std::error_code error;
    auto relative = std::filesystem::relative(destination.parent_path(), root, error);
    if (error || (relative.empty() && destination.parent_path() != root)) return false;
    auto cursor = root;
    for (const auto& component : relative) {
        if (component == L".") continue;
        cursor /= component;
        auto status = std::filesystem::symlink_status(cursor, error);
        if (error && !std::filesystem::exists(cursor)) error.clear();
        if (error) return false;
        if (status.type() == std::filesystem::file_type::not_found) {
            if (!std::filesystem::create_directory(cursor, error) || error) return false;
            status = std::filesystem::symlink_status(cursor, error);
        }
        if (error || status.type() != std::filesystem::file_type::directory) return false;
    }
    return existingAncestryContained(root, destination);
}

std::filesystem::path stagingFor(const std::filesystem::path& destination,
    std::size_t index)
{
    return destination.parent_path() /
        (destination.stem().wstring() + L".goden-staging-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(index) + destination.extension().wstring());
}

std::string jsonEscape(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), utf8.data(), bytes,
            nullptr, nullptr) != bytes) return {};
    std::string output;
    output.reserve(utf8.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : utf8) {
        const auto c = static_cast<unsigned char>(byte);
        if (c == '\\') output += "\\\\";
        else if (c == '\"') output += "\\\"";
        else if (c < 0x20) {
            output += "\\u00";
            output.push_back(hex[c >> 4U]);
            output.push_back(hex[c & 0x0fU]);
        } else {
            output.push_back(byte);
        }
    }
    return output;
}

const char* copyOutcomeText(MediaCopyOutcome outcome)
{
    switch (outcome) {
    case MediaCopyOutcome::staging: return "Staging";
    case MediaCopyOutcome::verified: return "Verified";
    case MediaCopyOutcome::backedUp: return "BackedUp";
    case MediaCopyOutcome::copied: return "Copied";
    case MediaCopyOutcome::moved: return "Moved";
    case MediaCopyOutcome::duplicateRecoverable: return "DuplicateRecoverable";
    case MediaCopyOutcome::skipped: return "Skipped";
    case MediaCopyOutcome::rejected: return "Rejected";
    case MediaCopyOutcome::conflict: return "Conflict";
    case MediaCopyOutcome::failed: return "Failed";
    case MediaCopyOutcome::cancelled: return "Cancelled";
    }
    return "Unknown";
}

bool persistCopyJournal(const std::filesystem::path& path,
    const MediaPlan& plan, const std::vector<MediaCopyResult>& results)
{
    if (path.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto staging = path.wstring() + L".staging";
    std::ofstream stream(std::filesystem::path(staging),
        std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << "{\n  \"schema\":\"goden.media-operation.v2\",\n"
           << "  \"mode\":\"" << (plan.mode == MediaOrganizationMode::move
                ? "Move" : plan.mode == MediaOrganizationMode::inPlace
                ? "InPlace" : "Copy") << "\",\n"
        << "  \"approvedRoot\":\"" << jsonEscape(plan.approvedRoot.wstring())
        << "\",\n  \"files\":[\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& item = results[index];
        stream << "    {\"source\":\"" << jsonEscape(item.source.wstring())
            << "\",\"destination\":\"" << jsonEscape(item.destination.wstring())
            << "\",\"staging\":\"" << jsonEscape(item.staging.wstring())
            << "\",\"backup\":\"" << jsonEscape(item.backup.wstring())
            << "\",\"sourceBackup\":\"" << jsonEscape(item.sourceBackup.wstring())
            << "\",\"outcome\":\"" << copyOutcomeText(item.outcome)
            << "\",\"bytes\":" << item.bytes << ",\"sourceSha256\":\""
            << item.sourceSha256 << "\",\"sha256\":\""
            << item.sha256 << "\",\"sidecarsExpected\":"
            << item.sidecarsExpected << ",\"sidecarsCommitted\":"
            << item.sidecarsCommitted << ",\"sidecarPaths\":[";
        for (std::size_t sidecar = 0; sidecar < item.sidecarPaths.size(); ++sidecar) {
            if (sidecar) stream << ',';
            stream << "\"" << jsonEscape(item.sidecarPaths[sidecar].wstring()) << "\"";
        }
        stream << "],\"sidecarBackups\":[";
        for (std::size_t backup = 0; backup < item.sidecarBackups.size(); ++backup) {
            if (backup) stream << ',';
            stream << "\"" << jsonEscape(item.sidecarBackups[backup].wstring()) << "\"";
        }
        stream << "],\"revisionBindings\":[";
        if (item.sha256.size() == 64 && item.outcome != MediaCopyOutcome::conflict &&
                item.outcome != MediaCopyOutcome::cancelled && item.outcome != MediaCopyOutcome::skipped)
            for (std::size_t revision = 0;
                revision < item.revisionBindings.size(); ++revision) {
            if (revision) stream << ',';
            const auto& binding = item.revisionBindings[revision];
            stream << "{\"previousRevisionTrackId\":\"" << binding.previousRevisionTrackId
                << "\",\"subsong\":" << binding.current.subsong << '}';
        }
        stream << "],\"detail\":\""
            << jsonEscape(item.detail) << "\"}"
            << (index + 1 == results.size() ? "\n" : ",\n");
    }
    stream << "  ]\n}\n";
    stream.flush();
    if (!stream) return false;
    stream.close();
    const auto handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed) return false;
    return MoveFileExW(staging.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool sameVolume(const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    wchar_t leftRoot[MAX_PATH]{}, rightRoot[MAX_PATH]{};
    wchar_t leftVolume[MAX_PATH]{}, rightVolume[MAX_PATH]{};
    if (!GetVolumePathNameW(left.c_str(), leftRoot, MAX_PATH) ||
        !GetVolumePathNameW(right.c_str(), rightRoot, MAX_PATH) ||
        !GetVolumeNameForVolumeMountPointW(leftRoot, leftVolume, MAX_PATH) ||
        !GetVolumeNameForVolumeMountPointW(rightRoot, rightVolume, MAX_PATH)) {
        return false;
    }
    return _wcsicmp(leftVolume, rightVolume) == 0;
}

std::filesystem::path backupFor(const std::filesystem::path& root,
    const std::filesystem::path& destination, std::size_t index)
{
    return root / (destination.filename().wstring() + L".goden-backup-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(index));
}

bool safeSidecar(const MediaSidecarWrite& sidecar,
    std::filesystem::path& filename)
{
    auto stem = sidecar.stem;
    auto extension = sidecar.extension;
    std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    static const std::set<std::wstring, std::less<>> stems{
        L"folder", L"back", L"artist", L"disc"};
    static const std::set<std::wstring, std::less<>> extensions{
        L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".tif", L".tiff", L".webp"};
    if (!stems.contains(stem) || !extensions.contains(extension) ||
        !sidecar.bytes || sidecar.bytes->empty() ||
        sidecar.bytes->size() > 32U * 1024U * 1024U)
        return false;
    filename = stem + extension;
    return true;
}

bool writeNewFileDurable(const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    bool okay = true;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(
            bytes.size() - offset, 1024U * 1024U));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            okay = false;
            break;
        }
        offset += written;
    }
    if (okay) okay = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return okay;
}

bool commitSidecars(const MediaPlan& plan, const MediaCopyExecutionOptions& options,
    MediaCopyResult& result, std::vector<MediaCopyResult>& results,
    std::size_t resultIndex, std::stop_token stop, std::wstring& failure)
{
    if (options.sidecars.empty()) return true;
    result.sidecarsExpected = options.sidecars.size();
    results[resultIndex] = result;
    const auto parent = result.destination.parent_path();
    std::error_code error;
    const auto parentStatus = std::filesystem::symlink_status(parent, error);
    if (error || parentStatus.type() != std::filesystem::file_type::directory) {
        failure = L"Sidecar destination folder is unavailable or a reparse point";
        return false;
    }
    for (std::size_t index = 0; index < options.sidecars.size(); ++index) {
        if (stop.stop_requested()) {
            failure = L"Sidecar transaction was cancelled";
            return false;
        }
        std::filesystem::path filename;
        if (!safeSidecar(options.sidecars[index], filename)) {
            failure = L"Sidecar filename, format, or size is not allowed";
            return false;
        }
        const auto target = parent / filename;
        result.sidecarPaths.push_back(target);
        results[resultIndex] = result;
        if (!persistCopyJournal(options.journalPath, plan, results)) {
            failure = L"Sidecar intent could not be made durable";
            return false;
        }
        std::string existingHash;
        const auto targetStatus = std::filesystem::symlink_status(target, error);
        const bool exists = !error &&
            targetStatus.type() != std::filesystem::file_type::not_found;
        if (error && error != std::errc::no_such_file_or_directory) {
            failure = L"Existing sidecar state could not be inspected safely";
            return false;
        }
        error.clear();
        if (exists && targetStatus.type() != std::filesystem::file_type::regular) {
            failure = L"Existing sidecar is not a regular file";
            return false;
        }
        const auto staging = parent / (filename.wstring() + L".goden-sidecar-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(index));
        if (!writeNewFileDurable(staging, *options.sidecars[index].bytes)) {
            failure = L"Sidecar staging write failed";
            return false;
        }
        std::string stagingHash;
        if (!sha256(staging, stagingHash, stop)) {
            std::filesystem::remove(staging, error);
            failure = L"Sidecar staging hash failed";
            return false;
        }
        if (exists) {
            if (!sha256(target, existingHash, stop)) {
                std::filesystem::remove(staging, error);
                failure = L"Existing sidecar could not be verified";
                return false;
            }
            if (existingHash == stagingHash) {
                std::filesystem::remove(staging, error);
                ++result.sidecarsCommitted;
                results[resultIndex] = result;
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            if (plan.collision != MediaCollisionPolicy::replace ||
                options.backupRoot.empty() || within(plan.approvedRoot,
                    options.backupRoot.lexically_normal())) {
                std::filesystem::remove(staging, error);
                failure = L"Different sidecar exists; choose Replace to preserve and update it";
                return false;
            }
            const auto backupRoot = options.backupRoot.lexically_normal();
            std::filesystem::create_directories(backupRoot, error);
            const auto rootStatus = std::filesystem::symlink_status(backupRoot, error);
            if (error || rootStatus.type() != std::filesystem::file_type::directory) {
                std::filesystem::remove(staging, error);
                failure = L"Sidecar recovery root is unavailable or unsafe";
                return false;
            }
            const auto backup = backupFor(backupRoot, target,
                resultIndex + index + plan.items.size() * 2U);
            std::filesystem::copy_file(target, backup,
                std::filesystem::copy_options::none, error);
            std::string backupHash;
            if (error || !sha256(backup, backupHash, stop) ||
                backupHash != existingHash) {
                std::filesystem::remove(staging, error);
                std::filesystem::remove(backup, error);
                failure = L"Existing sidecar recovery backup failed verification";
                return false;
            }
            result.sidecarBackups.push_back(backup);
            results[resultIndex] = result;
            if (!persistCopyJournal(options.journalPath, plan, results)) {
                std::filesystem::remove(staging, error);
                failure = L"Sidecar backup state could not be made durable";
                return false;
            }
            std::string currentHash;
            const auto currentStatus = std::filesystem::symlink_status(target, error);
            if (error || currentStatus.type() != std::filesystem::file_type::regular ||
                !sha256(target, currentHash, stop) || currentHash != existingHash) {
                std::filesystem::remove(staging, error);
                failure = L"Existing sidecar changed after its backup";
                return false;
            }
        }
        if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH |
                (exists ? MOVEFILE_REPLACE_EXISTING : 0))) {
            std::filesystem::remove(staging, error);
            failure = L"Sidecar final rename failed";
            return false;
        }
        std::string finalHash;
        if (!sha256(target, finalHash, {}) || finalHash != stagingHash) {
            failure = L"Sidecar final hash did not match staging";
            return false;
        }
        ++result.sidecarsCommitted;
        results[resultIndex] = result;
        if (!persistCopyJournal(options.journalPath, plan, results)) {
            failure = L"Sidecar committed but terminal journal update failed";
            return false;
        }
    }
    return true;
}

} // namespace

bool mediaFileSha256(const std::filesystem::path& path, std::string& output,
    std::stop_token stop)
{
    return sha256(path, output, stop);
}

MediaPlan buildMediaPlan(const std::filesystem::path& approvedRoot,
    const std::vector<MediaPlanRequest>& requests,
    MediaOrganizationMode mode, MediaCollisionPolicy collision, std::stop_token stop)
{
    MediaPlan plan;
    plan.approvedRoot = approvedRoot.lexically_normal();
    plan.mode = mode;
    plan.collision = collision;
    if (stop.stop_requested()) return plan;
    std::error_code error;
    const auto rootStatus = std::filesystem::symlink_status(plan.approvedRoot, error);
    const bool validRoot = mode == MediaOrganizationMode::inPlace ||
        (!error && rootStatus.type() == std::filesystem::file_type::directory);
    std::set<std::wstring, std::less<>> reserved;
    for (const auto& request : requests) {
        if (stop.stop_requested()) { plan.items.clear(); return plan; }
        MediaPlanItem item;
        item.source = request.source.lexically_normal();
        const auto sourceStatus = std::filesystem::symlink_status(item.source, error);
        if (error || sourceStatus.type() != std::filesystem::file_type::regular) {
            error.clear();
            item.detail = L"Source is unavailable or not a regular file";
            plan.items.push_back(std::move(item));
            continue;
        }
        if (mode == MediaOrganizationMode::inPlace) {
            item.destination = item.source;
            item.disposition = MediaPlanDisposition::ready;
            item.detail = L"In-place; no file relocation";
            reserved.insert(folded(item.destination));
            plan.items.push_back(std::move(item));
            continue;
        }
        if (!validRoot) {
            item.detail = L"Approved destination root is offline or invalid";
            plan.items.push_back(std::move(item));
            continue;
        }
        std::wstring reason;
        if (!validRelative(request.relativeDestination, reason)) {
            item.detail = std::move(reason);
            plan.items.push_back(std::move(item));
            continue;
        }
        const auto sourceExtension = folded(item.source.extension());
        const auto destinationExtension = folded(request.relativeDestination.extension());
        if (sourceExtension.empty() || sourceExtension != destinationExtension) {
            item.detail = L"Destination must preserve the source extension";
            plan.items.push_back(std::move(item));
            continue;
        }
        auto destination = (plan.approvedRoot /
            request.relativeDestination).lexically_normal();
        if (!within(plan.approvedRoot, destination) ||
            !existingAncestryContained(plan.approvedRoot, destination)) {
            item.detail = L"Destination escapes the approved root or crosses a reparse boundary";
            plan.items.push_back(std::move(item));
            continue;
        }
        item.caseOnlyRename = mode == MediaOrganizationMode::move &&
            samePath(item.source, destination) && item.source != destination;
        if (mode == MediaOrganizationMode::copy && samePath(item.source, destination)) {
            item.detail = L"Copy source and destination are the same file";
            plan.items.push_back(std::move(item));
            continue;
        }
        const auto occupied = [&reserved, &error](const auto& path) {
            const auto key = folded(path);
            const auto onDisk = std::filesystem::exists(path, error);
            error.clear();
            return onDisk || reserved.contains(key);
        };
        const bool reservedConflict = reserved.contains(folded(destination));
        if (!item.caseOnlyRename && occupied(destination)) {
            if (collision == MediaCollisionPolicy::skip) {
                item.destination = destination;
                item.disposition = MediaPlanDisposition::skipped;
                item.detail = L"Destination exists; Skip policy selected";
                plan.items.push_back(std::move(item));
                continue;
            }
            if (collision == MediaCollisionPolicy::rename) {
                unsigned suffix = 2;
                do {
                    if (stop.stop_requested()) { plan.items.clear(); return plan; }
                    destination = renamedCandidate(
                        (plan.approvedRoot / request.relativeDestination).lexically_normal(),
                        suffix++);
                }
                while (occupied(destination));
            } else {
                if (reservedConflict) {
                    item.destination = destination;
                    item.detail = L"Two Replace items cannot target the same resource";
                    plan.items.push_back(std::move(item));
                    continue;
                }
                item.replaceExisting = true;
            }
        }
        reserved.insert(folded(destination));
        item.destination = std::move(destination);
        item.disposition = MediaPlanDisposition::ready;
        item.detail = item.caseOnlyRename
            ? L"Case-only Move requires same-volume staging"
            : (item.replaceExisting ? L"Replace requires destination backup"
                                    : L"Destination reserved for this batch");
        plan.items.push_back(std::move(item));
    }
    return plan;
}

std::filesystem::path buildDefaultRelativeDestination(
    const std::filesystem::path& source, const MediaNamingFields& fields)
{
    auto albumArtist = fields.albumArtist.empty() ? fields.artist : fields.albumArtist;
    albumArtist = safeComponent(std::move(albumArtist), L"Unknown artist");
    auto album = safeComponent(fields.album, L"Unknown album");
    if (!fields.year.empty())
        album += L" (" + safeComponent(fields.year, L"Unknown year") + L")";
    std::wstring stem;
    if (fields.discCount > 1 && !fields.disc.empty())
        stem += safeComponent(fields.disc, L"0") + L"-";
    if (!fields.track.empty())
        stem += safeComponent(fields.track, L"0") + L" - ";
    stem += safeComponent(fields.title.empty() ? source.stem().wstring() : fields.title,
        L"Untitled");
    return std::filesystem::path(albumArtist) / album /
        (safeComponent(std::move(stem), L"Untitled") + source.extension().wstring());
}

std::vector<MediaCopyResult> executeCopyPlan(const MediaPlan& plan,
    std::stop_token stop, const MediaCopyExecutionOptions& options)
{
    if (plan.mode != MediaOrganizationMode::copy) {
        std::vector<MediaCopyResult> rejected;
        rejected.reserve(plan.items.size());
        for (const auto& item : plan.items) {
            MediaCopyResult result;
            result.source = item.source;
            result.destination = item.destination;
            result.detail = L"Only validated Copy plans can execute through this entrypoint";
            rejected.push_back(std::move(result));
        }
        return rejected;
    }
    return executeMediaPlan(plan, stop, options);
}

std::vector<MediaCopyResult> executeMediaPlan(const MediaPlan& plan,
    std::stop_token stop, const MediaCopyExecutionOptions& options)
{
    std::vector<MediaCopyResult> results;
    results.reserve(plan.items.size());
    const auto allowed = [&options](std::wstring_view step) {
        return !options.allowStep || options.allowStep(step);
    };
    for (std::size_t index = 0; index < plan.items.size(); ++index) {
        const auto& item = plan.items[index];
        const auto resultIndex = results.size();
        MediaCopyResult result;
        result.source = item.source;
        result.destination = item.destination;
        if (stop.stop_requested()) {
            result.outcome = MediaCopyOutcome::cancelled;
            result.detail = L"Cancelled before copy";
            results.push_back(std::move(result));
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        if ((plan.mode != MediaOrganizationMode::copy &&
                plan.mode != MediaOrganizationMode::move &&
                plan.mode != MediaOrganizationMode::inPlace) ||
            item.disposition == MediaPlanDisposition::rejected) {
            result.detail = L"Only validated Copy, Move, or in-place plans can execute";
            results.push_back(std::move(result));
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        if (item.disposition == MediaPlanDisposition::skipped) {
            result.outcome = MediaCopyOutcome::skipped;
            result.detail = item.detail;
            results.push_back(std::move(result));
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        const bool editInPlace = plan.mode == MediaOrganizationMode::inPlace;
        if (!editInPlace && (!within(plan.approvedRoot, item.destination) ||
            !makeDestinationParents(plan.approvedRoot, item.destination))) {
            result.detail = L"Destination root changed or became unsafe";
            results.push_back(std::move(result));
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        Revision before{}, after{};
        std::string sourceHash, stagingHash;
        if (!revisionOf(item.source, before) ||
            !sha256(item.source, sourceHash, stop)) {
            result.outcome = stop.stop_requested()
                ? MediaCopyOutcome::cancelled : MediaCopyOutcome::failed;
            result.detail = L"Could not freeze and hash source";
            results.push_back(std::move(result));
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        result.sourceSha256 = sourceHash;

        const bool isMove = plan.mode == MediaOrganizationMode::move;
        const bool sameDrive = options.sameVolumeOverride.value_or(
            sameVolume(item.source, item.destination));
        std::optional<Revision> replacedRevision;
        std::string replacedHash;

        const bool replaceTarget = item.replaceExisting || editInPlace;
        if (replaceTarget) {
            std::error_code backupError;
            const auto normalizedBackup = options.backupRoot.lexically_normal();
            if (normalizedBackup.empty() || (!plan.approvedRoot.empty() &&
                    within(plan.approvedRoot, normalizedBackup))) {
                result.detail = editInPlace
                    ? L"In-place edit requires a recovery backup root"
                    : L"Replace requires a backup root outside the library";
                results.push_back(std::move(result));
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            std::filesystem::create_directories(normalizedBackup, backupError);
            const auto backupStatus = std::filesystem::symlink_status(
                normalizedBackup, backupError);
            if (backupError || backupStatus.type() !=
                    std::filesystem::file_type::directory) {
                result.detail = L"Replace backup root is unavailable or unsafe";
                results.push_back(std::move(result));
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            result.backup = backupFor(normalizedBackup, item.destination, index);
            std::string oldHash, backupHash;
            Revision oldRevision{};
            if (!revisionOf(item.destination, oldRevision) ||
                !sha256(item.destination, oldHash, stop)) {
                result.detail = L"Replace destination changed before backup";
                results.push_back(std::move(result));
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            replacedRevision = oldRevision;
            replacedHash = oldHash;
            std::filesystem::copy_file(item.destination, result.backup,
                std::filesystem::copy_options::none, backupError);
            if (backupError || !sha256(result.backup, backupHash, stop) ||
                backupHash != oldHash) {
                std::filesystem::remove(result.backup, backupError);
                result.backup.clear();
                result.detail = L"Replace destination backup could not be verified";
                results.push_back(std::move(result));
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            result.outcome = MediaCopyOutcome::backedUp;
            result.detail = L"Existing destination has a byte-exact recovery backup";
            results.push_back(result);
            if (!persistCopyJournal(options.journalPath, plan, results)) {
                results.back().outcome = MediaCopyOutcome::failed;
                results.back().detail = L"Could not persist Replace backup state";
                continue;
            }
        }

        if (isMove && (options.mutateStaging || options.mutateStagingForSource)) {
            std::error_code backupError;
            const auto normalizedBackup = options.backupRoot.lexically_normal();
            if (normalizedBackup.empty() || within(plan.approvedRoot, normalizedBackup)) {
                result.outcome = MediaCopyOutcome::rejected;
                result.detail = L"Move with edits requires a source backup outside the library";
                if (results.size() == resultIndex) results.push_back(result);
                else results.back() = result;
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            std::filesystem::create_directories(normalizedBackup, backupError);
            const auto backupStatus = std::filesystem::symlink_status(
                normalizedBackup, backupError);
            if (backupError || backupStatus.type() !=
                    std::filesystem::file_type::directory) {
                result.outcome = MediaCopyOutcome::rejected;
                result.detail = L"Move source backup root is unavailable or unsafe";
                if (results.size() == resultIndex) results.push_back(result);
                else results.back() = result;
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            result.sourceBackup = backupFor(normalizedBackup, item.source,
                index + plan.items.size());
            std::filesystem::copy_file(item.source, result.sourceBackup,
                std::filesystem::copy_options::none, backupError);
            std::string sourceBackupHash;
            if (backupError || !sha256(result.sourceBackup, sourceBackupHash, stop) ||
                sourceBackupHash != sourceHash) {
                std::filesystem::remove(result.sourceBackup, backupError);
                result.sourceBackup.clear();
                result.outcome = MediaCopyOutcome::failed;
                result.detail = L"Move source backup could not be verified";
                if (results.size() == resultIndex) results.push_back(result);
                else results.back() = result;
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            result.outcome = MediaCopyOutcome::backedUp;
            result.detail = L"Move source has a byte-exact recovery backup";
            if (results.size() == resultIndex) results.push_back(result);
            else results.back() = result;
            if (!persistCopyJournal(options.journalPath, plan, results)) {
                results.back().outcome = MediaCopyOutcome::failed;
                results.back().detail = L"Could not persist Move source backup state";
                continue;
            }
        }

        // A pure same-volume Move can preserve the filesystem object. Any
        // staging mutation intentionally takes the verified-copy path instead.
        if (isMove && sameDrive && !options.mutateStaging &&
            !options.mutateStagingForSource &&
            options.sidecars.empty() &&
            !item.replaceExisting) {
            result.outcome = MediaCopyOutcome::verified;
            result.bytes = before.size;
            result.sha256 = sourceHash;
            result.detail = L"Source frozen; same-volume Move awaiting rename";
            results.push_back(result);
            if (!persistCopyJournal(options.journalPath, plan, results) ||
                !allowed(L"before-move-rename")) {
                results.back().outcome = MediaCopyOutcome::failed;
                results.back().detail = L"Move stopped before source rename";
                continue;
            }
            bool renamed = false;
            if (item.caseOnlyRename) {
                const auto caseStaging = stagingFor(item.destination, index);
                result.staging = caseStaging;
                results.back().staging = caseStaging;
                results.back().detail = L"Case-only intermediate path is durable; awaiting first rename";
                if (!persistCopyJournal(options.journalPath, plan, results)) {
                    results.back().outcome = MediaCopyOutcome::failed;
                    results.back().detail = L"Could not persist case-only intermediate path";
                    continue;
                }
                renamed = MoveFileExW(item.source.c_str(), caseStaging.c_str(),
                    MOVEFILE_WRITE_THROUGH) &&
                    MoveFileExW(caseStaging.c_str(), item.destination.c_str(),
                        MOVEFILE_WRITE_THROUGH);
                if (!renamed && std::filesystem::exists(caseStaging))
                    MoveFileExW(caseStaging.c_str(), item.source.c_str(),
                        MOVEFILE_WRITE_THROUGH);
                results.back().staging.clear();
            } else {
                renamed = MoveFileExW(item.source.c_str(), item.destination.c_str(),
                    MOVEFILE_WRITE_THROUGH) != FALSE;
            }
            if (!renamed) {
                results.back().outcome = MediaCopyOutcome::conflict;
                results.back().detail = L"Same-volume Move rename failed";
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            std::string finalHash;
            if (!sha256(item.destination, finalHash, {}) || finalHash != sourceHash) {
                results.back().outcome = MediaCopyOutcome::failed;
                results.back().detail = L"Moved destination failed verification";
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            results.back().outcome = MediaCopyOutcome::moved;
            results.back().detail = L"Moved atomically on the same volume and verified";
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }

        const auto staging = stagingFor(item.destination, index);
        result.staging = staging;
        result.outcome = MediaCopyOutcome::staging;
        result.detail = L"Staging path reserved; copy not yet committed";
        if (results.size() == resultIndex) results.push_back(result);
        else results.back() = result;
        if (!persistCopyJournal(options.journalPath, plan, results)) {
            results.back().outcome = MediaCopyOutcome::failed;
            results.back().detail = L"Could not persist journal before staging";
            continue;
        }
        std::error_code error;
        std::filesystem::copy_file(item.source, staging,
            std::filesystem::copy_options::none, error);
        const bool verified = !error && revisionOf(item.source, after) &&
            before.size == after.size && before.writeTime == after.writeTime &&
            std::filesystem::file_size(staging, error) == before.size && !error &&
            sha256(staging, stagingHash, stop) && stagingHash == sourceHash;
        if (!verified) {
            std::filesystem::remove(staging, error);
            result.outcome = stop.stop_requested()
                ? MediaCopyOutcome::cancelled : MediaCopyOutcome::conflict;
            result.detail = L"Copy verification failed or source changed";
            results.back() = std::move(result);
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        if (editInPlace && options.captureSourceRevisions) {
            try {
                if (plan.items.size() != 1 || options.journalPath.empty())
                    throw std::runtime_error("Identity recovery requires a durable one-file journal");
                result.revisionBindings = options.captureSourceRevisions(item.source);
                if (result.revisionBindings.empty() || result.revisionBindings.size() > 65536)
                    throw std::runtime_error("Invalid physical subsong count");
                std::set<std::uint32_t> subsongs;
                for (const auto& binding : result.revisionBindings) {
                    const auto& id = binding.previousRevisionTrackId;
                    if (id.size() != 67 || !id.starts_with("v1:") ||
                            id.find_first_not_of("0123456789abcdef", 3) != std::string::npos ||
                            !subsongs.insert(binding.current.subsong).second)
                        throw std::runtime_error("Invalid physical revision");
                }
                result.bindingJournalPath = options.journalPath;
            } catch (...) {
                std::filesystem::remove(staging, error);
                result.revisionBindings.clear();
                result.outcome = MediaCopyOutcome::failed;
                result.detail = L"Source identity could not be captured before editing";
                results.back() = std::move(result);
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
        }
        if (options.mutateStaging || options.mutateStagingForSource) {
            std::wstring mutationError;
            const auto mutated = options.mutateStagingForSource
                ? options.mutateStagingForSource(item.source, staging, mutationError)
                : options.mutateStaging(staging, mutationError);
            if (!mutated ||
                (options.verifyStaging &&
                    !options.verifyStaging(staging, mutationError))) {
                std::filesystem::remove(staging, error);
                result.outcome = MediaCopyOutcome::failed;
                result.detail = mutationError.empty()
                    ? L"Staging tag/artwork mutation failed verification"
                    : std::move(mutationError);
                results.back() = std::move(result);
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            if (!revisionOf(item.source, after) || before.size != after.size ||
                before.writeTime != after.writeTime ||
                !sha256(staging, stagingHash, stop)) {
                std::filesystem::remove(staging, error);
                result.outcome = MediaCopyOutcome::conflict;
                result.detail = L"Source changed or edited staging could not be hashed";
                results.back() = std::move(result);
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
        }
        result.bytes = before.size;
        result.sha256 = stagingHash;
        result.outcome = MediaCopyOutcome::verified;
        result.detail = L"Staging hash verified; awaiting final rename";
        results.back() = result;
        if (!allowed(L"before-verified-journal") || !persistCopyJournal(options.journalPath, plan, results)) {
            std::filesystem::remove(staging, error);
            results.back().outcome = MediaCopyOutcome::failed;
            results.back().detail = L"Could not persist verified-staging state";
            continue;
        }
        if (replaceTarget) {
            Revision currentDestination{};
            std::string currentDestinationHash;
            if (!replacedRevision ||
                !revisionOf(item.destination, currentDestination) ||
                !sameRevision(*replacedRevision, currentDestination) ||
                !sha256(item.destination, currentDestinationHash, stop) ||
                currentDestinationHash != replacedHash) {
                std::filesystem::remove(staging, error);
                result.outcome = MediaCopyOutcome::conflict;
                result.detail = L"Replace destination changed after its backup";
                results.back() = std::move(result);
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
        }
        if (!allowed(L"before-final-rename") || !MoveFileExW(staging.c_str(),
                item.destination.c_str(), MOVEFILE_WRITE_THROUGH |
                    (replaceTarget ? MOVEFILE_REPLACE_EXISTING : 0))) {
            std::filesystem::remove(staging, error);
            result.outcome = MediaCopyOutcome::conflict;
            result.detail = L"Destination appeared before final rename";
            results.back() = std::move(result);
            if (!persistCopyJournal(options.journalPath, plan, results) &&
                    !results.back().bindingJournalPath.empty())
                results.back().physicalCommitNeedsBinding = true;
            continue;
        }
        result.staging.clear();
        result.physicalCommitNeedsBinding = !result.revisionBindings.empty();
        if (options.allowStep) (void)options.allowStep(L"after-final-rename");
        std::string finalHash;
        const bool verifiedFinal = result.revisionBindings.empty()
            ? sha256(item.destination, finalHash, {}) && finalHash == stagingHash
            : freezeMediaBindingRevisions(item.destination, result.revisionBindings, {}, stagingHash);
        if (!verifiedFinal) {
            result.outcome = MediaCopyOutcome::failed;
            result.detail = L"Final destination did not match verified staging";
            results.back() = std::move(result);
            persistCopyJournal(options.journalPath, plan, results);
            continue;
        }
        result.outcome = MediaCopyOutcome::copied;
        if (!result.revisionBindings.empty()) finalHash = stagingHash;
        std::error_code finalSizeError;
        result.bytes = std::filesystem::file_size(item.destination, finalSizeError);
        if (finalSizeError) result.bytes = before.size;
        result.sha256 = std::move(finalHash);
        result.detail = editInPlace
            ? L"Edited in place through verified staging; recovery backup retained"
            : L"Copied through same-folder staging and hash verified";
        results.back() = std::move(result);
        if (!persistCopyJournal(options.journalPath, plan, results)) {
            results.back().detail =
                L"Destination is verified, but terminal journal persistence failed";
            continue;
        }
        if (!options.sidecars.empty()) {
            auto sidecarResult = results.back();
            std::wstring sidecarFailure;
            if (!commitSidecars(plan, options, sidecarResult, results,
                    resultIndex, stop, sidecarFailure)) {
                results.back() = std::move(sidecarResult);
                results.back().detail += L"; sidecar attention: " + sidecarFailure;
                if (isMove) {
                    results.back().outcome = MediaCopyOutcome::duplicateRecoverable;
                    results.back().detail += L"; source retained";
                }
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            results.back() = std::move(sidecarResult);
            results.back().detail += L"; sidecar artwork committed " +
                std::to_wstring(results.back().sidecarsCommitted) + L"/" +
                std::to_wstring(options.sidecars.size());
            persistCopyJournal(options.journalPath, plan, results);
        }
        if (isMove) {
            // This terminal destination state must be durable before crossing
            // the only destructive boundary in the operation.
            if (stop.stop_requested() || !allowed(L"before-source-delete")) {
                results.back().outcome = MediaCopyOutcome::duplicateRecoverable;
                results.back().detail = L"Destination verified; source retained for recovery";
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            Revision deleteRevision{};
            if (!revisionOf(item.source, deleteRevision) ||
                deleteRevision.size != before.size ||
                deleteRevision.writeTime != before.writeTime ||
                !DeleteFileW(item.source.c_str())) {
                results.back().outcome = MediaCopyOutcome::duplicateRecoverable;
                results.back().detail = L"Destination verified; source deletion failed, duplicate retained";
                persistCopyJournal(options.journalPath, plan, results);
                continue;
            }
            results.back().outcome = MediaCopyOutcome::moved;
            results.back().detail = sameDrive
                ? L"Move with staging edits committed and source removed"
                : L"Cross-volume Move verified before source removal";
            persistCopyJournal(options.journalPath, plan, results);
        }
    }
    for (auto& result : results) {
        if (!result.bindingJournalPath.empty() &&
                !mediaFileSha256(result.bindingJournalPath, result.bindingJournalSha256, {}))
            result.identityRefreshFailed = true;
    }
    return results;
}

std::vector<MediaCopyResult> restoreInPlaceBackups(
    const std::vector<MediaCopyResult>& completed, std::stop_token stop,
    const MediaRestoreExecutionOptions& options)
{
    std::vector<MediaCopyResult> restored;
    restored.reserve(completed.size());
    for (std::size_t index = 0; index < completed.size(); ++index) {
        const auto& committed = completed[index];
        MediaCopyResult result = committed;
        result.bindingJournalPath.clear();
        result.bindingJournalSha256.clear();
        result.physicalCommitNeedsBinding = false;
        result.outcome = MediaCopyOutcome::rejected;
        if (stop.stop_requested()) {
            result.outcome = MediaCopyOutcome::cancelled;
            result.detail = L"Undo cancelled before mutation";
            restored.push_back(std::move(result));
            continue;
        }
        if (options.allowRestore && !options.allowRestore(committed.destination)) {
            result.outcome = MediaCopyOutcome::skipped;
            result.detail = L"Undo deferred because the target is protected; recovery backup retained";
            restored.push_back(std::move(result));
            continue;
        }
        std::string currentHash, backupHash;
        if (committed.outcome != MediaCopyOutcome::copied ||
            committed.destination.empty() || committed.backup.empty() ||
            !sha256(committed.destination, currentHash, stop) ||
            currentHash != committed.sha256) {
            result.outcome = MediaCopyOutcome::conflict;
            result.detail = L"Undo refused because the edited file changed externally";
            restored.push_back(std::move(result));
            continue;
        }
        if (!sha256(committed.backup, backupHash, stop) ||
            backupHash != committed.sourceSha256) {
            result.outcome = MediaCopyOutcome::failed;
            result.detail = L"Undo recovery backup is missing or no longer byte-exact";
            restored.push_back(std::move(result));
            continue;
        }
        const auto staging = stagingFor(committed.destination,
            completed.size() + index);
        std::error_code error;
        std::filesystem::copy_file(committed.backup, staging,
            std::filesystem::copy_options::none, error);
        std::string stagingHash;
        if (error || !sha256(staging, stagingHash, stop) ||
            stagingHash != committed.sourceSha256) {
            std::filesystem::remove(staging, error);
            result.outcome = MediaCopyOutcome::failed;
            result.detail = L"Undo staging could not be verified";
            restored.push_back(std::move(result));
            continue;
        }
        if (!sha256(committed.destination, currentHash, stop) ||
            currentHash != committed.sha256) {
            std::filesystem::remove(staging, error);
            result.outcome = stop.stop_requested()
                ? MediaCopyOutcome::cancelled : MediaCopyOutcome::conflict;
            result.detail = L"Undo stopped because the destination changed or the operation was cancelled";
            restored.push_back(std::move(result));
            continue;
        }
        // Hashing/copying can take time. Playback or cancellation may have
        // changed since admission; neither is permission to replace the file.
        const bool mayRestore = !options.allowRestore ||
            options.allowRestore(committed.destination);
        if (stop.stop_requested() || !mayRestore) {
            std::filesystem::remove(staging, error);
            result.outcome = stop.stop_requested()
                ? MediaCopyOutcome::cancelled : MediaCopyOutcome::skipped;
            result.detail = stop.stop_requested()
                ? L"Undo cancelled before replacement; recovery backup retained"
                : L"Undo deferred because the target became protected; recovery backup retained";
            restored.push_back(std::move(result));
            continue;
        }
        MediaPlan restorePlan;
        restorePlan.mode = MediaOrganizationMode::inPlace;
        if (!result.revisionBindings.empty()) {
            if (options.journalPath.empty()) {
                std::filesystem::remove(staging, error);
                result.outcome = MediaCopyOutcome::failed;
                result.detail = L"Undo needs a durable identity recovery journal";
                restored.push_back(std::move(result));
                continue;
            }
            result.bindingJournalPath = options.journalPath.parent_path() /
                (options.journalPath.stem().wstring() + L"-undo-" + std::to_wstring(index) + L".json");
            result.source = result.destination;
            result.sourceSha256 = committed.sha256;
            result.sha256 = stagingHash;
            result.staging = staging;
            result.outcome = MediaCopyOutcome::verified;
            if (!persistCopyJournal(result.bindingJournalPath, restorePlan, {result})) {
                std::filesystem::remove(staging, error);
                result.outcome = MediaCopyOutcome::failed;
                result.detail = L"Undo stopped before replacement because its recovery intent was not durable";
                restored.push_back(std::move(result));
                continue;
            }
        }
        if (!MoveFileExW(staging.c_str(), committed.destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(staging, error);
            result.outcome = MediaCopyOutcome::conflict;
            result.detail = L"Undo stopped because the destination changed before commit";
            if (!result.bindingJournalPath.empty() &&
                    !persistCopyJournal(result.bindingJournalPath, restorePlan, {result}))
                result.physicalCommitNeedsBinding = true;
            restored.push_back(std::move(result));
            continue;
        }
        result.physicalCommitNeedsBinding = !result.revisionBindings.empty();
        if (options.allowStep) (void)options.allowStep(L"after-undo-rename");
        std::string finalHash;
        const bool verifiedFinal = result.revisionBindings.empty()
            ? sha256(committed.destination, finalHash, {}) && finalHash == committed.sourceSha256
            : freezeMediaBindingRevisions(committed.destination, result.revisionBindings, {}, committed.sourceSha256);
        if (!verifiedFinal) {
            result.outcome = MediaCopyOutcome::failed;
            result.detail = L"Undo commit did not match the recovery backup";
        } else {
            result.outcome = MediaCopyOutcome::moved;
            result.sha256 = result.revisionBindings.empty() ? std::move(finalHash) : committed.sourceSha256;
            result.detail = L"Restored byte-exact in-place recovery backup";
        }
        result.staging.clear();
        if (!result.bindingJournalPath.empty()) {
            if (!persistCopyJournal(result.bindingJournalPath, restorePlan, {result}))
                result.detail += L"; terminal journal update failed; verified intent retained";
            if (!mediaFileSha256(result.bindingJournalPath, result.bindingJournalSha256, {}))
                result.identityRefreshFailed = true;
        }
        restored.push_back(std::move(result));
    }
    return restored;
}

} // namespace nekorox::component
