#include "nekorox/lyrics_timeline.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace nekorox {
namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

bool isContinuation(unsigned char value) noexcept
{
    return (value & 0xC0U) == 0x80U;
}

std::string sanitizeUtf8(std::string_view source)
{
    std::string result;
    result.reserve(source.size());

    std::size_t index = 0;
    if (source.size() >= 3 &&
        static_cast<unsigned char>(source[0]) == 0xEFU &&
        static_cast<unsigned char>(source[1]) == 0xBBU &&
        static_cast<unsigned char>(source[2]) == 0xBFU) {
        index = 3;
    }

    while (index < source.size()) {
        const auto lead = static_cast<unsigned char>(source[index]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;

        if (lead <= 0x7FU) {
            result.push_back(static_cast<char>(lead));
            ++index;
            continue;
        }
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            codePoint = lead & 0x1FU;
            minimum = 0x80U;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
            codePoint = lead & 0x0FU;
            minimum = 0x800U;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            codePoint = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            result.append(kReplacementCharacter);
            ++index;
            continue;
        }

        if (index + length > source.size()) {
            result.append(kReplacementCharacter);
            ++index;
            continue;
        }

        bool valid = true;
        for (std::size_t part = 1; part < length; ++part) {
            const auto continuation =
                static_cast<unsigned char>(source[index + part]);
            if (!isContinuation(continuation)) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }

        valid = valid &&
            codePoint >= minimum &&
            codePoint <= 0x10FFFFU &&
            !(codePoint >= 0xD800U && codePoint <= 0xDFFFU);
        if (!valid) {
            result.append(kReplacementCharacter);
            ++index;
            continue;
        }

        result.append(source.substr(index, length));
        index += length;
    }
    return result;
}

bool isAsciiSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\v' ||
        value == '\f';
}

std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && isAsciiSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isAsciiSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

char lowerAscii(char value) noexcept
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

bool startsWithAsciiInsensitive(
    std::string_view value,
    std::string_view prefix) noexcept
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (lowerAscii(value[index]) != lowerAscii(prefix[index])) {
            return false;
        }
    }
    return true;
}

bool isMetadataTag(std::string_view tag) noexcept
{
    constexpr std::string_view prefixes[] = {
        "ar:", "al:", "ti:", "au:", "by:", "re:", "ve:", "length:",
    };
    return std::any_of(
        std::begin(prefixes),
        std::end(prefixes),
        [&](std::string_view prefix) {
            return startsWithAsciiInsensitive(tag, prefix);
        });
}

bool parseUnsigned(std::string_view value, std::uint64_t& result) noexcept
{
    if (value.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result);
    return parsed.ec == std::errc{} &&
        parsed.ptr == value.data() + value.size();
}

