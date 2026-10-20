#pragma once

#include "text_layout_cache.h"
#include <mutex>

namespace nekorox::component {

// Value-only style; the UI's mutable DirectWrite format never crosses threads.
struct AsyncTextStyle {
    std::wstring family = L"Segoe UI";
    std::wstring locale;
    float size = 14.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE slant = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    bool operator==(const AsyncTextStyle&) const = default;
    static AsyncTextStyle fromFormat(IDWriteTextFormat* format);
};

struct AsyncTextBlock {
    std::shared_ptr<const std::wstring> text;
    std::size_t style = 0;
    float before = 0.f, advanceScale = 1.f, after = 0.f;
};
struct AsyncTextBatch {
    std::vector<AsyncTextStyle> styles;
    std::vector<AsyncTextBlock> blocks;
    float leading = 0.f;
};
struct AsyncTextGeometry {
    struct Block {
        std::shared_ptr<const RetainedTextDocument> document;
        float top = 0.f, height = 0.f;
    };
    std::vector<Block> blocks;
    float height = 0.f;
};

enum class AsyncTextStatus { idle, pending, ready, failed };

struct AsyncTextFrame {
    std::shared_ptr<const RetainedTextDocument> document;
    std::shared_ptr<const AsyncTextGeometry> geometry;
    std::unique_ptr<TextLayoutCache> cache;
    float scroll = 0.0f;
    float viewportHeight = 0.0f;
    float width = 0.0f;
    std::uint64_t generation = 0;
    [[nodiscard]] bool covers(float top, float height) const noexcept;
};

struct AsyncTextStats {
    AsyncTextStatus status = AsyncTextStatus::idle;
    std::uint64_t generation = 0, submissions = 0, starts = 0;
    std::uint64_t cancellations = 0, publications = 0, layoutsCreated = 0;
    bool running = false;
};

// Only deterministic native tests supply these hooks. They run on the worker.
struct AsyncTextTestHooks {
    std::function<void()> beforeMeasure;
    std::function<HRESULT(IDWriteFactory**)> createFactory;
    std::atomic<bool> rejectSubmission{false};
    PTP_CALLBACK_ENVIRON callbackEnvironment = nullptr;
};

// UI-owned façade; callbacks retain only State, never a renderer or this.
// Destruction/cancel revokes both publication and wakes without joining.
class AsyncTextView final {
public:
    explicit AsyncTextView(std::shared_ptr<TextLayoutBudget> budget,
        std::shared_ptr<AsyncTextTestHooks> hooks = {});
    ~AsyncTextView();
    AsyncTextView(const AsyncTextView&) = delete;
    AsyncTextView& operator=(const AsyncTextView&) = delete;

    std::shared_ptr<const AsyncTextFrame> prepare(std::uint64_t owner,
        std::uint64_t revision, std::shared_ptr<const std::wstring> text,
        const AsyncTextStyle& style, float width, float viewportHeight,
        float scroll, HWND wakeWindow = nullptr);
    // revision includes content, styles and flow rules; immutable batch can be
    // reused across paints without hashing or copying its source in the UI.
    std::shared_ptr<const AsyncTextFrame> prepareBatch(std::uint64_t owner,
        std::uint64_t revision, std::shared_ptr<const AsyncTextBatch> batch,
        float width, float viewportHeight, float scroll, HWND wakeWindow = nullptr);
    void cancel() noexcept;
    [[nodiscard]] AsyncTextStats stats() const noexcept;
    static UINT readyMessage() noexcept;

private:
    struct State;
    struct Callback;
    static void CALLBACK run(PTP_CALLBACK_INSTANCE instance, void* context) noexcept;
    std::shared_ptr<const AsyncTextFrame> prepareImpl(std::uint64_t owner,
        std::uint64_t revision, std::shared_ptr<const std::wstring> text,
        const AsyncTextStyle& style, std::shared_ptr<const AsyncTextBatch> batch,
        float width, float viewportHeight, float scroll, HWND wakeWindow);
    std::shared_ptr<State> state_;
};

} // namespace nekorox::component
