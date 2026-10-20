#include "legacy_import_service.h"

#include "sdk_compat.h"
#include "text_util.h"

#include "nekorox/database.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <climits>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace nekorox::component {
namespace {

constexpr std::string_view kImportSetting = "legacy_yttm_import_v2";
constexpr std::string_view kCanonicalAssetPrefix = "assets/legacy-yttm/";
constexpr std::size_t kMaximumBiographyBytes = 512U * 1024U;
constexpr std::uint64_t kStatusPublishInterval = 16U;

std::int64_t utcNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring lowercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(value)));
    });
    return value;
}

bool isImage(const std::filesystem::path& path)
{
    const auto extension = lowercase(path.extension().wstring());
    return extension == L".jpg" || extension == L".jpeg" ||
        extension == L".png" || extension == L".webp" ||
        extension == L".bmp";
}

std::filesystem::path extendedPath(const std::filesystem::path& path)
{
    if (!path.is_absolute()) return path;
    const auto native = path.native();
    if (native.starts_with(LR"(\\?\)")) return path;
    if (native.starts_with(LR"(\\)")) {
        auto extended = std::filesystem::path(LR"(\\?\UNC\)");
        extended += native.substr(2U);
        return extended;
    }
    auto extended = std::filesystem::path(LR"(\\?\)");
    extended += native;
    return extended;
}

std::string singleLine(std::string value)
{
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

void writeStatusFile(const std::filesystem::path& profile,
    const LegacyImportSnapshot& state)
{
    if (profile.empty()) return;
    const auto directory = profile / L"nekorox";
    const auto destination = directory / L"legacy-import-status.txt";
    const auto temporary = directory / L".legacy-import-status.part";
    std::error_code fileError;
    std::filesystem::create_directories(extendedPath(directory), fileError);
    if (fileError) return;

    std::ofstream output(extendedPath(temporary),
        std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "version=1\n"
        << "phase=" << singleLine(state.phase) << '\n'
        << "running=" << (state.running ? 1 : 0) << '\n'
        << "complete=" << (state.complete ? 1 : 0) << '\n'
        << "source_available=" << (state.sourceAvailable ? 1 : 0) << '\n'
        << "files=" << state.files << '\n'
        << "total_files=" << state.totalFiles << '\n'
        << "bytes=" << state.bytes << '\n'
        << "personas=" << state.personas << '\n'
        << "art_assets=" << state.artAssets << '\n'
        << "current_file=" << singleLine(state.currentFile) << '\n'
        << "error=" << singleLine(state.error) << '\n'
        << "updated_utc_ms=" << utcNowMs() << '\n';
    output.flush();
    output.close();
    if (!output) {
        std::filesystem::remove(extendedPath(temporary), fileError);
        return;
    }
    if (!MoveFileExW(extendedPath(temporary).c_str(),
            extendedPath(destination).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(extendedPath(temporary), fileError);
    }
}

bool allowedLegacyFile(const std::filesystem::path& relative)
{
    if (relative.empty()) return false;
    const auto first = lowercase((*relative.begin()).wstring());
    if (first == L"biography" || first == L"review") {
        return lowercase(relative.extension().wstring()) == L".txt";
    }
    return (first == L"art_img" || first == L"rev_img") && isImage(relative);
}

std::wstring artistFromLegacyImage(const std::filesystem::path& path)
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

std::string stablePersonaId(std::string_view aliasKey)
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset;
    for (const auto byte : aliasKey) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    std::ostringstream output;
    output << "legacy:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

void restoreMarkerCounts(
    std::string_view marker, LegacyImportSnapshot& state) noexcept
{
    const auto read = [&](std::string_view name, std::uint64_t& destination) {
        const auto prefix = std::string(name) + "=";
        auto offset = marker.find(prefix);
        if (offset == std::string_view::npos) return;
        offset += prefix.size();
        const auto end = marker.find(';', offset);
        const auto text = marker.substr(
            offset, end == std::string_view::npos ? marker.size() - offset : end - offset);
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(
            text.data(), text.data() + text.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) {
            destination = parsed;
        }
    };
    read("files", state.files);
    state.totalFiles = state.files;
    read("bytes", state.bytes);
    read("personas", state.personas);
    read("art", state.artAssets);
}

class Sha256 final {
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(
                &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
        DWORD bytes = 0;
        DWORD objectLength = 0;
        DWORD hashLength = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                &bytes, 0) != 0 ||
            BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                &bytes, 0) != 0) {
            return;
        }
        object_.resize(objectLength);
        digest_.resize(hashLength);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(),
                static_cast<ULONG>(object_.size()), nullptr, 0, 0) != 0) {
            hash_ = nullptr;
        }
    }

    ~Sha256()
    {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    bool valid() const noexcept { return hash_ != nullptr; }

    bool update(const char* data, std::size_t size)
    {
        if (!valid() || size > static_cast<std::size_t>(ULONG_MAX)) return false;
        return BCryptHashData(hash_, reinterpret_cast<PUCHAR>(
            const_cast<char*>(data)), static_cast<ULONG>(size), 0) == 0;
    }

    std::string finish()
    {
        if (!valid() || BCryptFinishHash(hash_, digest_.data(),
                static_cast<ULONG>(digest_.size()), 0) != 0) {
            return {};
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto byte : digest_) output << std::setw(2) << unsigned(byte);
        return output.str();
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
    std::vector<UCHAR> digest_;
};

bool hashFile(const std::filesystem::path& path, std::atomic<bool>& stopping,
    std::string& hash, std::string& error)
{
    std::ifstream input(extendedPath(path), std::ios::binary);
    if (!input) {
        error = "could not open legacy asset for hashing";
        return false;
    }
    Sha256 digest;
    if (!digest.valid()) {
        error = "could not initialize SHA-256";
        return false;
    }
    std::vector<char> buffer(1024U * 1024U);
    while (!stopping.load(std::memory_order_acquire) && input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > 0 && !digest.update(buffer.data(), count)) {
            error = "could not hash legacy asset";
            return false;
        }
    }
    if (stopping.load(std::memory_order_acquire)) return false;
    if (!input.eof()) {
        error = "could not read legacy asset";
        return false;
    }
    hash = digest.finish();
    if (hash.empty()) error = "could not finalize legacy asset hash";
    return !hash.empty();
}

