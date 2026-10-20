#include "mf_audio_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nekorox::analysis {
namespace {

constexpr DWORD kFirstAudioStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    [[nodiscard]] T* get() const noexcept { return value_; }
    [[nodiscard]] T** put() noexcept
    {
        reset();
        return &value_;
    }
    T* operator->() const noexcept { return value_; }
    void reset() noexcept
    {
        if (value_ != nullptr) value_->Release();
        value_ = nullptr;
    }

private:
    T* value_ = nullptr;
};

std::string hresultText(const char* operation, HRESULT value)
{
    std::ostringstream text;
    text << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
         << static_cast<unsigned long>(value) << ')';
    return text.str();
}

std::uint32_t analysisRateForDuration(double durationSeconds)
{
    constexpr double kFeatureSampleBudget = 7'500'000.0;
    if (!(durationSeconds > 0.0)) return 12000;
    const auto budgetedRate = kFeatureSampleBudget / durationSeconds;
    if (budgetedRate >= 16000.0) return 16000;
    if (budgetedRate >= 12000.0) return 12000;
    if (budgetedRate >= 10000.0) return 10000;
    return 8000;
}

bool setFloatType(IMFSourceReader* reader, std::uint32_t sampleRate,
    std::uint32_t channels, std::string& error)
{
    ComPtr<IMFMediaType> type;
    HRESULT result = MFCreateMediaType(type.put());
    if (SUCCEEDED(result)) result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(result)) result = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (SUCCEEDED(result)) result = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(result)) result = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    if (SUCCEEDED(result)) result = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    if (SUCCEEDED(result)) result = type->SetUINT32(
        MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4U);
    if (SUCCEEDED(result)) result = type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 4U);
    if (SUCCEEDED(result)) result = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(kFirstAudioStream,
            nullptr, type.get());
    }
    if (FAILED(result)) {
        error = hresultText("Media Foundation output format negotiation", result);
        return false;
    }
    return true;
}

std::uint32_t readNativeChannelCount(IMFSourceReader* reader) noexcept
{
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetNativeMediaType(
            kFirstAudioStream, 0, type.put()))) return 1U;
    UINT32 channels = 1U;
    if (FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))) {
        return 1U;
    }
    return std::max<UINT32>(1U, channels);
}

double readDuration(IMFSourceReader* reader)
{
    PROPVARIANT duration;
    PropVariantInit(&duration);
    const HRESULT result = reader->GetPresentationAttribute(
        kMediaSource, MF_PD_DURATION, &duration);
    double seconds = 0.0;
    if (SUCCEEDED(result) && duration.vt == VT_UI8) {
        seconds = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&duration);
    return seconds;
}

} // namespace

