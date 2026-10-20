#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <atomic>
#include <functional>
#include <compare>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox::component {

struct PreparedTextRuns;

// Shared by synchronous labels and asynchronously prepared reading surfaces.
// A reservation outlives its COM layout, including frames held by a painter.
struct TextLayoutBudget {
    std::atomic<std::size_t> live{0};
    std::atomic<std::size_t> peak{0};
    static constexpr std::size_t limit = 4096U;
};

struct TextLayoutLease {
    explicit TextLayoutLease(std::shared_ptr<TextLayoutBudget> budget) noexcept;
    ~TextLayoutLease();
    std::shared_ptr<TextLayoutBudget> budget;
    bool reserved = false;
};

struct RetainedTextParagraph {
    std::size_t offset = 0;
    std::size_t length = 0;
    float top = 0.0f;
    float height = 0.0f;
    float width = 0.0f;
};

struct RetainedTextDocument {
    std::wstring text;
    std::vector<RetainedTextParagraph> paragraphs;
    float height = 0.0f;
    std::size_t utf16Bytes = 0;

private:
    friend class TextLayoutCache;
    std::uint64_t identity = 0;
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    float layoutWidth = 0.0f;
    float layoutHeight = 0.0f;
};

struct TextLayoutCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t layoutsCreated = 0;
    std::uint64_t evictions = 0;
    std::size_t residentLayouts = 0;
    std::size_t peakResidentLayouts = 0;
    std::size_t residentUtf16Bytes = 0;
};

// Main-thread cache for semantically immutable text revisions. The key never
// hashes text during paint; callers own the revision and block identities.
class TextLayoutCache final {
public:
    explicit TextLayoutCache(std::shared_ptr<TextLayoutBudget> budget = {},
        std::size_t residentLimit = 4096U, bool telemetry = true);
    TextLayoutCache(const TextLayoutCache&) = delete;
    TextLayoutCache& operator=(const TextLayoutCache&) = delete;
    static constexpr std::size_t maximumSharedLayouts = 4096U;
    static constexpr std::size_t maximumSharedUtf16Bytes = 4U * 1024U * 1024U;
    static constexpr std::size_t maximumOpenDocumentUtf16Bytes =
        1U * 1024U * 1024U;

    std::shared_ptr<const RetainedTextDocument> getOrCreate(
        IDWriteFactory* factory,
        std::uint64_t contentRevision,
        std::uint64_t blockId,
        std::uint64_t formatRevision,
        std::wstring_view text,
        IDWriteTextFormat* format,
        float width,
        float height = 1000000.0f,
        const std::function<bool()>& cancelled = {});

    // Compatibility entry point for callers whose renderer owns exactly one
    // immutable text style. The cache identity remains semantic and never
    // depends on the COM object's address.
    std::shared_ptr<const RetainedTextDocument> getOrCreate(
        IDWriteFactory* factory,
        std::uint64_t contentRevision,
        std::uint64_t blockId,
        std::wstring_view text,
        IDWriteTextFormat* format,
        float width,
        float height = 1000000.0f)
    {
        return getOrCreate(factory, contentRevision, blockId, 1U,
            text, format, width, height);
    }

    void drawVisible(
        ID2D1RenderTarget* target,
        const RetainedTextDocument& document,
        const D2D1_RECT_F& viewport,
        ID2D1Brush* brush,
        float contentTop,
        float scrollOffset = 0.0f);

    void clear() noexcept;
    [[nodiscard]] TextLayoutCacheStats stats() const noexcept;
    void setResidentLimit(std::size_t limit) noexcept;
    void clearLayouts() noexcept;
    // After worker preparation, release measurement-only layouts outside the
    // prepared slice. Ready frames never need these entries to draw.
    void releaseUnpreparedLayouts() noexcept;
    bool prepareVisible(const RetainedTextDocument& document, float top,
        float bottom, const std::function<bool()>& cancelled = {});
    [[nodiscard]] bool hasVisible(const RetainedTextDocument& document,
        float top, float bottom) const noexcept;
    // Never shapes, allocates, changes the LRU or falls back to DrawText.
    bool drawPrepared(ID2D1RenderTarget* target,
        const RetainedTextDocument& document, const D2D1_RECT_F& viewport,
        ID2D1Brush* brush, float contentTop, float scrollOffset) const;

private:
    struct Key {
        std::uint64_t revision = 0;
        std::uint64_t block = 0;
        std::uint64_t formatRevision = 0;
        std::uint32_t widthBits = 0;
        std::uint32_t heightBits = 0;
        auto operator<=>(const Key&) const = default;
    };
    struct Entry {
        std::shared_ptr<const RetainedTextDocument> document;
        std::uint64_t use = 0;
    };
    struct LayoutKey {
        std::uint64_t document = 0;
        std::size_t paragraph = 0;
        auto operator<=>(const LayoutKey&) const = default;
    };
    struct LayoutEntry {
        std::shared_ptr<TextLayoutLease> lease;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::list<LayoutKey>::iterator recency;
        std::shared_ptr<const PreparedTextRuns> prepared;
    };

    void enforceBudget(const Key& protectedKey);
    IDWriteTextLayout* acquireLayout(
        const RetainedTextDocument& document, std::size_t paragraph);

    std::map<Key, Entry> entries_;
    // Documents retain source and metrics, never COM layouts. Even callers
    // holding evicted documents cannot pin layouts outside this per-cache pool.
    std::map<LayoutKey, LayoutEntry> layouts_;
    std::list<LayoutKey> layoutRecency_;
    std::uint64_t useCounter_ = 0;
    TextLayoutCacheStats stats_{};
    std::shared_ptr<TextLayoutBudget> budget_;
    std::size_t residentLimit_ = maximumSharedLayouts;
    bool telemetry_ = true;
};

} // namespace nekorox::component
