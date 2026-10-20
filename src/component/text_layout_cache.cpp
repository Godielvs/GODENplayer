#include "text_layout_cache.h"
#include "performance_telemetry.h"
#include "prepared_text_runs.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>

namespace nekorox::component {
namespace {

std::atomic<std::uint64_t> nextDocumentIdentity{1U};

std::vector<std::wstring_view> linesPreservingEmpty(std::wstring_view text)
{
    std::vector<std::wstring_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find_first_of(L"\r\n", start);
        lines.push_back(text.substr(start,
            end == std::wstring_view::npos ? text.size() - start : end - start));
        if (end == std::wstring_view::npos) break;
        start = end + 1U;
        if (text[end] == L'\r' && start < text.size() && text[start] == L'\n') {
            ++start;
        }
        if (start == text.size()) {
            lines.emplace_back(text.data() + text.size(), 0U);
            break;
        }
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

} // namespace

TextLayoutLease::TextLayoutLease(std::shared_ptr<TextLayoutBudget> owner) noexcept
    : budget(std::move(owner))
{
    auto count = budget->live.load(std::memory_order_relaxed);
    while (count < TextLayoutBudget::limit) {
        if (budget->live.compare_exchange_weak(count, count + 1U)) {
            reserved = true;
            auto peak = budget->peak.load(std::memory_order_relaxed);
            while (peak < count + 1U &&
                !budget->peak.compare_exchange_weak(peak, count + 1U)) {}
            break;
        }
    }
}

TextLayoutLease::~TextLayoutLease()
{
    if (reserved) budget->live.fetch_sub(1U);
}

TextLayoutCache::TextLayoutCache(std::shared_ptr<TextLayoutBudget> budget,
    std::size_t residentLimit, bool telemetry)
    : budget_(budget ? std::move(budget) : std::make_shared<TextLayoutBudget>()),
      residentLimit_(std::clamp(residentLimit, std::size_t{1}, maximumSharedLayouts)),
      telemetry_(telemetry)
{
}

std::shared_ptr<const RetainedTextDocument> TextLayoutCache::getOrCreate(
    IDWriteFactory* factory,
    std::uint64_t contentRevision,
    std::uint64_t blockId,
    std::uint64_t formatRevision,
    std::wstring_view text,
    IDWriteTextFormat* format,
    float width,
    float height,
    const std::function<bool()>& cancelled)
{
    if (factory == nullptr || format == nullptr || !std::isfinite(width) ||
            !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
        return {};
    }
    const Key key{
        contentRevision,
        blockId,
        formatRevision,
        std::bit_cast<std::uint32_t>(width),
        std::bit_cast<std::uint32_t>(height),
    };
    if (const auto found = entries_.find(key); found != entries_.end()) {
        found->second.use = ++useCounter_;
        ++stats_.hits;
        return found->second.document;
    }

    ++stats_.misses;
    auto document = std::make_shared<RetainedTextDocument>();
    document->identity = nextDocumentIdentity.fetch_add(1U,
        std::memory_order_relaxed);
    document->factory = factory;
    document->format = format;
    document->layoutWidth = width;
    document->layoutHeight = height;
    document->text = text;
    document->utf16Bytes = text.size() * sizeof(wchar_t);
    const auto lines = linesPreservingEmpty(document->text);
    document->paragraphs.reserve(lines.size());
    float emptyHeight = 0.0f;
    float emptyWidth = 0.0f;
    for (const auto line : lines) {
        if (cancelled && cancelled()) return {};
        RetainedTextParagraph paragraph;
        paragraph.offset = static_cast<std::size_t>(line.data() - document->text.data());
        paragraph.length = line.size();
        paragraph.top = document->height;
        document->paragraphs.push_back(paragraph);
        if (line.empty() && emptyHeight > 0.0f) {
            paragraph.height = emptyHeight;
            paragraph.width = emptyWidth;
        } else {
            const auto span = telemetry_ ? PerformanceTelemetry::instance().beginSpan(
                TelemetrySurface::workspace, 2U, line.size()) : 0U;
            auto* layout = acquireLayout(*document, document->paragraphs.size() - 1U);
            DWRITE_TEXT_METRICS metrics{};
            const auto result = layout ? layout->GetMetrics(&metrics) : E_FAIL;
            if (telemetry_) PerformanceTelemetry::instance().endSpan(
                TelemetrySurface::workspace, 2U, span, static_cast<std::uint32_t>(result));
            if (FAILED(result)) return {};
            paragraph.height = std::max(1.0f, metrics.height);
            paragraph.width = metrics.width;
            if (line.empty()) {
                emptyHeight = paragraph.height;
                emptyWidth = paragraph.width;
            }
        }
        document->height += paragraph.height;
        document->paragraphs.back() = paragraph;
    }

    stats_.residentUtf16Bytes += document->utf16Bytes;
    entries_.emplace(key, Entry{document, ++useCounter_});
    enforceBudget(key);
    return document;
}

IDWriteTextLayout* TextLayoutCache::acquireLayout(
    const RetainedTextDocument& document, std::size_t index)
{
    const auto& paragraph = document.paragraphs[index];
    // Blank lines have identical layout properties within this document.
    const LayoutKey key{document.identity, paragraph.length == 0U
        ? std::numeric_limits<std::size_t>::max() : index};
    if (const auto found = layouts_.find(key); found != layouts_.end()) {
        layoutRecency_.splice(layoutRecency_.end(), layoutRecency_,
            found->second.recency);
        return found->second.layout.Get();
    }
    // Reserve before creating: temporary layouts count toward the same ceiling.
    while (!layouts_.empty() && (layouts_.size() >= residentLimit_ ||
        budget_->live.load() >= TextLayoutBudget::limit)) {
        layouts_.erase(layoutRecency_.front());
        layoutRecency_.pop_front();
        ++stats_.evictions;
    }
    stats_.residentLayouts = layouts_.size();
    auto lease = std::make_shared<TextLayoutLease>(budget_);
    if (!lease->reserved) return nullptr;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    const auto result = document.factory->CreateTextLayout(
        paragraph.length == 0U ? L" " : document.text.data() + paragraph.offset,
        static_cast<UINT32>(std::min<std::size_t>(
            paragraph.length == 0U ? 1U : paragraph.length,
            std::numeric_limits<UINT32>::max())),
        document.format.Get(), document.layoutWidth, document.layoutHeight,
        layout.ReleaseAndGetAddressOf());
    if (FAILED(result) || !layout) return nullptr;
    ++stats_.layoutsCreated;
    layoutRecency_.push_back(key);
    auto [entry, inserted] = layouts_.emplace(key,
        LayoutEntry{std::move(lease), std::move(layout), std::prev(layoutRecency_.end()), {}});
    stats_.residentLayouts = layouts_.size();
    stats_.peakResidentLayouts = std::max(stats_.peakResidentLayouts,
        stats_.residentLayouts);
    return entry->second.layout.Get();
}

void TextLayoutCache::drawVisible(
    ID2D1RenderTarget* target,
    const RetainedTextDocument& document,
    const D2D1_RECT_F& viewport,
    ID2D1Brush* brush,
    float contentTop,
    float scrollOffset)
{
    if (target == nullptr || brush == nullptr || document.paragraphs.empty()) {
        return;
    }
    const auto visibleTop = viewport.top - contentTop + scrollOffset;
    const auto visibleBottom = viewport.bottom - contentTop + scrollOffset;
    const auto first = std::lower_bound(
        document.paragraphs.begin(), document.paragraphs.end(), visibleTop,
        [](const RetainedTextParagraph& paragraph, float y) {
            return paragraph.top + paragraph.height <= y;
        });
    target->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    for (auto iterator = first; iterator != document.paragraphs.end(); ++iterator) {
        if (iterator->top >= visibleBottom) break;
        auto* layout = acquireLayout(document,
            static_cast<std::size_t>(iterator - document.paragraphs.begin()));
        if (layout == nullptr) {
            // A failed cache allocation must not silently erase a ready line.
            // DrawText has the same natural layout and reports drawing errors
            // through the owner's EndDraw. Local coordinates preserve the
            // exact layout dimensions even at large/fractional scroll offsets.
            D2D1::Matrix3x2F original;
            target->GetTransform(&original);
            target->SetTransform(D2D1::Matrix3x2F::Translation(viewport.left,
                contentTop - scrollOffset + iterator->top) * original);
            target->DrawText(iterator->length == 0U ? L" " :
                    document.text.data() + iterator->offset,
                static_cast<UINT32>(std::min<std::size_t>(
                    iterator->length == 0U ? 1U : iterator->length,
                    std::numeric_limits<UINT32>::max())),
                document.format.Get(), D2D1::RectF(0.0f, 0.0f,
                    document.layoutWidth, document.layoutHeight), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            target->SetTransform(original);
            continue;
        }
        target->DrawTextLayout(
            D2D1::Point2F(
                viewport.left,
                contentTop - scrollOffset + iterator->top),
            layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    target->PopAxisAlignedClip();
}

void TextLayoutCache::enforceBudget(const Key& protectedKey)
{
    while (entries_.size() > 1U &&
        (entries_.size() > maximumSharedLayouts ||
         stats_.residentUtf16Bytes > maximumSharedUtf16Bytes)) {
        auto oldest = entries_.end();
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            if (iterator->first == protectedKey) continue;
            if (oldest == entries_.end() || iterator->second.use < oldest->second.use) {
                oldest = iterator;
            }
        }
        if (oldest == entries_.end()) break;
        stats_.residentUtf16Bytes -= oldest->second.document->utf16Bytes;
        entries_.erase(oldest);
        ++stats_.evictions;
    }
}

void TextLayoutCache::clear() noexcept
{
    entries_.clear();
    clearLayouts();
    stats_.residentUtf16Bytes = 0;
}

void TextLayoutCache::clearLayouts() noexcept
{
    layouts_.clear();
    layoutRecency_.clear();
    stats_.residentLayouts = 0;
}

void TextLayoutCache::releaseUnpreparedLayouts() noexcept
{
    for (auto it = layouts_.begin(); it != layouts_.end();) {
        if (it->second.prepared) {
            ++it;
        } else {
            layoutRecency_.erase(it->second.recency);
            it = layouts_.erase(it);
            ++stats_.evictions;
        }
    }
    stats_.residentLayouts = layouts_.size();
}

void TextLayoutCache::setResidentLimit(std::size_t limit) noexcept
{
    residentLimit_ = std::clamp(limit, std::size_t{1}, maximumSharedLayouts);
    while (layouts_.size() > residentLimit_) {
        layouts_.erase(layoutRecency_.front());
        layoutRecency_.pop_front();
        ++stats_.evictions;
    }
    stats_.residentLayouts = layouts_.size();
}

bool TextLayoutCache::prepareVisible(const RetainedTextDocument& document,
    float top, float bottom, const std::function<bool()>& cancelled)
{
    const auto first = std::lower_bound(document.paragraphs.begin(),
        document.paragraphs.end(), top, [](const auto& p, float y) {
            return p.top + p.height <= y;
        });
    for (auto it = first; it != document.paragraphs.end() && it->top < bottom; ++it) {
        if (cancelled && cancelled()) return false;
        auto* layout = acquireLayout(document,
            static_cast<std::size_t>(it - document.paragraphs.begin()));
        DWRITE_TEXT_METRICS metrics{};
        if (!layout || FAILED(layout->GetMetrics(&metrics))) return false;
        const LayoutKey key{document.identity, it->length == 0U
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(it - document.paragraphs.begin())};
        auto& entry = layouts_.find(key)->second;
        // Capture glyph runs on the worker as well. Drawing a million-character
        // layout directly otherwise walks/rasterizes its offscreen contents.
        const auto localTop = std::max(0.f, top - it->top);
        const auto localBottom = std::min(it->height, bottom - it->top);
        if (!entry.prepared || entry.prepared->top > localTop || entry.prepared->bottom < localBottom) {
            // Long paragraphs retain only the current slice; normal paragraphs
            // retain all lines, so nearby scrolling needs no new preparation.
            entry.prepared = PreparedTextRuns::capture(layout,
                it->height > 4096.f ? std::max(0.f, localTop - 128.f) : 0.f,
                it->height > 4096.f ? std::min(it->height, localBottom + 128.f) : it->height, cancelled);
            if (!entry.prepared) return false;
        }
    }
    return hasVisible(document, top, bottom);
}

bool TextLayoutCache::hasVisible(const RetainedTextDocument& document,
    float top, float bottom) const noexcept
{
    const auto first = std::lower_bound(document.paragraphs.begin(),
        document.paragraphs.end(), top, [](const auto& p, float y) {
            return p.top + p.height <= y;
        });
    for (auto it = first; it != document.paragraphs.end() && it->top < bottom; ++it) {
        const LayoutKey key{document.identity, it->length == 0U
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(it - document.paragraphs.begin())};
        const auto found = layouts_.find(key);
        if (found == layouts_.end() || !found->second.prepared ||
            found->second.prepared->top > std::max(0.f, top - it->top) ||
            found->second.prepared->bottom < std::min(it->height, bottom - it->top)) return false;
    }
    return true;
}

bool TextLayoutCache::drawPrepared(ID2D1RenderTarget* target,
    const RetainedTextDocument& document, const D2D1_RECT_F& viewport,
    ID2D1Brush* brush, float contentTop, float scrollOffset) const
{
    const auto top = viewport.top - contentTop + scrollOffset;
    const auto bottom = viewport.bottom - contentTop + scrollOffset;
    if (!target || !brush || !hasVisible(document, top, bottom)) return false;
    const auto first = std::lower_bound(document.paragraphs.begin(),
        document.paragraphs.end(), top, [](const auto& p, float y) {
            return p.top + p.height <= y;
        });
    target->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    for (auto it = first; it != document.paragraphs.end() && it->top < bottom; ++it) {
        const LayoutKey key{document.identity, it->length == 0U
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(it - document.paragraphs.begin())};
        const auto originY = contentTop - scrollOffset + it->top;
        target->PushAxisAlignedClip(D2D1::RectF(viewport.left, originY,
            viewport.left + document.layoutWidth, originY + document.layoutHeight),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        layouts_.find(key)->second.prepared->draw(target, brush, viewport.left, originY);
        target->PopAxisAlignedClip();
    }
    target->PopAxisAlignedClip();
    return true;
}

TextLayoutCacheStats TextLayoutCache::stats() const noexcept
{
    return stats_;
}

} // namespace nekorox::component