bool decodeLocalAudio(const std::filesystem::path& path, DecodedAudio& output,
    const DecodeProgress& progress, std::string& error)
{
    output = {};
    error.clear();
    if (!std::filesystem::is_regular_file(path)) {
        error = "The analysis source is not a readable local file.";
        return false;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        error = hresultText("COM initialization", comResult);
        return false;
    }
    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startupResult)) {
        if (ownsCom) CoUninitialize();
        error = hresultText("Media Foundation startup", startupResult);
        return false;
    }

    bool succeeded = false;
    do {
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(attributes.put(), 2);
        if (SUCCEEDED(result)) {
            result = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        }
        if (FAILED(result)) {
            error = hresultText("Media Foundation reader attributes", result);
            break;
        }

        ComPtr<IMFSourceReader> reader;
        result = MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put());
        if (FAILED(result)) {
            error = hresultText("Media Foundation source reader", result);
            break;
        }
        reader->SetStreamSelection(kAllStreams, FALSE);
        result = reader->SetStreamSelection(kFirstAudioStream, TRUE);
        if (FAILED(result)) {
            error = hresultText("Media Foundation audio stream selection", result);
            break;
        }

        output.durationSeconds = readDuration(reader.get());
        if (output.durationSeconds > 2.0 * 60.0 * 60.0) {
            error = "Deep analysis is limited to two hours per local track.";
            break;
        }
        output.sampleRate = analysisRateForDuration(output.durationSeconds);
        output.sourceChannels = readNativeChannelCount(reader.get());
        const auto decodeChannels = output.sourceChannels > 1U ? 2U : 1U;
        if (!setFloatType(reader.get(), output.sampleRate,
                decodeChannels, error)) break;

        const auto expectedSamples = output.durationSeconds > 0.0
            ? static_cast<std::uint64_t>(std::ceil(
                output.durationSeconds * static_cast<double>(output.sampleRate)))
            : 0ULL;
        if (expectedSamples > 0 &&
            expectedSamples < static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            output.samples.reserve(static_cast<std::size_t>(expectedSamples));
        }

        std::uint64_t lastProgress = 0;
        double sumLeftSquared = 0.0;
        double sumRightSquared = 0.0;
        double sumCross = 0.0;
        double sumMidSquared = 0.0;
        double sumSideSquared = 0.0;
        std::uint64_t stereoFrames = 0;
        std::uint64_t outOfPhaseFrames = 0;
        for (;;) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(kFirstAudioStream, 0,
                &actualStream, &flags, &timestamp, sample.put());
            if (FAILED(result)) {
                error = hresultText("Media Foundation audio decode", result);
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                succeeded = true;
                break;
            }
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                error = "Media Foundation reported a decode error.";
                break;
            }
            if (sample.get() == nullptr) continue;

            ComPtr<IMFMediaBuffer> buffer;
            result = sample->ConvertToContiguousBuffer(buffer.put());
            if (FAILED(result)) {
                error = hresultText("Media Foundation sample buffer", result);
                break;
            }
            BYTE* bytes = nullptr;
            DWORD byteCount = 0;
            result = buffer->Lock(&bytes, nullptr, &byteCount);
            if (FAILED(result)) {
                error = hresultText("Media Foundation sample lock", result);
                break;
            }
            if ((byteCount % sizeof(float)) != 0) {
                buffer->Unlock();
                error = "Media Foundation returned a misaligned float sample buffer.";
                break;
            }
            const auto count = static_cast<std::size_t>(byteCount / sizeof(float));
            if ((count % decodeChannels) != 0U) {
                buffer->Unlock();
                error = "Media Foundation returned a partial audio frame.";
                break;
            }
            const auto frameCount = count / decodeChannels;
            const auto oldSize = output.samples.size();
            output.samples.resize(oldSize + frameCount);
            const auto* decoded = reinterpret_cast<const float*>(bytes);
            for (std::size_t frame = 0; frame < frameCount; ++frame) {
                auto left = decoded[frame * decodeChannels];
                auto right = decodeChannels > 1U
                    ? decoded[frame * decodeChannels + 1U] : left;
                if (!std::isfinite(left)) left = 0.0f;
                if (!std::isfinite(right)) right = 0.0f;
                left = std::clamp(left, -1.0f, 1.0f);
                right = std::clamp(right, -1.0f, 1.0f);
                output.samples[oldSize + frame] = std::clamp(
                    (left + right) * 0.5f, -1.0f, 1.0f);
                if (decodeChannels > 1U) {
                    const auto mid = static_cast<double>(left + right) * 0.5;
                    const auto side = static_cast<double>(left - right) * 0.5;
                    sumLeftSquared += static_cast<double>(left) * left;
                    sumRightSquared += static_cast<double>(right) * right;
                    sumCross += static_cast<double>(left) * right;
                    sumMidSquared += mid * mid;
                    sumSideSquared += side * side;
                    if (static_cast<double>(left) * right < 0.0) {
                        ++outOfPhaseFrames;
                    }
                    ++stereoFrames;
                }
            }
            buffer->Unlock();
            if (progress && expectedSamples > 0) {
                const auto completed = std::min<std::uint64_t>(
                    output.samples.size(), expectedSamples);
                if (completed >= lastProgress + output.sampleRate / 5U ||
                    (completed == expectedSamples && lastProgress != expectedSamples)) {
                    lastProgress = completed;
                    progress(completed, expectedSamples);
                }
            }
        }
        if (succeeded && stereoFrames > 0U) {
            const auto denominator = std::sqrt(
                sumLeftSquared * sumRightSquared);
            output.hasStereoMetrics = denominator > 1.0e-18;
            output.stereoCorrelation = output.hasStereoMetrics
                ? std::clamp(sumCross / denominator, -1.0, 1.0) : 0.0;
            output.midEnergy = std::sqrt(
                sumMidSquared / static_cast<double>(stereoFrames));
            output.sideEnergy = std::sqrt(
                sumSideSquared / static_cast<double>(stereoFrames));
            output.outOfPhaseFraction = static_cast<double>(outOfPhaseFrames) /
                static_cast<double>(stereoFrames);
        }
    } while (false);

    MFShutdown();
    if (ownsCom) CoUninitialize();
    if (!succeeded) output = {};
    if (succeeded && output.samples.empty()) {
        error = "The decoder produced no audio samples.";
        return false;
    }
    if (succeeded && output.sampleRate > 0) {
        output.durationSeconds = static_cast<double>(output.samples.size()) /
            static_cast<double>(output.sampleRate);
    }
    return succeeded;
}

} // namespace nekorox::analysis
