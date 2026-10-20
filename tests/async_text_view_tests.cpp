#include "../src/component/async_text_view.h"
#include "../src/component/album_mode_view.h"
#include "text_layout_test_factory.h"
#include "../src/component/prepared_text_runs.h"
#include <wincodec.h>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <fstream>

namespace {
using namespace nekorox::component;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
void requireAsync(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void waitAsyncAt(Predicate condition, int line) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("async text test timed out at line " + std::to_string(line));
        Sleep(2);
    }
}
#define waitAsync(...) waitAsyncAt(__VA_ARGS__, __LINE__)
struct Barrier {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void block() {
        std::unique_lock lock(mutex);
        entered = true; changed.notify_all();
        if (!changed.wait_for(lock, 10s, [&]{return released;}))
            throw std::runtime_error("async text test barrier expired");
    }
    void wait() {
        std::unique_lock lock(mutex);
        requireAsync(changed.wait_for(lock, 5s, [&]{return entered;}), "worker missed barrier");
    }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
    ~Barrier() { release(); }
};
}

void runAsyncTextViewTests()
{
    const auto began = std::chrono::steady_clock::now();
    ComPtr<IDWriteFactory> factory;
    requireAsync(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "async factory");
    ComPtr<IDWriteTextFormat> format;
    requireAsync(SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.f, L"", format.GetAddressOf())), "async format");
    const auto wrapped = Microsoft::WRL::Make<TextLayoutTestFactory>();
    wrapped->real = factory;
    wrapped->uiThread = GetCurrentThreadId();
    auto hooks = std::make_shared<AsyncTextTestHooks>();
    hooks->createFactory = [wrapped](IDWriteFactory** output) { return wrapped.CopyTo(output); };
    auto budget = std::make_shared<TextLayoutBudget>();
    AsyncTextView view(budget, hooks);
    auto source = std::make_shared<std::wstring>();
    for (unsigned i = 0; i < 6000U; ++i)
        *source += std::to_wstring(i) + L" música 日本語 العربية — wrapped prose with words\r\n";
    auto submit = [&](std::uint64_t owner, std::uint64_t revision,
            const std::shared_ptr<const std::wstring>& text, float width, float scroll = 0.f) {
        return view.prepare(owner, revision, text, AsyncTextStyle{}, width, 180.f, scroll);
    };
    auto ready = [&](std::uint64_t owner, std::uint64_t revision,
            const std::shared_ptr<const std::wstring>& text, float width, float scroll = 0.f) {
        std::shared_ptr<const AsyncTextFrame> frame;
        const auto started = std::chrono::steady_clock::now();
        try { waitAsync([&] {
            frame = submit(owner, revision, text, width, scroll);
            requireAsync(view.stats().status != AsyncTextStatus::failed, "async preparation failed");
            return frame && view.stats().status == AsyncTextStatus::ready;
        }); } catch (const std::exception& e) {
            const auto stats = view.stats();
            throw std::runtime_error(std::string(e.what()) + " owner=" + std::to_string(owner) +
                " revision=" + std::to_string(revision) + " status=" +
                std::to_string(static_cast<unsigned>(stats.status)) + " running=" +
                std::to_string(stats.running));
        }
        std::cout << "Async ready owner=" << owner << " revision=" << revision << " ms=" <<
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-started).count() << '\n';
        return frame;
    };
    auto frame = ready(1U, 10U, source, 240.f);
    requireAsync(frame->document->text == *source && frame->document->paragraphs.size() == 6001U,
        "async text lost source/paragraphs");
    auto verifyGeometry = [&](const AsyncTextFrame& frame, float width) {
        float total = 0.f;
        for (const auto& paragraph : frame.document->paragraphs) {
            ComPtr<IDWriteTextLayout> oracle;
            requireAsync(SUCCEEDED(factory->CreateTextLayout(paragraph.length
                    ? frame.document->text.data() + paragraph.offset : L" ",
                static_cast<UINT32>(std::max<std::size_t>(1U, paragraph.length)),
                format.Get(), width, 1000000.f, oracle.GetAddressOf())), "async oracle create");
            DWRITE_TEXT_METRICS metrics{};
            requireAsync(SUCCEEDED(oracle->GetMetrics(&metrics)) && paragraph.top == total &&
                paragraph.height == std::max(1.f, metrics.height) && paragraph.width == metrics.width,
                "async geometry diverged");
            total += paragraph.height;
        }
        requireAsync(total == frame.document->height, "async full extent diverged");
    };
    verifyGeometry(*frame, 240.f);

    // Drawing uses the prepared cache, including a long single paragraph.
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComGuard { bool owned; ~ComGuard(){if(owned)CoUninitialize();} } comGuard{SUCCEEDED(com)};
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<ID2D1Factory> drawing;
    requireAsync(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&imaging))) && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        drawing.GetAddressOf())), "async pixel factories");
    auto pixels = [&](const AsyncTextFrame& frame, bool prepared, float width, float offset,
            float dpi = 96.f, float scale = 1.f) {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
        requireAsync(SUCCEEDED(imaging->CreateBitmap(300, 220, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, bitmap.GetAddressOf())), "async WIC bitmap");
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        requireAsync(SUCCEEDED(drawing->CreateWicBitmapRenderTarget(bitmap.Get(), properties,
            target.GetAddressOf())) && SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), brush.GetAddressOf())), "async D2D target");
        target->SetDpi(dpi, dpi);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale,
            D2D1::Point2F(11.25f, 51.7f)) * D2D1::Matrix3x2F::Translation(.25f, .375f));
        const auto drawStart = std::chrono::steady_clock::now();
        target->BeginDraw(); target->Clear(D2D1::ColorF(0.f, 0.f, 0.f));
        const auto viewport = D2D1::RectF(11.25f, 13.5f, 11.25f + width, 193.5f);
        if (prepared) {
            requireAsync(frame.cache->drawPrepared(target.Get(), *frame.document, viewport,
                brush.Get(), viewport.top, offset), "prepared coverage missing");
        } else {
            target->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
            for (const auto& paragraph : frame.document->paragraphs) {
                if (paragraph.top + paragraph.height <= offset) continue;
                if (paragraph.top >= offset + 180.f) break;
                ComPtr<IDWriteTextLayout> layout;
                requireAsync(SUCCEEDED(factory->CreateTextLayout(paragraph.length
                        ? frame.document->text.data() + paragraph.offset : L" ",
                    static_cast<UINT32>(std::max<std::size_t>(1U, paragraph.length)),
                    format.Get(), width, 1000000.f, layout.GetAddressOf())), "async pixel oracle");
                target->DrawTextLayout(D2D1::Point2F(viewport.left, viewport.top + paragraph.top - offset),
                    layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            target->PopAxisAlignedClip();
        }
        requireAsync(SUCCEEDED(target->EndDraw()), "async EndDraw");
        if (prepared && frame.document->text.size() >= 524288U)
            std::cout << "Async longline prepared draw+EndDraw only ms=" <<
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawStart).count() << '\n';
        std::vector<BYTE> result(300U * 220U * 4U);
        requireAsync(SUCCEEDED(bitmap->CopyPixels(nullptr, 1200,
            static_cast<UINT>(result.size()), result.data())), "async pixels read");
        return result;
    };
    const auto firstPrepared = pixels(*frame, true, 240.f, 0.f);
    const auto firstOracle = pixels(*frame, false, 240.f, 0.f);
    if (firstPrepared != firstOracle) {
        std::ofstream("artifacts/continued-review-20261008/text-prepared.bgra", std::ios::binary)
            .write(reinterpret_cast<const char*>(firstPrepared.data()), firstPrepared.size());
        std::ofstream("artifacts/continued-review-20261008/text-oracle.bgra", std::ios::binary)
            .write(reinterpret_cast<const char*>(firstOracle.data()), firstOracle.size());
    }
    requireAsync(firstPrepared == firstOracle, "async first viewport pixels differ");
    for (const auto dpi : {96.f, 120.f, 144.f, 168.f, 192.f, 240.f, 288.f})
        for (const auto scale : {0.94f, 1.f, 1.06f}) {
            const auto actual = pixels(*frame, true, 240.f, 0.f, dpi, scale);
            const auto expected = pixels(*frame, false, 240.f, 0.f, dpi, scale);
            if (actual != expected) {
                std::cout << "DPI/scale failure dpi=" << dpi << " scale=" << scale << '\n';
                std::ofstream("artifacts/continued-review-20261008/text-prepared.bgra", std::ios::binary)
                    .write(reinterpret_cast<const char*>(actual.data()), actual.size());
                std::ofstream("artifacts/continued-review-20261008/text-oracle.bgra", std::ios::binary)
                    .write(reinterpret_cast<const char*>(expected.data()), expected.size());
            }
            requireAsync(actual == expected, "async DPI/scale pixels differ");
        }
    const auto oldDocument = frame->document;
    const auto beforeWarm = wrapped->createdCalls.load();
    auto scrolled = ready(1, 10, source, 240.f, 5200.f);
    requireAsync(scrolled->document == oldDocument && wrapped->createdCalls.load() - beforeWarm < 30U,
        "scroll remeasured geometry");
    requireAsync(pixels(*scrolled, true, 240.f, 5200.f) == pixels(*scrolled, false, 240.f, 5200.f),
        "async scroll pixels differ");
    frame = ready(1, 10, source, 171.f, 5200.f);
    verifyGeometry(*frame, 171.f);
    requireAsync(pixels(*frame, true, 171.f, 5200.f) == pixels(*frame, false, 171.f, 5200.f),
        "async resize pixels differ");
    auto longLine = std::make_shared<std::wstring>();
    while (longLine->size() < 524288U) *longLine += L"long paragraph música 日本語 ";
    const auto beforeLongPreparation = wrapped->createdCalls.load();
    frame = ready(2, 11, longLine, 240.f);
    requireAsync(wrapped->createdCalls.load() - beforeLongPreparation == 1U,
        "long paragraph was reshaped between measurement and visible preparation");
    verifyGeometry(*frame, 240.f);
    const auto beforeDraw = wrapped->createdCalls.load();
    requireAsync(pixels(*frame, true, 240.f, 0.f) == pixels(*frame, false, 240.f, 0.f),
        "async long paragraph pixels differ");
    requireAsync(wrapped->createdCalls == beforeDraw && wrapped->uiCalls == 0U,
        "long paragraph reshaped on UI");
    auto blanks = std::make_shared<std::wstring>(L"START\n");
    blanks->append(100000U, L'\n'); *blanks += L"END\n";
    frame = ready(3, 12, blanks, 240.f);
    requireAsync(frame->document->text == *blanks && frame->document->paragraphs.size() == 100003U,
        "async blank content lost");
    frame.reset(); scrolled.reset(); view.cancel();
    waitAsync([&]{return !view.stats().running;});
    requireAsync(budget->live == 0U, "async clear retained COM layouts");

    // Work blocked inside layout creation cannot delay UI cancellation/destruction.
    auto blocked = std::make_shared<Barrier>();
    wrapped->beforeCreate = [blocked]{blocked->block();};
    (void)submit(4, 13, source, 240.f);
    blocked->wait();
    auto b = std::make_shared<const std::wstring>(L"STALE B");
    auto c = std::make_shared<const std::wstring>(L"CURRENT C\nsecond line");
    requireAsync(!submit(5, 14, b, 240.f) && !submit(6, 15, c, 171.f, 100.f),
        "old owner appeared during Pending");
    blocked->release();
    frame = ready(6, 15, c, 171.f, 100.f);
    requireAsync(frame->document->text == *c && view.stats().publications == 6U,
        "stale completion published");
    view.cancel(); frame.reset(); waitAsync([&]{return !view.stats().running;});
    wrapped->beforeCreate = {};
    blocked = std::make_shared<Barrier>();
    wrapped->beforeCreate = [blocked]{blocked->block();};
    {
        AsyncTextView disposable(budget, hooks);
        (void)disposable.prepare(9, 99, source, AsyncTextStyle{}, 240.f, 180.f, 0.f);
        blocked->wait();
        disposable.cancel();
        requireAsync(disposable.stats().status == AsyncTextStatus::idle &&
            disposable.stats().running, "cancel drained/kept admission");
        // Destructor must also return before release below.
    }
    blocked->release();
    waitAsync([&]{return budget->live == 0U;});
    // Wait for the cancelled callback to leave the hook before replacing it.
    Sleep(10);
    wrapped->beforeCreate = {};

    hooks->rejectSubmission = true;
    (void)submit(7, 16, c, 240.f);
    requireAsync(view.stats().status == AsyncTextStatus::failed && !view.stats().running,
        "submit failure stranded Pending");
    const auto submissions = view.stats().submissions;
    for (unsigned i = 0; i < 20; ++i) (void)submit(7, 16, c, 240.f);
    requireAsync(view.stats().submissions == submissions, "submit failure retries every paint");
    hooks->rejectSubmission = false;
    frame = ready(7, 17, c, 240.f); frame.reset(); view.cancel();
    waitAsync([&]{return !view.stats().running;});
    wrapped->fail = true;
    (void)submit(7, 18, c, 240.f);
    waitAsync([&]{return !view.stats().running;});
    requireAsync(view.stats().status == AsyncTextStatus::failed, "layout failure did not terminate");
    const auto failedSubmissions = view.stats().submissions;
    (void)submit(7, 18, c, 240.f);
    requireAsync(view.stats().submissions == failedSubmissions, "layout failure paint loop");
    wrapped->fail = false; view.cancel();

    // Reflow retains the old complete frame, without calling it current Ready.
    frame = ready(10, 21, source, 240.f, 5000.f);
    waitAsync([&]{return !view.stats().running;});
    blocked = std::make_shared<Barrier>();
    wrapped->beforeCreate = [blocked]{blocked->block();};
    auto reflowFrame = submit(10, 21, source, 190.f, 5000.f);
    blocked->wait();
    requireAsync(reflowFrame == frame && view.stats().status == AsyncTextStatus::pending,
        "reflow erased snapshot or claimed old geometry Ready");
    blocked->release();
    frame = ready(10, 21, source, 190.f, 5000.f);
    waitAsync([&]{return !view.stats().running;});
    wrapped->beforeCreate = {};
    requireAsync(frame->width == 190.f && frame->generation == view.stats().generation,
        "reflow adopted stale width");
    wrapped->fail = true;
    (void)submit(10, 21, source, 190.f, 17000.f);
    waitAsync([&]{return !view.stats().running;});
    requireAsync(view.stats().status == AsyncTextStatus::failed &&
        submit(10, 21, source, 190.f, 17000.f) == frame,
        "failed warm lost previous complete snapshot");
    wrapped->fail = false; view.cancel(); frame.reset(); reflowFrame.reset();

    // Production factory path plus real pointer-free window notification.
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = instance;
    wc.lpszClassName = L"Goden.AsyncText.TestWindow";
    requireAsync(RegisterClassW(&wc) != 0, "async test window class");
    const auto window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    requireAsync(window != nullptr, "async test window");
    AsyncTextView productionFactory(budget);
    (void)productionFactory.prepare(44, 44, c, AsyncTextStyle{}, 240.f, 180.f, 0.f, window);
    waitAsync([&]{return !productionFactory.stats().running;});
    requireAsync(productionFactory.stats().status == AsyncTextStatus::ready,
        "isolated production factory failed");
    MSG message{};
    requireAsync(PeekMessageW(&message, window, AsyncTextView::readyMessage(),
        AsyncTextView::readyMessage(), PM_REMOVE) != FALSE && message.lParam == 0,
        "ready completion did not wake window");
    productionFactory.cancel();
    blocked = std::make_shared<Barrier>();
    wrapped->beforeCreate = [blocked]{blocked->block();};
    (void)view.prepare(45, 45, c, AsyncTextStyle{}, 240.f, 180.f, 0.f, window);
    blocked->wait(); view.cancel(); blocked->release();
    waitAsync([&]{return !view.stats().running;});
    requireAsync(PeekMessageW(&message, window, AsyncTextView::readyMessage(),
        AsyncTextView::readyMessage(), PM_REMOVE) == FALSE, "cancelled job posted wake");
    wrapped->beforeCreate = {};
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName, instance);

    // Real full synchronous pool, then the Album reservation policy before admission.
    TextLayoutCache sync(budget);
    (void)sync.getOrCreate(factory.Get(), 19, 1, *source, format.Get(), 240.f);
    requireAsync(budget->live == 4096U, "full budget fixture not full");
    sync.setResidentLimit(2048U);
    frame = ready(8, 20, source, 240.f);
    auto oldFrame = frame;
    scrolled = ready(8, 20, source, 240.f, 9000.f);
    requireAsync(budget->peak <= 4096U && budget->live <= 4096U,
        "async/sync COM lease budget exceeded");
    sync.clear(); view.cancel(); frame.reset(); oldFrame.reset(); scrolled.reset();
    waitAsync([&]{return !view.stats().running && budget->live == 0U;});
    requireAsync(wrapped->uiCalls == 0U, "worker layouts created on UI");
    // Multiple independently formatted blocks use complete, atomic geometry.
    // Oracle formats/layouts are created separately, never from the frame cache.
    auto batch = std::make_shared<AsyncTextBatch>();
    batch->leading = 10.f;
    AsyncTextStyle plain; plain.size = 20.f;
    auto centered = plain; centered.size = 15.f; centered.alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    centered.locale = L"en-us"; centered.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    auto italic = plain; italic.family = L"Arial"; italic.size = 11.f; italic.slant = DWRITE_FONT_STYLE_ITALIC;
    batch->styles = {plain, centered, italic};
    for (unsigned i = 0; i < 45; ++i) {
        const auto value = i % 6 == 1 ? L"" : i % 6 == 2 ? L"\r\n\n" :
            L"line " + std::to_wstring(i) + L" música e\u0301 日本語 العربية \rwrapped\ntrailing\n";
        batch->blocks.push_back({std::make_shared<const std::wstring>(value), i % 3,
            i % 4 == 0 ? 29.f : .25f, 1.08f, 12.f});
    }
    AsyncTextView batchView(budget, hooks);
    auto batchReady = [&](const std::shared_ptr<const AsyncTextBatch>& input, std::uint64_t revision,
            float width, float offset, float height = 240.f) {
        std::shared_ptr<const AsyncTextFrame> result;
        waitAsync([&] {
            result = batchView.prepareBatch(101, revision, input, width, height, offset);
            requireAsync(batchView.stats().status != AsyncTextStatus::failed, "batch preparation failed");
            return result && batchView.stats().status == AsyncTextStatus::ready;
        });
        waitAsync([&]{return !batchView.stats().running;});
        return result;
    };
    struct OracleBlock {
        struct Paragraph { ComPtr<IDWriteTextLayout> layout; float top = 0, height = 0; };
        float top = 0, height = 0;
        std::vector<Paragraph> paragraphs;
    };
    auto batchOracle = [&](float width) {
        std::vector<OracleBlock> blocks;
        float next = batch->leading;
        for (const auto& input : batch->blocks) {
            const auto& style = batch->styles[input.style];
            ComPtr<IDWriteTextFormat> oracleFormat;
            requireAsync(SUCCEEDED(factory->CreateTextFormat(style.family.c_str(), nullptr, style.weight,
                style.slant, style.stretch, style.size, style.locale.c_str(), oracleFormat.GetAddressOf())), "batch oracle format");
            oracleFormat->SetTextAlignment(style.alignment);
            OracleBlock block; next += input.before; block.top = next;
            std::size_t start = 0;
            do {
                auto end = input.text->find_first_of(L"\r\n", start);
                if (end == std::wstring::npos) end = input.text->size();
                const auto length = end - start;
                ComPtr<IDWriteTextLayout> layout;
                requireAsync(SUCCEEDED(factory->CreateTextLayout(length ? input.text->data() + start : L" ",
                    static_cast<UINT32>(std::max<std::size_t>(1, length)), oracleFormat.Get(), width, 1000000.f,
                    layout.GetAddressOf())), "batch oracle layout");
                DWRITE_TEXT_METRICS metrics{};
                requireAsync(SUCCEEDED(layout->GetMetrics(&metrics)), "batch oracle metrics");
                const auto height = std::max(1.f, metrics.height);
                block.paragraphs.push_back({layout, block.height, height});
                block.height += height;
                if (end == input.text->size()) break;
                start = end + 1;
                if ((*input.text)[end] == L'\r' && start < input.text->size() && (*input.text)[start] == L'\n') ++start;
            } while (true);
            block.height *= input.advanceScale;
            next += block.height + input.after;
            blocks.push_back(std::move(block));
        }
        return std::pair{std::move(blocks), next};
    };
    auto batchPixels = [&](const AsyncTextFrame& frame, const std::vector<OracleBlock>& oracle,
            bool prepared, float offset, float height, float dpi, float scale) {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
        const auto pixelHeight = std::max(460U, static_cast<UINT>(std::ceil((height + 32.f) * dpi / 96.f)));
        requireAsync(SUCCEEDED(imaging->CreateBitmap(460, pixelHeight, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, bitmap.GetAddressOf())), "batch bitmap");
        auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        requireAsync(SUCCEEDED(drawing->CreateWicBitmapRenderTarget(bitmap.Get(), properties, target.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush.GetAddressOf())), "batch target");
        target->SetDpi(dpi, dpi);
        const auto viewport = D2D1::RectF(11.25f, 13.5f, 11.25f + frame.width, 13.5f + height);
        target->BeginDraw(); target->Clear(D2D1::ColorF(0.f, 0.f, 0.f));
        for (std::size_t i = 0; i < oracle.size(); ++i) {
            const auto& block = oracle[i];
            if (block.top + block.height <= offset || block.top >= offset + height) continue;
            target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(viewport.left,
                albumPlainLyricsScaleCenter(viewport, block.top, block.height, offset))) * D2D1::Matrix3x2F::Translation(.25f, .375f));
            if (prepared) {
                requireAsync(frame.cache->drawPrepared(target.Get(), *frame.geometry->blocks[i].document,
                    viewport, brush.Get(), viewport.top + block.top, offset), "batch glyph coverage");
            } else {
                target->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
                for (const auto& paragraph : block.paragraphs) {
                    const auto top = block.top + paragraph.top;
                    if (top + paragraph.height <= offset || top >= offset + height) continue;
                    target->DrawTextLayout(D2D1::Point2F(viewport.left, viewport.top + top - offset),
                        paragraph.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                target->PopAxisAlignedClip();
            }
        }
        requireAsync(SUCCEEDED(target->EndDraw()), "batch EndDraw");
        std::vector<BYTE> result(460U * pixelHeight * 4U);
        requireAsync(SUCCEEDED(bitmap->CopyPixels(nullptr, 460U * 4U, static_cast<UINT>(result.size()), result.data())), "batch pixels");
        return result;
    };
    for (const auto width : {280.f, 171.f}) {
        auto [oracle, fullHeight] = batchOracle(width);
        auto bf = batchReady(batch, 201, width, 0.f);
        requireAsync(bf->geometry->height == fullHeight && bf->geometry->blocks.size() == batch->blocks.size(), "batch full geometry mismatch");
        for (std::size_t i = 0; i < oracle.size(); ++i) {
            const auto& actual = bf->geometry->blocks[i];
            requireAsync(actual.document->text == *batch->blocks[i].text && actual.top == oracle[i].top &&
                actual.height == oracle[i].height && actual.document->paragraphs.size() == oracle[i].paragraphs.size(), "batch block/flow mismatch");
        }
        for (const auto offset : {0.f, 349.25f, fullHeight - 240.f}) {
            bf = batchReady(batch, 201, width, offset);
            const auto callsBefore = wrapped->createdCalls.load();
            for (const auto dpi : {96.f, 144.f}) for (const auto scale : {.94f, 1.f, 1.06f}) {
                requireAsync(batchPixels(*bf, oracle, true, offset, 240.f, dpi, scale) ==
                    batchPixels(*bf, oracle, false, offset, 240.f, dpi, scale), "batch pixel oracle mismatch");
            }
            requireAsync(wrapped->createdCalls == callsBefore, "batch draw reshaped");
        }
        bf = batchReady(batch, 201, width, 0.f);
        const auto startsBefore = batchView.stats().starts;
        auto taller = batchReady(batch, 201, width, 0.f, 304.f);
        requireAsync(taller == bf && batchView.stats().starts == startsBefore && taller->covers(0.f, 304.f), "vertical prefetch resize was not reused");
        requireAsync(batchPixels(*taller, oracle, true, 0.f, 304.f, 96.f, .94f) ==
            batchPixels(*taller, oracle, false, 0.f, 304.f, 96.f, .94f), "vertical resize omitted pixels");
    }
    // The last measured paragraph can be enormous and outside the warm slice.
    // Keep useful measurement work, but do not pin that offscreen COM layout.
    {
    auto offscreenBatch = std::make_shared<AsyncTextBatch>();
    offscreenBatch->styles.push_back(plain);
    offscreenBatch->blocks.push_back({std::make_shared<const std::wstring>(L"Visible title"),
        0U, 0.f, 1.f, 2000.f});
    auto offscreenText = std::make_shared<std::wstring>();
    while (offscreenText->size() < 32768U) *offscreenText += L"Offscreen paragraph stays complete. ";
    offscreenBatch->blocks.push_back({offscreenText});
    auto offscreenFrame = batchReady(offscreenBatch, 209, 240.f, 0.f, 180.f);
    requireAsync(offscreenFrame->geometry->blocks.size() == 2U &&
        offscreenFrame->geometry->blocks[1].document->text == *offscreenText &&
        offscreenFrame->cache->stats().residentLayouts == 1U && offscreenFrame->covers(0.f, 180.f),
        "ready frame retained an unprepared offscreen layout or lost source");
    const auto retainedOffscreenFrame = offscreenFrame;
    offscreenFrame = batchReady(offscreenBatch, 209, 240.f, 2100.f, 180.f);
    requireAsync(retainedOffscreenFrame->covers(0.f, 180.f) && offscreenFrame->covers(2100.f, 180.f) &&
        retainedOffscreenFrame->cache->stats().residentLayouts == 1U &&
        offscreenFrame->cache->stats().residentLayouts == 1U,
        "scroll mutated a published frame or kept unrelated layouts");
    offscreenFrame.reset();
    }
    // Cancel an already measured prefix before block two completes.
    batchView.cancel(); waitAsync([&]{return !batchView.stats().running;});
    blocked = std::make_shared<Barrier>();
    std::atomic<unsigned> visits{0};
    wrapped->beforeCreate = [blocked, &visits]{if (++visits == 2U) blocked->block();};
    auto interruptedBatch = std::make_shared<AsyncTextBatch>();
    interruptedBatch->styles.push_back(plain);
    for (unsigned i = 0; i < 4; ++i) interruptedBatch->blocks.push_back({std::make_shared<const std::wstring>(L"block " + std::to_wstring(i))});
    const auto pubsBefore = batchView.stats().publications;
    (void)batchView.prepareBatch(101, 202, interruptedBatch, 240.f, 180.f, 0.f);
    blocked->wait();
    requireAsync(!batchView.prepareBatch(101, 203, batch, 240.f, 180.f, 0.f), "batch prefix published");
    requireAsync(!batchView.prepareBatch(101, 204, interruptedBatch, 240.f, 180.f, 0.f), "stale batch prefix visible");
    blocked->release();
    auto finalBatch = batchReady(interruptedBatch, 204, 240.f, 0.f, 180.f);
    requireAsync(finalBatch->geometry->blocks.size() == 4U && batchView.stats().publications == pubsBefore + 1U, "partial/stale batch publication");
    wrapped->beforeCreate = {};
    // Keep two live readers and old frames, as in the Booklet/plain fade.
    frame = ready(102, 205, source, 240.f);
    auto previousBatch = finalBatch;
    finalBatch = batchReady(batch, 206, 240.f, 400.f);
    requireAsync(budget->peak <= 4096U && budget->live <= 4096U, "two reader budget exceeded");
    frame.reset(); previousBatch.reset(); finalBatch.reset(); batchView.cancel(); view.cancel();
    waitAsync([&]{return !view.stats().running && !batchView.stats().running && budget->live == 0U;});
    // Long paragraph interior/end with combining marks and fallback: no edge-only oracle.
    longLine = std::make_shared<std::wstring>();
    while (longLine->size() < 524288U) *longLine += L"e\u0301\u0308 música 日本語 العربية \U0001F600 ";
    const auto beforeFallbackPreparation = wrapped->createdCalls.load();
    frame = ready(103, 207, longLine, 240.f);
    requireAsync(wrapped->createdCalls.load() - beforeFallbackPreparation == 1U,
        "fallback paragraph was reshaped between measurement and visible preparation");
    const auto longHeight = frame->document->height;
    for (const auto offset : {longHeight * .51f, longHeight - 180.f}) {
        frame = ready(103, 207, longLine, 240.f, offset);
        requireAsync(pixels(*frame, true, 240.f, offset) == pixels(*frame, false, 240.f, offset), "long paragraph interior/end mismatch");
    }
    // The real Album pivot must keep a single enormous block on screen.
    // Comparing two blank images would hide a bad offscreen scale center.
    batch = std::make_shared<AsyncTextBatch>();
    batch->leading = 10.f; batch->styles.push_back(plain);
    auto tallText = std::make_shared<std::wstring>();
    while (tallText->size() < 32768U) *tallText += L"Tall paragraph stays visible while scrolling. ";
    batch->blocks.push_back({tallText, 0U, 0.f, 1.08f, 12.f});
    auto [longOracle, longBatchHeight] = batchOracle(240.f);
    const auto realTextBottom = longOracle[0].top + longOracle[0].paragraphs.back().height;
    const auto pivotViewport = D2D1::RectF(0.f, 0.f, 240.f, 240.f);
    requireAsync(albumPlainLyricsScaleCenter(pivotViewport, -30.f, 20.f, 0.f) == -20.f &&
        albumPlainLyricsScaleCenter(pivotViewport, 245.f, 20.f, 0.f) == 255.f,
        "Partially clipped short lyrics lost their legacy scale pivot");
    for (const auto viewHeight : {240.f, 2134.f, 3000.f})
    for (const auto offset : {0.f, realTextBottom * .51f, realTextBottom - viewHeight, longBatchHeight - viewHeight}) {
        auto bf = batchReady(batch, 208, 240.f, offset, viewHeight);
        for (const auto dpi : {96.f, 144.f}) {
            const auto actual = batchPixels(*bf, longOracle, true, offset, viewHeight, dpi, .94f);
            const auto expected = batchPixels(*bf, longOracle, false, offset, viewHeight, dpi, .94f);
            requireAsync(actual == expected, "Album giant paragraph scaled pixels differ");
            std::size_t lit = 0;
            for (std::size_t i = 0; i < actual.size(); i += 4U)
                if (actual[i] || actual[i + 1] || actual[i + 2]) ++lit;
            if (offset != longBatchHeight - viewHeight)
                requireAsync(lit > 500U, "Album giant paragraph was scaled off screen");
        }
    }
    batchView.cancel();
    // Abort during glyph collection (after line-metric preparation).
    ComPtr<IDWriteTextLayout> cancelLayout;
    requireAsync(SUCCEEDED(factory->CreateTextLayout(longLine->data(), static_cast<UINT32>(longLine->size()),
        format.Get(), 240.f, 1000000.f, cancelLayout.GetAddressOf())), "cancel run layout");
    UINT32 lineCount = 0; (void)cancelLayout->GetLineMetrics(nullptr, 0, &lineCount);
    unsigned cancelChecks = 0;
    auto cancelledRuns = PreparedTextRuns::capture(cancelLayout.Get(), 0.f, 200.f,
        [&]{return ++cancelChecks > lineCount + 5U;});
    std::cout << "Glyph cancel checks=" << cancelChecks << " lines=" << lineCount
        << " published=" << bool(cancelledRuns) << '\n';
    requireAsync(!cancelledRuns && cancelChecks > lineCount + 5U, "glyph cancellation ignored");
    frame.reset(); view.cancel(); waitAsync([&]{return !view.stats().running && budget->live == 0U;});
    requireAsync(wrapped->uiCalls == 0U, "batch worker layouts created on UI");
    std::cout << "Async text lifecycle/geometry/pixels/budget PASS ms=" <<
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() << '\n';
}
