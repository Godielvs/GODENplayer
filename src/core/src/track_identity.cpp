#include "nekorox/track_identity.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nekorox {
namespace {

class Sha256 final {
public:
    Sha256()
    {
        check(BCryptOpenAlgorithmProvider(
            &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0));

        DWORD copied = 0;
        check(BCryptGetProperty(
            algorithm_,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength_),
            sizeof(objectLength_),
            &copied,
            0));
        check(BCryptGetProperty(
            algorithm_,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength_),
            sizeof(hashLength_),
            &copied,
            0));
        object_.resize(objectLength_);
        hash_.resize(hashLength_);
        check(BCryptCreateHash(
            algorithm_,
            &handle_,
            object_.data(),
            static_cast<ULONG>(object_.size()),
            nullptr,
            0,
            0));
    }

    ~Sha256()
    {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    void add(const void* data, std::size_t bytes)
    {
        if (bytes == 0) {
            return;
        }
        check(BCryptHashData(
            handle_,
            const_cast<PUCHAR>(static_cast<const UCHAR*>(data)),
            static_cast<ULONG>(bytes),
            0));
    }

    std::string finish()
    {
        check(BCryptFinishHash(
            handle_, hash_.data(), static_cast<ULONG>(hash_.size()), 0));
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const auto byte : hash_) {
            out << std::setw(2) << static_cast<unsigned>(byte);
        }
        return out.str();
    }

private:
    static void check(NTSTATUS status)
    {
        if (status < 0) {
            throw std::runtime_error("BCrypt SHA-256 operation failed");
        }
    }

    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE handle_ = nullptr;
    DWORD objectLength_ = 0;
    DWORD hashLength_ = 0;
    std::vector<UCHAR> object_;
    std::vector<UCHAR> hash_;
};

std::string utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("Path could not be converted to UTF-8");
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), required,
        nullptr, nullptr);
    return output;
}

void hashFileWindow(
    Sha256& sha,
    std::ifstream& stream,
    std::uint64_t offset,
    std::uint64_t size)
{
    constexpr std::size_t kWindow = 64 * 1024;
    if (size == 0) {
        return;
    }
    const auto available = size > offset ? size - offset : 0;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(kWindow, available));
    if (count == 0) {
        return;
    }
    std::vector<char> buffer(count);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(buffer.data(), static_cast<std::streamsize>(count));
    const auto read = static_cast<std::size_t>(stream.gcount());
    sha.add(buffer.data(), read);
}

} // namespace

std::wstring normalizeWindowsPath(const std::filesystem::path& path)
{
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::wstring value = absolute.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');

    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr,
        0);
    if (required <= 0) {
        throw std::runtime_error("Path normalization failed");
    }
    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        value.data(),
        static_cast<int>(value.size()),
        lowered.data(),
        required,
        nullptr,
        nullptr,
        0);
    return lowered;
}

NekoTrackId makeTrackId(const TrackIdentityInput& input)
{
    const auto normalized = normalizeWindowsPath(input.path);
    const auto normalizedUtf8 = utf8(normalized);

    Sha256 content;
    content.add(&input.sizeBytes, sizeof(input.sizeBytes));
    content.add(&input.modifiedTicks, sizeof(input.modifiedTicks));
    content.add(&input.subsong, sizeof(input.subsong));

    std::ifstream stream(input.path, std::ios::binary);
    if (stream) {
        hashFileWindow(content, stream, 0, input.sizeBytes);
        if (input.sizeBytes > 128 * 1024) {
            hashFileWindow(
                content,
                stream,
                input.sizeBytes / 2 - 32 * 1024,
                input.sizeBytes);
            hashFileWindow(
                content,
                stream,
                input.sizeBytes - 64 * 1024,
                input.sizeBytes);
        }
    }
    const auto partial = content.finish();

    Sha256 identity;
    identity.add(normalizedUtf8.data(), normalizedUtf8.size());
    identity.add(partial.data(), partial.size());
    identity.add(&input.subsong, sizeof(input.subsong));

    return {
        "v1:" + identity.finish(),
        normalizedUtf8,
        partial,
    };
}

} // namespace nekorox
