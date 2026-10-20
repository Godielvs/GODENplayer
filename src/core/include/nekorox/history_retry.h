#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <string>
#include <string_view>

namespace nekorox {

enum class HistoryFailureClass {
    busyOrLocked,
    storageFull,
    readOnly,
    corrupt,
    incompatibleSchema,
    transient,
};

struct HistoryRetryDecision {
    HistoryFailureClass failure = HistoryFailureClass::transient;
    bool retry = false;
    std::chrono::milliseconds delay{};
};

inline HistoryRetryDecision historyRetryDecision(
    std::string_view message, unsigned failedAttempt)
{
    std::string lower(message);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto has = [&](std::string_view token) {
        return lower.find(token) != std::string::npos;
    };
    HistoryFailureClass failure = HistoryFailureClass::transient;
    unsigned budget = 4;
    if (has("busy") || has("locked")) {
        failure = HistoryFailureClass::busyOrLocked;
        budget = 8;
    } else if (has("full") || has("disk space")) {
        failure = HistoryFailureClass::storageFull;
        budget = 0;
    } else if (has("readonly") || has("read-only") || has("read only")) {
        failure = HistoryFailureClass::readOnly;
        budget = 0;
    } else if (has("corrupt") || has("malformed")) {
        failure = HistoryFailureClass::corrupt;
        budget = 0;
    } else if (has("schema is newer") || has("unsupported schema")) {
        failure = HistoryFailureClass::incompatibleSchema;
        budget = 0;
    }
    constexpr std::array delays{
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(4000),
        std::chrono::milliseconds(30000),
    };
    return {failure, failedAttempt <= budget && budget != 0,
        delays[(std::min)(failedAttempt == 0 ? 0U : failedAttempt - 1U,
            static_cast<unsigned>(delays.size() - 1U))]};
}

} // namespace nekorox
