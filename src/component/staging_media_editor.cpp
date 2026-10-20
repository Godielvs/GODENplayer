#include "staging_media_editor.h"

#include "sdk_compat.h"
#include "text_util.h"

#include <foobar2000/SDK/album_art_helpers.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#pragma comment(lib, "windowscodecs.lib")

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;

const GUID& roleGuid(ArtworkRole role)
{
    switch (role) {
    case ArtworkRole::front: return album_art_ids::cover_front;
    case ArtworkRole::back: return album_art_ids::cover_back;
    case ArtworkRole::artist: return album_art_ids::artist;
    case ArtworkRole::other: return album_art_ids::disc;
    }
    return album_art_ids::cover_front;
}

std::string canonicalPath(const std::filesystem::path& path)
{
    pfc::string8 canonical;
    filesystem::g_get_canonical_path(wideToUtf8(path.wstring()).c_str(), canonical);
    return canonical.c_str();
}

bool mutateInfo(file_info& info, const std::vector<nekorox::TagMutation>& edits)
{
    bool changed = false;
    for (const auto& edit : edits) {
        if (edit.mode == nekorox::TagMutationMode::keep) continue;
        const auto current = info.meta_find(edit.field.c_str());
        if (edit.mode == nekorox::TagMutationMode::remove) {
            if (current != pfc_infinite) {
                info.meta_remove_field(edit.field.c_str());
                changed = true;
            }
            continue;
        }
        if (edit.values.empty()) return false;
        info.meta_remove_field(edit.field.c_str());
        const auto field = info.meta_set(edit.field.c_str(), edit.values.front().c_str());
        for (std::size_t index = 1; index < edit.values.size(); ++index)
            info.meta_add_value(field, edit.values[index].c_str());
        changed = true;
    }
    return changed;
}

bool sameBytes(album_art_data_ptr data, const std::vector<std::uint8_t>& expected)
{
    return data.is_valid() && data->size() == expected.size() &&
        (expected.empty() || std::memcmp(data->data(), expected.data(), expected.size()) == 0);
}

struct InfoSnapshot {
    nekorox::TagValueMap metadata;
    replaygain_info replayGain;
};

InfoSnapshot snapshotInfo(const file_info& info)
{
    InfoSnapshot snapshot;
    snapshot.replayGain = info.get_replaygain();
    for (t_size field = 0; field < info.meta_get_count(); ++field) {
        auto name = std::string(info.meta_enum_name(field));
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::toupper(value));
            });
        auto& values = snapshot.metadata[name];
        for (t_size value = 0; value < info.meta_enum_value_count(field); ++value)
            values.emplace_back(info.meta_enum_value(field, value));
    }
    return snapshot;
}

bool sameSnapshot(const InfoSnapshot& left, const InfoSnapshot& right)
{
    return left.metadata == right.metadata &&
        replaygain_info::g_equal(left.replayGain, right.replayGain);
}

bool applyArtwork(const std::string& canonical,
    const ArtworkDelta& artwork, abort_callback& abort, std::wstring& error)
{
    if (artwork.empty()) return true;
    auto editor = album_art_editor::g_open(nullptr, canonical.c_str(), abort);
    for (const auto& edit : artwork) {
        if (edit.mode == ArtworkMutationMode::keep ||
            edit.mode == ArtworkMutationMode::setSidecar) continue;
        if (edit.mode == ArtworkMutationMode::remove)
            editor->remove(roleGuid(edit.role));
        else if (edit.originalBytes && !edit.originalBytes->empty())
            editor->set(roleGuid(edit.role), album_art_data_impl::g_create(
                edit.originalBytes->data(), edit.originalBytes->size()), abort);
        else {
            error = L"Selected artwork has no bytes";
            return false;
        }
    }
    editor->commit(abort);
    return true;
}

