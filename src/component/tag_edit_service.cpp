#include "tag_edit_service.h"
#include "qa_hooks.h"

#include "database_service.h"
#include "media_file_operation_service.h"
#include "operation_sequence.h"
#include "tag_recovery_journal.h"
#include "text_util.h"
#include "util/json.h"

#include <nekorox/track_identity.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <system_error>

namespace nekorox::component {
namespace {

constexpr std::uint64_t kBackupBudget = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kUndoRecoveryEnumeratedLimit = 4096;
constexpr std::size_t kUndoRecoveryNewestLimit = 64;
constexpr std::size_t kUndoRecoveryParsedBytesLimit = 32U * 1024U * 1024U;
constexpr std::size_t kUndoRecoveryCandidateLimit = 16;

bool qaProfileEnabled() noexcept
{
    if (!qa::hooksAllowed()) return false;
    wchar_t hooks[4]{}, profile[4]{}, background[4]{};
    return GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks, 4) == 1 &&
        hooks[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile, 4) == 1 &&
        profile[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_BACKGROUND", background, 4) == 1 &&
        background[0] == L'1';
}

std::optional<unsigned long> qaUnsigned(const wchar_t* name) noexcept
{
    if (!qaProfileEnabled()) return std::nullopt;
    wchar_t value[32]{};
    const auto length = GetEnvironmentVariableW(name, value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) return std::nullopt;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoul(value, &end, 10);
    if (end == value || *end != L'\0') return std::nullopt;
    return parsed;
}

struct Revision {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};
};

bool revisionOf(const std::filesystem::path& path, Revision& output) noexcept
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    output.size = std::filesystem::file_size(path, error);
    if (error) return false;
    output.writeTime = std::filesystem::last_write_time(path, error);
    return !error;
}

bool sameRevision(const Revision& left, const Revision& right) noexcept
{
    return left.size == right.size && left.writeTime == right.writeTime;
}

std::string hex(const std::array<std::uint8_t, 32>& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto value : bytes) {
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

bool sha256File(const std::filesystem::path& path, std::string& output,
    std::stop_token stop = {})
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD resultBytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
            &resultBytes, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<std::uint8_t> object(objectBytes);
    std::array<std::uint8_t, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
            nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    // foobar worker threads can have a 1 MiB stack. Keeping the I/O buffer on
    // that stack made Apply deterministically overflow before the first write.
    std::vector<char> buffer(256 * 1024);
    bool okay = static_cast<bool>(stream);
    while (okay && stream && !stop.stop_requested()) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) != 0) okay = false;
    }
    if (stream.bad() || stop.stop_requested()) okay = false;
    if (okay && BCryptFinishHash(hash, digest.data(),
            static_cast<ULONG>(digest.size()), 0) == 0) output = hex(digest);
    else okay = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return okay;
}

bool copyFileCancellable(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::stop_token stop)
{
    if (stop.stop_requested()) return false;
    std::error_code destinationError;
    if (std::filesystem::exists(destination, destinationError) ||
            destinationError) return false;
    // CopyFileEx preserves source file attributes and times for the guarded
    // restore, while its progress callback lets shutdown cancel a long copy.
    auto progress = [](LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
            LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID context) -> DWORD {
        return static_cast<std::stop_token*>(context)->stop_requested()
            ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
    };
    const auto okay = CopyFileExW(source.c_str(), destination.c_str(),
        progress, &stop, nullptr, COPY_FILE_FAIL_IF_EXISTS) != FALSE &&
        !stop.stop_requested();
    if (!okay) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
    }
    return okay;
}

std::filesystem::path nativePath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        const auto native = filesystem::g_get_native_path(track->get_path());
        return std::filesystem::path(utf8ToWide(native.c_str()));
    } catch (...) {
        return {};
    }
}

std::filesystem::path backupRoot()
{
    try {
        const auto native = filesystem::g_get_native_path(
            core_api::get_profile_path());
        return std::filesystem::path(utf8ToWide(native.c_str())) /
            L"goden" / L"tag-backups";
    } catch (...) {
        return {};
    }
}

std::wstring operationId()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::wostringstream value;
    value << std::hex << ticks << L'-' << GetCurrentProcessId() << L'-'
          << GetTickCount64();
    return value.str();
}

std::string jsonEscape(std::wstring_view value)
{
    const auto utf8 = wideToUtf8(value);
    std::string output;
    output.reserve(utf8.size() + 8);
    for (const auto c : utf8) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output.push_back(c); break;
        }
    }
    return output;
}

std::string jsonEscape(std::string_view value)
{
    return jsonEscape(utf8ToWide(value));
}

const char* mutationModeText(nekorox::TagMutationMode mode) noexcept
{
    switch (mode) {
    case nekorox::TagMutationMode::keep: return "keep";
    case nekorox::TagMutationMode::set: return "set";
    case nekorox::TagMutationMode::remove: return "remove";
    }
    return "unknown";
}

