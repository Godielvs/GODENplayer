#include "text_util.h"

#include "sdk_compat.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace nekorox::component {

std::wstring utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const auto size = static_cast<int>(value.size());
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, nullptr, 0);
    if (count <= 0) return {};
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, output.data(), count);
    return output;
}

std::string wideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto size = static_cast<int>(value.size());
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
        output.data(), count, nullptr, nullptr);
    return output;
}

std::wstring trimText(std::wstring value)
{
    const auto isSpace = [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) != 0;
    };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string personaAliasKey(std::wstring_view value)
{
    auto trimmed = trimText(std::wstring(value));
    std::wstring collapsed;
    collapsed.reserve(trimmed.size());
    bool pendingSpace = false;
    for (const auto character : trimmed) {
        if (std::iswspace(static_cast<wint_t>(character)) != 0) {
            pendingSpace = !collapsed.empty();
            continue;
        }
        if (pendingSpace) collapsed.push_back(L' ');
        pendingSpace = false;
        collapsed.push_back(character);
    }
    if (collapsed.empty()) return {};
    const auto required = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
        collapsed.data(), static_cast<int>(collapsed.size()),
        nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) return {};
    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
            collapsed.data(), static_cast<int>(collapsed.size()),
            lowered.data(), required, nullptr, nullptr, 0) <= 0) {
        return {};
    }
    return wideToUtf8(lowered);
}

std::wstring readLocalTextFile(
    const std::filesystem::path& path, std::size_t maximumBytes)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximumBytes ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::vector<char> bytes(static_cast<std::size_t>(size));
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) return {};

    const auto* data = bytes.data();
    auto byteCount = bytes.size();
    if (byteCount >= 3 && static_cast<unsigned char>(data[0]) == 0xef &&
        static_cast<unsigned char>(data[1]) == 0xbb &&
        static_cast<unsigned char>(data[2]) == 0xbf) {
        data += 3;
        byteCount -= 3;
    }
    std::wstring result;
    if (byteCount >= 2 && static_cast<unsigned char>(data[0]) == 0xff &&
        static_cast<unsigned char>(data[1]) == 0xfe) {
        const auto count = (byteCount - 2U) / sizeof(wchar_t);
        result.assign(reinterpret_cast<const wchar_t*>(data + 2), count);
    } else {
        auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            data, static_cast<int>(byteCount), nullptr, 0);
        auto codePage = CP_UTF8;
        auto flags = MB_ERR_INVALID_CHARS;
        if (required <= 0) {
            codePage = CP_ACP;
            flags = 0;
            required = MultiByteToWideChar(codePage, flags,
                data, static_cast<int>(byteCount), nullptr, 0);
        }
        if (required <= 0) return {};
        result.resize(static_cast<std::size_t>(required));
        MultiByteToWideChar(codePage, flags, data,
            static_cast<int>(byteCount), result.data(), required);
    }
    result.erase(std::remove(result.begin(), result.end(), L'\r'), result.end());
    return trimText(std::move(result));
}

int compareText(std::wstring_view left, std::wstring_view right) noexcept
{
    const int result = CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

} // namespace nekorox::component
