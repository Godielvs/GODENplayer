#include "qa_hooks.h"
#include "daw_window.h"

#include "daw_output_backend.h"
#include "daw_visual_bridge.h"
#include "discord_presence_service.h"
#include "display_environment.h"
#include "frameless_window.h"
#include "frame_clock.h"
#include "master_dsp_controller.h"
#include "master_dsp_engine.h"
#include "panel_d2d.h"
#include "rollout_flags.h"
#include "runtime.h"
#include "text_util.h"

#include "nekorox/waveform_pyramid.h"
#include "nekorox/qa_contracts.h"

#include "streaming/stream_resampler.h"

#include <foobar2000/SDK/input.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shellapi.h>
#include <shobjidl.h>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include <windowsx.h>
#include <mmsystem.h>

namespace nekorox::component {
namespace {

using namespace std::chrono_literals;
constexpr wchar_t kWindowClass[] = L"GodenDawModeV1";
constexpr UINT kDecodeMessage = WM_APP + 0x5d0;
constexpr UINT kPlaybackEndedMessage = WM_APP + 0x5d1;
constexpr UINT kPlaybackLifecycleMessage = WM_APP + 0x5d2;
constexpr UINT kDecoderStoppedMessage = WM_APP + 0x5d3;
constexpr UINT kFrameClockMessage = WM_APP + 0x5d4;
constexpr std::uint32_t kProjectSampleRate = 48000;
constexpr std::size_t kOutputBufferFrames = 1024;
constexpr float kTimelineRailDip = 228.0f;
constexpr float kTimelineRightPaddingDip = 10.0f;
constexpr std::size_t kMaximumDecodedFramesPerTrack =
    static_cast<std::size_t>(kProjectSampleRate) * 60U * 60U * 6U;
constexpr std::size_t kMaximumProjectPcmBytes = 768U * 1024U * 1024U;
constexpr std::size_t kHotPcmBytesPerClip = 64U * 1024U * 1024U;
constexpr std::size_t kHotPcmBytesTotal = 256U * 1024U * 1024U;
constexpr COLORREF kWindowBackground = RGB(8, 10, 10);
constexpr COLORREF kWindowText = RGB(216, 223, 223);
constexpr COLORREF kWindowBorder = RGB(48, 56, 57);

constexpr double timelineXForSeconds(
    double seconds, double viewStart, double pixelsPerSecond) noexcept
{
    return kTimelineRailDip + (seconds - viewStart) * pixelsPerSecond;
}

constexpr double timelineSecondsAtX(
    double x, double viewStart, double pixelsPerSecond) noexcept
{
    return viewStart + (x - kTimelineRailDip) / pixelsPerSecond;
}

static_assert(timelineSecondsAtX(
    timelineXForSeconds(12.5, 2.0, 90.0), 2.0, 90.0) == 12.5);
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

UINT dawQaStateMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(L"GodenDawQaStateV1");
    return value;
}

UINT dawQaCommandMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(L"GodenDawQaCommandV1");
    return value;
}

UINT dawQaMonitorStateMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"GodenDawQaMonitorStateV1");
    return value;
}

UINT dawQaPositionMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(L"GodenDawQaPositionV1");
    return value;
}

UINT dawQaLifecycleMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"GodenDawQaLifecycleV2");
    return value;
}

bool dawQaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

bool dawQaBackgroundRequested() noexcept
{
    if (!dawQaHooksAllowed()) return false;
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"NEKOROX_QA_BACKGROUND", value,
               static_cast<DWORD>(std::size(value))) == 1U &&
        value[0] == L'1';
}

float clientDip(HWND window, LONG pixels) noexcept
{
    return static_cast<float>(pixels) * 96.0f /
        static_cast<float>((std::max)(96U, GetDpiForWindow(window)));
}

POINT dipPoint(HWND window, LPARAM value) noexcept
{
    return POINT{
        static_cast<LONG>(std::lround(clientDip(window, GET_X_LPARAM(value)))),
        static_cast<LONG>(std::lround(clientDip(window, GET_Y_LPARAM(value)))),
    };
}

bool inRect(const D2D1_RECT_F& rect, float x, float y) noexcept
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::wstring trackLabel(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return L"Untitled sample";
    try {
        titleformat_object::ptr format;
        titleformat_compiler::get()->compile_safe(format,
            "$if2(%title%,$filename(%path%))");
        pfc::string8 value;
        track->format_title(nullptr, value, format, nullptr);
        if (!value.is_empty()) return utf8ToWide(value.c_str());
    } catch (...) {
    }
    try {
        const auto native = filesystem::g_get_native_path(track->get_path());
        return std::filesystem::path(utf8ToWide(native.c_str())).filename().wstring();
    } catch (...) {
        return L"Untitled sample";
    }
}

std::wstring formatTime(double seconds)
{
    seconds = std::max(0.0, seconds);
    const auto totalMilliseconds = static_cast<std::uint64_t>(std::llround(
        seconds * 1000.0));
    const auto minutes = totalMilliseconds / 60000U;
    const auto remainder = (totalMilliseconds / 1000U) % 60U;
    const auto milliseconds = totalMilliseconds % 1000U;
    wchar_t text[64]{};
    swprintf_s(text, L"%llu:%02llu.%03llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(remainder),
        static_cast<unsigned long long>(milliseconds));
    return text;
}

class PcmClip final {
public:
    static constexpr std::size_t kMapWindowBytes = 64U << 20U;
    static std::shared_ptr<PcmClip> fromHot(std::vector<float> samples)
    {
        auto clip = std::shared_ptr<PcmClip>(new PcmClip());
        clip->sampleCount_ = samples.size();
        clip->hot_ = std::move(samples);
        return clip;
    }

