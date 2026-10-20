#include "async_text_view.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace nekorox::component {
namespace {
struct TextKey {
    std::uint64_t owner = 0, revision = 0;
    std::uint32_t width = 0;
    AsyncTextStyle style;
    bool batch = false;
    bool operator==(const TextKey&) const = default;
};
struct TextRequest {
    TextKey key;
    std::shared_ptr<const std::wstring> text;
    std::shared_ptr<const AsyncTextBatch> batch;
    AsyncTextStyle style;
    float width = 0.0f;
    std::uint64_t generation = 0;
};
}

AsyncTextStyle AsyncTextStyle::fromFormat(IDWriteTextFormat* format)
{
    AsyncTextStyle value;
    if (!format) return value;
    value.family.resize(format->GetFontFamilyNameLength() + 1U);
    if (FAILED(format->GetFontFamilyName(value.family.data(), static_cast<UINT32>(value.family.size()))))
        throw std::runtime_error("font family snapshot");
    value.family.pop_back();
    value.locale.resize(format->GetLocaleNameLength() + 1U);
    if (FAILED(format->GetLocaleName(value.locale.data(), static_cast<UINT32>(value.locale.size()))))
        throw std::runtime_error("font locale snapshot");
    value.locale.pop_back();
    value.size = format->GetFontSize(); value.weight = format->GetFontWeight();
    value.slant = format->GetFontStyle(); value.stretch = format->GetFontStretch();
    value.alignment = format->GetTextAlignment();
    return value;
}

bool AsyncTextFrame::covers(float top, float height) const noexcept
{
    if (!geometry || !cache) return false;
    const auto first = std::lower_bound(geometry->blocks.begin(), geometry->blocks.end(), top,
        [](const auto& block, float y){return block.top + block.height <= y;});
    for (auto it = first; it != geometry->blocks.end() && it->top < top + height; ++it) {
        if (!cache->hasVisible(*it->document, top - it->top, top + height - it->top)) return false;
    }
    return true;
}

struct AsyncTextView::State {
    std::mutex mutex;
    std::shared_ptr<TextLayoutBudget> budget;
    std::shared_ptr<AsyncTextTestHooks> hooks;
    std::shared_ptr<const TextRequest> wanted;
    std::shared_ptr<const AsyncTextGeometry> geometry;
    std::shared_ptr<const AsyncTextFrame> ready;
    AsyncTextStats stats;
    HWND wakeWindow = nullptr;
    UINT wakeMessage = 0;
    float scroll = 0.0f, height = 0.0f;
    bool pending = false;
    bool allocationFailed = false;
};

struct AsyncTextView::Callback {
    std::shared_ptr<State> state;
    HMODULE module = nullptr;
};

UINT AsyncTextView::readyMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(L"Goden.AsyncTextReady.V1");
    return message;
}

AsyncTextView::AsyncTextView(std::shared_ptr<TextLayoutBudget> budget,
    std::shared_ptr<AsyncTextTestHooks> hooks) : state_(std::make_shared<State>())
{
    state_->budget = budget ? std::move(budget) : std::make_shared<TextLayoutBudget>();
    state_->hooks = std::move(hooks);
    state_->wakeMessage = readyMessage();
}

AsyncTextView::~AsyncTextView() { cancel(); }

void AsyncTextView::cancel() noexcept
{
    std::lock_guard lock(state_->mutex);
    if (state_->wanted) {
        ++state_->stats.generation;
        ++state_->stats.cancellations;
    }
    state_->wakeWindow = nullptr;
    state_->pending = false;
    state_->allocationFailed = false;
    state_->wanted.reset();
    state_->ready.reset();
    state_->geometry.reset();
    state_->stats.status = AsyncTextStatus::idle;
}

AsyncTextStats AsyncTextView::stats() const noexcept
{
    std::lock_guard lock(state_->mutex);
    return state_->stats;
}