bool copyAndHash(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::atomic<bool>& stopping,
    std::string& hash, std::uint64_t& bytes, std::string& error)
{
    std::error_code fileError;
    const auto sourceSize = std::filesystem::file_size(
        extendedPath(source), fileError);
    if (fileError) {
        error = "could not inspect legacy asset: " + fileError.message();
        return false;
    }
    std::filesystem::create_directories(
        extendedPath(destination.parent_path()), fileError);
    if (fileError) {
        error = "could not create canonical asset directory: " + fileError.message();
        return false;
    }
    const auto destinationSize = std::filesystem::file_size(
        extendedPath(destination), fileError);
    if (!fileError && destinationSize == sourceSize) {
        std::string sourceHash;
        std::string destinationHash;
        if (!hashFile(source, stopping, sourceHash, error) ||
                !hashFile(destination, stopping, destinationHash, error)) {
            return false;
        }
        if (sourceHash == destinationHash) {
            hash = std::move(sourceHash);
            bytes += sourceSize;
            return true;
        }
    }
    fileError.clear();
    const auto temporary = destination.parent_path() / L".nrx-import.part";
    std::filesystem::remove(extendedPath(temporary), fileError);

    std::ifstream input(extendedPath(source), std::ios::binary);
    std::ofstream output(extendedPath(temporary),
        std::ios::binary | std::ios::trunc);
    Sha256 digest;
    if (!input || !output || !digest.valid()) {
        error = "could not open transactional legacy asset copy";
        return false;
    }
    std::vector<char> buffer(1024U * 1024U);
    while (!stopping.load(std::memory_order_acquire) && input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) continue;
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output || !digest.update(buffer.data(), count)) {
            error = "could not write canonical legacy asset";
            break;
        }
    }
    output.flush();
    output.close();
    const auto cancelled = stopping.load(std::memory_order_acquire);
    if (!error.empty() || cancelled || (!input.eof() && input.fail())) {
        std::filesystem::remove(extendedPath(temporary), fileError);
        if (!cancelled && error.empty()) error = "could not read legacy asset";
        return false;
    }
    hash = digest.finish();
    if (hash.empty()) {
        std::filesystem::remove(extendedPath(temporary), fileError);
        error = "could not finalize canonical legacy asset hash";
        return false;
    }
    if (!MoveFileExW(extendedPath(temporary).c_str(),
            extendedPath(destination).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto nativeError = GetLastError();
        std::filesystem::remove(extendedPath(temporary), fileError);
        error = "could not promote canonical legacy asset: " +
            std::system_category().message(static_cast<int>(nativeError));
        return false;
    }
    const auto modified = std::filesystem::last_write_time(
        extendedPath(source), fileError);
    if (!fileError) {
        std::filesystem::last_write_time(
            extendedPath(destination), modified, fileError);
    }
    bytes += sourceSize;
    return true;
}