    static std::shared_ptr<PcmClip> fromSpill(std::vector<float> hot,
        std::filesystem::path path, std::size_t sampleCount)
    {
        auto clip = std::shared_ptr<PcmClip>(new PcmClip());
        clip->sampleCount_ = sampleCount;
        clip->hot_ = std::move(hot);
        clip->path_ = std::move(path);
        clip->file_ = CreateFileW(clip->path_.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (clip->file_ == INVALID_HANDLE_VALUE) return {};
        clip->mapping_ = CreateFileMappingW(
            clip->file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (clip->mapping_ == nullptr) return {};
        return clip;
    }

    ~PcmClip()
    {
        if (mappedView_) UnmapViewOfFile(mappedView_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    [[nodiscard]] std::size_t frameCount() const noexcept
    {
        return sampleCount_ / 2U;
    }

    [[nodiscard]] float sample(std::size_t index) const noexcept
    {
        if (index >= sampleCount_) return 0.0f;
        if (index < hot_.size()) return hot_[index];
        return mapped_ && index >= mappedFirstSample_ &&
                index < mappedFirstSample_ + mappedSampleCount_
            ? mapped_[index - mappedFirstSample_] : 0.0f;
    }

    [[nodiscard]] std::size_t residentBytes() const noexcept
    {
        return hot_.size() * sizeof(float);
    }

    [[nodiscard]] bool spillBacked() const noexcept { return mapping_ != nullptr; }

    void dropHot()
    {
        if (spillBacked()) std::vector<float>().swap(hot_);
    }

    void prefetch(std::size_t startFrame, std::size_t frames, bool warm = false) noexcept
    {
        if (!mapping_ || startFrame >= frameCount()) return;
        const auto beginSample = startFrame * 2U;
        const auto sampleCount = std::min(
            frames, frameCount() - startFrame) * 2U;
        if (beginSample < hot_.size() &&
                beginSample + sampleCount <= hot_.size()) return;
        const bool alreadyMapped=mapped_ && beginSample>=mappedFirstSample_ &&
            beginSample-mappedFirstSample_<=mappedSampleCount_ &&
            sampleCount<=mappedSampleCount_-(beginSample-mappedFirstSample_);
        if (!mapWindow(beginSample, sampleCount)) return;
        if (alreadyMapped && !warm) return;
        WIN32_MEMORY_RANGE_ENTRY range{};
        const auto local = beginSample >= mappedFirstSample_
            ? beginSample - mappedFirstSample_ : 0U;
        range.VirtualAddress = const_cast<float*>(mapped_ + local);
        range.NumberOfBytes = std::min(sampleCount,
            mappedSampleCount_ - std::min(local, mappedSampleCount_)) * sizeof(float);
        (void)PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
    }

private:
    bool mapWindow(std::size_t firstSample, std::size_t requiredSamples) noexcept
    {
        if (mapped_ && firstSample >= mappedFirstSample_ &&
                firstSample + requiredSamples <=
                    mappedFirstSample_ + mappedSampleCount_) return true;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto alignment = std::max<std::uint64_t>(
            info.dwAllocationGranularity, 64U * 1024U);
        const auto byteOffset = static_cast<std::uint64_t>(firstSample) * sizeof(float);
        const auto alignedOffset = byteOffset - byteOffset % alignment;
        const auto bias = static_cast<std::size_t>(byteOffset - alignedOffset);
        const auto remainingBytes = (sampleCount_ -
            static_cast<std::size_t>(alignedOffset / sizeof(float))) * sizeof(float);
        const auto viewBytes = std::min<std::size_t>(remainingBytes,
            std::max<std::size_t>(kMapWindowBytes, bias +
                requiredSamples * sizeof(float)));
        if (mappedView_) {
            UnmapViewOfFile(mappedView_);
            mappedView_ = nullptr;
            mapped_ = nullptr;
        }
        mappedView_ = MapViewOfFile(mapping_, FILE_MAP_READ,
            static_cast<DWORD>(alignedOffset >> 32U),
            static_cast<DWORD>(alignedOffset & 0xffffffffU), viewBytes);
        if (!mappedView_) return false;
        mappedFirstSample_ = firstSample;
        mappedSampleCount_ = (viewBytes - bias) / sizeof(float);
        mapped_ = reinterpret_cast<const float*>(
            static_cast<const std::byte*>(mappedView_) + bias);
        return mappedSampleCount_ >= std::min(requiredSamples,
            sampleCount_ - firstSample);
    }

    PcmClip() = default;
    std::vector<float> hot_;
    std::filesystem::path path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    void* mappedView_ = nullptr;
    const float* mapped_ = nullptr;
    std::size_t mappedFirstSample_ = 0;
    std::size_t mappedSampleCount_ = 0;
    std::size_t sampleCount_ = 0;
};

struct SpillFileGuard {
    std::filesystem::path path;
    bool handedOff = false;
    ~SpillFileGuard()
    {
        if (!handedOff && !path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

struct DecodeMessage {
    std::uint64_t generation = 0;
    std::uint64_t trackId = 0;
    std::shared_ptr<const WaveformPyramidV1> waveform;
    std::shared_ptr<PcmClip> audio;
    double progress = 0.0;
    bool final = false;
    std::string error;
};

struct MixTrack {
    std::shared_ptr<PcmClip> audio;
    std::int64_t offsetFrames = 0;
    std::uint64_t sourceStartFrame = 0;
    std::uint64_t sourceEndFrame = 0;
    bool muted = false;
};

class PreviewMixer final {
public:
    enum class State : std::uint8_t {
        idle,
        starting,
        playing,
        stopping,
        paused,
        eof,
        error,
    };

    enum class StopReason : std::uint8_t {
        pause,
        stop,
        restart,
        close,
    };

    PreviewMixer()
        : worker_([this] { workerMain(); })
    {
    }

    ~PreviewMixer()
    {
        shutdown();
    }

    void shutdown()
    {
        bool notify = false;
        {
            std::lock_guard lock(commandMutex_);
            if (!shuttingDown_) {
                shuttingDown_ = true;
                ++nextGeneration_;
                commandGeneration_.store(nextGeneration_,
                    std::memory_order_release);
                notify = true;
            }
        }
        if (notify) commandChanged_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void setTarget(HWND target) noexcept
    {
        target_.store(target, std::memory_order_release);
    }

    std::uint64_t play(std::vector<MixTrack> tracks, double startSeconds,
        bool muteLeft, bool muteRight)
    {
        setChannelMutes(muteLeft, muteRight);
        Command command;
        command.action = CommandAction::play;
        command.tracks = std::move(tracks);
        command.startFrame = static_cast<std::uint64_t>(std::max(
            0.0, startSeconds) * kProjectSampleRate);
        // Mapping belongs exclusively to the mixer worker. A UI-side prefetch
        // here can unmap memory that the previous command is still reading.
        state_.store(State::starting, std::memory_order_release);
        const auto generation = publishCommand(std::move(command));
        Runtime::instance().notifyDawPreviewChanged();
        return generation;
    }

    std::uint64_t pause()
    {
        return requestStop(StopReason::pause, true);
    }

    std::uint64_t stop(bool preservePosition)
    {
        return requestStop(StopReason::stop, preservePosition);
    }

    std::uint64_t requestStop(StopReason reason, bool preservePosition)
    {
        Command command;
        command.action = reason == StopReason::pause
            ? CommandAction::pause : CommandAction::stop;
        command.reason = reason;
        command.preservePosition = preservePosition;
        state_.store(State::stopping, std::memory_order_release);
        const auto generation = publishCommand(std::move(command));
        Runtime::instance().notifyDawPreviewChanged();
        return generation;
    }

    bool playing() const noexcept
    {
        const auto state = state_.load(std::memory_order_acquire);
        return state == State::starting || state == State::playing;
    }

    [[nodiscard]] State state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t commandGeneration() const noexcept
    {
        return commandGeneration_.load(std::memory_order_acquire);
    }

    double positionSeconds() const noexcept
    {
        auto frames = positionFrames_.load(std::memory_order_acquire);
        const auto clock = positionClockQpc_.load(std::memory_order_acquire);
        if (clock != 0 && playing()) {
            LARGE_INTEGER now{};
            LARGE_INTEGER frequency{};
            if (QueryPerformanceCounter(&now) &&
                    QueryPerformanceFrequency(&frequency) &&
                    frequency.QuadPart > 0 &&
                    now.QuadPart >= static_cast<LONGLONG>(clock)) {
                const auto elapsed = static_cast<long double>(
                    now.QuadPart - static_cast<LONGLONG>(clock)) /
                    static_cast<long double>(frequency.QuadPart);
                const auto interpolated = frames +
                    static_cast<std::uint64_t>(elapsed * kProjectSampleRate);
                frames = (std::min)(interpolated,
                    audibleLimitFrames_.load(std::memory_order_acquire));
            }
        }
        return static_cast<double>(frames) / kProjectSampleRate;
    }

    void setMasterVolume(float gain, bool muted) noexcept
    {
        masterGain_.store(std::clamp(gain, 0.0f, 1.0f),
            std::memory_order_release);
        masterMuted_.store(muted, std::memory_order_release);
    }

    void setChannelMutes(bool left, bool right) noexcept
    {
        muteLeft_.store(left, std::memory_order_release);
        muteRight_.store(right, std::memory_order_release);
    }

    [[nodiscard]] float masterGain() const noexcept
    {
        return masterGain_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool masterMuted() const noexcept
    {
        return masterMuted_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::array<float, 2> peaks() const noexcept
    {
        return {
            peakLeft_.load(std::memory_order_acquire),
            peakRight_.load(std::memory_order_acquire),
        };
    }

    void seek(double seconds)
    {
        positionFrames_.store(static_cast<std::uint64_t>(std::max(
            0.0, seconds) * kProjectSampleRate), std::memory_order_release);
        positionClockQpc_.store(0, std::memory_order_release);
        audibleLimitFrames_.store(positionFrames_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

private:
    enum class CommandAction : std::uint8_t { play, pause, stop };

    struct Command {
        std::uint64_t generation = 0;
        CommandAction action = CommandAction::stop;
        StopReason reason = StopReason::stop;
        bool preservePosition = false;
        std::uint64_t startFrame = 0;
        std::vector<MixTrack> tracks;
    };

    struct SessionResult {
        bool eof = false;
        MMRESULT error = MMSYSERR_NOERROR;
    };

    std::uint64_t publishCommand(Command command)
    {
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(commandMutex_);
            generation = ++nextGeneration_;
            command.generation = generation;
            command_ = std::move(command);
            commandGeneration_.store(generation, std::memory_order_release);
        }
        commandChanged_.notify_all();
        return generation;
    }

    void postLifecycle(
        std::uint64_t generation, State state, MMRESULT result = MMSYSERR_NOERROR)
        const noexcept
    {
        const auto target = target_.load(std::memory_order_acquire);
        if (target == nullptr) return;
        const auto packed = static_cast<LPARAM>(
            (static_cast<std::uint32_t>(result) << 8U) |
            static_cast<std::uint32_t>(state));
        PostMessageW(target, kPlaybackLifecycleMessage,
            static_cast<WPARAM>(generation), packed);
    }

    void workerMain()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::uint64_t handledGeneration = 0;
        for (;;) {
            Command command;
            {
                std::unique_lock lock(commandMutex_);
                commandChanged_.wait(lock, [&] {
                    return shuttingDown_ || command_.generation != handledGeneration;
                });
                if (shuttingDown_) break;
                command = command_;
                handledGeneration = command.generation;
            }

            if (command.action != CommandAction::play) {
                if (!command.preservePosition) {
                    positionFrames_.store(0, std::memory_order_release);
                    audibleLimitFrames_.store(0, std::memory_order_release);
                }
                positionClockQpc_.store(0, std::memory_order_release);
                peakLeft_.store(0.0f, std::memory_order_release);
                peakRight_.store(0.0f, std::memory_order_release);
                const auto finalState = command.action == CommandAction::pause
                    ? State::paused : State::idle;
                state_.store(finalState, std::memory_order_release);
                postLifecycle(command.generation, finalState);
                continue;
            }

            positionFrames_.store(command.startFrame, std::memory_order_release);
            audibleLimitFrames_.store(command.startFrame,
                std::memory_order_release);
            positionClockQpc_.store(0, std::memory_order_release);
            const auto result = runSession(command);
            const auto latest = commandGeneration_.load(std::memory_order_acquire);
            if (latest != command.generation) continue;

            const auto finalState = result.error != MMSYSERR_NOERROR
                ? State::error : (result.eof ? State::eof : State::idle);
            state_.store(finalState, std::memory_order_release);
            postLifecycle(command.generation, finalState, result.error);
        }
        peakLeft_.store(0.0f, std::memory_order_release);
        peakRight_.store(0.0f, std::memory_order_release);
        state_.store(State::idle, std::memory_order_release);
    }

    SessionResult runSession(const Command& command)
    {
        SessionResult result{};
        masterProcessor_.reset();
        const auto prepare = masterProcessor_.prewarm(
            kProjectSampleRate, 2);
        bool masterDryFallback = false;
        if (prepare.state == goden_audio::MasterDspPrepareState::pending &&
                !masterProcessor_.waitUntilPrepared(
                    std::chrono::milliseconds(2000))) {
            masterDryFallback = true;
            MasterDspController::instance().publishFault(
                "DAW DSP preparation timed out; this session is playing dry.");
        }
        if (masterProcessor_.prepareTicket().state ==
                goden_audio::MasterDspPrepareState::fault) {
            masterDryFallback = true;
            MasterDspController::instance().publishFault(
                "DAW DSP could not be prepared; this session is playing dry.");
        }
        auto& output = DawOutputBackend::instance();
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = kProjectSampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            result.error = MMSYSERR_NOMEM;
            return result;
        }
        HWAVEOUT wave = nullptr;
        result.error = output.open(&wave, &format, event);
        if (result.error != MMSYSERR_NOERROR) {
            CloseHandle(event);
            return result;
        }
        const auto bridgeToken = DawVisualBridge::instance().begin();
        state_.store(State::playing, std::memory_order_release);
        postLifecycle(command.generation, State::playing);

        struct OutputBuffer {
            std::array<float, kOutputBufferFrames * 2U> samples{};
            std::array<float, kOutputBufferFrames * 2U> visualSamples{};
            WAVEHDR header{};
            std::uint64_t startFrame = 0;
            std::uint64_t endFrame = 0;
            std::uint64_t audibleStartFrame = 0;
            std::uint64_t audibleEndFrame = 0;
            std::uint32_t frameCount = 0;
            float peakLeft = 0.0f;
            float peakRight = 0.0f;
            std::uint32_t rackLatencyFrames = 0;
            bool queued = false;
            bool prepared = false;
        };
        std::array<OutputBuffer, 3> buffers{};
        bool prepareFailed = false;
        for (auto& buffer : buffers) {
            buffer.header.lpData = reinterpret_cast<LPSTR>(
                buffer.samples.data());
            buffer.header.dwBufferLength = static_cast<DWORD>(
                buffer.samples.size() * sizeof(float));
            result.error = output.prepare(wave, &buffer.header);
            if (result.error != MMSYSERR_NOERROR) {
                prepareFailed = true;
                break;
            }
            buffer.prepared = true;
        }

        auto submitCursor = positionFrames_.load(std::memory_order_acquire);
        std::uint64_t maximumEnd = 0;
        for (const auto& track : command.tracks) {
            if (!track.audio) continue;
            const auto sourceEnd = std::min<std::uint64_t>(
                track.sourceEndFrame, track.audio->frameCount());
            const auto sourceLength = sourceEnd > track.sourceStartFrame
                ? sourceEnd - track.sourceStartFrame : 0U;
            maximumEnd = std::max(maximumEnd,
                static_cast<std::uint64_t>(std::max<std::int64_t>(0,
                    track.offsetFrames)) + sourceLength);
        }
        const auto inputEnd = maximumEnd;
        std::uint64_t quietTailFrames = 0;
        bool tailFinished = false;
        const auto fill = [&](OutputBuffer& buffer) {
            const auto declaredTail = std::min<std::uint64_t>(
                masterProcessor_.tailSamples(),
                static_cast<std::uint64_t>(kProjectSampleRate) * 60U);
            maximumEnd = std::max(maximumEnd, inputEnd + declaredTail);
            if (tailFinished || submitCursor >= maximumEnd) return false;
            const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(
                kOutputBufferFrames, maximumEnd - submitCursor));
            const bool warmViews=(submitCursor-command.startFrame) %
                (static_cast<std::uint64_t>(kProjectSampleRate)*2U)<kOutputBufferFrames;
            std::fill(buffer.samples.begin(), buffer.samples.end(), 0.0f);
            for (const auto& track : command.tracks) {
                if (track.muted || !track.audio) continue;
                const auto localStart=static_cast<std::int64_t>(submitCursor)-track.offsetFrames;
                if(localStart+static_cast<std::int64_t>(frames)<=0)continue;
                // Prepare immediately before this track's reads. Two MixTracks
                // may refer to the same PcmClip at different source offsets.
                track.audio->prefetch(static_cast<std::size_t>(track.sourceStartFrame+
                    static_cast<std::uint64_t>(std::max<std::int64_t>(localStart,0))),
                    warmViews?static_cast<std::size_t>(kProjectSampleRate)*6U:frames,warmViews);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const auto timelineFrame = static_cast<std::int64_t>(
                        submitCursor + frame);
                    const auto local = timelineFrame - track.offsetFrames;
                    const auto sourceFrame = local >= 0
                        ? track.sourceStartFrame +
                            static_cast<std::uint64_t>(local)
                        : 0U;
                    const auto sourceEnd = std::min<std::uint64_t>(
                        track.sourceEndFrame, track.audio->frameCount());
                    if (local < 0 || sourceFrame >= sourceEnd) {
                        continue;
                    }
                    const auto source = static_cast<std::size_t>(
                        sourceFrame) * 2U;
                    buffer.samples[frame * 2U] += track.audio->sample(source);
                    buffer.samples[frame * 2U + 1U] +=
                        track.audio->sample(source + 1U);
                }
            }
            if (!masterDryFallback) {
                (void)masterProcessor_.process(buffer.samples.data(), frames, 2,
                    kProjectSampleRate);
                buffer.rackLatencyFrames = masterProcessor_.latencySamples();
            } else {
                buffer.rackLatencyFrames = 0;
            }
            std::copy_n(buffer.samples.data(), frames * 2U,
                buffer.visualSamples.data());
            if (submitCursor >= inputEnd) {
                float tailPeak = 0.0f;
                for (std::size_t index = 0; index < frames * 2U; ++index) {
                    tailPeak = std::max(tailPeak,
                        std::abs(buffer.visualSamples[index]));
                }
                quietTailFrames = tailPeak < 1.0e-6f
                    ? quietTailFrames + frames : 0U;
                if (quietTailFrames >=
                        static_cast<std::uint64_t>(kProjectSampleRate) / 2U) {
                    tailFinished = true;
                }
            }
            const auto muteLeft = muteLeft_.load(std::memory_order_acquire);
            const auto muteRight = muteRight_.load(std::memory_order_acquire);
            const auto masterMuted = masterMuted_.load(
                std::memory_order_acquire);
            const auto masterGain = masterMuted ? 0.0f :
                masterGain_.load(std::memory_order_acquire);
            buffer.peakLeft = 0.0f;
            buffer.peakRight = 0.0f;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto left = buffer.samples[frame * 2U];
                const auto right = buffer.samples[frame * 2U + 1U];
                const auto outputLeft = muteLeft ? 0.0f :
                    left * masterGain;
                const auto outputRight = muteRight ? 0.0f :
                    right * masterGain;
                buffer.samples[frame * 2U] = outputLeft;
                buffer.samples[frame * 2U + 1U] = outputRight;
                buffer.peakLeft = std::max(
                    buffer.peakLeft, std::abs(outputLeft));
                buffer.peakRight = std::max(
                    buffer.peakRight, std::abs(outputRight));
            }
            buffer.startFrame = submitCursor;
            buffer.endFrame = submitCursor + frames;
            const auto relativeStart = submitCursor >= command.startFrame
                ? submitCursor - command.startFrame : 0U;
            const auto relativeEnd = relativeStart + frames;
            buffer.audibleStartFrame = command.startFrame +
                (relativeStart > buffer.rackLatencyFrames
                    ? relativeStart - buffer.rackLatencyFrames : 0U);
            buffer.audibleEndFrame = command.startFrame +
                (relativeEnd > buffer.rackLatencyFrames
                    ? relativeEnd - buffer.rackLatencyFrames : 0U);
            buffer.frameCount = static_cast<std::uint32_t>(frames);
            buffer.header.dwBufferLength = static_cast<DWORD>(
                frames * 2U * sizeof(float));
            buffer.header.dwFlags &= ~WHDR_DONE;
            submitCursor += frames;
            return true;
        };
        MMRESULT writeError = MMSYSERR_NOERROR;
        const auto submit = [&](OutputBuffer& buffer) {
            if (!buffer.prepared) return false;
            if (!fill(buffer)) return false;
            writeError = output.write(wave, &buffer.header);
            if (writeError != MMSYSERR_NOERROR) {
                return false;
            }
            buffer.queued = true;
            return true;
        };

        std::size_t head = 0;
        std::size_t queued = 0;
        if (!prepareFailed) {
            while (queued < buffers.size() && submit(buffers[queued])) ++queued;
        }
        if (writeError != MMSYSERR_NOERROR) result.error = writeError;
        if (queued != 0) {
            const auto& audible = buffers[head];
            audibleLimitFrames_.store(audible.audibleEndFrame,
                std::memory_order_release);
            peakLeft_.store(audible.peakLeft, std::memory_order_release);
            peakRight_.store(audible.peakRight, std::memory_order_release);
            DawVisualBridge::instance().publish(bridgeToken,
                std::span<const float>(audible.visualSamples.data(),
                    static_cast<std::size_t>(audible.frameCount) * 2U),
                audible.frameCount, kProjectSampleRate,
                audible.audibleStartFrame);
        }

        auto lastOutputProgress = std::chrono::steady_clock::now();
        while (result.error == MMSYSERR_NOERROR &&
                commandGeneration_.load(std::memory_order_acquire) ==
                    command.generation && queued != 0) {
            WaitForSingleObject(event, 10);
            if (std::chrono::steady_clock::now() - lastOutputProgress >
                    std::chrono::seconds(2)) {
                result.error = MMSYSERR_ERROR;
                MasterDspController::instance().publishFault(
                    "DAW audio device stopped responding; output was stopped safely.");
                break;
            }
            while (commandGeneration_.load(std::memory_order_acquire) ==
                        command.generation &&
                    queued != 0 &&
                    output.completed(&buffers[head].header)) {
                auto& completed = buffers[head];
                lastOutputProgress = std::chrono::steady_clock::now();
                completed.queued = false;
                positionFrames_.store(
                    completed.audibleEndFrame, std::memory_order_release);
                LARGE_INTEGER now{};
                if (QueryPerformanceCounter(&now)) {
                    positionClockQpc_.store(
                        static_cast<std::uint64_t>(now.QuadPart),
                        std::memory_order_release);
                }
                head = (head + 1U) % buffers.size();
                --queued;

                if (queued != 0) {
                    const auto& audible = buffers[head];
                    audibleLimitFrames_.store(
                        audible.audibleEndFrame, std::memory_order_release);
                    peakLeft_.store(
                        audible.peakLeft, std::memory_order_release);
                    peakRight_.store(
                        audible.peakRight, std::memory_order_release);
                    DawVisualBridge::instance().publish(bridgeToken,
                        std::span<const float>(audible.visualSamples.data(),
                            static_cast<std::size_t>(
                                audible.frameCount) * 2U),
                        audible.frameCount, kProjectSampleRate,
                        audible.audibleStartFrame);
                } else {
                    audibleLimitFrames_.store(
                        completed.audibleEndFrame, std::memory_order_release);
                }

                const auto tail = (head + queued) % buffers.size();
                if (submit(buffers[tail])) {
                    ++queued;
                } else if (writeError != MMSYSERR_NOERROR) {
                    result.error = writeError;
                }
            }
        }
        const auto resetResult = output.reset(wave);
        if (result.error == MMSYSERR_NOERROR &&
                resetResult != MMSYSERR_NOERROR) {
            result.error = resetResult;
        }
        for (auto& buffer : buffers) {
            if (buffer.prepared &&
                    (buffer.header.dwFlags & WHDR_PREPARED) != 0) {
                const auto unprepareResult = output.unprepare(
                    wave, &buffer.header);
                if (result.error == MMSYSERR_NOERROR &&
                        unprepareResult != MMSYSERR_NOERROR) {
                    result.error = unprepareResult;
                }
            }
        }
        const auto closeResult = output.close(wave);
        if (result.error == MMSYSERR_NOERROR &&
                closeResult != MMSYSERR_NOERROR) {
            result.error = closeResult;
        }
        CloseHandle(event);
        masterProcessor_.flush();
        positionClockQpc_.store(0, std::memory_order_release);
        peakLeft_.store(0.0f, std::memory_order_release);
        peakRight_.store(0.0f, std::memory_order_release);
        DawVisualBridge::instance().end(bridgeToken);
        result.eof = result.error == MMSYSERR_NOERROR && queued == 0 &&
            commandGeneration_.load(std::memory_order_acquire) ==
                command.generation;
        return result;
    }

    mutable std::mutex commandMutex_;
    std::condition_variable commandChanged_;
    Command command_{};
    bool shuttingDown_ = false;
    std::uint64_t nextGeneration_ = 0;
    std::atomic<std::uint64_t> commandGeneration_{0};
    std::thread worker_;
    std::atomic<State> state_{State::idle};
    std::atomic<HWND> target_{nullptr};
    std::atomic<std::uint64_t> positionFrames_{0};
    std::atomic<std::uint64_t> audibleLimitFrames_{0};
    std::atomic<std::uint64_t> positionClockQpc_{0};
    std::atomic<bool> muteLeft_{false};
    std::atomic<bool> muteRight_{false};
    std::atomic<float> masterGain_{1.0f};
    std::atomic<bool> masterMuted_{false};
    std::atomic<float> peakLeft_{0.0f};
    std::atomic<float> peakRight_{0.0f};
    goden_audio::MasterDspProcessor masterProcessor_{
        goden_audio::MasterDspDomain::daw};
};

} // namespace

struct DawWindow::Impl {
    struct Track {
        std::uint64_t id = 0;
        metadb_handle_ptr handle;
        std::wstring name;
        double duration = 0.0;
        double offset = 0.0;
        double sourceStart = 0.0;
        double sourceEnd = 0.0;
        double progress = 0.0;
        bool muted = false;
        bool solo = false;
        bool failed = false;
        std::shared_ptr<const WaveformPyramidV1> waveform;
        std::shared_ptr<PcmClip> audio;
    };

    HWND window = nullptr;
    HWND owner = nullptr;
    PanelD2D render;
    bool staticSceneReady = false;
    std::uint64_t staticThemeGeneration = 0;
    std::uint64_t staticScenePaints = 0;
    std::uint64_t cachedScenePaints = 0;
    WindowFrameClock frameClock;
    std::vector<Track> tracks;
    PreviewMixer mixer;
    std::jthread decoder;
    std::mutex decodeMutex;
    std::condition_variable_any decodeChanged;
    std::vector<std::uint64_t> decodeQueue;
    std::shared_ptr<abort_callback_impl> activeAbort;
    std::uint64_t activeDecodeTrackId = 0;
    std::uint64_t nextTrackId = 1;
    std::atomic<std::uint64_t> generation{1};
    double pixelsPerSecond = 90.0;
    double viewStart = 0.0;
    double pausedPosition = 0.0;
    float verticalScroll = 0.0f;
    int selected = -1;
    enum class ClipEditMode : std::uint8_t {
        none,
        move,
        trimStart,
        trimEnd,
    };
    ClipEditMode clipEditMode = ClipEditMode::none;
    bool resumeAfterClipEdit = false;
    bool resumePlaybackAfterPreview = false;
    bool monitorMuteLeft = false;
    bool monitorMuteRight = false;
    std::array<float, 2> meterLevel{};
    std::array<float, 2> meterHold{};
    std::array<std::chrono::steady_clock::time_point, 2> meterHoldUntil{};
    std::chrono::steady_clock::time_point meterSampleTime{};
    POINT dragOrigin{};
    double dragOffset = 0.0;
    double dragSourceStart = 0.0;
    double dragSourceEnd = 0.0;
    bool closing = false;
    bool decoderStopped = false;
    bool mixerStopped = false;
    std::uint64_t closeMixerGeneration = 0;
    std::uint64_t resetPositionGeneration = 0;
    std::uint64_t lastLifecycleGeneration = 0;
    MMRESULT lastLifecycleResult = MMSYSERR_NOERROR;

    static LRESULT CALLBACK windowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(
                window, GWLP_USERDATA));
        }
        return self != nullptr
            ? self->handleMessage(window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    void show(HWND requestedOwner)
    {
        if (!rolloutFlagEnabled(RolloutFlag::dawAsyncLifecycleV2)) return;
        if (window != nullptr) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
        owner = requestedOwner != nullptr
            ? GetAncestor(requestedOwner, GA_ROOT)
            : static_cast<HWND>(core_api::get_main_window());
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &Impl::windowProc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        const auto dpiValue = owner != nullptr
            ? (std::max)(96U, GetDpiForWindow(owner)) : 96U;
        const auto width = MulDiv(1180, dpiValue, 96);
        const auto height = MulDiv(720, dpiValue, 96);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const auto monitor = MonitorFromWindow(
            owner, MONITOR_DEFAULTTONEAREST);
        RECT work{};
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        else
            work = monitorInfo.rcWork;
        const auto x = work.left + (work.right - work.left - width) / 2;
        const auto y = work.top + (work.bottom - work.top - height) / 2;
        // This is a persistent tool surface with its own taskbar entry. Keep
        // the main window only as a placement/DPI anchor; assigning it as an
        // owner couples minimize/activation when this window closes.
        window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_ACCEPTFILES,
            kWindowClass, L"GODENplayer DAW Mode - Temporary preview (clears on close)",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, nullptr, nullptr,
            core_api::get_my_instance(), this);
        if (window == nullptr) return;
        closing = false;
        decoderStopped = false;
        mixerStopped = false;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const COLORREF caption = kWindowBackground;
        const COLORREF captionText = kWindowText;
        const COLORREF border = kWindowBorder;
        DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR,
            &caption, sizeof(caption));
        DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR,
            &captionText, sizeof(captionText));
        DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR,
            &border, sizeof(border));
        if (dawQaBackgroundRequested()) {
            ShowWindow(window, SW_HIDE);
        } else {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
        }
    }