bool verifyArtwork(const std::string& canonical,
    const ArtworkDelta& artwork, abort_callback& abort, std::wstring& error)
{
    if (artwork.empty()) return true;
    auto extractor = album_art_extractor::g_open_allowempty(
        nullptr, canonical.c_str(), abort);
    for (const auto& edit : artwork) {
        if (edit.mode == ArtworkMutationMode::keep ||
            edit.mode == ArtworkMutationMode::setSidecar) continue;
        try {
            const auto data = extractor->query(roleGuid(edit.role), abort);
            if (edit.mode == ArtworkMutationMode::remove ||
                !edit.originalBytes ||
                !sameBytes(data, *edit.originalBytes)) {
                error = L"Artwork readback did not match the staging edit";
                return false;
            }
        } catch (const exception_album_art_not_found&) {
            if (edit.mode != ArtworkMutationMode::remove) {
                error = L"Artwork was not present after commit";
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool loadAndValidateArtwork(const std::filesystem::path& path,
    ValidatedArtwork& output, std::wstring& error, std::stop_token stop)
{
    output = {};
    std::error_code ioError;
    const auto size = std::filesystem::file_size(path, ioError);
    if (ioError || size == 0 || size > 32ULL * 1024ULL * 1024ULL) {
        error = L"Artwork must be a readable image no larger than 32 MiB";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    output.originalBytes.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < output.originalBytes.size() && !stop.stop_requested()) {
        const auto chunk = (std::min<std::size_t>)(
            output.originalBytes.size() - offset, 1024U * 1024U);
        if (!stream.read(reinterpret_cast<char*>(
                output.originalBytes.data() + offset),
                static_cast<std::streamsize>(chunk))) break;
        offset += chunk;
    }
    if (stop.stop_requested()) {
        error = L"Artwork validation was cancelled";
        output = {};
        return false;
    }
    if (offset != output.originalBytes.size()) {
        error = L"Artwork bytes could not be read completely";
        output = {};
        return false;
    }
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE
        ? S_OK : initialized;
    UINT width = 0, height = 0;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> memory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        GUID container{};
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&memory);
        if (SUCCEEDED(hr)) hr = memory->InitializeFromMemory(output.originalBytes.data(),
            static_cast<DWORD>(output.originalBytes.size()));
        if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(memory.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetContainerFormat(&container);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
        if (SUCCEEDED(hr) && width != 0 && height != 0 &&
            static_cast<std::uint64_t>(width) * height <= 64ULL * 1024ULL * 1024ULL) {
            constexpr UINT limit = 128;
            const auto scale = (std::min)(
                static_cast<double>(limit) / width,
                static_cast<double>(limit) / height);
            output.previewWidth = (std::max)(1U,
                static_cast<UINT>(std::lround(width * (std::min)(1.0, scale))));
            output.previewHeight = (std::max)(1U,
                static_cast<UINT>(std::lround(height * (std::min)(1.0, scale))));
            ComPtr<IWICBitmapScaler> scaler;
            ComPtr<IWICFormatConverter> converter;
            hr = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.Get(),
                output.previewWidth, output.previewHeight,
                WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
            if (SUCCEEDED(hr)) hr = converter->Initialize(scaler.Get(),
                GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) {
                const auto stride = output.previewWidth * 4U;
                output.previewBgra.resize(static_cast<std::size_t>(stride) *
                    output.previewHeight);
                hr = converter->CopyPixels(nullptr, stride,
                    static_cast<UINT>(output.previewBgra.size()),
                    output.previewBgra.data());
            }
        }
        if (container == GUID_ContainerFormatPng) output.mime = L"image/png";
        else if (container == GUID_ContainerFormatJpeg) output.mime = L"image/jpeg";
        else if (container == GUID_ContainerFormatGif) output.mime = L"image/gif";
        else if (container == GUID_ContainerFormatTiff) output.mime = L"image/tiff";
        else if (container == GUID_ContainerFormatBmp) output.mime = L"image/bmp";
        else output.mime = L"image/unknown";
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(hr) || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > 64ULL * 1024ULL * 1024ULL) {
        error = L"Artwork is corrupt, unsupported, or exceeds 64 megapixels";
        output = {};
        return false;
    }
    output.width = width;
    output.height = height;
    output.format = path.extension().wstring();
    return true;
}

bool applyAndVerifyStagingEdits(const std::filesystem::path& path,
    const StagingMediaEdits& edits, std::wstring& error)
{
    try {
        abort_callback_dummy abort;
        const auto canonical = canonicalPath(path);
        PhysicalFileEditPlan plan;
        plan.path = path;
        if (!edits.tags.empty()) {
            input_info_reader::ptr reader;
            input_entry::g_open_for_info_read(reader, nullptr, canonical.c_str(), abort);
            for (t_uint32 index = 0; index < reader->get_subsong_count(); ++index) {
                const auto subsong = static_cast<std::uint32_t>(reader->get_subsong(index));
                plan.bySubsong.emplace(subsong, edits.tags);
            }
        }
        plan.fileArtwork = edits.artwork;
        return applyAndVerifyStagingEdits(path, plan, error);
    } catch (const std::exception& exception) {
        error = L"SDK staging writer failed: " + utf8ToWide(exception.what());
        return false;
    } catch (...) {
        error = L"SDK staging writer failed with an unknown error";
        return false;
    }
}

bool applyAndVerifyStagingEdits(const std::filesystem::path& path,
    const PhysicalFileEditPlan& plan, std::wstring& error)
{
    try {
        for (const auto& [_, mutations] : plan.bySubsong) {
            std::string validation;
            if (!nekorox::validateTagMutations(mutations, validation)) {
                error = L"Invalid per-subsong tag plan: " + utf8ToWide(validation);
                return false;
            }
        }

        abort_callback_dummy abort;
        const auto canonical = canonicalPath(path);
        auto lock = file_lock_manager::get()->acquire_write(canonical.c_str(), abort);
        std::map<std::uint32_t, InfoSnapshot> before;
        std::set<std::uint32_t> foundPlannedSubsongs;

        if (!plan.bySubsong.empty()) {
            input_info_writer::ptr writer;
            input_entry::g_open_for_info_write(writer, nullptr, canonical.c_str(), abort);
            const auto count = writer->get_subsong_count();
            bool changed = false;
            for (t_uint32 index = 0; index < count; ++index) {
                const auto subsong = static_cast<std::uint32_t>(writer->get_subsong(index));
                file_info_impl info;
                writer->get_info(subsong, info, abort);
                before.emplace(subsong, snapshotInfo(info));
                const auto edits = plan.bySubsong.find(subsong);
                if (edits == plan.bySubsong.end()) continue;
                foundPlannedSubsongs.insert(subsong);
                if (mutateInfo(info, edits->second)) {
                    writer->set_info(subsong, info, abort);
                    changed = true;
                }
            }
            if (foundPlannedSubsongs.size() != plan.bySubsong.size()) {
                error = L"Physical edit plan references a missing subsong";
                return false;
            }
            if (changed) writer->commit(abort);
        }

        if (!applyArtwork(canonical, plan.fileArtwork, abort, error)) return false;

        if (!plan.bySubsong.empty()) {
            input_info_reader::ptr reader;
            input_entry::g_open_for_info_read(reader, nullptr, canonical.c_str(), abort);
            std::size_t readbackCount = 0;
            for (t_uint32 index = 0; index < reader->get_subsong_count(); ++index) {
                const auto subsong = static_cast<std::uint32_t>(reader->get_subsong(index));
                file_info_impl info;
                reader->get_info(subsong, info, abort);
                const auto original = before.find(subsong);
                if (original == before.end()) {
                    error = L"Subsong set changed during staging write";
                    return false;
                }
                auto expected = original->second;
                const auto edits = plan.bySubsong.find(subsong);
                if (edits != plan.bySubsong.end()) {
                    std::string mutationError;
                    (void)nekorox::applyTagMutations(
                        expected.metadata, edits->second, mutationError);
                }
                if (!sameSnapshot(snapshotInfo(info), expected)) {
                    error = edits == plan.bySubsong.end()
                        ? L"An unselected subsong or ReplayGain value changed"
                        : L"Selected subsong readback did not match its exact delta";
                    return false;
                }
                ++readbackCount;
            }
            if (readbackCount != before.size()) {
                error = L"Subsong count changed during staging write";
                return false;
            }
        }
        return verifyArtwork(canonical, plan.fileArtwork, abort, error);
    } catch (const std::exception& exception) {
        error = L"SDK physical-file staging writer failed: " +
            utf8ToWide(exception.what());
        return false;
    } catch (...) {
        error = L"SDK physical-file staging writer failed with an unknown error";
        return false;
    }
}

} // namespace nekorox::component
