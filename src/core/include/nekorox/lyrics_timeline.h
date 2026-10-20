#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox {

inline constexpr std::uint32_t kLyricsTimelineVersion = 1;

struct LyricsLineV1 {
    std::string text;
    double timestampSeconds = -1.0;
    std::size_t sourceLine = 0;

    [[nodiscard]] bool synchronized() const noexcept
    {
        return timestampSeconds >= 0.0;
    }
};

class LyricsTimelineV1 final {
public:
    [[nodiscard]] static LyricsTimelineV1 parseUtf8(std::string_view source);

    [[nodiscard]] std::uint32_t version() const noexcept
    {
        return kLyricsTimelineVersion;
    }
    [[nodiscard]] const std::vector<LyricsLineV1>& lines() const noexcept
    {
        return lines_;
    }
    [[nodiscard]] bool synchronized() const noexcept
    {
        return synchronized_;
    }
    [[nodiscard]] bool empty() const noexcept { return lines_.empty(); }
    [[nodiscard]] std::size_t timedLineCount() const noexcept
    {
        return synchronized_ ? lines_.size() : 0;
    }
    [[nodiscard]] std::int32_t offsetMilliseconds() const noexcept
    {
        return offsetMilliseconds_;
    }

    // Returns the latest timed line at or before playbackSeconds.
    [[nodiscard]] std::optional<std::size_t> activeLineIndex(
        double playbackSeconds) const noexcept;

    // Returns the first timed line at or after playbackSeconds, clamped to
    // the final timed line. This is useful when mapping a scrub position to
    // a clickable lyric row.
    [[nodiscard]] std::optional<std::size_t> seekTargetIndex(
        double playbackSeconds) const noexcept;

    [[nodiscard]] std::optional<double> seekTargetSeconds(
        std::size_t lineIndex) const noexcept;

private:
    std::vector<LyricsLineV1> lines_;
    std::int32_t offsetMilliseconds_ = 0;
    bool synchronized_ = false;
};

} // namespace nekorox