std::shared_ptr<const AsyncTextFrame> AsyncTextView::prepare(
    std::uint64_t owner, std::uint64_t revision,
    std::shared_ptr<const std::wstring> text, const AsyncTextStyle& style,
    float width, float viewportHeight, float scroll, HWND wakeWindow)
{
    return prepareImpl(owner, revision, std::move(text), style, {}, width,
        viewportHeight, scroll, wakeWindow);
}

std::shared_ptr<const AsyncTextFrame> AsyncTextView::prepareBatch(std::uint64_t owner,
    std::uint64_t revision, std::shared_ptr<const AsyncTextBatch> batch,
    float width, float viewportHeight, float scroll, HWND wakeWindow) try
{
    static const AsyncTextStyle keyStyle;
    return prepareImpl(owner, revision, {}, keyStyle, std::move(batch), width,
        viewportHeight, scroll, wakeWindow);
}
catch (...) {
    cancel();
    std::lock_guard lock(state_->mutex);
    state_->stats.status = AsyncTextStatus::failed;
    state_->allocationFailed = true;
    return {};
}

std::shared_ptr<const AsyncTextFrame> AsyncTextView::prepareImpl(
    std::uint64_t owner, std::uint64_t revision, std::shared_ptr<const std::wstring> text,
    const AsyncTextStyle& style, std::shared_ptr<const AsyncTextBatch> batch,
    float width, float viewportHeight, float scroll, HWND wakeWindow) try
{
    if ((!text && !batch) || !std::isfinite(width) || width <= 0.0f ||
        !std::isfinite(viewportHeight) || viewportHeight <= 0.0f ||
        !std::isfinite(scroll) || !std::isfinite(style.size) || style.size <= 0.0f) {
        cancel();
        return {};
    }
    std::lock_guard lock(state_->mutex);
    if (state_->allocationFailed) return {};
    state_->wakeWindow = wakeWindow;
    const auto widthBits = std::bit_cast<std::uint32_t>(width);
    const auto* key = state_->wanted ? &state_->wanted->key : nullptr;
    const bool changed = !key || key->owner != owner || key->revision != revision ||
        key->width != widthBits || !(key->style == style) || key->batch != bool(batch);
    if (changed) {
        const bool reflow = key && key->owner == owner && key->revision == revision &&
            key->style == style && key->batch == bool(batch);
        ++state_->stats.generation;
        state_->wanted = std::make_shared<const TextRequest>(TextRequest{
            TextKey{owner, revision, widthBits, style, bool(batch)},
            std::move(text), std::move(batch), style, width, state_->stats.generation});
        state_->geometry.reset();
        if (!reflow) state_->ready.reset();
        state_->pending = true;
        state_->stats.status = AsyncTextStatus::pending;
    }
    const auto newScroll = std::max(0.0f, scroll);
    const bool viewChanged = state_->scroll != newScroll || state_->height != viewportHeight;
    state_->scroll = newScroll;
    state_->height = viewportHeight;
    if (state_->geometry) state_->scroll = std::min(state_->scroll,
        std::max(0.0f, state_->geometry->height - state_->height));
    const auto covered = state_->ready && state_->ready->generation == state_->stats.generation &&
        state_->ready->covers(state_->scroll, state_->height);
    if (covered) {
        state_->stats.status = AsyncTextStatus::ready;
        state_->pending = false;
    } else if (state_->stats.status != AsyncTextStatus::failed && (changed || viewChanged)) {
        state_->pending = true;
        state_->stats.status = AsyncTextStatus::pending;
    }
    if (state_->pending && !state_->stats.running) {
        // Pin before enqueue; callbacks may begin AND finish before submit returns.
        HMODULE module = nullptr;
        auto callback = std::make_unique<Callback>();
        callback->state = state_;
        const auto pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&AsyncTextView::run), &module);
        callback->module = module;
        state_->stats.running = true;
        ++state_->stats.submissions;
        if (!pinned || (state_->hooks && state_->hooks->rejectSubmission.load()) ||
            !TrySubmitThreadpoolCallback(&AsyncTextView::run, callback.get(),
                state_->hooks ? state_->hooks->callbackEnvironment : nullptr)) {
            state_->stats.running = false;
            state_->pending = false;
            state_->stats.status = AsyncTextStatus::failed;
            if (module) FreeLibrary(module);
        } else {
            (void)callback.release();
        }
    }
    return state_->ready;
}
catch (...) {
    cancel();
    std::lock_guard lock(state_->mutex);
    state_->stats.status = AsyncTextStatus::failed;
    state_->allocationFailed = true;
    return {};
}