bool persistJournal(const TagApplySummary& summary)
{
    if (summary.journalPath.empty()) return false;
    const auto staging = summary.journalPath.wstring() + L".staging";
    std::ofstream stream(std::filesystem::path(staging),
        std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << "{\n  \"schema\":\"goden.tag-operation.v2\",\n"
           << "  \"operationSeq\":" << summary.operationSeq << ",\n"
           << "  \"operationId\":\"" << jsonEscape(summary.operationId)
           << "\",\n  \"mutations\":[\n";
    for (std::size_t index = 0; index < summary.mutations.size(); ++index) {
        const auto& mutation = summary.mutations[index];
        stream << "    {\"field\":\"" << jsonEscape(mutation.field)
               << "\",\"mode\":\"" << mutationModeText(mutation.mode)
               << "\",\"values\":[";
        for (std::size_t value = 0; value < mutation.values.size(); ++value) {
            stream << "\"" << jsonEscape(mutation.values[value]) << "\""
                   << (value + 1U == mutation.values.size() ? "" : ",");
        }
        stream << "]}" << (index + 1U == summary.mutations.size() ? "\n" : ",\n");
    }
    stream << "  ],\n  \"files\":[\n";
    for (std::size_t index = 0; index < summary.files.size(); ++index) {
        const auto& item = summary.files[index];
        stream << "    {\"path\":\"" << jsonEscape(item.path.wstring())
               << "\",\"backupPath\":\"" << jsonEscape(item.backupPath.wstring())
               << "\",\"sourceSha256\":\"" << item.sourceHash
               << "\",\"postWriteSha256\":\"" << item.postWriteHash
               << "\",\"subsongs\":[";
        for (std::size_t subsong = 0; subsong < item.subsongs.size(); ++subsong) {
            if (subsong) stream << ',';
            stream << item.subsongs[subsong];
        }
        stream << "],\"outcome\":\""
               << jsonEscape(tagFileOutcomeText(item.outcome))
               << "\",\"detail\":\"" << jsonEscape(item.detail) << "\"}"
               << (index + 1U == summary.files.size() ? "\n" : ",\n");
    }
    stream << "  ],\n  \"items\":[\n";
    for (std::size_t index = 0; index < summary.items.size(); ++index) {
        const auto& item = summary.items[index];
        stream << "    {\"itemId\":\"" << jsonEscape(item.itemId)
               << "\",\"subsong\":" << item.subsong
               << ",\"draftRevision\":" << item.draftRevision
               << ",\"beforeRevision\":\"" << item.beforeRevision
               << "\",\"afterRevision\":\"" << item.afterRevision
               << "\",\"outcome\":\""
               << jsonEscape(tagFileOutcomeText(item.outcome)) << "\"}"
               << (index + 1U == summary.items.size() ? "\n" : ",\n");
    }
    stream << "  ]\n}\n";
    stream.flush();
    if (!stream) return false;
    stream.close();
    return MoveFileExW(staging.c_str(), summary.journalPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool undoable(const TagFileResult& item) noexcept
{
    const auto terminalWrite = item.outcome == TagFileOutcome::committed ||
        item.outcome == TagFileOutcome::recoverablePartial;
    return terminalWrite && !item.backupPath.empty() &&
        item.sourceHash.size() == 64 && item.postWriteHash.size() == 64;
}

std::optional<TagRecoveryJournal> readRecoveryJournal(const std::filesystem::path& path)
{
    try {
        const auto bytes = std::filesystem::file_size(path);
        if (bytes == 0 || bytes > TagRecoveryJournal::maxBytes) return std::nullopt;
        std::ifstream stream(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(bytes), '\0');
        if (!stream.read(text.data(), static_cast<std::streamsize>(bytes)) ||
                stream.peek() != std::char_traits<char>::eof()) return std::nullopt;
        return TagRecoveryJournal::parse(std::move(text));
    } catch (...) {
        return std::nullopt;
    }
}

bool persistRecoveryJournal(const std::filesystem::path& path, const TagRecoveryJournal& document)
{
    const auto text = document.render();
    if (!text) return false;
    const auto staging = path.wstring() + L".staging";
    std::ofstream stream(std::filesystem::path(staging), std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(text->data(), static_cast<std::streamsize>(text->size()));
    stream.flush();
    if (!stream) return false;
    stream.close();
    return MoveFileExW(staging.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool applyMutationsToInfo(file_info& info,
    const std::vector<nekorox::TagMutation>& mutations)
{
    bool changed = false;
    for (const auto& mutation : mutations) {
        if (mutation.mode == nekorox::TagMutationMode::keep) continue;
        const auto existing = info.meta_find(mutation.field.c_str());
        if (mutation.mode == nekorox::TagMutationMode::remove) {
            if (existing != pfc_infinite) {
                info.meta_remove_field(mutation.field.c_str());
                changed = true;
            }
            continue;
        }
        bool same = existing != pfc_infinite &&
            info.meta_enum_value_count(existing) == mutation.values.size();
        if (same) {
            for (std::size_t index = 0; index < mutation.values.size(); ++index) {
                if (std::string_view(info.meta_enum_value(existing, index)) !=
                        mutation.values[index]) {
                    same = false;
                    break;
                }
            }
        }
        if (same) continue;
        info.meta_remove_field(mutation.field.c_str());
        const auto field = info.meta_set(mutation.field.c_str(),
            mutation.values.front().c_str());
        for (std::size_t index = 1; index < mutation.values.size(); ++index) {
            info.meta_add_value(field, mutation.values[index].c_str());
        }
        changed = true;
    }
    return changed;
}

bool verifyInfo(const file_info& info,
    const std::vector<nekorox::TagMutation>& mutations)
{
    for (const auto& mutation : mutations) {
        if (mutation.mode == nekorox::TagMutationMode::keep) continue;
        const auto field = info.meta_find(mutation.field.c_str());
        if (mutation.mode == nekorox::TagMutationMode::remove) {
            if (field != pfc_infinite) return false;
            continue;
        }
        if (field == pfc_infinite ||
            info.meta_enum_value_count(field) != mutation.values.size()) return false;
        for (std::size_t index = 0; index < mutation.values.size(); ++index) {
            if (std::string_view(info.meta_enum_value(field, index)) !=
                    mutation.values[index]) return false;
        }
    }
    return true;
}

} // namespace

struct TagEditService::Operation {
    struct PhysicalFile {
        struct OldRevision {
            std::string trackId;
            std::uint32_t subsong = 0;
        };
        std::filesystem::path path;
        Revision expected;
        std::string sourceHash;
        std::filesystem::path backup;
        metadb_handle_list handles;
        std::map<std::uint32_t, std::vector<nekorox::TagMutation>>
            mutationsBySubsong;
        std::shared_ptr<std::atomic_bool> changed;
        std::vector<OldRevision> oldRevisions;
        std::vector<std::size_t> itemResultIndices;
        std::size_t resultIndex = 0;
        std::size_t undoSourceResultIndex = 0;
        std::size_t recoveredFileIndex = 0;
    };
    std::uint64_t generation = 0;
    HWND owner = nullptr;
    Completion completion;
    std::vector<PhysicalFile> physical;
    std::optional<std::size_t> activeIndex;
    bool bindingStarted = false;
    bool undoMode = false;
    bool recovered = false;
    TagApplySummary summary;
    // The worker consumes a private copy; observers keep reading the original
    // operation until finish publishes this snapshot under mutex_.
    std::shared_ptr<Operation> undoSource;
    TagApplySummary undoSourceSummary;
};

struct TagEditService::RecoveryState {
    struct File {
        TagFileResult result;
        Revision expected;
        std::size_t journalFileIndex = 0;
    };
    struct Candidate {
        std::filesystem::path journal;
        std::uint64_t sequence = 0;
        std::wstring id;
        std::vector<File> files;
    };
    std::vector<Candidate> candidates;
    std::size_t candidateIndex = 0;
    std::size_t fileIndex = 0;
    std::size_t subsongIndex = 0;
    std::optional<std::size_t> currentPhysicalIndex;
    std::string canonicalPath;
    std::shared_ptr<Operation> operation;
};

TagEditService& TagEditService::instance()
{
    static TagEditService value;
    return value;
}

TagEditService::~TagEditService()
{
    shutdownAndDrain();
}

bool TagEditService::busy() const noexcept
{
    std::lock_guard lock(mutex_);
    return active_ != nullptr;
}

bool TagEditService::hasUndo() const noexcept
{
    std::lock_guard lock(mutex_);
    return lastCompleted_ != nullptr;
}

std::uint64_t TagEditService::undoOperationSequence() const noexcept
{
    std::lock_guard lock(mutex_);
    return lastCompleted_ ? lastCompleted_->summary.operationSeq : 0;
}

bool TagEditService::undoRecoveryPending() const noexcept
{
    std::lock_guard lock(mutex_);
    return undoRecoveryPending_;
}

bool TagEditService::undoRecoveryLimited() const noexcept
{
    std::lock_guard lock(mutex_);
    return undoRecoveryLimited_;
}

void TagEditService::beginUndoRecovery(HWND observer, UINT completedMessage)
{
    // The SDK profile path is captured on the main thread. Only ordinary
    // filesystem and JSON work runs on the discovery worker.
    undoObserver_ = observer;
    undoCompletedMessage_ = completedMessage;
    {
        std::lock_guard lock(mutex_);
        if (undoLoadAttempted_ || lastCompleted_) return;
        undoLoadAttempted_ = true;
        undoRecoveryPending_ = true;
        undoRecoveryLimited_ = false;
    }
    const auto root = backupRoot();
    const auto generation = undoGeneration_.fetch_add(1) + 1;
    undoWorker_ = std::jthread([this, root, generation](std::stop_token stop) {
        std::vector<RecoveryState::Candidate> candidates;
        bool limited = false;
        try {
        std::vector<std::pair<std::filesystem::file_time_type,
            std::filesystem::path>> journals;
        std::size_t enumerated = 0;
        std::error_code error;
        if (std::filesystem::symlink_status(root, error).type() ==
                std::filesystem::file_type::directory) {
            error.clear();
            for (std::filesystem::directory_iterator iterator(root,
                    std::filesystem::directory_options::skip_permission_denied,
                    error), end; !error && iterator != end &&
                    !stop.stop_requested(); iterator.increment(error)) {
                if (++enumerated > kUndoRecoveryEnumeratedLimit) {
                    limited = true;
                    break;
                }
                if (iterator->symlink_status(error).type() !=
                        std::filesystem::file_type::directory || error) {
                    error.clear();
                    continue;
                }
                const auto journal = iterator->path() / L"journal.json";
                if (std::filesystem::symlink_status(journal, error).type() !=
                        std::filesystem::file_type::regular || error) {
                    error.clear();
                    continue;
                }
                const auto size = std::filesystem::file_size(journal, error);
                if (error || size == 0 || size > 4U * 1024U * 1024U) {
                    error.clear();
                    continue;
                }
                const auto written = std::filesystem::last_write_time(journal, error);
                if (!error) {
                    if (journals.size() < kUndoRecoveryNewestLimit) {
                        journals.emplace_back(written, journal);
                    } else {
                        limited = true;
                        const auto oldest = std::min_element(journals.begin(),
                            journals.end(), [](const auto& a, const auto& b) {
                                return a.first < b.first;
                            });
                        if (written > oldest->first) *oldest = {written, journal};
                    }
                }
                error.clear();
            }
        }
        std::sort(journals.begin(), journals.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
        std::size_t parsedBytes = 0;
        for (const auto& [_, journal] : journals) {
            if (stop.stop_requested()) break;
            try {
                const auto bytes = std::filesystem::file_size(journal);
                if (bytes == 0 || bytes > 4U * 1024U * 1024U) continue;
                if (bytes > kUndoRecoveryParsedBytesLimit - parsedBytes) {
                    limited = true;
                    break;
                }
                parsedBytes += static_cast<std::size_t>(bytes);
                std::ifstream stream(journal, std::ios::binary);
                std::string text(static_cast<std::size_t>(bytes), '\0');
                if (!stream.read(text.data(), static_cast<std::streamsize>(bytes)) ||
                        stream.peek() != std::char_traits<char>::eof()) continue;
                const auto document = sonare::util::json::parse_strict(text);
                if (!document.is_object()) continue;
                const auto* schema = document.find("schema");
                const auto* files = document.find("files");
                if (!schema || !schema->is_string() ||
                    schema->as_string() != "goden.tag-operation.v2" ||
                    !files || !files->is_array()) continue;
                RecoveryState::Candidate candidate;
                candidate.journal = journal;
                if (const auto* sequence = document.find("operationSeq");
                        sequence && sequence->is_number()) {
                    const auto number = sequence->as_number();
                    if (number > 0.0 && number <= 9007199254740991.0) {
                        candidate.sequence = static_cast<std::uint64_t>(number);
                        observeOperationSequence(candidate.sequence);
                    }
                }
                if (const auto* id = document.find("operationId");
                        id && id->is_string())
                    candidate.id = utf8ToWide(id->as_string());
                for (std::size_t journalIndex = 0; journalIndex < files->as_array().size(); ++journalIndex) {
                    const auto& entry = files->as_array()[journalIndex];
                    if (stop.stop_requested()) break;
                    if (!entry.is_object()) continue;
                    const auto* outcome = entry.find("outcome");
                    const auto* path = entry.find("path");
                    const auto* backup = entry.find("backupPath");
                    const auto* source = entry.find("sourceSha256");
                    const auto* post = entry.find("postWriteSha256");
                    const auto* subsongs = entry.find("subsongs");
                    if (!outcome || !outcome->is_string() ||
                        (outcome->as_string() != "Committed" &&
                         outcome->as_string() != "RecoverablePartial") ||
                        !path || !path->is_string() ||
                        !backup || !backup->is_string() ||
                        !source || !source->is_string() ||
                        !post || !post->is_string() ||
                        source->as_string().size() != 64 ||
                        post->as_string().size() != 64 ||
                        !subsongs || !subsongs->is_array()) continue;
                    RecoveryState::File file;
                    file.journalFileIndex = journalIndex;
                    file.result.path = utf8ToWide(path->as_string());
                    file.result.backupPath = utf8ToWide(backup->as_string());
                    error.clear();
                    if (std::filesystem::symlink_status(file.result.path, error).type() !=
                            std::filesystem::file_type::regular || error) continue;
                    error.clear();
                    if (std::filesystem::symlink_status(file.result.backupPath,
                            error).type() !=
                            std::filesystem::file_type::regular || error ||
                            !revisionOf(file.result.path, file.expected)) continue;
                    for (const auto& value : subsongs->as_array()) {
                        if (value.is_number() && value.as_number() >= 0 &&
                                value.as_number() <= UINT32_MAX)
                            file.result.subsongs.push_back(
                                static_cast<std::uint32_t>(value.as_number()));
                    }
                    if (file.result.subsongs.empty()) continue;
                    file.result.sourceHash = source->as_string();
                    file.result.postWriteHash = post->as_string();
                    file.result.outcome = outcome->as_string() == "Committed"
                        ? TagFileOutcome::committed : TagFileOutcome::recoverablePartial;
                    file.result.detail = L"Recovered durable Undo plan after restart";
                    candidate.files.push_back(std::move(file));
                }
                if (!candidate.files.empty()) {
                    candidates.push_back(std::move(candidate));
                    if (candidates.size() == kUndoRecoveryCandidateLimit) {
                        limited = limited || journals.back().second != journal;
                        break;
                    }
                }
            } catch (...) {
                continue;
            }
        }
        } catch (...) {
            // An inaccessible backup root must not terminate the host.
            limited = true;
        }
        if (stop.stop_requested()) return;
        fb2k::inMainThread([this, generation,
                limited, candidates = std::move(candidates)]() mutable {
            if (generation != undoGeneration_.load()) return;
            {
                std::lock_guard lock(mutex_);
                undoRecoveryLimited_ = limited;
            }
            recovery_ = std::make_unique<RecoveryState>();
            recovery_->candidates = std::move(candidates);
            bindRecoveredUndo(generation);
        });
    });
}

void TagEditService::clearUndoRecoveryObserver(HWND observer) noexcept
{
    if (undoObserver_ == observer) {
        undoObserver_ = nullptr;
        undoCompletedMessage_ = 0;
    }
}

void TagEditService::bindRecoveredUndo(std::uint64_t generation)
{
    if (generation != undoGeneration_.load() || !recovery_) return;
    auto& state = *recovery_;
    // At most eight SDK handle bindings per main-thread callback. A path that
    // needs canonicalization is processed only once in that callback.
    unsigned steps = 0;
    while (steps < 8 && state.candidateIndex < state.candidates.size()) {
        const auto& candidate = state.candidates[state.candidateIndex];
        if (!state.operation) {
            state.operation = std::make_shared<Operation>();
            state.operation->recovered = true;
            state.operation->summary.journalPath = candidate.journal;
            state.operation->summary.operationSeq = candidate.sequence;
            state.operation->summary.operationId = candidate.id;
        }
        if (state.fileIndex == candidate.files.size()) {
            if (!state.operation->physical.empty()) {
                std::lock_guard lock(mutex_);
                if (!lastCompleted_) lastCompleted_ = state.operation;
                break;
            }
            ++state.candidateIndex;
            state.fileIndex = 0;
            state.operation.reset();
            continue;
        }
        const auto& source = candidate.files[state.fileIndex];
        if (state.subsongIndex == 0) {
            try {
                pfc::string8 canonical;
                filesystem::g_get_canonical_path(
                    wideToUtf8(source.result.path.wstring()).c_str(), canonical);
                state.canonicalPath = canonical.c_str();
            } catch (...) {
                state.canonicalPath.clear();
            }
            ++steps;
        }
        if (state.subsongIndex < source.result.subsongs.size() &&
                steps < 8) {
            const auto subsong = source.result.subsongs[state.subsongIndex++];
            try {
                if (!state.canonicalPath.empty()) {
                    auto handle = metadb::get()->handle_create(
                        state.canonicalPath.c_str(), subsong);
                    if (handle.is_valid()) {
                        if (!state.currentPhysicalIndex) {
                            Operation::PhysicalFile physical;
                            physical.path = source.result.path;
                            physical.expected = source.expected;
                            physical.sourceHash = source.result.sourceHash;
                            physical.backup = source.result.backupPath;
                            physical.resultIndex = state.operation->summary.files.size();
                            physical.recoveredFileIndex = source.journalFileIndex;
                            state.currentPhysicalIndex = state.operation->physical.size();
                            state.operation->physical.push_back(std::move(physical));
                            auto result = source.result;
                            result.subsongs.clear();
                            state.operation->summary.files.push_back(std::move(result));
                        }
                        state.operation->physical[*state.currentPhysicalIndex].handles.add_item(handle);
                        state.operation->summary.files.back().subsongs.push_back(subsong);
                    }
                }
            } catch (...) {
            }
            ++steps;
        }
        if (state.subsongIndex == source.result.subsongs.size() ||
                state.canonicalPath.empty()) {
            ++state.fileIndex;
            state.subsongIndex = 0;
            state.currentPhysicalIndex.reset();
            state.canonicalPath.clear();
        }
    }
    if (state.candidateIndex == state.candidates.size() ||
            (state.operation && !state.operation->physical.empty() &&
             state.fileIndex == state.candidates[state.candidateIndex].files.size())) {
        recovery_.reset();
        {
            std::lock_guard lock(mutex_);
            undoRecoveryPending_ = false;
        }
        if (undoObserver_ && undoCompletedMessage_)
            PostMessageW(undoObserver_, undoCompletedMessage_, 0, 0);
    } else {
        fb2k::inMainThread([this, generation] { bindRecoveredUndo(generation); });
    }
}

bool TagEditService::start(metadb_handle_list_cref tracks,
    std::vector<nekorox::TagMutation> mutations, HWND owner,
    Completion completion)
{
    std::vector<TagEditPlanItem> plan;
    plan.reserve(tracks.get_count());
    for (t_size index = 0; index < tracks.get_count(); ++index) {
        plan.push_back({tracks[index], std::to_string(index), mutations, 0});
    }
    return start(std::move(plan), owner, std::move(completion));
}

bool TagEditService::start(std::vector<TagEditPlanItem> plan, HWND owner,
    Completion completion)
{
    if (MediaFileOperationService::instance().busy()) return false;
    for (const auto& item : plan)
        if (!MediaFileOperationService::instance().mutationAllowed(nativePath(item.track))) return false;
    std::string error;
    if (plan.empty()) return false;
    for (const auto& item : plan) {
        if (!item.track.is_valid() || item.itemId.empty() ||
            !nekorox::validateTagMutations(item.mutations, error) ||
            std::none_of(item.mutations.begin(), item.mutations.end(),
                [](const auto& mutation) {
                    return mutation.mode != nekorox::TagMutationMode::keep;
                })) return false;
    }
    std::lock_guard lock(mutex_);
    if (active_) return false;
    if (worker_.joinable()) worker_.join();
    auto operation = std::make_shared<Operation>();
    operation->generation = generation_.fetch_add(1) + 1;
    operation->owner = owner;
    operation->completion = std::move(completion);
    operation->summary.operationSeq = nextOperationSequence();
    // Kept only as a human-readable union in the existing journal schema. The
    // authoritative write plan lives per subsong in PhysicalFile.
    for (const auto& item : plan) {
        operation->summary.mutations.insert(operation->summary.mutations.end(),
            item.mutations.begin(), item.mutations.end());
    }
    operation->summary.operationId = operationId();
    const auto root = backupRoot() / operation->summary.operationId;
    operation->summary.journalPath = root / L"journal.json";
    std::map<std::wstring, std::size_t, std::less<>> physicalByPath;
    for (auto& item : plan) {
        const auto track = item.track;
        const auto path = nativePath(track);
        if (path.empty()) continue;
        const auto subsong = static_cast<std::uint32_t>(track->get_subsong_index());
        const auto itemResultIndex = operation->summary.items.size();
        operation->summary.items.push_back({item.itemId, subsong,
            item.draftRevision, {}, {}, TagFileOutcome::planned});
        auto key = path.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        auto found = physicalByPath.find(key);
        if (found == physicalByPath.end()) {
            Operation::PhysicalFile file;
            file.path = path;
            file.resultIndex = operation->summary.files.size();
            file.handles.add_item(track);
            file.mutationsBySubsong.emplace(subsong, std::move(item.mutations));
            file.itemResultIndices.push_back(itemResultIndex);
            physicalByPath.emplace(std::move(key), operation->physical.size());
            operation->physical.push_back(std::move(file));
            TagFileResult result;
            result.path = path;
            result.outcome = TagFileOutcome::planned;
            result.detail = L"Waiting for validation";
            operation->summary.files.push_back(std::move(result));
        } else {
            auto& file = operation->physical[found->second];
            if (!file.mutationsBySubsong.emplace(
                    subsong, std::move(item.mutations)).second) return false;
            file.handles.add_item(track);
            file.itemResultIndices.push_back(itemResultIndex);
        }
    }
    if (operation->physical.empty()) return false;
    active_ = operation;
    worker_ = std::jthread([this, operation](std::stop_token stop) {
        prepare(operation, stop);
    });
    return true;
}

void TagEditService::cancelPending() noexcept
{
    generation_.fetch_add(1);
    if (worker_.joinable()) worker_.request_stop();
}

void TagEditService::shutdownAndDrain() noexcept
{
    generation_.fetch_add(1);
    undoGeneration_.fetch_add(1);
    if (undoWorker_.joinable()) {
        undoWorker_.request_stop();
        undoWorker_.join();
    }
    recovery_.reset();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    std::shared_ptr<Operation> operation;
    {
        std::lock_guard lock(mutex_);
        operation = std::move(active_);
    }
    if (!operation) return;
    for (auto& result : operation->summary.files) {
        if (result.outcome == TagFileOutcome::planned ||
            result.outcome == TagFileOutcome::deferredUntilReleased) {
            result.outcome = TagFileOutcome::cancelledBeforeWrite;
            result.detail = L"Host shutdown completed before this write began";
        } else if (result.outcome == TagFileOutcome::mutationIntentDurable ||
            result.outcome == TagFileOutcome::restoreIntentDurable ||
            result.outcome == TagFileOutcome::bindingPending) {
            result.outcome = TagFileOutcome::recoverablePartial;
            result.detail = L"Write completed before shutdown; identity reconciliation is required";
        }
    }
    persistJournal(operation->summary);
}

void TagEditService::resumeDeferred()
{
    metadb_handle_ptr playing;
    const auto playingPath = playback_control::get()->get_now_playing(playing)
        ? nativePath(playing) : std::filesystem::path{};
    std::shared_ptr<Operation> operation;
    {
        std::lock_guard lock(mutex_);
        playbackPath_ = playingPath;
        operation = active_;
    }
    // Undo has its own worker and completes with a manual retry when protected.
    // Its results must never be changed concurrently or dispatched as Apply.
    if (!operation || operation->undoMode ||
            operation->generation != generation_.load()) return;
    bool resumed = false;
    for (auto& file : operation->physical) {
        auto& result = operation->summary.files[file.resultIndex];
        if (result.outcome != TagFileOutcome::deferredUntilReleased) continue;
        if (!playingPath.empty() &&
            _wcsicmp(playingPath.c_str(), file.path.c_str()) == 0) continue;
        result.outcome = TagFileOutcome::planned;
        result.detail = L"Playback released; retrying verified mutation";
        resumed = true;
    }
    if (!resumed) return;
    if (!persistJournal(operation->summary)) return;
    if (!operation->activeIndex) startNext(operation);
}

void TagEditService::prepare(std::shared_ptr<Operation> operation,
    std::stop_token stop)
{
    std::error_code error;
    const auto root = operation->summary.journalPath.parent_path();
    std::filesystem::create_directories(root, error);
    std::uint64_t reserved = 0;
    for (std::size_t index = 0; index < operation->physical.size(); ++index) {
        auto& file = operation->physical[index];
        auto& result = operation->summary.files[file.resultIndex];
        if (stop.stop_requested() || operation->generation != generation_.load()) {
            result.outcome = TagFileOutcome::cancelledBeforeWrite;
            result.detail = L"Cancelled before mutation";
            continue;
        }
        if (!revisionOf(file.path, file.expected)) {
            result.outcome = TagFileOutcome::failedBeforeWrite;
            result.detail = L"Source is unavailable or not a regular file";
            continue;
        }
        if (file.expected.size > kBackupBudget - reserved) {
            result.outcome = TagFileOutcome::failedBeforeWrite;
            result.detail = L"2 GiB backup budget would be exceeded";
            continue;
        }
        reserved += file.expected.size;
        file.backup = root / (std::to_wstring(index) + file.path.extension().wstring());
        if (!sha256File(file.path, file.sourceHash, stop)) {
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::failedBeforeWrite;
            result.detail = stop.stop_requested()
                ? L"Cancelled while hashing source"
                : L"Could not hash source";
            continue;
        }
        try {
            std::set<std::uint32_t> physicalSubsongs;
            input_info_reader::ptr reader;
            abort_callback_impl inputAbort;
            std::stop_callback cancelInput(stop, [&] { inputAbort.abort(); });
            input_entry::g_open_for_info_read(reader, nullptr,
                file.handles[0]->get_path(), inputAbort);
            for (t_uint32 subsongIndex = 0;
                    subsongIndex < reader->get_subsong_count(); ++subsongIndex)
                physicalSubsongs.insert(reader->get_subsong(subsongIndex));
            if (physicalSubsongs.empty()) {
                for (t_size handle = 0; handle < file.handles.get_count(); ++handle)
                    physicalSubsongs.insert(file.handles[handle]->get_subsong_index());
            }
            for (const auto subsong : physicalSubsongs) {
                const auto identity = nekorox::makeTrackId({file.path, subsong,
                    file.expected.size, static_cast<std::uint64_t>(
                        file.expected.writeTime.time_since_epoch().count())});
                file.oldRevisions.push_back({identity.value, subsong});
                result.subsongs.push_back(subsong);
            }
            for (const auto itemIndex : file.itemResultIndices) {
                if (itemIndex >= operation->summary.items.size()) continue;
                auto& item = operation->summary.items[itemIndex];
                const auto old = std::find_if(file.oldRevisions.begin(),
                    file.oldRevisions.end(), [&](const auto& value) {
                        return value.subsong == item.subsong;
                    });
                if (old != file.oldRevisions.end()) item.beforeRevision = old->trackId;
            }
        } catch (...) {
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::failedBeforeWrite;
            result.detail = stop.stop_requested()
                ? L"Cancelled during metadata read"
                : L"Could not freeze the pre-write track identity";
            continue;
        }
        const auto copied = copyFileCancellable(file.path, file.backup, stop);
        Revision after{};
        std::string backupHash;
        if (!copied || !revisionOf(file.path, after) ||
            !sameRevision(file.expected, after) ||
            !sha256File(file.backup, backupHash, stop) ||
            backupHash != file.sourceHash) {
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::failedBeforeWrite;
            result.detail = stop.stop_requested()
                ? L"Cancelled before mutation"
                : L"Backup verification failed or source changed";
            error.clear();
            std::filesystem::remove(file.backup, error);
            continue;
        }
        result.outcome = TagFileOutcome::backedUp;
        result.detail = L"Backup verified; mutation has not started";
        result.backupPath = file.backup;
        result.sourceHash = file.sourceHash;
    }
    if (!persistJournal(operation->summary)) {
        for (auto& result : operation->summary.files) {
            if (result.outcome == TagFileOutcome::backedUp) {
                result.outcome = TagFileOutcome::failedBeforeWrite;
                result.detail = L"Backup is safe, but mutation journal could not be made durable";
            }
        }
        fb2k::inMainThread([this, operation] { finish(operation); });
        return;
    }
    for (auto& result : operation->summary.files) {
        if (result.outcome == TagFileOutcome::backedUp) {
            result.outcome = TagFileOutcome::planned;
            result.detail = L"Verified backup is durable; waiting for mutation intent";
        }
    }
    if (const auto pause = qaUnsigned(L"NEKOROX_QA_TAG_AFTER_BACKUP_PAUSE_MS")) {
        // Bounded and available only under the three-part disposable-QA gate.
        auto remaining = (std::min)(*pause, 10000UL);
        while (remaining > 0 && !stop.stop_requested()) {
            const auto slice = (std::min)(remaining, 20UL);
            Sleep(slice);
            remaining -= slice;
        }
    }
    fb2k::inMainThread([this, operation] { startNext(operation); });
}

void TagEditService::startNext(std::shared_ptr<Operation> operation)
{
    if (operation->undoMode) return;
    if (operation->activeIndex) return;
    if (operation->generation != generation_.load()) {
        for (auto& result : operation->summary.files) {
            if (result.outcome == TagFileOutcome::planned) {
                result.outcome = TagFileOutcome::cancelledBeforeWrite;
                result.detail = L"Cancelled before mutation";
            }
        }
        finish(operation);
        return;
    }
    for (std::size_t itemIndex = 0;
            itemIndex < operation->physical.size(); ++itemIndex) {
        auto& file = operation->physical[itemIndex];
        auto& result = operation->summary.files[file.resultIndex];
        if (result.outcome != TagFileOutcome::planned) continue;
        if (const auto failIndex = qaUnsigned(
                L"NEKOROX_QA_TAG_FAIL_BEFORE_WRITE_INDEX");
            failIndex && *failIndex == itemIndex) {
            result.outcome = TagFileOutcome::failedBeforeWrite;
            result.detail = L"Disposable-QA injected failure before write";
            persistJournal(operation->summary);
            continue;
        }
        metadb_handle_ptr playing;
        if (playback_control::get()->get_now_playing(playing) &&
            !nativePath(playing).empty() &&
            _wcsicmp(nativePath(playing).c_str(), file.path.c_str()) == 0) {
            result.outcome = TagFileOutcome::deferredUntilReleased;
            result.detail = L"Current playback file was not modified";
            persistJournal(operation->summary);
            continue;
        }
        Revision current{};
        if (!revisionOf(file.path, current) || !sameRevision(file.expected, current)) {
            result.outcome = TagFileOutcome::conflictExternalChange;
            result.detail = L"Source changed after the operation was planned";
            persistJournal(operation->summary);
            continue;
        }
        result.outcome = TagFileOutcome::mutationIntentDurable;
        result.detail = L"Mutation intent is durable; SDK write starting";
        if (!persistJournal(operation->summary)) {
            result.outcome = TagFileOutcome::failedBeforeWrite;
            result.detail = L"Mutation journal could not be made durable";
            continue;
        }
        operation->activeIndex = itemIndex;
        const auto mutationsBySubsong = file.mutationsBySubsong;
        file.changed = std::make_shared<std::atomic_bool>(false);
        const auto changed = file.changed;
        const auto filter = file_info_filter::create(
            [mutationsBySubsong, changed](auto trackRef, t_filestats,
                    file_info& info) {
                const auto found = mutationsBySubsong.find(
                    static_cast<std::uint32_t>(trackRef->get_subsong_index()));
                if (found == mutationsBySubsong.end()) return false;
                const auto didChange = applyMutationsToInfo(info, found->second);
                if (didChange) changed->store(true, std::memory_order_relaxed);
                return didChange;
            });
        auto notify = fb2k::makeCompletionNotify(
            [this, operation, itemIndex](unsigned status) {
                updateCompleted(operation, itemIndex, status);
            });
        metadb_io_v2::get()->update_info_async(file.handles, filter,
            operation->owner,
            metadb_io_v2::op_flag_background |
                metadb_io_v2::op_flag_delay_ui |
                metadb_io_v2::op_flag_no_errors,
            notify);
        return;
    }
    const auto deferred = std::any_of(operation->summary.files.begin(),
        operation->summary.files.end(), [](const auto& result) {
            return result.outcome == TagFileOutcome::deferredUntilReleased;
        });
    if (deferred) {
        persistJournal(operation->summary);
        return;
    }
    const auto binding = std::any_of(operation->summary.files.begin(),
        operation->summary.files.end(), [](const auto& result) {
            return result.outcome == TagFileOutcome::bindingPending;
        });
    if (binding && !operation->bindingStarted) {
        operation->bindingStarted = true;
        std::vector<TrackRevisionRebind> requests;
        for (const auto& file : operation->physical) {
            const auto& result = operation->summary.files[file.resultIndex];
            if (result.outcome != TagFileOutcome::bindingPending) continue;
            for (const auto& old : file.oldRevisions) {
                requests.push_back({old.trackId, file.path, old.subsong});
            }
        }
        DatabaseService::instance().rebindTagRevisions(std::move(requests),
            [this, operation](bool success, std::string error) mutable {
                bindCompleted(operation, success, std::move(error));
            });
        return;
    }
    finish(operation);
}

void TagEditService::updateCompleted(std::shared_ptr<Operation> operation,
    std::size_t itemIndex, unsigned status)
{
    if (!operation->activeIndex || *operation->activeIndex != itemIndex ||
        itemIndex >= operation->physical.size()) return;
    auto& file = operation->physical[itemIndex];
    auto& result = operation->summary.files[file.resultIndex];
    if (status != metadb_io::update_info_success) {
        result.outcome = TagFileOutcome::recoverablePartial;
        result.detail = status == metadb_io::update_info_aborted
            ? L"Host cancelled the write; verified backup retained"
            : L"Host reported a write error; verified backup retained";
        operation->activeIndex.reset();
        persistJournal(operation->summary);
        startNext(operation);
        return;
    }
    auto notify = fb2k::makeCompletionNotify(
        [this, operation, itemIndex](unsigned loadStatus) {
            rereadCompleted(operation, itemIndex, loadStatus);
        });
    metadb_io_v2::get()->load_info_async(file.handles,
        metadb_io::load_info_force, operation->owner,
        metadb_io_v2::op_flag_background |
            metadb_io_v2::op_flag_delay_ui |
            metadb_io_v2::op_flag_no_errors,
        notify);
}

void TagEditService::rereadCompleted(std::shared_ptr<Operation> operation,
    std::size_t itemIndex, unsigned status)
{
    if (!operation->activeIndex || *operation->activeIndex != itemIndex ||
        itemIndex >= operation->physical.size()) return;
    auto& file = operation->physical[itemIndex];
    auto& result = operation->summary.files[file.resultIndex];
    bool verified = status == metadb_io::load_info_success;
    for (t_size index = 0; verified && index < file.handles.get_count(); ++index) {
        const auto container = file.handles[index]->get_info_ref();
        const auto found = file.mutationsBySubsong.find(static_cast<std::uint32_t>(
            file.handles[index]->get_subsong_index()));
        verified = container.is_valid() &&
            found != file.mutationsBySubsong.end() &&
            verifyInfo(container->info(), found->second);
    }
    const auto changed = file.changed &&
        file.changed->load(std::memory_order_relaxed);
    if (verified && changed) {
        result.detail = L"Re-read verified; hashing post-write revision";
        persistJournal(operation->summary);
        capturePostWrite(operation, itemIndex);
        return;
    }
    result.outcome = verified ? TagFileOutcome::noChange
                              : TagFileOutcome::recoverablePartial;
    result.detail = verified
        ? L"Requested values already matched; no write was needed"
        : L"Re-read did not confirm every requested field; backup retained";
    operation->activeIndex.reset();
    persistJournal(operation->summary);
    startNext(operation);
}

void TagEditService::capturePostWrite(std::shared_ptr<Operation> operation,
    std::size_t itemIndex)
{
    if (!operation->activeIndex || *operation->activeIndex != itemIndex ||
        itemIndex >= operation->physical.size()) return;
    // Freeze inputs on the UI thread. The worker must not read or modify the
    // live summary while cancellation or shutdown updates operation state.
    const auto path = operation->physical[itemIndex].path;
    std::vector<std::pair<std::size_t, std::uint32_t>> items;
    for (const auto resultIndex : operation->physical[itemIndex].itemResultIndices) {
        if (resultIndex < operation->summary.items.size()) {
            items.emplace_back(resultIndex, operation->summary.items[resultIndex].subsong);
        }
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, operation, itemIndex, path,
            items = std::move(items)](std::stop_token stop) {
        std::string hash;
        std::vector<std::pair<std::size_t, std::string>> identities;
        bool okay = false;
        try {
            Revision postWriteRevision{};
            okay = !stop.stop_requested() && sha256File(path, hash, stop) &&
                revisionOf(path, postWriteRevision);
            if (okay) {
                identities.reserve(items.size());
                for (const auto& [resultIndex, subsong] : items) {
                    if (stop.stop_requested()) { okay = false; break; }
                    identities.emplace_back(resultIndex, nekorox::makeTrackId({
                        path, subsong, postWriteRevision.size,
                        static_cast<std::uint64_t>(postWriteRevision.writeTime
                            .time_since_epoch().count())}).value);
                }
                okay = okay && !stop.stop_requested();
            }
        } catch (...) {
            // An identity failure must not escape the worker or publish a
            // partially updated set of subsongs. Keep a completed full hash
            // so guarded Undo remains possible after an identity-only failure.
            okay = false;
        }
        fb2k::inMainThread([this, operation, itemIndex, okay,
                hash = std::move(hash), identities = std::move(identities)]() mutable {
            {
                std::lock_guard lock(mutex_);
                if (active_ != operation) return; // Shutdown has already drained it.
            }
            if (!operation->activeIndex || *operation->activeIndex != itemIndex ||
                itemIndex >= operation->physical.size()) return;
            auto& file = operation->physical[itemIndex];
            auto& result = operation->summary.files[file.resultIndex];
            okay = okay && operation->generation == generation_.load() &&
                std::all_of(identities.begin(), identities.end(), [&](const auto& item) {
                    return item.first < operation->summary.items.size();
                });
            result.postWriteHash = std::move(hash);
            if (okay) {
                for (auto& [resultIndex, identity] : identities) {
                    operation->summary.items[resultIndex].afterRevision = std::move(identity);
                }
                result.outcome = TagFileOutcome::bindingPending;
                result.detail = L"Re-read verified; binding identity revision";
            } else {
                result.outcome = TagFileOutcome::recoverablePartial;
                result.detail = L"Post-write verification interrupted or failed; verified backup retained";
            }
            operation->activeIndex.reset();
            persistJournal(operation->summary);
            startNext(operation);
        });
    });
}

void TagEditService::bindCompleted(std::shared_ptr<Operation> operation,
    bool success, std::string error)
{
    for (auto& result : operation->summary.files) {
        if (result.outcome != TagFileOutcome::bindingPending) continue;
        result.outcome = success ? TagFileOutcome::committed
                                 : TagFileOutcome::recoverablePartial;
        result.detail = success
            ? L"Re-read and identity rebound verified; backup retained for Undo"
            : L"Tags were written but identity rebound failed: " + utf8ToWide(error);
    }
    persistJournal(operation->summary);
    finish(operation);
}

void TagEditService::finish(std::shared_ptr<Operation> operation)
{
    {
        std::lock_guard lock(mutex_);
        if (active_ != operation) return;
    }
    for (const auto& file : operation->physical) {
        if (file.resultIndex >= operation->summary.files.size()) continue;
        const auto outcome = operation->summary.files[file.resultIndex].outcome;
        for (const auto itemIndex : file.itemResultIndices) {
            if (itemIndex >= operation->summary.items.size()) continue;
            auto& item = operation->summary.items[itemIndex];
            item.outcome = outcome;
            if (outcome != TagFileOutcome::committed &&
                outcome != TagFileOutcome::noChange) item.afterRevision.clear();
        }
    }
    // Undo's final attempt was persisted on its worker. It has no item rows
    // to update here; avoid repeating serialization and file I/O on the UI.
    if (!operation->undoMode) persistJournal(operation->summary);
    Completion completion;
    {
        std::lock_guard lock(mutex_);
        if (active_ == operation) {
            if (!operation->undoMode && std::any_of(
                    operation->summary.files.begin(),
                    operation->summary.files.end(), [](const auto& item) {
                        return undoable(item);
                    })) {
                lastCompleted_ = operation;
            } else if (operation->undoMode && lastCompleted_ == operation->undoSource) {
                lastCompleted_->summary = std::move(operation->undoSourceSummary);
                if (std::none_of(lastCompleted_->summary.files.begin(),
                        lastCompleted_->summary.files.end(), undoable)) {
                    lastCompleted_.reset();
                }
            }
            completion = std::move(operation->completion);
            active_.reset();
        }
    }
    // Keep the completed operation intact as the guarded in-memory Undo plan.
    // Moving the summary here left lastCompleted_ with an empty result vector.
    if (completion) completion(operation->summary);
}

bool TagEditService::undoLast(HWND owner, Completion completion)
{
    if (MediaFileOperationService::instance().busy()) return false;
    std::lock_guard lock(mutex_);
    if (active_ || !lastCompleted_) return false;
    for (const auto& file : lastCompleted_->summary.files)
        if (!MediaFileOperationService::instance().mutationAllowed(file.path)) return false;
    if (worker_.joinable()) worker_.join();
    auto operation = std::make_shared<Operation>();
    operation->generation = generation_.fetch_add(1) + 1;
    operation->owner = owner;
    operation->completion = std::move(completion);
    operation->undoMode = true;
    operation->recovered = lastCompleted_->recovered;
    operation->undoSource = lastCompleted_;
    operation->undoSourceSummary = lastCompleted_->summary;
    operation->summary.operationId = L"undo-" + operationId();
    const auto root = backupRoot() / operation->summary.operationId;
    operation->summary.journalPath = root / L"journal.json";
    for (const auto& sourcePhysical : lastCompleted_->physical) {
        if (sourcePhysical.resultIndex >= lastCompleted_->summary.files.size()) continue;
        const auto& sourceResult =
            lastCompleted_->summary.files[sourcePhysical.resultIndex];
        if (!undoable(sourceResult)) continue;
        auto physical = sourcePhysical;
        physical.undoSourceResultIndex = sourcePhysical.resultIndex;
        physical.resultIndex = operation->summary.files.size();
        operation->physical.push_back(std::move(physical));
        operation->summary.files.push_back(sourceResult);
    }
    if (operation->physical.empty()) return false;
    metadb_handle_ptr playing;
    const auto playingPath = playback_control::get()->get_now_playing(playing)
        ? nativePath(playing) : std::filesystem::path{};
    playbackPath_ = playingPath;
    for (auto& item : operation->summary.files) {
        item.outcome = TagFileOutcome::planned;
        item.detail = L"Waiting for guarded restore";
        if (!playingPath.empty() &&
            _wcsicmp(playingPath.c_str(), item.path.c_str()) == 0) {
            item.outcome = TagFileOutcome::deferredUntilReleased;
            item.detail = L"Stop playback before retrying Undo";
        }
    }
    active_ = operation;
    worker_ = std::jthread([this, operation](std::stop_token stop) {
        prepareUndo(operation, stop);
    });
    return true;
}

bool TagEditService::playbackProtects(const std::filesystem::path& path) const
{
    std::lock_guard lock(mutex_);
    return !playbackPath_.empty() &&
        _wcsicmp(playbackPath_.c_str(), path.c_str()) == 0;
}

void TagEditService::prepareUndo(std::shared_ptr<Operation> operation,
    std::stop_token stop)
{
    std::error_code error;
    std::filesystem::create_directories(
        operation->summary.journalPath.parent_path(), error);
    metadb_handle_list reload;
    // Read only the selected journal when Undo is requested, on this worker.
    // Discovery retains no extra multi-megabyte payloads for other candidates.
    std::optional<TagRecoveryJournal> sourceDocument;
    if (operation->recovered) {
        sourceDocument = readRecoveryJournal(operation->undoSourceSummary.journalPath);
        if (sourceDocument && !sourceDocument->matchesOperation(
                wideToUtf8(operation->undoSourceSummary.operationId))) sourceDocument.reset();
        for (const auto& file : operation->physical) {
            if (!sourceDocument) break;
            const auto& result = operation->summary.files[file.resultIndex];
            if (!sourceDocument->matchesFile(file.recoveredFileIndex,
                    wideToUtf8(result.path.wstring()), wideToUtf8(result.backupPath.wstring()),
                    result.sourceHash, result.postWriteHash)) sourceDocument.reset();
        }
    }
    const auto persistSource = [&] {
        if (!operation->recovered) return persistJournal(operation->undoSourceSummary);
        return sourceDocument && persistRecoveryJournal(
            operation->undoSourceSummary.journalPath, *sourceDocument);
    };
    bool sourceJournalPending = false;
    const auto consume = [&](const Operation::PhysicalFile& file, TagFileResult& result) {
        result.outcome = TagFileOutcome::restored;
        result.detail = L"Original bytes restored and SHA-256 verified";
        auto& source = operation->undoSourceSummary;
        source.files[file.undoSourceResultIndex].outcome = TagFileOutcome::restored;
        source.files[file.undoSourceResultIndex].detail = result.detail;
        for (const auto itemIndex : file.itemResultIndices) {
            if (itemIndex >= source.items.size()) continue;
            source.items[itemIndex].outcome = TagFileOutcome::restored;
            source.items[itemIndex].afterRevision.clear();
        }
        // Consume each verified restore in the original Apply journal as well.
        // Recovery reads that journal, not this attempt's undo-* audit record.
        if (sourceDocument && !sourceDocument->markRestored(file.recoveredFileIndex,
                wideToUtf8(result.detail))) sourceDocument.reset();
        sourceJournalPending = !persistSource();
        if (sourceJournalPending) {
            result.detail += L"; original journal update failed; recovery may offer Undo again";
        }
    };
    for (auto& file : operation->physical) {
        auto& result = operation->summary.files[file.resultIndex];
        if (result.outcome != TagFileOutcome::planned) continue;
        if (stop.stop_requested() || operation->generation != generation_.load()) {
            result.outcome = TagFileOutcome::cancelledBeforeWrite;
            result.detail = L"Undo cancelled before restore";
            continue;
        }
        if (playbackProtects(file.path)) {
            result.outcome = TagFileOutcome::deferredUntilReleased;
            result.detail = L"Stop playback before retrying Undo";
            continue;
        }
        std::string currentHash, backupHash;
        const auto currentVerified = sha256File(file.path, currentHash, stop);
        // A crash or journal failure can occur after the atomic replacement.
        // Recognize the original bytes without overwriting them a second time.
        if (currentVerified && result.sourceHash.size() == 64 &&
                currentHash == result.sourceHash) {
            consume(file, result);
            reload.add_items(file.handles);
            continue;
        }
        if (result.postWriteHash.empty() || result.backupPath.empty() ||
            !currentVerified ||
            currentHash != result.postWriteHash) {
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::conflictExternalChange;
            result.detail = stop.stop_requested()
                ? L"Undo cancelled while hashing current file"
                : L"Current file no longer matches the committed revision";
            continue;
        }
        if (!sha256File(result.backupPath, backupHash, stop) ||
            backupHash != result.sourceHash) {
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::failedBeforeWrite;
            result.detail = stop.stop_requested()
                ? L"Undo cancelled while hashing backup"
                : L"Recovery backup failed SHA-256 verification";
            continue;
        }
        result.outcome = TagFileOutcome::restoreIntentDurable;
        result.detail = L"Guarded restore intent is durable; replacement starting";
        if (!persistJournal(operation->summary)) {
            result.outcome = TagFileOutcome::failedBeforeWrite;
            result.detail = L"Undo journal could not be made durable";
            continue;
        }
        const auto staging = file.path.wstring() + L".goden-restore-" +
            operation->summary.operationId;
        error.clear();
        const auto copied = copyFileCancellable(result.backupPath, staging, stop);
        std::string stagingHash;
        if (!copied || !sha256File(staging, stagingHash, stop) ||
                stagingHash != result.sourceHash) {
            std::filesystem::remove(staging, error);
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::failedBeforeWrite;
            result.detail = L"Undo staging could not be verified";
            continue;
        }
        if (!sha256File(file.path, currentHash, stop) ||
                currentHash != result.postWriteHash || stop.stop_requested()) {
            std::filesystem::remove(staging, error);
            result.outcome = stop.stop_requested()
                ? TagFileOutcome::cancelledBeforeWrite
                : TagFileOutcome::conflictExternalChange;
            result.detail = L"Undo stopped before commit: current file changed or operation cancelled";
            continue;
        }
        bool replaced = false, protectedNow = false, cancelledNow = false;
        {
            // Order replacement against playback notifications observed by us.
            // The host's decoder opening a file is still independently guarded
            // by Windows file sharing, not by this mutex.
            std::lock_guard lock(mutex_);
            protectedNow = !playbackPath_.empty() &&
                _wcsicmp(playbackPath_.c_str(), file.path.c_str()) == 0;
            cancelledNow = stop.stop_requested() ||
                operation->generation != generation_.load();
            if (!protectedNow && !cancelledNow) {
                replaced = MoveFileExW(staging.c_str(), file.path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            }
        }
        if (!replaced) {
            std::filesystem::remove(staging, error);
            result.outcome = cancelledNow ? TagFileOutcome::cancelledBeforeWrite
                : protectedNow ? TagFileOutcome::deferredUntilReleased
                               : TagFileOutcome::recoverablePartial;
            result.detail = cancelledNow ? L"Undo cancelled before replacement"
                : protectedNow ? L"Stop playback before retrying Undo"
                               : L"Guarded restore could not replace the current file";
            continue;
        }
        reload.add_items(file.handles);
        std::string restoredHash;
        // Cancellation after commit cannot turn verified restored bytes into
        // a pending mutation. Finish verification and recovery bookkeeping.
        if (!sha256File(file.path, restoredHash) ||
            restoredHash != result.sourceHash) {
            result.outcome = TagFileOutcome::recoverablePartial;
            result.detail = L"Restored bytes failed final verification";
            continue;
        }
        consume(file, result);
    }
    if (sourceJournalPending) {
        sourceJournalPending = !persistSource();
    }
    if (!sourceJournalPending) {
        // A later per-file write or the final retry includes every consumed
        // file. Do not report a recovery warning once that snapshot is durable.
        for (auto& result : operation->summary.files) {
            if (result.outcome == TagFileOutcome::restored) {
                result.detail = L"Original bytes restored and SHA-256 verified";
            }
        }
    }
    // Keep the retry off the UI too. A source-journal failure is independently
    // reported above; no retry can overwrite a partially reconstructed source.
    if (!persistJournal(operation->summary)) persistJournal(operation->summary);
    auto owned = std::make_shared<metadb_handle_list>();
    owned->add_items(reload);
    fb2k::inMainThread([this, operation, owned] {
        {
            std::lock_guard lock(mutex_);
            if (active_ != operation) return;
        }
        if (owned->get_count() == 0) return finish(operation);
        auto notify = fb2k::makeCompletionNotify(
            [this, operation](unsigned) { finish(operation); });
        metadb_io_v2::get()->load_info_async(*owned, metadb_io::load_info_force,
            operation->owner, metadb_io_v2::op_flag_background |
                metadb_io_v2::op_flag_delay_ui |
                metadb_io_v2::op_flag_no_errors, notify);
    });
}

const wchar_t* tagFileOutcomeText(TagFileOutcome value) noexcept
{
    switch (value) {
    case TagFileOutcome::planned: return L"Planned";
    case TagFileOutcome::backedUp: return L"BackedUp";
    case TagFileOutcome::mutationIntentDurable: return L"MutationIntentDurable";
    case TagFileOutcome::restoreIntentDurable: return L"RestoreIntentDurable";
    case TagFileOutcome::bindingPending: return L"BindingPending";
    case TagFileOutcome::committed: return L"Committed";
    case TagFileOutcome::restored: return L"Restored";
    case TagFileOutcome::noChange: return L"NoChange";
    case TagFileOutcome::deferredUntilReleased: return L"DeferredUntilReleased";
    case TagFileOutcome::conflictExternalChange: return L"ConflictExternalChange";
    case TagFileOutcome::failedBeforeWrite: return L"FailedBeforeWrite";
    case TagFileOutcome::recoverablePartial: return L"RecoverablePartial";
    case TagFileOutcome::cancelledBeforeWrite: return L"CancelledBeforeWrite";
    }
    return L"Unknown";
}

} // namespace nekorox::component
