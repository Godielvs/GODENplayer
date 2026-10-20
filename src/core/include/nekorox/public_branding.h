#pragma once

#include <string>
#include <string_view>

namespace nekorox {

[[nodiscard]] inline std::wstring godenPublicWindowTitle(
    std::wstring_view source)
{
    if (source == L"foobar2000") return L"Goden foobar2000";

    constexpr std::wstring_view legacy =
        L" - NekoRoX - foobar2000";
    constexpr std::wstring_view branded =
        L" - Goden foobar2000";
    std::wstring result(source);
    for (auto offset = result.find(legacy); offset != std::wstring::npos;
            offset = result.find(legacy, offset + branded.size())) {
        result.replace(offset, legacy.size(), branded);
    }
    return result;
}

} // namespace nekorox