void CALLBACK AsyncTextView::run(PTP_CALLBACK_INSTANCE instance, void* context) noexcept
{
    std::unique_ptr<Callback> callback(static_cast<Callback*>(context));
    FreeLibraryWhenCallbackReturns(instance, callback->module);
    (void)CallbackMayRunLong(instance);
    const auto state = callback->state;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope { bool owned; ~ComScope() { if (owned) CoUninitialize(); } }
        comScope{SUCCEEDED(com)};
    for (;;) {
        // Resources for this iteration die before running=false is published.
        {
            std::shared_ptr<const TextRequest> request;
            std::shared_ptr<const AsyncTextGeometry> geometry;
            {
                std::lock_guard lock(state->mutex);
                if (state->pending && state->wanted) {
                    request = state->wanted;
                    geometry = state->geometry;
                    state->pending = false;
                    ++state->stats.starts;
                }
            }
            if (request) {
                std::unique_ptr<TextLayoutCache> cache;
                std::shared_ptr<AsyncTextFrame> frame;
                const auto cancelled = [&] {
                    std::lock_guard lock(state->mutex);
                    return !state->wanted || state->stats.generation != request->generation;
                };
                bool failed = false;
                try {
                    cache = std::make_unique<TextLayoutCache>(state->budget, 1U, false);
                    if (!geometry && !cancelled()) {
                        if (state->hooks && state->hooks->beforeMeasure) state->hooks->beforeMeasure();
                        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
                        const auto result = state->hooks && state->hooks->createFactory
                            ? state->hooks->createFactory(factory.ReleaseAndGetAddressOf())
                            : DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,
                                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(
                                    factory.ReleaseAndGetAddressOf()));
                        if (SUCCEEDED(result) && factory && !cancelled()) {
                            AsyncTextBatch single;
                            if (!request->batch) {
                                single.styles.push_back(request->style);
                                single.blocks.push_back({request->text});
                            }
                            const auto& batch = request->batch ? *request->batch : single;
                            if (!std::isfinite(batch.leading) || batch.leading < 0.f)
                                throw std::runtime_error("invalid text flow");
                            std::vector<Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats;
                            for (const auto& style : batch.styles) {
                                Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
                                if (!std::isfinite(style.size) || style.size <= 0.f ||
                                    FAILED(factory->CreateTextFormat(style.family.c_str(), nullptr,
                                        style.weight, style.slant, style.stretch, style.size,
                                        style.locale.c_str(), format.GetAddressOf())))
                                    throw std::runtime_error("text format unavailable");
                                format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                                format->SetTextAlignment(style.alignment);
                                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                                formats.push_back(std::move(format));
                            }
                            auto measured = std::make_shared<AsyncTextGeometry>();
                            measured->height = batch.leading;
                            std::vector<std::shared_ptr<const RetainedTextDocument>> blanks(formats.size());
                            for (const auto& block : batch.blocks) {
                                if (cancelled()) break;
                                if (!block.text || block.style >= formats.size() ||
                                    !std::isfinite(block.before) || block.before < 0.f ||
                                    !std::isfinite(block.after) || block.after < 0.f ||
                                    !std::isfinite(block.advanceScale) || block.advanceScale < 1.f)
                                    throw std::runtime_error("invalid text block");
                                auto document = block.text->empty() ? blanks[block.style] : nullptr;
                                if (!document) document = cache->getOrCreate(factory.Get(),
                                    request->key.revision, measured->blocks.size() + 1U, block.style + 1U,
                                    *block.text, formats[block.style].Get(), request->width, 1000000.f, cancelled);
                                if (!document) break;
                                if (block.text->empty()) blanks[block.style] = document;
                                measured->height += block.before;
                                const auto height = document->height * block.advanceScale;
                                measured->blocks.push_back({document, measured->height, height});
                                measured->height += height + block.after;
                                if (!std::isfinite(measured->height)) throw std::runtime_error("text extent overflow");
                            }
                            if (!cancelled() && measured->blocks.size() == batch.blocks.size())
                                geometry = std::move(measured);
                        }
                    }
                    if (geometry && !cancelled()) {
                        float scroll = 0.0f, height = 0.0f;
                        {
                            std::lock_guard lock(state->mutex);
                            if (state->wanted && state->stats.generation == request->generation) {
                                state->geometry = geometry;
                                height = state->height;
                                scroll = std::clamp(state->scroll, 0.0f,
                                    std::max(0.0f, geometry->height - height));
                            }
                        }
                        // Measurement retains at most one layout. Reuse it if
                        // the visible slice needs that paragraph: reshaping a
                        // long paragraph here can cost as much as measuring it.
                        // The existing LRU and shared leases still bound it.
                        cache->setResidentLimit(TextLayoutBudget::limit);
                        bool prepared = height > 0.f;
                        const auto warmTop = std::max(0.f, scroll - 128.f);
                        const auto warmBottom = std::min(geometry->height, scroll + height + 128.f);
                        const auto first = std::lower_bound(geometry->blocks.begin(), geometry->blocks.end(), warmTop,
                            [](const auto& block, float y){return block.top + block.height <= y;});
                        for (auto it = first; prepared && it != geometry->blocks.end() && it->top < warmBottom; ++it)
                            prepared = cache->prepareVisible(*it->document, warmTop - it->top,
                                warmBottom - it->top, cancelled);
                        if (prepared && !cancelled()) {
                            cache->releaseUnpreparedLayouts();
                            frame = std::make_shared<AsyncTextFrame>();
                            frame->geometry = geometry;
                            if (!geometry->blocks.empty()) frame->document = geometry->blocks.front().document;
                            frame->cache = std::move(cache);
                            frame->scroll = scroll;
                            frame->viewportHeight = height;
                            frame->width = request->width;
                            frame->generation = request->generation;
                        }
                    }
                    failed = !frame && !cancelled();
                } catch (...) { failed = !cancelled(); }
                std::lock_guard lock(state->mutex);
                if (state->wanted && state->stats.generation == request->generation) {
                    if (frame) {
                        state->stats.layoutsCreated += frame->cache->stats().layoutsCreated;
                        state->ready = std::move(frame);
                        ++state->stats.publications;
                        const auto top = std::clamp(state->scroll, 0.0f,
                            std::max(0.0f, geometry->height - state->height));
                        state->pending = !state->ready->covers(top, state->height);
                        state->stats.status = state->pending
                            ? AsyncTextStatus::pending : AsyncTextStatus::ready;
                    } else if (failed) {
                        state->pending = false;
                        state->stats.status = AsyncTextStatus::failed;
                    }
                    // Revocation uses this same short mutex; no post can race
                    // past cancel/destruction. The message carries no pointers.
                    if ((state->ready || failed) && state->wakeWindow && state->wakeMessage)
                        PostMessageW(state->wakeWindow, state->wakeMessage, 0, 0);
                }
            }
        }
        std::lock_guard lock(state->mutex);
        if (!state->pending || !state->wanted) {
            state->stats.running = false;
            return;
        }
    }
}

} // namespace nekorox::component