struct ImportedFile {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path relative;
    std::string hash;
};

} // namespace

LegacyImportService& LegacyImportService::instance()
{
    static LegacyImportService service;
    return service;
}

void LegacyImportService::start()
{
    if (worker_.joinable()) return;
    std::filesystem::path profile;
    try {
        const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
        profile = utf8ToWide(native.c_str());
    } catch (...) {
    }
    stopping_.store(false, std::memory_order_release);
    worker_ = std::thread([this, profile = std::move(profile)] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        run(profile);
    });
}

void LegacyImportService::stop()
{
    stopping_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

LegacyImportSnapshot LegacyImportService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void LegacyImportService::publish(LegacyImportSnapshot snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snapshot);
}

void LegacyImportService::run(std::filesystem::path profile)
{
    LegacyImportSnapshot state;
    state.running = true;
    state.phase = "starting";
    auto publishState = [&] {
        publish(state);
        writeStatusFile(profile, state);
    };
    auto publishCancelled = [&] {
        state.running = false;
        state.complete = false;
        state.phase = "cancelled";
        state.currentFile.clear();
        publishState();
    };
    publishState();
    if (profile.empty()) {
        state.running = false;
        state.phase = "error";
        state.error = "portable profile path is unavailable";
        publishState();
        return;
    }

    const auto sourceRoot = profile / L"yttm";
    const auto destinationRoot =
        profile / L"nekorox" / L"assets" / L"legacy-yttm";
    std::error_code fileError;
    state.sourceAvailable = std::filesystem::is_directory(sourceRoot, fileError);

    NekoDatabase database;
    std::string error;
    if (!database.open(profile / L"nekorox" / L"nekorox.db", error)) {
        state.running = false;
        state.phase = "error";
        state.error = "legacy import database open failed: " + error;
        publishState();
        return;
    }
    std::string marker;
    bool markerFound = false;
    if (!database.readSetting(
            std::string(kImportSetting), marker, markerFound, error)) {
        state.running = false;
        state.phase = "error";
        state.error = "legacy import marker read failed: " + error;
        publishState();
        return;
    }
    if (markerFound && marker.starts_with("complete;version=2;") &&
            std::filesystem::is_directory(destinationRoot, fileError)) {
        restoreMarkerCounts(marker, state);
        state.running = false;
        state.complete = true;
        state.phase = "already-complete";
        publishState();
        return;
    }
    if (!state.sourceAvailable) {
        state.running = false;
        state.complete = true;
        state.phase = "no-source";
        publishState();
        return;
    }

    state.phase = "scanning";
    publishState();
    std::vector<ImportedFile> files;
    std::filesystem::recursive_directory_iterator iterator(
        sourceRoot, std::filesystem::directory_options::skip_permission_denied,
        fileError);
    const std::filesystem::recursive_directory_iterator end;
    while (!fileError && iterator != end) {
        if (stopping_.load(std::memory_order_acquire)) {
            publishCancelled();
            return;
        }
        const auto status = iterator->symlink_status(fileError);
        if (!fileError && std::filesystem::is_regular_file(status) &&
                !std::filesystem::is_symlink(status)) {
            auto relative = std::filesystem::relative(
                iterator->path(), sourceRoot, fileError);
            if (!fileError && allowedLegacyFile(relative)) {
                files.push_back({
                    iterator->path(), destinationRoot / relative,
                    std::move(relative), {}});
            }
        }
        iterator.increment(fileError);
    }
    if (fileError) {
        state.running = false;
        state.phase = "error";
        state.error = "legacy source scan failed: " + fileError.message();
        publishState();
        return;
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.relative.native() < right.relative.native();
    });
    state.totalFiles = files.size();
    state.phase = "copying";
    publishState();

    std::map<std::string, PersistedPersona> personas;
    std::vector<std::pair<std::string, PersistedArtAsset>> artAssets;
    for (auto& file : files) {
        if (stopping_.load(std::memory_order_acquire)) {
            publishCancelled();
            return;
        }
        state.currentFile = wideToUtf8(file.relative.generic_wstring());
        if (!copyAndHash(file.source, file.destination, stopping_,
                file.hash, state.bytes, error)) {
            if (stopping_.load(std::memory_order_acquire)) {
                publishCancelled();
                return;
            }
            state.running = false;
            state.phase = "error";
            state.error = error + " [relative=" + state.currentFile + "]";
            publishState();
            return;
        }
        ++state.files;
        if (state.files % kStatusPublishInterval == 0U) publishState();
        const auto first = lowercase((*file.relative.begin()).wstring());
        if (first == L"biography") {
            const auto display = file.relative.stem().wstring();
            const auto key = personaAliasKey(display);
            const auto profileText = readLocalTextFile(
                file.destination, kMaximumBiographyBytes);
            if (!key.empty() && !profileText.empty()) {
                auto& persona = personas[key];
                persona.personaId = stablePersonaId(key);
                persona.displayName = wideToUtf8(display);
                persona.profile = wideToUtf8(profileText);
                persona.aliases = {{persona.displayName, key}};
                persona.updatedUtcMs = utcNowMs();
            }
        } else if (first == L"art_img") {
            const auto display = artistFromLegacyImage(file.relative);
            const auto key = personaAliasKey(display);
            if (!key.empty() && !file.hash.empty()) {
                auto& persona = personas[key];
                if (persona.personaId.empty()) {
                    persona.personaId = stablePersonaId(key);
                    persona.displayName = wideToUtf8(display);
                    persona.aliases = {{persona.displayName, key}};
                    persona.updatedUtcMs = utcNowMs();
                }
                PersistedArtAsset asset;
                asset.assetId = stablePersonaId(key) + ":sha256:" + file.hash;
                asset.personaId = persona.personaId;
                asset.role = "artist";
                const auto canonicalRelative = file.destination.lexically_relative(
                    profile / L"nekorox");
                if (canonicalRelative.empty()) {
                    state.running = false;
                    state.phase = "error";
                    state.error = "canonical asset escaped its root [relative=" +
                        state.currentFile + "]";
                    publishState();
                    return;
                }
                asset.path = wideToUtf8(canonicalRelative.generic_wstring());
                asset.contentHash = file.hash;
                asset.updatedUtcMs = utcNowMs();
                artAssets.emplace_back(key, std::move(asset));
            }
        }
    }

    state.phase = "database";
    state.currentFile.clear();
    publishState();
    for (auto& [key, imported] : personas) {
        if (stopping_.load(std::memory_order_acquire)) {
            publishCancelled();
            return;
        }
        PersistedPersona existing;
        if (!database.readPersonaByAliasKey(key, existing, error)) break;
        if (existing.found) {
            if (existing.profile.empty() && !imported.profile.empty()) {
                existing.profile = imported.profile;
                existing.updatedUtcMs = imported.updatedUtcMs;
                if (!database.upsertPersona(existing, error)) break;
            }
            imported.personaId = existing.personaId;
        } else if (!database.upsertPersona(imported, error)) {
            break;
        }
        ++state.personas;
    }
    if (error.empty() && !database.deleteUnselectedArtAssetsWithPathPrefix(
            std::string(kCanonicalAssetPrefix), error)) {
        error = "could not replace the reproducible legacy art index: " + error;
    }
    if (error.empty()) {
        std::set<std::string> indexedAssets;
        for (auto& [key, asset] : artAssets) {
            const auto persona = personas.find(key);
            if (persona == personas.end()) continue;
            asset.personaId = persona->second.personaId;
            if (!indexedAssets.insert(asset.assetId).second) continue;
            if (!database.upsertArtAsset(asset, error)) break;
            ++state.artAssets;
        }
    }
    if (!error.empty()) {
        state.running = false;
        state.phase = "error";
        state.error = "legacy database import failed: " + error;
        publishState();
        return;
    }

    std::ostringstream markerValue;
    markerValue << "complete;version=2;files=" << state.files
        << ";bytes=" << state.bytes
        << ";personas=" << state.personas
        << ";art=" << state.artAssets;
    if (!database.setSetting(std::string(kImportSetting), markerValue.str(),
            utcNowMs(), error)) {
        state.running = false;
        state.phase = "error";
        state.error = "legacy import marker write failed: " + error;
        publishState();
        return;
    }
    state.running = false;
    state.complete = true;
    state.phase = "complete";
    state.currentFile.clear();
    publishState();
}

} // namespace nekorox::component