    void close()
    {
        if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
    }

    void shutdown()
    {
        clearDawPresence();
        closing = true;
        generation.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard lock(decodeMutex);
            if (activeAbort) activeAbort->abort();
            if (decoder.joinable()) decoder.request_stop();
            decodeChanged.notify_all();
        }
        if (decoder.joinable()) decoder.join();
        decoderStopped = true;
        // The decoder can post its final payload while shutdown is joining it.
        // DestroyWindow discards queued messages without deleting their payloads.
        drainDecodeMessages();
        mixer.setTarget(nullptr);
        mixer.shutdown();
        mixerStopped = true;
        if (window != nullptr) {
            frameClock.detach();
            DragAcceptFiles(window, FALSE);
            if (GetCapture() == window) ReleaseCapture();
            activeAbort.reset();
            decodeQueue.clear();
            tracks.clear();
            render.reset();
            DestroyWindow(window);
        }
    }

    void startDecoder()
    {
        if (decoder.joinable()) return;
        const auto currentGeneration = generation.load(std::memory_order_acquire);
        decoder = std::jthread([this, currentGeneration](std::stop_token token) {
            decodeMain(token, currentGeneration);
        });
    }

    void beginClose()
    {
        if (closing) return;
        closing = true;
        clearDawPresence();
        clipEditMode = ClipEditMode::none;
        resumeAfterClipEdit = false;
        if (GetCapture() == window) ReleaseCapture();
        frameClock.stop();
        DragAcceptFiles(window, FALSE);
        generation.fetch_add(1, std::memory_order_acq_rel);

        {
            std::lock_guard lock(decodeMutex);
            if (activeAbort) activeAbort->abort();
            if (decoder.joinable()) decoder.request_stop();
            decodeChanged.notify_all();
        }
        decoderStopped = !decoder.joinable();
        closeMixerGeneration = mixer.requestStop(
            PreviewMixer::StopReason::close, false);
        mixerStopped = false;
        invalidateScene();
        finishCloseIfReady();
    }

    void drainDecodeMessages()
    {
        if (window != nullptr) {
            MSG pending{};
            while (PeekMessageW(&pending, window, kDecodeMessage,
                    kDecodeMessage, PM_REMOVE)) {
                delete reinterpret_cast<DecodeMessage*>(pending.lParam);
            }
        }
    }

    void finishCloseIfReady()
    {
        if (!closing || !decoderStopped || !mixerStopped || window == nullptr)
            return;
        drainDecodeMessages();
        activeAbort.reset();
        decodeQueue.clear();
        tracks.clear();
        selected = -1;
        resetMeters();
        restoreNormalPlayback();
        render.reset();
        DestroyWindow(window);
    }

    void postDecode(std::unique_ptr<DecodeMessage> message)
    {
        const auto target = window;
        if (target == nullptr || !PostMessageW(target, kDecodeMessage, 0,
                reinterpret_cast<LPARAM>(message.get()))) {
            return;
        }
        message.release();
    }

    void decodeMain(std::stop_token token, std::uint64_t workerGeneration)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::uint64_t trackId = 0;
            metadb_handle_ptr handle;
            std::shared_ptr<abort_callback_impl> aborter;
            {
                std::unique_lock lock(decodeMutex);
                decodeChanged.wait(lock, token, [&] {
                    return !decodeQueue.empty();
                });
                if (token.stop_requested()) break;
                trackId = decodeQueue.front();
                decodeQueue.erase(decodeQueue.begin());
                const auto track = std::find_if(
                    tracks.begin(), tracks.end(), [&](const Track& item) {
                        return item.id == trackId;
                    });
                if (track == tracks.end()) continue;
                handle = track->handle;
                aborter = std::make_shared<abort_callback_impl>();
                activeAbort = aborter;
                activeDecodeTrackId = trackId;
            }
            try {
                decodeTrack(trackId, handle, aborter, workerGeneration, token);
            } catch (const exception_aborted&) {
                if (token.stop_requested()) break;
            } catch (const std::exception& error) {
                auto result = std::make_unique<DecodeMessage>();
                result->generation = workerGeneration;
                result->trackId = trackId;
                result->final = true;
                result->error = error.what();
                postDecode(std::move(result));
            } catch (...) {
                auto result = std::make_unique<DecodeMessage>();
                result->generation = workerGeneration;
                result->trackId = trackId;
                result->final = true;
                result->error = "decode failed";
                postDecode(std::move(result));
            }
            std::lock_guard lock(decodeMutex);
            activeAbort.reset();
            activeDecodeTrackId = 0;
        }
        const auto target = window;
        if (target != nullptr) {
            PostMessageW(target, kDecoderStoppedMessage,
                static_cast<WPARAM>(workerGeneration), 0);
        }
    }

    void decodeTrack(std::uint64_t trackId, const metadb_handle_ptr& handle,
        const std::shared_ptr<abort_callback_impl>& aborter,
        std::uint64_t workerGeneration, std::stop_token token)
    {
        service_ptr_t<input_decoder> input;
        service_ptr_t<file> hint;
        input_entry::g_open_for_decoding(input, hint,
            handle->get_path(), *aborter);
        input->initialize(handle->get_subsong_index(),
            input_flag_no_seeking | input_flag_no_looping, *aborter);
        audio_chunk_impl chunk;
        std::vector<float> decoded;
        SpillFileGuard spillGuard;
        std::ofstream spill;
        std::size_t decodedSampleCount = 0;
        const auto decodeStarted = std::chrono::steady_clock::now();
        std::uint32_t sourceRate = 0;
        std::uint64_t totalFrames = 0;
        std::unique_ptr<sonare::streaming_detail::StreamResampler>
            leftResampler;
        std::unique_ptr<sonare::streaming_detail::StreamResampler>
            rightResampler;
        bool previewOverflow = false;
        const auto duration = handle->get_length();
        std::unique_ptr<WaveformPyramidBuilder> builder;
        auto lastPublish = std::chrono::steady_clock::now();
        while (!token.stop_requested() && input->run(chunk, *aborter)) {
            aborter->check();
            const auto frames = static_cast<std::size_t>(chunk.get_sample_count());
            const auto channels = chunk.get_channel_count();
            const auto rate = chunk.get_sample_rate();
            if (frames == 0 || channels == 0 || rate == 0) continue;
            if (sourceRate == 0) sourceRate = rate;
            if (sourceRate != rate) throw std::runtime_error(
                "sample-rate change inside file is not supported");
            if (!builder) {
                builder = std::make_unique<WaveformPyramidBuilder>(
                    workerGeneration,
                    duration > 0.0 ? static_cast<std::uint64_t>(
                        duration * sourceRate) : 0);
                if (duration > 0.0) {
                    const auto estimatedFrames = static_cast<std::size_t>(
                        std::min<double>(duration * kProjectSampleRate,
                            kMaximumDecodedFramesPerTrack));
                    decoded.reserve(std::min(estimatedFrames * 2U,
                        kHotPcmBytesPerClip / sizeof(float)));
                }
            }
            const auto* data = chunk.get_data();
            std::vector<float> stereo(frames * 2U);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto base = frame * channels;
                const auto left = static_cast<float>(data[base]);
                const auto right = channels > 1
                    ? static_cast<float>(data[base + 1U]) : left;
                stereo[frame * 2U] = left;
                stereo[frame * 2U + 1U] = right;
            }
            std::string appendError;
            if (!builder->appendInterleaved(stereo, frames, 2, rate, appendError)) {
                throw std::runtime_error(appendError);
            }
            if (!previewOverflow) {
                std::vector<float> left(frames);
                std::vector<float> right(frames);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    left[frame] = stereo[frame * 2U];
                    right[frame] = stereo[frame * 2U + 1U];
                }
                std::vector<float> outputLeft;
                std::vector<float> outputRight;
                if (sourceRate == kProjectSampleRate) {
                    outputLeft = std::move(left);
                    outputRight = std::move(right);
                } else {
                    if (!leftResampler) {
                        leftResampler = std::make_unique<
                            sonare::streaming_detail::StreamResampler>(
                                static_cast<int>(sourceRate),
                                static_cast<int>(kProjectSampleRate));
                        rightResampler = std::make_unique<
                            sonare::streaming_detail::StreamResampler>(
                                static_cast<int>(sourceRate),
                                static_cast<int>(kProjectSampleRate));
                    }
                    leftResampler->process(left.data(), left.size(), outputLeft);
                    rightResampler->process(right.data(), right.size(), outputRight);
                }
                const auto outputFrames = std::min(
                    outputLeft.size(), outputRight.size());
                if (decodedSampleCount / 2U + outputFrames >
                        kMaximumDecodedFramesPerTrack) {
                    previewOverflow = true;
                    decoded.clear();
                    decoded.shrink_to_fit();
                } else {
                    std::vector<float> output(outputFrames * 2U);
                    for (std::size_t frame = 0; frame < outputFrames; ++frame) {
                        output[frame * 2U] = outputLeft[frame];
                        output[frame * 2U + 1U] =
                            outputRight[frame];
                    }
                    const auto shouldSpill = spill.is_open() ||
                        (decoded.size() + output.size()) * sizeof(float) >
                            kHotPcmBytesPerClip ||
                        std::chrono::steady_clock::now() - decodeStarted >= 2s;
                    if (shouldSpill && !spill.is_open()) {
                        const auto native = filesystem::g_get_native_path(
                            core_api::get_profile_path());
                        const auto directory = std::filesystem::path(
                            utf8ToWide(native.c_str())) / L"nekorox" / L"daw-cache";
                        std::error_code ioError;
                        std::filesystem::create_directories(directory, ioError);
                        if (ioError) throw std::runtime_error(
                            "could not create DAW PCM spill cache");
                        spillGuard.path = directory /
                            (L"preview-" + std::to_wstring(GetCurrentProcessId()) +
                             L"-" + std::to_wstring(workerGeneration) + L"-" +
                             std::to_wstring(trackId) + L"-" +
                             std::to_wstring(GetTickCount64()) + L".f32");
                        spill.open(spillGuard.path,
                            std::ios::binary | std::ios::trunc);
                        if (!spill) throw std::runtime_error(
                            "could not open DAW PCM spill cache");
                        spill.write(reinterpret_cast<const char*>(decoded.data()),
                            static_cast<std::streamsize>(
                                decoded.size() * sizeof(float)));
                    }
                    if (spill.is_open()) {
                        spill.write(reinterpret_cast<const char*>(output.data()),
                            static_cast<std::streamsize>(
                                output.size() * sizeof(float)));
                        if (!spill) throw std::runtime_error(
                            "could not write DAW PCM spill cache");
                    } else {
                        decoded.insert(decoded.end(), output.begin(), output.end());
                    }
                    decodedSampleCount += output.size();
                }
            }
            totalFrames += frames;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPublish >= 100ms) {
                auto result = std::make_unique<DecodeMessage>();
                result->generation = workerGeneration;
                result->trackId = trackId;
                result->waveform = std::make_shared<WaveformPyramidV1>(
                    builder->snapshot(false));
                result->progress = duration > 0.0
                    ? std::clamp(static_cast<double>(totalFrames) /
                        (duration * sourceRate), 0.0, 0.99) : 0.0;
                postDecode(std::move(result));
                lastPublish = now;
            }
        }
        aborter->check();
        if (!builder) throw std::runtime_error("decoder produced no PCM");
        if (!previewOverflow && leftResampler && rightResampler) {
            std::vector<float> outputLeft;
            std::vector<float> outputRight;
            leftResampler->flush(outputLeft);
            rightResampler->flush(outputRight);
            const auto outputFrames = std::min(
                outputLeft.size(), outputRight.size());
            if (decodedSampleCount / 2U + outputFrames >
                    kMaximumDecodedFramesPerTrack) {
                previewOverflow = true;
                decoded.clear();
                decoded.shrink_to_fit();
            } else if (outputFrames != 0) {
                std::vector<float> output(outputFrames * 2U);
                for (std::size_t frame = 0; frame < outputFrames; ++frame) {
                    output[frame * 2U] = outputLeft[frame];
                    output[frame * 2U + 1U] = outputRight[frame];
                }
                if (spill.is_open()) {
                    spill.write(reinterpret_cast<const char*>(output.data()),
                        static_cast<std::streamsize>(output.size() * sizeof(float)));
                    if (!spill) throw std::runtime_error(
                        "could not flush the DAW resampler cache");
                } else {
                    decoded.insert(decoded.end(), output.begin(), output.end());
                }
                decodedSampleCount += output.size();
            }
        }
        auto waveform = std::make_shared<WaveformPyramidV1>(builder->finalize());
        std::shared_ptr<PcmClip> audio;
        if (!previewOverflow && decodedSampleCount != 0) {
            decoded.shrink_to_fit();
            if (spill.is_open()) {
                spill.close();
                audio = PcmClip::fromSpill(std::move(decoded),
                    spillGuard.path, decodedSampleCount);
                if (audio) spillGuard.handedOff = true;
            } else {
                audio = PcmClip::fromHot(std::move(decoded));
            }
        }
        auto result = std::make_unique<DecodeMessage>();
        result->generation = workerGeneration;
        result->trackId = trackId;
        result->waveform = std::move(waveform);
        result->audio = std::move(audio);
        result->progress = 1.0;
        result->final = true;
        if (!result->audio) {
            result->error = "waveform complete; preview exceeds the six-hour safety guard";
        }
        postDecode(std::move(result));
    }

    void addResolved(const metadb_handle_list& items)
    {
        if (window == nullptr) return;
        std::size_t previousSize = 0;
        std::vector<std::uint64_t> addedTrackIds;
        {
            std::lock_guard lock(decodeMutex);
            previousSize = tracks.size();
            for (t_size index = 0; index < items.get_count(); ++index) {
                const auto handle = items[index];
                if (!handle.is_valid()) continue;
                const auto duplicate = std::any_of(tracks.begin(), tracks.end(),
                    [&](const Track& track) {
                        return track.handle.is_valid() &&
                            track.handle->get_subsong_index() ==
                                handle->get_subsong_index() &&
                            std::strcmp(track.handle->get_path(),
                                handle->get_path()) == 0;
                    });
                if (duplicate) continue;
                Track track;
                track.id = nextTrackId++;
                track.handle = handle;
                track.name = trackLabel(handle);
                track.duration = std::max(0.0, handle->get_length());
                track.sourceEnd = track.duration;
                addedTrackIds.push_back(track.id);
                tracks.push_back(std::move(track));
            }
            decodeQueue.insert(
                decodeQueue.end(), addedTrackIds.begin(), addedTrackIds.end());
        }
        decodeChanged.notify_all();
        if (previousSize == 0 && !tracks.empty()) fitTimeline();
        if (mixer.state() == PreviewMixer::State::paused) {
            publishDawPresence(PlaybackState::paused);
        }
        invalidateScene();
    }

    void resolvePaths(std::vector<std::string> paths)
    {
        if (paths.empty() || window == nullptr) return;
        pfc::list_t<const char*> input;
        for (const auto& path : paths) input.add_item(path.c_str());
        auto notify = process_locations_notify::create(
            [this](metadb_handle_list_cref items) {
                auto owned = std::make_shared<metadb_handle_list>();
                owned->add_items(items);
                fb2k::inMainThread([this, owned] { addResolved(*owned); });
            });
        playlist_incoming_item_filter_v2::get()->process_locations_async(
            input,
            playlist_incoming_item_filter_v2::op_flag_delay_ui |
                playlist_incoming_item_filter_v2::op_flag_no_filter,
            nullptr, nullptr, window, std::move(notify));
    }

    void openPicker(bool folders)
    {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        DWORD options = 0;
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (folders) options |= FOS_PICKFOLDERS;
        else options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
        dialog->SetOptions(options);
        dialog->SetTitle(folders ? L"Add sample folder" : L"Add samples / stems");
        if (SUCCEEDED(dialog->Show(window))) {
            std::vector<std::string> paths;
            if (folders) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        paths.push_back(wideToUtf8(path));
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            } else {
                IShellItemArray* array = nullptr;
                if (SUCCEEDED(dialog->GetResults(&array))) {
                    DWORD count = 0;
                    array->GetCount(&count);
                    for (DWORD index = 0; index < count; ++index) {
                        IShellItem* item = nullptr;
                        if (FAILED(array->GetItemAt(index, &item))) continue;
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            paths.push_back(wideToUtf8(path));
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                    array->Release();
                }
            }
            resolvePaths(std::move(paths));
        }
        dialog->Release();
    }

    D2D1_RECT_F addRect() const noexcept { return D2D1::RectF(204, 9, 288, 39); }
    D2D1_RECT_F folderRect() const noexcept { return D2D1::RectF(294, 9, 394, 39); }
    D2D1_RECT_F playRect() const noexcept { return D2D1::RectF(410, 9, 484, 39); }
    D2D1_RECT_F stopRect() const noexcept { return D2D1::RectF(490, 9, 558, 39); }
    D2D1_RECT_F fitRect() const noexcept { return D2D1::RectF(574, 9, 632, 39); }
    D2D1_RECT_F minusRect() const noexcept { return D2D1::RectF(638, 9, 674, 39); }
    D2D1_RECT_F plusRect() const noexcept { return D2D1::RectF(680, 9, 716, 39); }
    D2D1_RECT_F muteLeftRect() const noexcept { return D2D1::RectF(728, 4, 794, 23); }
    D2D1_RECT_F muteRightRect() const noexcept { return D2D1::RectF(728, 25, 794, 44); }
    D2D1_RECT_F meterLeftRect(float width) const noexcept
    {
        return D2D1::RectF(800, 7, std::max(801.0f,
            std::min(936.0f, width - 12.0f)), 20);
    }
    D2D1_RECT_F meterRightRect(float width) const noexcept
    {
        return D2D1::RectF(800, 28, std::max(801.0f,
            std::min(936.0f, width - 12.0f)), 41);
    }

    static double clipDuration(const Track& track) noexcept
    {
        return std::max(0.0, track.sourceEnd - track.sourceStart);
    }

    static D2D1_RECT_F cutRectForRow(float top) noexcept
    {
        return D2D1::RectF(120.0f, top + 43.0f, 170.0f, top + 76.0f);
    }

    static D2D1_RECT_F removeRectForRow(float top) noexcept
    {
        return D2D1::RectF(
            kTimelineRailDip - 34.0f, top + 6.0f,
            kTimelineRailDip - 8.0f, top + 32.0f);
    }

    void clampVerticalScroll() noexcept
    {
        if (window == nullptr) {
            verticalScroll = 0.0f;
            return;
        }
        RECT client{};
        GetClientRect(window, &client);
        const auto maximum = std::max(0.0f,
            static_cast<float>(tracks.size()) * 84.0f -
            std::max(0.0f, clientDip(window, client.bottom) - 82.0f));
        verticalScroll = std::clamp(verticalScroll, 0.0f, maximum);
    }

    void removeStem(std::size_t index)
    {
        if (closing) return;
        if (index >= tracks.size()) return;
        const auto wasPlaying = mixer.playing();
        if (wasPlaying) {
            pausedPosition = mixer.positionSeconds();
            mixer.pause();
        }
        clipEditMode = ClipEditMode::none;
        resumeAfterClipEdit = false;
        if (GetCapture() == window) ReleaseCapture();

        const auto removedId = tracks[index].id;
        {
            std::lock_guard lock(decodeMutex);
            std::erase(decodeQueue, removedId);
            if (activeDecodeTrackId == removedId && activeAbort) {
                activeAbort->abort();
            }
            tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(index));
        }

        if (tracks.empty()) {
            selected = -1;
            pausedPosition = 0.0;
            resetMeters();
            frameClock.stop();
            clearDawPresence();
            restoreNormalPlayback();
        } else {
            selected = static_cast<int>(std::min<std::size_t>(
                index, tracks.size() - 1U));
            const auto hasReadyAudio = std::any_of(
                tracks.begin(), tracks.end(), [](const Track& track) {
                    return track.audio != nullptr;
                });
            if (wasPlaying && hasReadyAudio) togglePlay();
            else if (mixer.state() == PreviewMixer::State::paused) {
                publishDawPresence(PlaybackState::paused);
            }
        }
        clampVerticalScroll();
        invalidateScene();
    }

    bool splitSelectedAtPlayhead()
    {
        if (closing) return false;
        if (selected < 0 || selected >= static_cast<int>(tracks.size()))
            return false;
        const auto selectedIndex = static_cast<std::size_t>(selected);
        const auto playhead = currentPosition();
        double sourceAtPlayhead = 0.0;
        // A loading clip has only one decoder destination. Waiting until its
        // audio is ready keeps both halves backed by the same immutable PCM
        // and avoids duplicate decoding or a half that never finishes.
        constexpr double minimumClipSeconds = 0.010;
        {
            std::lock_guard lock(decodeMutex);
            if (selectedIndex >= tracks.size()) return false;
            const auto& original = tracks[selectedIndex];
            if (!original.audio ||
                    original.sourceEnd <= original.sourceStart) return false;
            sourceAtPlayhead = original.sourceStart +
                (playhead - original.offset);
            if (sourceAtPlayhead <=
                    original.sourceStart + minimumClipSeconds ||
                    sourceAtPlayhead >=
                        original.sourceEnd - minimumClipSeconds) return false;
        }

        const auto wasPlaying = mixer.playing();
        if (wasPlaying) {
            pausedPosition = mixer.positionSeconds();
            mixer.pause();
        }
        {
            std::lock_guard lock(decodeMutex);
            auto& original = tracks[selectedIndex];
            Track right = original;
            right.id = nextTrackId++;
            right.sourceStart = sourceAtPlayhead;
            right.offset = playhead;
            original.sourceEnd = sourceAtPlayhead;
            tracks.insert(tracks.begin() +
                static_cast<std::ptrdiff_t>(selectedIndex + 1U),
                std::move(right));
        }
        ++selected;
        if (wasPlaying) togglePlay();
        invalidateScene();
        return true;
    }

    void showStemMenu(std::size_t index, POINT screenPoint)
    {
        if (index >= tracks.size()) return;
        selected = static_cast<int>(index);
        const auto menu = CreatePopupMenu();
        if (menu == nullptr) return;
        const auto canSplit = tracks[index].audio != nullptr &&
            currentPosition() > tracks[index].offset + 0.010 &&
            currentPosition() < tracks[index].offset +
                clipDuration(tracks[index]) - 0.010;
        AppendMenuW(menu, MF_STRING | (canSplit ? 0U : MF_GRAYED),
            1U, L"Split at playhead");
        AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
        AppendMenuW(menu, MF_STRING, 2U, L"Remove stem");
        SetForegroundWindow(window);
        const auto command = TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x, screenPoint.y, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (command == 1U) splitSelectedAtPlayhead();
        else if (command == 2U) removeStem(index);
    }

    void finishClipEdit(bool resumePreview)
    {
        const auto shouldResume = resumeAfterClipEdit && resumePreview;
        clipEditMode = ClipEditMode::none;
        resumeAfterClipEdit = false;
        if (GetCapture() == window) ReleaseCapture();
        pausedPosition = std::clamp(
            pausedPosition, 0.0, timelineDuration());
        if (shouldResume && std::any_of(
                tracks.begin(), tracks.end(), [](const Track& track) {
                    return track.audio != nullptr;
                })) {
            togglePlay();
        }
    }

    double timelineDuration() const noexcept
    {
        double result = 1.0;
        for (const auto& track : tracks) {
            result = std::max(result, track.offset + clipDuration(track));
        }
        return result;
    }

    void fitTimeline()
    {
        if (window == nullptr) return;
        RECT client{};
        GetClientRect(window, &client);
        const auto widthDip = clientDip(window, client.right) -
            kTimelineRailDip - kTimelineRightPaddingDip;
        pixelsPerSecond = std::clamp(widthDip / timelineDuration(), 2.0, 800.0);
        viewStart = 0.0;
    }

    double currentPosition() const noexcept
    {
        return mixer.playing() ? mixer.positionSeconds() : pausedPosition;
    }

    void publishDawPresence(PlaybackState state)
    {
        DiscordDawPresence presence;
        presence.active = true;
        presence.state = state;
        presence.trackCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                tracks.size(),
                (std::numeric_limits<std::uint32_t>::max)()));
        presence.positionSeconds = currentPosition();
        presence.durationSeconds = timelineDuration();
        if (selected >= 0 && selected < static_cast<int>(tracks.size())) {
            presence.focus = wideToUtf8(tracks[selected].name);
        } else if (!tracks.empty()) {
            presence.focus = wideToUtf8(tracks.front().name);
        } else {
            presence.focus = "Mix preview";
        }
        DiscordPresenceService::instance().publishDawPreview(
            std::move(presence));
    }

    void clearDawPresence()
    {
        DiscordPresenceService::instance().clearDawPreview();
    }

    void syncMasterVolume()
    {
        const auto control = playback_control::get();
        auto decibels = control->get_volume();
        if (!std::isfinite(decibels)) decibels = -100.0f;
        decibels = std::clamp(decibels, -100.0f, 0.0f);
        const auto gain = std::pow(10.0f, decibels / 20.0f);
        mixer.setMasterVolume(gain, control->is_muted());
    }

    void resetMeters() noexcept
    {
        meterLevel.fill(0.0f);
        meterHold.fill(0.0f);
        meterHoldUntil = {};
        meterSampleTime = {};
    }

    void updateMeters()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = meterSampleTime.time_since_epoch().count() == 0
            ? 0.0f
            : std::clamp(std::chrono::duration<float>(
                now - meterSampleTime).count(), 0.0f, 0.1f);
        meterSampleTime = now;
        auto peaks = mixer.peaks();
        if (!mixer.playing() || mixer.masterMuted()) {
            peaks = {};
        }
        if (monitorMuteLeft) peaks[0] = 0.0f;
        if (monitorMuteRight) peaks[1] = 0.0f;

        constexpr auto holdDuration = 650ms;
        const auto levelRelease = std::pow(10.0f, -18.0f * elapsed / 20.0f);
        const auto holdRelease = std::pow(10.0f, -12.0f * elapsed / 20.0f);
        for (std::size_t channel = 0; channel < peaks.size(); ++channel) {
            if (!mixer.playing() || mixer.masterMuted() ||
                (channel == 0 ? monitorMuteLeft : monitorMuteRight)) {
                meterLevel[channel] = 0.0f;
                meterHold[channel] = 0.0f;
                meterHoldUntil[channel] = now;
                continue;
            }
            meterLevel[channel] = std::max(
                peaks[channel], meterLevel[channel] * levelRelease);
            if (peaks[channel] >= meterHold[channel]) {
                meterHold[channel] = peaks[channel];
                meterHoldUntil[channel] = now + holdDuration;
            } else if (now >= meterHoldUntil[channel]) {
                meterHold[channel] *= holdRelease;
            }
        }
    }

    void suspendNormalPlayback()
    {
        if (resumePlaybackAfterPreview) return;
        const auto control = playback_control::get();
        if (control->is_playing() && !control->is_paused()) {
            control->pause(true);
            resumePlaybackAfterPreview = true;
        }
    }

    void restoreNormalPlayback()
    {
        if (!resumePlaybackAfterPreview) return;
        resumePlaybackAfterPreview = false;
        const auto control = playback_control::get();
        if (control->is_playing() && control->is_paused()) {
            control->pause(false);
        }
    }

    void togglePlay()
    {
        if (closing) return;
        if (mixer.playing()) {
            pausedPosition = mixer.positionSeconds();
            mixer.pause();
            publishDawPresence(PlaybackState::paused);
            frameClock.stop();
            invalidateScene();
            return;
        }
        const bool anySolo = std::any_of(tracks.begin(), tracks.end(),
            [](const Track& track) { return track.solo; });
        std::vector<MixTrack> mix;
        for (const auto& track : tracks) {
            if (!track.audio) continue;
            const auto sourceStartFrame = static_cast<std::uint64_t>(
                std::llround(std::max(0.0, track.sourceStart) *
                    kProjectSampleRate));
            const auto sourceEndFrame = std::min<std::uint64_t>(
                track.audio->frameCount(),
                static_cast<std::uint64_t>(std::llround(
                    std::max(track.sourceStart, track.sourceEnd) *
                    kProjectSampleRate)));
            if (sourceEndFrame <= sourceStartFrame) continue;
            mix.push_back({track.audio,
                static_cast<std::int64_t>(std::llround(
                    track.offset * kProjectSampleRate)),
                sourceStartFrame,
                sourceEndFrame,
                track.muted || (anySolo && !track.solo)});
        }
        if (mix.empty()) {
            MessageBoxW(window,
                L"The audio preview is still decoding. Waveforms appear progressively.",
                L"GODENplayer DAW Mode", MB_OK | MB_ICONINFORMATION);
            return;
        }
        // Install the DAW override before pausing normal playback so its
        // callback cannot briefly republish the song as a paused activity.
        publishDawPresence(PlaybackState::playing);
        suspendNormalPlayback();
        syncMasterVolume();
        mixer.play(std::move(mix), pausedPosition,
            monitorMuteLeft, monitorMuteRight);
        frameClock.start();
        invalidateScene();
    }

    void restartPreviewForMixChange()
    {
        if (closing) return;
        if (!mixer.playing()) return;
        pausedPosition = mixer.positionSeconds();
        mixer.pause();
        togglePlay();
    }

    void stopPlayback()
    {
        if (closing) return;
        resetPositionGeneration = mixer.stop(false);
        resetMeters();
        frameClock.stop();
        invalidateScene();
    }

    void drawButton(D2D1_RECT_F rect, std::wstring_view text, bool active = false)
    {
        render.fill(rect, active ? PanelColor::selected : PanelColor::panel);
        render.line(rect.left, rect.top, rect.right, rect.top,
            active ? PanelColor::accent : PanelColor::border);
        render.line(rect.left, rect.bottom, rect.right, rect.bottom,
            active ? PanelColor::accent : PanelColor::border);
        render.text(text, rect, PanelText::center,
            active ? PanelColor::accent : PanelColor::text);
    }

    static float peakPosition(float amplitude) noexcept
    {
        const auto decibels = 20.0f * std::log10(std::max(amplitude, 0.001f));
        return std::clamp((decibels + 60.0f) / 60.0f, 0.0f, 1.0f);
    }

    void drawPeakMeter(
        const D2D1_RECT_F& bounds,
        float level,
        float hold)
    {
        render.fill(bounds, PanelColor::panel);
        render.line(bounds.left, bounds.top, bounds.right, bounds.top,
            PanelColor::border);
        render.line(bounds.left, bounds.bottom, bounds.right, bounds.bottom,
            PanelColor::border);
        render.line(bounds.left, bounds.top, bounds.left, bounds.bottom,
            PanelColor::border);
        render.line(bounds.right, bounds.top, bounds.right, bounds.bottom,
            PanelColor::border);

        const auto inner = D2D1::RectF(
            bounds.left + 1.0f, bounds.top + 1.0f,
            std::max(bounds.left + 1.0f, bounds.right - 1.0f),
            bounds.bottom - 1.0f);
        const auto width = std::max(0.0f, inner.right - inner.left);
        const auto levelX = inner.left + width * peakPosition(level);
        const auto minusTwelve = inner.left + width * 0.8f;
        const auto minusThree = inner.left + width * 0.95f;
        if (levelX > inner.left) {
            render.fill(D2D1::RectF(
                inner.left, inner.top,
                std::min(levelX, minusTwelve), inner.bottom),
                PanelColor::secondary, 0.92f);
        }
        if (levelX > minusTwelve) {
            render.fill(D2D1::RectF(
                minusTwelve, inner.top,
                std::min(levelX, minusThree), inner.bottom),
                PanelColor::accent, 0.96f);
        }
        if (levelX > minusThree) {
            render.fill(D2D1::RectF(
                minusThree, inner.top, levelX, inner.bottom),
                PanelColor::highlight);
        }
        render.line(minusTwelve, inner.top, minusTwelve, inner.bottom,
            PanelColor::border);
        render.line(minusThree, inner.top, minusThree, inner.bottom,
            PanelColor::border);
        if (hold > 0.001f) {
            const auto holdX = inner.left + width * peakPosition(hold);
            render.line(holdX, inner.top, holdX, inner.bottom,
                PanelColor::bright, 1.0f);
        }
    }

    void drawWaveform(const Track& track, const D2D1_RECT_F& bounds,
        bool selectedTrack)
    {
        auto* target = render.target();
        auto* brush = render.brush(selectedTrack
            ? PanelColor::accent : PanelColor::secondary);
        if (target == nullptr || brush == nullptr || !track.waveform) return;
        const auto secondsPerPixel = 1.0 / pixelsPerSecond;
        const WaveformLevelV1* level = &track.waveform->levels.front();
        for (const auto& candidate : track.waveform->levels) {
            if (candidate.bins.empty()) continue;
            level = &candidate;
            if (candidate.resolutionMs / 1000.0 >= secondsPerPixel) break;
        }
        if (level->bins.empty() || level->resolutionMs == 0) return;
        const auto center = (bounds.top + bounds.bottom) * 0.5f;
        const auto amplitude = (bounds.bottom - bounds.top) * 0.42f;
        const auto opacity = brush->GetOpacity();
        brush->SetOpacity(track.muted ? 0.25f : 0.82f);
        const auto firstX = std::max(bounds.left,
            static_cast<float>(timelineXForSeconds(
                track.offset, viewStart, pixelsPerSecond)));
        const auto lastX = std::min(bounds.right,
            static_cast<float>(timelineXForSeconds(
                track.offset + clipDuration(track),
                viewStart, pixelsPerSecond)));
        for (auto x = static_cast<int>(std::floor(firstX)); x <=
             static_cast<int>(std::ceil(lastX)); ++x) {
            const auto localStart = timelineSecondsAtX(
                x, viewStart, pixelsPerSecond) - track.offset +
                track.sourceStart;
            const auto localEnd = localStart + secondsPerPixel;
            const auto first = static_cast<std::size_t>(std::clamp(
                std::floor(localStart * 1000.0 / level->resolutionMs),
                0.0, static_cast<double>(level->bins.size() - 1U)));
            const auto last = static_cast<std::size_t>(std::clamp(
                std::ceil(localEnd * 1000.0 / level->resolutionMs),
                static_cast<double>(first + 1U),
                static_cast<double>(level->bins.size())));
            float peak = 0.0f;
            for (auto index = first; index < last; ++index) {
                peak = std::max(peak, level->bins[index].peak);
            }
            target->DrawLine(D2D1::Point2F(static_cast<float>(x), center - peak * amplitude),
                D2D1::Point2F(static_cast<float>(x), center + peak * amplitude),
                brush, selectedTrack ? 1.25f : 1.0f);
        }
        brush->SetOpacity(opacity);
    }

    void invalidateScene()
    {
        staticSceneReady = false;
        InvalidateRect(window, nullptr, FALSE);
    }

    void paint()
    {
        PAINTSTRUCT state{};
        BeginPaint(window, &state);
        const auto cached = staticSceneReady &&
            render.beginCachedScene(window, staticThemeGeneration);
        if (!cached && !render.begin(window, state.hdc)) {
            staticSceneReady = false;
            EndPaint(window, &state);
            return;
        }
        const auto size = render.sizeDip();
        updateMeters();
        if (!cached) {
            ++staticScenePaints;
            render.line(0, 48, size.width, 48, PanelColor::border);
            render.text(L"GODENplayer DAW MODE", D2D1::RectF(14, 4, 194, 44),
                PanelText::strong, PanelColor::accent);
            drawButton(addRect(), L"ADD FILES");
            drawButton(folderRect(), L"ADD FOLDER");
            drawButton(playRect(), mixer.playing() ? L"PAUSE" : L"PLAY",
                mixer.playing());
            drawButton(stopRect(), L"STOP");
            drawButton(fitRect(), L"FIT");
            drawButton(minusRect(), L"\u2212");
            drawButton(plusRect(), L"+");
            drawButton(muteLeftRect(), L"MUTE L", monitorMuteLeft);
            drawButton(muteRightRect(), L"MUTE R", monitorMuteRight);
            if (size.width >= 1080.0f) {
                render.text(L"48 kHz \u00b7 Ctrl+wheel zoom \u00b7 edges trim \u00b7 S split \u00b7 Del remove",
                    D2D1::RectF(948, 5, size.width - 12, 43),
                    PanelText::right, PanelColor::muted);
            }

            constexpr float rail = kTimelineRailDip;
            constexpr float rulerTop = 49.0f;
            constexpr float rulerBottom = 82.0f;
            constexpr float rowHeight = 84.0f;
            render.fill(D2D1::RectF(0, rulerTop, rail, size.height), PanelColor::panel);
            render.line(rail, rulerTop, rail, size.height, PanelColor::border);
            render.line(0, rulerBottom, size.width, rulerBottom, PanelColor::border);
            render.text(L"TRACKS / STEMS", D2D1::RectF(10, rulerTop, rail - 8, rulerBottom),
                PanelText::normal, PanelColor::muted);

            const auto visibleStart = viewStart;
            const auto visibleEnd = viewStart + std::max(0.0f,
                size.width - rail - 10.0f) / pixelsPerSecond;
            const auto coarse = pixelsPerSecond > 180 ? 1.0 :
                pixelsPerSecond > 45 ? 5.0 : pixelsPerSecond > 12 ? 15.0 : 60.0;
            for (auto second = std::floor(visibleStart / coarse) * coarse;
                 second <= visibleEnd; second += coarse) {
                const auto x = static_cast<float>(rail +
                    (second - viewStart) * pixelsPerSecond);
                render.line(x, rulerTop, x, size.height, PanelColor::border);
                // Keep the fixed transport clock at the right edge readable. The
                // grid line remains useful even when its label would overlap it.
                if (x + 82.0f < size.width - 112.0f) {
                    render.text(formatTime(second), D2D1::RectF(x + 4, rulerTop,
                        x + 82, rulerBottom), PanelText::caption,
                        PanelColor::muted);
                }
            }

            auto* target = render.target();
            if (target != nullptr) {
                target->PushAxisAlignedClip(
                    D2D1::RectF(0.0f, rulerBottom + 1.0f,
                        size.width, size.height),
                    D2D1_ANTIALIAS_MODE_ALIASED);
            }
            const auto firstVisibleRow = static_cast<std::size_t>(std::max(0.0,
                std::ceil(static_cast<double>(verticalScroll) / rowHeight) - 1.0));
            const auto endVisibleRow = std::min(tracks.size(),
                static_cast<std::size_t>(std::max(0.0,
                    std::floor((size.height - rulerBottom +
                        static_cast<double>(verticalScroll)) / rowHeight) + 1.0)));
            for (auto index = firstVisibleRow; index < endVisibleRow; ++index) {
                const auto top = rulerBottom + static_cast<float>(index) * rowHeight -
                    verticalScroll;
                const auto bottom = top + rowHeight;
                if (bottom < rulerBottom || top > size.height) continue;
                const auto isSelected = static_cast<int>(index) == selected;
                if (isSelected) render.fill(D2D1::RectF(0, top, size.width, bottom),
                    PanelColor::selected, 0.62f);
                render.line(0, bottom, size.width, bottom, PanelColor::border);
                render.text(std::to_wstring(index + 1U),
                    D2D1::RectF(10, top + 5, 35, bottom - 5),
                    PanelText::caption, PanelColor::muted);
                render.text(tracks[index].name,
                    D2D1::RectF(38, top + 5, rail - 40, top + 36),
                    PanelText::normal, isSelected ? PanelColor::text : PanelColor::muted);
                drawButton(removeRectForRow(top), L"\u00d7");
                const auto muteRect = D2D1::RectF(38, top + 43, 72, bottom - 8);
                const auto soloRect = D2D1::RectF(78, top + 43, 112, bottom - 8);
                drawButton(muteRect, L"M", tracks[index].muted);
                drawButton(soloRect, L"S", tracks[index].solo);
                drawButton(cutRectForRow(top), L"CUT", isSelected);
                const auto status = tracks[index].failed ? L"Waveform only" :
                    tracks[index].audio ? L"" :
                    (L"Loading audio \u00b7 " + std::to_wstring(static_cast<int>(
                        tracks[index].progress * 100.0)) + L"%");
                render.text(status, D2D1::RectF(174, top + 43, rail - 8, bottom - 8),
                    PanelText::caption, tracks[index].failed
                        ? PanelColor::highlight : PanelColor::secondary);
                const auto clipLeft = static_cast<float>(timelineXForSeconds(
                    tracks[index].offset, viewStart, pixelsPerSecond));
                const auto clipRight = static_cast<float>(timelineXForSeconds(
                    tracks[index].offset + clipDuration(tracks[index]),
                    viewStart, pixelsPerSecond));
                const auto visibleClip = D2D1::RectF(
                    std::max(rail + 1.0f, clipLeft), top + 4.0f,
                    std::min(size.width - 2.0f, clipRight), bottom - 4.0f);
                if (visibleClip.right > visibleClip.left) {
                    render.fill(visibleClip,
                        isSelected ? PanelColor::selected : PanelColor::panel,
                        isSelected ? 0.34f : 0.18f);
                }
                drawWaveform(tracks[index],
                    D2D1::RectF(rail + 1, top + 4, size.width - 2, bottom - 4),
                    isSelected);
                if (clipRight >= rail && clipLeft <= size.width) {
                    const auto left = std::clamp(clipLeft, rail + 1.0f,
                        size.width - 2.0f);
                    const auto right = std::clamp(clipRight, rail + 1.0f,
                        size.width - 2.0f);
                    const auto boundaryColor = isSelected
                        ? PanelColor::accent : PanelColor::secondary;
                    render.line(left, top + 4.0f, left, bottom - 4.0f,
                        boundaryColor, isSelected ? 2.0f : 1.0f);
                    render.line(right, top + 4.0f, right, bottom - 4.0f,
                        boundaryColor, isSelected ? 2.0f : 1.0f);
                    if (isSelected) {
                        render.line(left, top + 4.0f, left + 7.0f, top + 4.0f,
                            PanelColor::accent, 2.0f);
                        render.line(left, bottom - 4.0f, left + 7.0f,
                            bottom - 4.0f, PanelColor::accent, 2.0f);
                        render.line(right - 7.0f, top + 4.0f, right,
                            top + 4.0f, PanelColor::accent, 2.0f);
                        render.line(right - 7.0f, bottom - 4.0f, right,
                            bottom - 4.0f, PanelColor::accent, 2.0f);
                    }
                }
            }
            if (tracks.empty()) {
                render.text(L"DROP FILES OR FOLDERS HERE",
                    D2D1::RectF(rail + 40, rulerBottom + 80,
                        size.width - 40, rulerBottom + 130),
                    PanelText::center, PanelColor::secondary);
                render.text(L"Add a track to start, then press Play to preview your arrangement.",
                    D2D1::RectF(rail + 40, rulerBottom + 132,
                        size.width - 40, rulerBottom + 180),
                    PanelText::center, PanelColor::muted);
            }
            if (target != nullptr) target->PopAxisAlignedClip();
            if (!render.finishStaticSceneAndBeginOverlay()) {
                staticSceneReady = false;
                EndPaint(window, &state);
                return;
            }
            staticSceneReady = render.retainedBacking() != nullptr;
            staticThemeGeneration = render.appliedThemeGeneration();
        } else {
            ++cachedScenePaints;
        }
        // Live pixels never enter the retained static scene. Recomposition
        // restores the old playhead and clock without leaving trails.
        drawPeakMeter(meterLeftRect(size.width), meterLevel[0], meterHold[0]);
        drawPeakMeter(meterRightRect(size.width), meterLevel[1], meterHold[1]);

        constexpr float rail = kTimelineRailDip;
        constexpr float rulerTop = 49.0f;
        constexpr float rulerBottom = 82.0f;
        const auto playhead = static_cast<float>(timelineXForSeconds(
            currentPosition(), viewStart, pixelsPerSecond));
        if (playhead >= rail && playhead < size.width) {
            render.line(playhead, rulerTop, playhead, size.height,
                PanelColor::accent, 1.5f);
        }
        render.text(formatTime(currentPosition()),
            D2D1::RectF(size.width - 110, rulerTop, size.width - 10, rulerBottom),
            PanelText::right, PanelColor::text);
        if (FAILED(render.end())) staticSceneReady = false;
        EndPaint(window, &state);
    }

    int rowAt(float y) const noexcept
    {
        constexpr float top = 82.0f;
        constexpr float height = 84.0f;
        if (y < top) return -1;
        const auto index = static_cast<int>(std::floor(
            (y - top + verticalScroll) / height));
        return index >= 0 && index < static_cast<int>(tracks.size()) ? index : -1;
    }

    bool discardClosingInput(UINT message, WPARAM wParam) const noexcept
    {
        if (!closing) return false;
        // Keep the close command current until its acknowledgement arrives.
        // Paint, decode/lifecycle completions and read-only probes still drain.
        if (message == WM_DROPFILES) {
            DragFinish(reinterpret_cast<HDROP>(wParam));
            return true;
        }
        return message == dawQaCommandMessage() || message == WM_COPYDATA ||
            message == WM_COMMAND || message == WM_CAPTURECHANGED ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST);
    }

    LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (discardClosingInput(message, wParam)) return 0;
        if (message == dawQaStateMessage()) {
            if (wParam == 4U) return static_cast<LRESULT>(staticScenePaints);
            if (wParam == 5U) return static_cast<LRESULT>(cachedScenePaints);
            if (wParam == 6U) return staticSceneReady ? TRUE : FALSE;
            std::uint64_t ready = 0;
            std::uint64_t audioReady = 0;
            std::uint64_t failed = 0;
            for (const auto& track : tracks) {
                if (track.waveform) ++ready;
                if (track.audio) ++audioReady;
                if (track.failed) ++failed;
            }
            if (wParam == 1U) {
                return static_cast<LRESULT>(std::count_if(
                    tracks.begin(), tracks.end(), [](const Track& track) {
                        return track.audio && track.audio->spillBacked();
                    }));
            }
            if (wParam == 2U) {
                std::vector<const PcmClip*> unique;
                for (const auto& track : tracks) {
                    if (!track.audio) continue;
                    const auto* pointer = track.audio.get();
                    if (std::find(unique.begin(), unique.end(), pointer) ==
                            unique.end()) unique.push_back(pointer);
                }
                return static_cast<LRESULT>(unique.size());
            }
            if (wParam == 3U) {
                const auto first = std::find_if(
                    tracks.begin(), tracks.end(), [](const Track& track) {
                        return track.audio != nullptr;
                    });
                if (first == tracks.end()) return 0;
                return static_cast<LRESULT>(std::count_if(
                    tracks.begin(), tracks.end(), [&](const Track& track) {
                        return track.audio == first->audio;
                    }));
            }
            return static_cast<LRESULT>(
                (std::min<std::uint64_t>(tracks.size(), 0xffffU)) |
                (std::min<std::uint64_t>(ready, 0xffffU) << 16U) |
                (static_cast<std::uint64_t>(mixer.playing()) << 32U) |
                (std::min<std::uint64_t>(audioReady, 0x7fffU) << 33U) |
                (std::min<std::uint64_t>(failed, 0x7fffU) << 48U) |
                (static_cast<std::uint64_t>(
                    DawVisualBridge::instance().active()) << 63U));
        }
        if (message == dawQaMonitorStateMessage()) {
            switch (wParam) {
            case 0U:
                return static_cast<LRESULT>(
                    static_cast<unsigned>(monitorMuteLeft) |
                    (static_cast<unsigned>(monitorMuteRight) << 1U));
            case 1U:
                return static_cast<LRESULT>(std::llround(
                    mixer.masterGain() * 1000000.0f));
            case 2U:
                return mixer.masterMuted() ? TRUE : FALSE;
            case 3U: {
                const auto peaks = mixer.peaks();
                const auto left = static_cast<std::uint64_t>(std::llround(
                    std::clamp(peaks[0], 0.0f, 1.0f) * 65535.0f));
                const auto right = static_cast<std::uint64_t>(std::llround(
                    std::clamp(peaks[1], 0.0f, 1.0f) * 65535.0f));
                return static_cast<LRESULT>(left | (right << 16U));
            }
            default:
                return E_INVALIDARG;
            }
        }
        if (message == dawQaPositionMessage()) {
            return static_cast<LRESULT>(std::llround(
                currentPosition() * 1000.0));
        }
        if (message == dawQaLifecycleMessage()) {
            switch (wParam) {
            case 0U:
                return static_cast<LRESULT>(mixer.state());
            case 1U:
                return static_cast<LRESULT>(lastLifecycleResult);
            case 2U:
                return static_cast<LRESULT>(lastLifecycleGeneration);
            default:
                return E_INVALIDARG;
            }
        }
        if (message == dawQaCommandMessage()) {
            if (!dawQaHooksAllowed()) return E_ACCESSDENIED;
            if (wParam == 1U) togglePlay();
            else if (wParam == 2U) stopPlayback();
            else if (wParam == 3U) {
                fitTimeline();
                invalidateScene();
            } else if (wParam == 4U || wParam == 6U) {
                auto* value = wParam == 4U
                    ? &monitorMuteLeft : &monitorMuteRight;
                *value = !*value;
                mixer.setChannelMutes(
                    monitorMuteLeft, monitorMuteRight);
                invalidateScene();
            } else if (wParam == 7U) {
                pausedPosition = std::max(0.0,
                    static_cast<double>(lParam) / 1000.0);
                if (mixer.playing()) {
                    mixer.pause();
                    togglePlay();
                }
                invalidateScene();
            } else if (wParam == 8U) {
                const auto index = static_cast<std::size_t>(lParam);
                if (index >= tracks.size()) return FALSE;
                selected = static_cast<int>(index);
                removeStem(index);
            } else if (wParam == 9U) {
                if (tracks.empty()) return FALSE;
                selected = std::clamp(selected, 0,
                    static_cast<int>(tracks.size()) - 1);
                pausedPosition = std::max(0.0,
                    static_cast<double>(lParam) / 1000.0);
                if (!splitSelectedAtPlayhead()) return FALSE;
            } else if (wParam == 10U) {
                // QA-only ownership probe: add a second MixTrack that shares
                // the immutable PcmClip but starts at a distant source offset.
                // With a spill-backed clip this forces two mapping windows in
                // one mixer buffer and catches UI/worker view mutation races.
                if (tracks.empty()) return FALSE;
                const auto index = static_cast<std::size_t>(std::clamp(
                    selected, 0, static_cast<int>(tracks.size()) - 1));
                const auto& source = tracks[index];
                if (!source.audio) return FALSE;
                const auto requested = std::max(0.0,
                    static_cast<double>(lParam) / 1000.0);
                const auto maximum = static_cast<double>(
                    source.audio->frameCount()) / kProjectSampleRate;
                if (requested + 0.010 >= maximum) return FALSE;
                Track duplicate = source;
                duplicate.id = nextTrackId++;
                duplicate.offset = 0.0;
                duplicate.sourceStart = requested;
                duplicate.sourceEnd = maximum;
                tracks.insert(tracks.begin() +
                    static_cast<std::ptrdiff_t>(index + 1U),
                    std::move(duplicate));
                selected = static_cast<int>(index + 1U);
                clampVerticalScroll();
                invalidateScene();
            } else if (wParam == 11U) {
                render.reset();
                invalidateScene();
            } else if (wParam == 12U) {
                pixelsPerSecond = std::clamp(
                    static_cast<double>(lParam) / 1000.0, 2.0, 2000.0);
                invalidateScene();
            } else if (wParam == 13U) {
                viewStart = std::max(0.0, static_cast<double>(lParam) / 1000.0);
                invalidateScene();
            } else return FALSE;
            return TRUE;
        }
        switch (message) {
        case WM_CREATE:
            window = hwnd;
            DisplayEnvironmentService::instance().registerPersistentRoot(window);
            frameClock.attach(window, kFrameClockMessage);
            frameClock.setTelemetrySurface(TelemetrySurface::daw);
            frameClock.setLatencyCritical(true);
            mixer.setTarget(window);
            DragAcceptFiles(window, TRUE);
            syncMasterVolume();
            startDecoder();
            return 0;
        case WM_SIZE:
            render.resize();
            invalidateScene();
            return 0;
        case WM_WINDOWPOSCHANGED:
            DisplayEnvironmentService::instance().notifyWindowChanged(window);
            if (mixer.playing()) frameClock.requestImmediate();
            break;
        case WM_DPICHANGED: {
            DisplayEnvironmentService::instance().notifyWindowChanged(window);
            const auto* bounds = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top,
                bounds->right - bounds->left, bounds->bottom - bounds->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            render.reset();
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_DEVICECHANGE:
        case WM_SETTINGCHANGE:
            DisplayEnvironmentService::instance().notifyWindowChanged(window);
            if (mixer.playing()) frameClock.requestImmediate();
            invalidateScene();
            break;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            applyDpiAwareMinimum(window, limits, 920, 430);
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT:
            paint();
            return 0;
        case kFrameClockMessage:
            if (frameClock.consume(wParam) && mixer.playing()) {
                syncMasterVolume();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wParam);
            const auto count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT index = 0; index < count; ++index) {
                const auto length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(length + 1U, L'\0');
                const auto copied = DragQueryFileW(drop, index,
                    path.data(), static_cast<UINT>(path.size()));
                path.resize(copied);
                if (!path.empty()) paths.push_back(wideToUtf8(path));
            }
            DragFinish(drop);
            resolvePaths(std::move(paths));
            return 0;
        }
        case WM_COPYDATA: {
            const auto* payload = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (payload == nullptr || payload->dwData !=
                    nekorox::kFileDropCopyDataTagV1 ||
                payload->lpData == nullptr || payload->cbData < sizeof(wchar_t) ||
                payload->cbData % sizeof(wchar_t) != 0) {
                return FALSE;
            }
            const auto* path = static_cast<const wchar_t*>(payload->lpData);
            const auto count = payload->cbData / sizeof(wchar_t);
            if (path[count - 1U] != L'\0') return FALSE;
            resolvePaths({wideToUtf8(path)});
            return TRUE;
        }
        case kDecodeMessage: {
            std::unique_ptr<DecodeMessage> update(
                reinterpret_cast<DecodeMessage*>(lParam));
            if (!update || update->generation != generation.load(
                    std::memory_order_acquire)) return 0;
            const auto found = std::find_if(
                tracks.begin(), tracks.end(), [&](const Track& track) {
                    return track.id == update->trackId;
                });
            // Removing a stem is allowed while its decoder is finishing. Its
            // late message is intentionally harmless instead of being
            // applied to whichever row shifted into the old index.
            if (found == tracks.end()) return 0;
            auto& track = *found;
            if (update->waveform) track.waveform = std::move(update->waveform);
            if (update->audio) {
                track.audio = std::move(update->audio);
                const auto decodedDuration = static_cast<double>(
                    track.audio->frameCount()) / kProjectSampleRate;
                if (track.duration <= 0.0) track.duration = decodedDuration;
                if (track.sourceEnd <= track.sourceStart) {
                    track.sourceEnd = decodedDuration;
                } else {
                    track.sourceEnd = std::min(
                        track.sourceEnd, decodedDuration);
                }
                auto residentBytes = [&] {
                    std::size_t total = 0;
                    for (const auto& item : tracks) {
                        if (item.audio) total += item.audio->residentBytes();
                    }
                    return total;
                };
                auto resident = residentBytes();
                const auto mixerState = mixer.state();
                const auto safeToEvict = mixerState != PreviewMixer::State::starting &&
                    mixerState != PreviewMixer::State::playing &&
                    mixerState != PreviewMixer::State::stopping;
                if (safeToEvict) {
                    for (auto& item : tracks) {
                        if (resident <= kHotPcmBytesTotal) break;
                        if (!item.audio || !item.audio->spillBacked()) continue;
                        item.audio->dropHot();
                        resident = residentBytes();
                    }
                }
                if (resident > kMaximumProjectPcmBytes) {
                    track.audio.reset();
                    track.failed = true;
                }
            }
            track.progress = update->progress;
            track.failed = track.failed ||
                (update->final && !update->error.empty());
            invalidateScene();
            return 0;
        }
        case kPlaybackLifecycleMessage: {
            const auto lifecycleGeneration =
                static_cast<std::uint64_t>(wParam);
            // Worker checks cannot revoke messages already in the UI queue.
            // Only the current command may change clocks, presence or playback.
            if (lifecycleGeneration != mixer.commandGeneration()) return 0;
            const auto state = static_cast<PreviewMixer::State>(
                static_cast<std::uint32_t>(lParam) & 0xffU);
            const auto result = static_cast<MMRESULT>(
                static_cast<std::uint32_t>(lParam) >> 8U);
            lastLifecycleGeneration = lifecycleGeneration;
            lastLifecycleResult = result;
            Runtime::instance().notifyDawPreviewChanged();
            if (state == PreviewMixer::State::playing) {
                frameClock.start();
                publishDawPresence(PlaybackState::playing);
            } else {
                frameClock.stop();
            }
            if (state == PreviewMixer::State::eof) {
                pausedPosition = 0.0;
                resetMeters();
                clearDawPresence();
                restoreNormalPlayback();
            } else if (state == PreviewMixer::State::error) {
                pausedPosition = mixer.positionSeconds();
                resetMeters();
                clearDawPresence();
                restoreNormalPlayback();
                if (!closing && !dawQaBackgroundRequested()) {
                    wchar_t errorText[160]{};
                    swprintf_s(errorText,
                        L"Preview output failed (WinMM %u). The timeline is still available.",
                        static_cast<unsigned>(result));
                    MessageBoxW(window, errorText, L"GODENplayer DAW Mode",
                        MB_OK | MB_ICONERROR);
                }
            } else if (state == PreviewMixer::State::paused) {
                publishDawPresence(PlaybackState::paused);
            } else if (state == PreviewMixer::State::idle) {
                if (lifecycleGeneration == resetPositionGeneration) {
                    pausedPosition = 0.0;
                    resetMeters();
                }
                clearDawPresence();
                restoreNormalPlayback();
            }
            if (closing && lifecycleGeneration == closeMixerGeneration &&
                    (state == PreviewMixer::State::idle ||
                     state == PreviewMixer::State::error)) {
                mixerStopped = true;
                finishCloseIfReady();
            }
            invalidateScene();
            return 0;
        }
        case kDecoderStoppedMessage:
            if (closing) {
                if (decoder.joinable()) decoder.join();
                decoderStopped = true;
                finishCloseIfReady();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(window, &cursor);
                const auto x = clientDip(window, cursor.x);
                const auto y = clientDip(window, cursor.y);
                const auto row = rowAt(y);
                if (row >= 0) {
                    const auto rowTop = 82.0f + row * 84.0f -
                        verticalScroll;
                    if (inRect(removeRectForRow(rowTop), x, y) ||
                            inRect(cutRectForRow(rowTop), x, y)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    }
                    const auto& track = tracks[static_cast<std::size_t>(row)];
                    const auto left = timelineXForSeconds(track.offset,
                        viewStart, pixelsPerSecond);
                    const auto right = timelineXForSeconds(
                        track.offset + clipDuration(track),
                        viewStart, pixelsPerSecond);
                    if (x >= kTimelineRailDip &&
                            (std::abs(x - left) <= 8.0f ||
                                std::abs(x - right) <= 8.0f)) {
                        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                        return TRUE;
                    }
                    if (x >= left && x <= right) {
                        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                        return TRUE;
                    }
                }
            }
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(window);
            const auto point = dipPoint(window, lParam);
            const auto x = static_cast<float>(point.x);
            const auto y = static_cast<float>(point.y);
            if (inRect(addRect(), x, y)) openPicker(false);
            else if (inRect(folderRect(), x, y)) openPicker(true);
            else if (inRect(playRect(), x, y)) togglePlay();
            else if (inRect(stopRect(), x, y)) stopPlayback();
            else if (inRect(fitRect(), x, y)) { fitTimeline(); invalidateScene(); }
            else if (inRect(minusRect(), x, y)) { pixelsPerSecond = std::max(2.0, pixelsPerSecond / 1.35); invalidateScene(); }
            else if (inRect(plusRect(), x, y)) { pixelsPerSecond = std::min(2000.0, pixelsPerSecond * 1.35); invalidateScene(); }
            else if (inRect(muteLeftRect(), x, y)) {
                monitorMuteLeft = !monitorMuteLeft;
                mixer.setChannelMutes(monitorMuteLeft, monitorMuteRight);
                invalidateScene();
            }
            else if (inRect(muteRightRect(), x, y)) {
                monitorMuteRight = !monitorMuteRight;
                mixer.setChannelMutes(monitorMuteLeft, monitorMuteRight);
                invalidateScene();
            }
            else if (y >= 49.0f && y < 82.0f && x >= kTimelineRailDip) {
                pausedPosition = std::max(0.0,
                    timelineSecondsAtX(x, viewStart, pixelsPerSecond));
                if (mixer.playing()) {
                    mixer.pause();
                    togglePlay();
                }
                invalidateScene();
            } else {
                const auto row = rowAt(y);
                if (row >= 0) {
                    selected = row;
                    const auto rowTop = 82.0f + row * 84.0f - verticalScroll;
                    if (inRect(removeRectForRow(rowTop), x, y)) {
                        removeStem(static_cast<std::size_t>(row));
                    } else if (inRect(cutRectForRow(rowTop), x, y)) {
                        splitSelectedAtPlayhead();
                    } else if (x >= 38 && x < 72 && y >= rowTop + 43) {
                        tracks[row].muted = !tracks[row].muted;
                        restartPreviewForMixChange();
                    } else if (x >= 78 && x < 112 && y >= rowTop + 43) {
                        tracks[row].solo = !tracks[row].solo;
                        restartPreviewForMixChange();
                    } else if (x >= kTimelineRailDip) {
                        auto& track = tracks[static_cast<std::size_t>(row)];
                        const auto clipLeft = static_cast<float>(
                            timelineXForSeconds(track.offset,
                                viewStart, pixelsPerSecond));
                        const auto clipRight = static_cast<float>(
                            timelineXForSeconds(
                                track.offset + clipDuration(track),
                                viewStart, pixelsPerSecond));
                        constexpr float handleReachDip = 8.0f;
                        if (x >= clipLeft - handleReachDip &&
                                x <= clipRight + handleReachDip) {
                            resumeAfterClipEdit = mixer.playing();
                            if (resumeAfterClipEdit) {
                                pausedPosition = mixer.positionSeconds();
                                mixer.pause();
                            }
                            clipEditMode = std::abs(x - clipLeft) <=
                                    handleReachDip
                                ? ClipEditMode::trimStart
                                : std::abs(x - clipRight) <= handleReachDip
                                    ? ClipEditMode::trimEnd
                                    : ClipEditMode::move;
                            dragOrigin = point;
                            dragOffset = track.offset;
                            dragSourceStart = track.sourceStart;
                            dragSourceEnd = track.sourceEnd;
                            SetCapture(window);
                        }
                    }
                    invalidateScene();
                }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const auto point = dipPoint(window, lParam);
            const auto row = rowAt(static_cast<float>(point.y));
            if (row >= 0) {
                POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(window, &screenPoint);
                showStemMenu(static_cast<std::size_t>(row), screenPoint);
                invalidateScene();
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (clipEditMode != ClipEditMode::none && selected >= 0 &&
                    selected < static_cast<int>(tracks.size())) {
                const auto point = dipPoint(window, lParam);
                auto& track = tracks[static_cast<std::size_t>(selected)];
                auto delta = (point.x - dragOrigin.x) / pixelsPerSecond;
                constexpr double minimumClipSeconds = 0.010;
                if (clipEditMode == ClipEditMode::move) {
                    track.offset = std::max(0.0, dragOffset + delta);
                } else if (clipEditMode == ClipEditMode::trimStart) {
                    delta = std::clamp(delta,
                        std::max(-dragOffset, -dragSourceStart),
                        dragSourceEnd - minimumClipSeconds -
                            dragSourceStart);
                    track.offset = dragOffset + delta;
                    track.sourceStart = dragSourceStart + delta;
                } else if (clipEditMode == ClipEditMode::trimEnd) {
                    const auto maximumEnd = std::max(
                        track.duration, dragSourceEnd);
                    track.sourceEnd = std::clamp(
                        dragSourceEnd + delta,
                        dragSourceStart + minimumClipSeconds,
                        maximumEnd);
                }
                invalidateScene();
            }
            return 0;
        case WM_LBUTTONUP:
            if (clipEditMode != ClipEditMode::none) {
                finishClipEdit(true);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (clipEditMode != ClipEditMode::none) {
                finishClipEdit(false);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            const auto delta = GET_WHEEL_DELTA_WPARAM(wParam) /
                static_cast<double>(WHEEL_DELTA);
            if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
                POINT cursor{
                    GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
                };
                ScreenToClient(window, &cursor);
                const auto cursorDip = std::max<double>(
                    static_cast<double>(kTimelineRailDip),
                    static_cast<double>(clientDip(window, cursor.x)));
                const auto cursorOffsetDip = cursorDip - kTimelineRailDip;
                const auto anchorSeconds = viewStart +
                    cursorOffsetDip / pixelsPerSecond;
                const auto newPixelsPerSecond = std::clamp(
                    pixelsPerSecond * std::pow(1.25, delta), 2.0, 2000.0);
                viewStart = std::max(0.0,
                    anchorSeconds - cursorOffsetDip / newPixelsPerSecond);
                pixelsPerSecond = newPixelsPerSecond;
                invalidateScene();
                return 0;
            }
            if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) {
                viewStart = std::max(0.0,
                    viewStart - delta * 90.0 / pixelsPerSecond);
            } else {
                RECT client{};
                GetClientRect(window, &client);
                const auto maximum = std::max(0.0f,
                    static_cast<float>(tracks.size()) * 84.0f -
                    std::max(0.0f, clientDip(window, client.bottom) - 82.0f));
                verticalScroll = std::clamp(verticalScroll -
                    static_cast<float>(delta * 84.0), 0.0f, maximum);
            }
            invalidateScene();
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_SPACE) { togglePlay(); return 0; }
            if (wParam == VK_HOME) { pausedPosition = 0.0; invalidateScene(); return 0; }
            if (wParam == VK_DELETE && selected >= 0) {
                removeStem(static_cast<std::size_t>(selected));
                return 0;
            }
            if (wParam == 'S') {
                splitSelectedAtPlayhead();
                return 0;
            }
            break;
        case WM_CLOSE:
            beginClose();
            return 0;
        case WM_DESTROY:
            DisplayEnvironmentService::instance().unregisterPersistentRoot(window);
            frameClock.detach();
            DragAcceptFiles(window, FALSE);
            mixer.setTarget(nullptr);
            render.reset();
            clearDawPresence();
            window = nullptr;
            owner = nullptr;
            closing = false;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

DawWindow::DawWindow() : impl_(std::make_unique<Impl>()) {}
DawWindow::~DawWindow() = default;

DawWindow& DawWindow::instance()
{
    static DawWindow value;
    return value;
}

void DawWindow::show(HWND owner)
{
    impl_->show(owner);
}

void DawWindow::close()
{
    impl_->close();
}

void DawWindow::shutdown()
{
    impl_->shutdown();
}

} // namespace nekorox::component