std::optional<double> parseTimestamp(std::string_view tag) noexcept
{
    const auto colon = tag.find(':');
    if (colon == std::string_view::npos ||
        tag.find(':', colon + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    std::uint64_t minutes = 0;
    if (!parseUnsigned(trim(tag.substr(0, colon)), minutes)) {
        return std::nullopt;
    }

    auto secondsText = trim(tag.substr(colon + 1));
    const auto decimal = secondsText.find_first_of(".,");
    const auto wholeText = secondsText.substr(0, decimal);
    std::uint64_t seconds = 0;
    if (!parseUnsigned(wholeText, seconds) || seconds >= 60) {
        return std::nullopt;
    }

    double fraction = 0.0;
    if (decimal != std::string_view::npos) {
        const auto fractionText = secondsText.substr(decimal + 1);
        if (fractionText.empty() || fractionText.size() > 6) {
            return std::nullopt;
        }
        std::uint64_t fractionValue = 0;
        if (!parseUnsigned(fractionText, fractionValue)) {
            return std::nullopt;
        }
        double divisor = 1.0;
        for (std::size_t digit = 0; digit < fractionText.size(); ++digit) {
            divisor *= 10.0;
        }
        fraction = static_cast<double>(fractionValue) / divisor;
    }

    constexpr auto kMaximumMinutes =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    if (minutes > kMaximumMinutes) {
        return std::nullopt;
    }
    return static_cast<double>(minutes) * 60.0 +
        static_cast<double>(seconds) + fraction;
}

std::optional<std::int32_t> parseOffset(std::string_view tag) noexcept
{
    if (!startsWithAsciiInsensitive(tag, "offset:")) {
        return std::nullopt;
    }
    const auto value = trim(tag.substr(7));
    std::int64_t parsedValue = 0;
    const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsedValue);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        parsedValue,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

struct ParsedLine {
    std::string text;
    std::vector<double> timestamps;
    std::size_t sourceLine = 0;
    bool metadataOnly = false;
};

} // namespace

LyricsTimelineV1 LyricsTimelineV1::parseUtf8(std::string_view source)
{
    LyricsTimelineV1 timeline;
    const auto safeSource = sanitizeUtf8(source);
    std::vector<ParsedLine> parsedLines;

    std::size_t lineStart = 0;
    std::size_t sourceLine = 0;
    while (lineStart <= safeSource.size()) {
        const auto lineEnd = safeSource.find('\n', lineStart);
        const auto end = lineEnd == std::string::npos
            ? safeSource.size()
            : lineEnd;
        auto line = std::string_view(safeSource).substr(lineStart, end - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        ParsedLine parsedLine;
        parsedLine.sourceLine = sourceLine;
        std::size_t cursor = 0;
        bool sawMetadata = false;
        while (cursor < line.size() && line[cursor] == '[') {
            const auto close = line.find(']', cursor + 1);
            if (close == std::string_view::npos) {
                break;
            }
            const auto tag = trim(line.substr(cursor + 1, close - cursor - 1));
            if (const auto offset = parseOffset(tag)) {
                timeline.offsetMilliseconds_ = *offset;
                sawMetadata = true;
            } else if (const auto timestamp = parseTimestamp(tag)) {
                parsedLine.timestamps.push_back(*timestamp);
            } else if (isMetadataTag(tag)) {
                sawMetadata = true;
            } else {
                break;
            }
            cursor = close + 1;
        }

        const auto text = trim(line.substr(cursor));
        parsedLine.text.assign(text);
        parsedLine.metadataOnly =
            sawMetadata && parsedLine.timestamps.empty() && text.empty();
        if (!parsedLine.text.empty() || !parsedLine.timestamps.empty()) {
            parsedLines.push_back(std::move(parsedLine));
        }

        ++sourceLine;
        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    const bool hasTimedLines = std::any_of(
        parsedLines.begin(),
        parsedLines.end(),
        [](const ParsedLine& line) { return !line.timestamps.empty(); });
    timeline.synchronized_ = hasTimedLines;

    if (hasTimedLines) {
        for (const auto& parsedLine : parsedLines) {
            for (const auto timestamp : parsedLine.timestamps) {
                const double adjusted = std::max(
                    0.0,
                    timestamp +
                        static_cast<double>(timeline.offsetMilliseconds_) /
                            1000.0);
                timeline.lines_.push_back({
                    parsedLine.text,
                    adjusted,
                    parsedLine.sourceLine,
                });
            }
        }
        std::stable_sort(
            timeline.lines_.begin(),
            timeline.lines_.end(),
            [](const LyricsLineV1& left, const LyricsLineV1& right) {
                return left.timestampSeconds < right.timestampSeconds;
            });
    } else {
        for (const auto& parsedLine : parsedLines) {
            if (!parsedLine.metadataOnly && !parsedLine.text.empty()) {
                timeline.lines_.push_back({
                    parsedLine.text,
                    -1.0,
                    parsedLine.sourceLine,
                });
            }
        }
    }
    return timeline;
}

std::optional<std::size_t> LyricsTimelineV1::activeLineIndex(
    double playbackSeconds) const noexcept
{
    if (!synchronized_ || lines_.empty() ||
        !std::isfinite(playbackSeconds) ||
        playbackSeconds < lines_.front().timestampSeconds) {
        return std::nullopt;
    }
    const auto iterator = std::upper_bound(
        lines_.begin(),
        lines_.end(),
        playbackSeconds,
        [](double value, const LyricsLineV1& line) {
            return value < line.timestampSeconds;
        });
    return static_cast<std::size_t>(
        std::distance(lines_.begin(), iterator) - 1);
}

std::optional<std::size_t> LyricsTimelineV1::seekTargetIndex(
    double playbackSeconds) const noexcept
{
    if (!synchronized_ || lines_.empty() ||
        !std::isfinite(playbackSeconds)) {
        return std::nullopt;
    }
    const auto iterator = std::lower_bound(
        lines_.begin(),
        lines_.end(),
        std::max(0.0, playbackSeconds),
        [](const LyricsLineV1& line, double value) {
            return line.timestampSeconds < value;
        });
    if (iterator == lines_.end()) {
        return lines_.size() - 1;
    }
    return static_cast<std::size_t>(std::distance(lines_.begin(), iterator));
}

std::optional<double> LyricsTimelineV1::seekTargetSeconds(
    std::size_t lineIndex) const noexcept
{
    if (!synchronized_ || lineIndex >= lines_.size()) {
        return std::nullopt;
    }
    return lines_[lineIndex].timestampSeconds;
}

} // namespace nekorox
