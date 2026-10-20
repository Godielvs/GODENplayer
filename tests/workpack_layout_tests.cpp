#include "../src/component/text_layout_cache.h"
#include "../src/component/ui_form_layout.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

void runTextLayoutResidencyTests();
void runAsyncTextViewTests();

void runWorkpackLayoutTests()
{
    using Microsoft::WRL::ComPtr;
    using nekorox::component::TextLayoutCache;
    using nekorox::component::UiFormFlow;

    // OptionsSectionsFlowWithoutMagicY: every section consumes the previous
    // section's measured height, so future copy changes cannot overlap it.
    UiFormFlow form(12);
    const auto appearance = form.take(26, 668, 24, 12);
    const auto appearanceRow = form.take(26, 668, 30, 20);
    const auto continuity = form.take(26, 668, 24, 12);
    const auto continuityRow = form.take(26, 668, 30, 20);
    const auto lyrics = form.take(26, 668, 24, 12);
    const auto tools = form.take(26, 668, 30, 24);
    if (appearance.y != 12 || appearanceRow.y <= appearance.y ||
        continuity.y <= appearanceRow.y ||
        continuityRow.y <= continuity.y || lyrics.y <= continuityRow.y ||
        tools.y <= lyrics.y || form.cursor() <= tools.y + tools.height) {
        throw std::runtime_error(
            "Q04 Options flow sections overlapped or lost their final gap");
    }

    // Narrow-form inputs are clamped to positive control bounds; the native
    // viewport owns clipping, never a negative-width hidden hit target.
    UiFormFlow narrow(0);
    const auto cardHeader = narrow.take(16, 588, 20, 4);
    const auto cardRow = narrow.take(16, 588, 30, 12);
    const auto preview = narrow.take(16, 112, 72, 12);
    if (cardHeader.width <= 0 || cardRow.width <= 0 || preview.width <= 0 ||
        cardRow.y < cardHeader.y + cardHeader.height ||
        preview.y < cardRow.y + cardRow.height) {
        throw std::runtime_error("Q04 narrow artwork card flow overlapped");
    }
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.ReleaseAndGetAddressOf())))) {
        throw std::runtime_error("WP01 could not create DirectWrite factory");
    }
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us",
            format.ReleaseAndGetAddressOf()))) {
        throw std::runtime_error("WP01 could not create DirectWrite format");
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    TextLayoutCache cache;
    const auto count9=cache.getOrCreate(factory.Get(),100U,0x300000002ULL,L"9",format.Get(),80.f,24.f);
    const auto count10=cache.getOrCreate(factory.Get(),101U,0x300000002ULL,L"10",format.Get(),80.f,24.f);
    if(!count9||!count10||count9->paragraphs.empty()||count10->paragraphs.empty()||
        count9.get()==count10.get()||
        count10->paragraphs.front().width<=count9->paragraphs.front().width)
        throw std::runtime_error("statistics text revision retained the old DirectWrite count");
    const std::wstring text = L"Unicode: música 日本語\n\nPunctuation — kept.";
    const auto first = cache.getOrCreate(
        factory.Get(), 10U, 1U, text, format.Get(), 240.0f);
    if (!first || first->paragraphs.size() != 3U || first->height <= 0.0f) {
        throw std::runtime_error("WP01 did not preserve blank text lines");
    }
    const auto afterFirst = cache.stats();
    const auto hot = cache.getOrCreate(
        factory.Get(), 10U, 1U, text, format.Get(), 240.0f);
    const auto afterHot = cache.stats();
    if (hot.get() != first.get() ||
        afterHot.layoutsCreated != afterFirst.layoutsCreated ||
        afterHot.hits != afterFirst.hits + 1U) {
        throw std::runtime_error("WP01 hot lookup recreated text layouts");
    }
    const auto resized = cache.getOrCreate(
        factory.Get(), 10U, 1U, text, format.Get(), 180.0f);
    const auto revised = cache.getOrCreate(
        factory.Get(), 11U, 1U, text, format.Get(), 180.0f);
    const auto restyled = cache.getOrCreate(
        factory.Get(), 11U, 1U, 2U, text, format.Get(), 180.0f);
    if (!resized || !revised || resized.get() == first.get() ||
        revised.get() == resized.get() || !restyled ||
        restyled.get() == revised.get()) {
        throw std::runtime_error(
            "WP01 width/content/style revision did not rebuild");
    }

    // The current open document may consume the documented 1 MiB UTF-16
    // exception without truncating its contents.
    TextLayoutCache largeCache;
    const std::wstring oneMiBDocument(
        TextLayoutCache::maximumOpenDocumentUtf16Bytes / sizeof(wchar_t), L'x');
    const auto large = largeCache.getOrCreate(
        factory.Get(), 20U, 1U, oneMiBDocument, format.Get(), 640.0f);
    if (!large || large->utf16Bytes !=
            TextLayoutCache::maximumOpenDocumentUtf16Bytes) {
        throw std::runtime_error("WP01 truncated the 1 MiB open document");
    }

    // Shared entries remain bounded. Keep the returned shared pointers alive
    // to prove eviction removes cache ownership without invalidating a caller
    // that is currently drawing an older document.
    TextLayoutCache bounded;
    std::vector<std::shared_ptr<const nekorox::component::RetainedTextDocument>>
        liveDocuments;
    liveDocuments.reserve(TextLayoutCache::maximumSharedLayouts + 1U);
    for (std::size_t index = 0;
         index <= TextLayoutCache::maximumSharedLayouts; ++index) {
        liveDocuments.push_back(bounded.getOrCreate(
            factory.Get(), 100U + index, 1U, L"row", format.Get(), 240.0f));
    }
    const auto boundedStats = bounded.stats();
    if (boundedStats.residentLayouts > TextLayoutCache::maximumSharedLayouts ||
        boundedStats.evictions == 0U || !liveDocuments.front()) {
        throw std::runtime_error("WP01 layout cache budget was not enforced");
    }
    runTextLayoutResidencyTests();
    runAsyncTextViewTests();
}
