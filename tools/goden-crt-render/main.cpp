#include "crt_offscreen.h"

#include "nekorox/contracts.h"
#include "nekorox/crt_xy_model.h"

#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nekorox::CrtBeamSegment;
using nekorox::CrtXyModel;
using nekorox::CrtXyOrientation;
using nekorox::RealtimeFrameV1;
using nekorox::tools::CrtOffscreenRenderer;
using nekorox::tools::CrtReadbackFrame;

namespace {

struct Options {
    fs::path input;
    fs::path output;
    fs::path editingOutput;
    fs::path logPath;
    fs::path manifest;
    fs::path selfTestOutput;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint64_t startSample = 0;
    std::uint64_t contentFrames = 0;
    double tailSeconds = 2.0;
    CrtXyOrientation orientation = CrtXyOrientation::midSide;
    std::uint64_t seed = 1;
    bool forceWarp = false;
    bool selfTest = false;
    std::wstring codec = L"ffv1";
};

struct DecodedPcm {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::vector<float> samples;
    fs::path temporaryPath;

    [[nodiscard]] std::uint64_t frames() const noexcept {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

struct RenderResult {
    bool success = false;
    std::wstring error;
    std::uint64_t contentFrames = 0;
    std::uint64_t tailFrames = 0;
    std::uint64_t totalFrames = 0;
    double wallSeconds = 0.0;
    double realtimeMultiple = 0.0;
    bool warp = false;
    std::wstring adapter;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint64_t readbackWaits = 0;
};

std::wstring quote(const fs::path& path)
{
    std::wstring value = path.wstring();
    std::wstring result = L"\"";
    for (const auto character : value) {
        if (character == L'\"') result += L'\\';
        result += character;
    }
    result += L"\"";
    return result;
}

std::wstring jsonEscape(std::wstring_view value)
{
    std::wstring result;
    result.reserve(value.size() + 8);
    for (const auto character : value) {
        switch (character) {
        case L'\\': result += L"\\\\"; break;
        case L'\"': result += L"\\\""; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (character < 0x20) {
                std::wostringstream escaped;
                escaped << L"\\u" << std::hex << std::setw(4)
                        << std::setfill(L'0') << static_cast<unsigned>(character);
                result += escaped.str();
            } else {
                result += character;
            }
        }
    }
    return result;
}

std::string utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value)
{
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

bool runCapture(const std::wstring& command, std::wstring& output, int& exitCode)
{
    output.clear();
    auto* pipe = _wpopen(command.c_str(), L"rt, ccs=UTF-8");
    if (pipe == nullptr) {
        exitCode = -1;
        return false;
    }
    std::array<wchar_t, 4096> buffer{};
    while (fgetws(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    exitCode = _pclose(pipe);
    return exitCode == 0;
}

bool runCommand(const std::wstring& command, int& exitCode)
{
    exitCode = _wsystem(command.c_str());
    return exitCode == 0;
}

std::optional<std::pair<std::uint32_t, std::uint16_t>> probeAudio(
    const fs::path& input,
    std::wstring& error)
{
    const auto command =
        L"ffprobe -v error -select_streams a:0 "
        L"-show_entries stream=sample_rate,channels "
        L"-of csv=p=0:s=x " + quote(input);
    std::wstring output;
    int exitCode = 0;
    if (!runCapture(command, output, exitCode)) {
        error = L"ffprobe failed with exit code " + std::to_wstring(exitCode);
        return std::nullopt;
    }
    output.erase(std::remove(output.begin(), output.end(), L'\r'), output.end());
    output.erase(std::remove(output.begin(), output.end(), L'\n'), output.end());
    const auto separator = output.find(L'x');
    if (separator == std::wstring::npos) {
        error = L"Unable to parse ffprobe audio stream: " + output;
        return std::nullopt;
    }
    try {
        const auto rate = std::stoul(output.substr(0, separator));
        const auto channels = std::stoul(output.substr(separator + 1));
        if (rate == 0 || rate > 768000 || channels == 0 || channels > 64) {
            throw std::out_of_range("audio stream");
        }
        return std::pair{
            static_cast<std::uint32_t>(rate),
            static_cast<std::uint16_t>(channels)};
    } catch (...) {
        error = L"Invalid ffprobe audio stream: " + output;
        return std::nullopt;
    }
}

fs::path temporaryRawPath()
{
    std::array<wchar_t, MAX_PATH + 1> directory{};
    std::array<wchar_t, MAX_PATH + 1> name{};
    if (GetTempPathW(static_cast<DWORD>(directory.size()), directory.data()) == 0 ||
        GetTempFileNameW(directory.data(), L"gcr", 0, name.data()) == 0) {
        return {};
    }
    DeleteFileW(name.data());
    return fs::path(name.data()).replace_extension(L".f32le");
}

bool decodePcm(const fs::path& input, DecodedPcm& pcm, std::wstring& error)
{
    const auto stream = probeAudio(input, error);
    if (!stream) return false;
    if (stream->second != 2) {
        error = L"Exact CRT rendering currently requires a stereo source; found " +
            std::to_wstring(stream->second) + L" channels";
        return false;
    }
    const auto temporary = temporaryRawPath();
    if (temporary.empty()) {
        error = L"Unable to create temporary PCM path";
        return false;
    }
    const auto command =
        L"ffmpeg -nostdin -hide_banner -loglevel error -y -i " +
        quote(input) +
        L" -map 0:a:0 -vn -sn -dn -c:a pcm_f32le -f f32le " +
        quote(temporary);
    int exitCode = 0;
    if (!runCommand(command, exitCode)) {
        error = L"ffmpeg PCM decode failed with exit code " +
            std::to_wstring(exitCode);
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    std::error_code sizeError;
    const auto bytes = fs::file_size(temporary, sizeError);
    if (sizeError || bytes == 0 || bytes % (sizeof(float) * 2U) != 0 ||
        bytes > static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max())) {
        error = L"Decoded PCM has an invalid size";
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    try {
        pcm.samples.resize(static_cast<std::size_t>(bytes / sizeof(float)));
    } catch (...) {
        error = L"Unable to allocate decoded PCM";
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    std::ifstream file(temporary, std::ios::binary);
    file.read(
        reinterpret_cast<char*>(pcm.samples.data()),
        static_cast<std::streamsize>(bytes));
    if (!file) {
        error = L"Unable to read decoded PCM";
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    pcm.sampleRate = stream->first;
    pcm.channels = stream->second;
    pcm.temporaryPath = temporary;
    return true;
}

void releaseDecodedPcm(DecodedPcm& pcm)
{
    pcm.samples.clear();
    pcm.samples.shrink_to_fit();
    if (!pcm.temporaryPath.empty()) {
        std::error_code ignored;
        fs::remove(pcm.temporaryPath, ignored);
    }
    pcm = {};
}

std::wstring ffmpegAudioFilter(
    std::uint64_t startSample,
    std::uint64_t endSample,
    double tailSeconds)
{
    std::wostringstream filter;
    filter << L"[1:a:0]atrim=start_sample=" << startSample
           << L":end_sample=" << endSample
           << L",asetpts=PTS-STARTPTS";
    if (tailSeconds > 0.0) {
        filter << L",apad=pad_dur=" << std::fixed << std::setprecision(9)
               << tailSeconds;
    }
    filter << L"[aligned]";
    return filter.str();
}

FILE* openEncoderPipe(
    const Options& options,
    std::uint64_t contentSamples,
    std::uint64_t totalFrames,
    std::wstring& command,
    std::wstring& error)
{
    const auto endSample = options.startSample + contentSamples;
    std::wostringstream stream;
    stream << L"ffmpeg -nostdin -hide_banner -loglevel error -y "
           << L"-f rawvideo -pixel_format bgra -video_size "
           << options.width << L"x" << options.height
           << L" -framerate 60/1 -i pipe:0 -i " << quote(options.input)
           << L" -filter_complex \""
           << ffmpegAudioFilter(
                  options.startSample, endSample, options.tailSeconds)
           << L"\" -map 0:v:0 -map \"[aligned]\" "
           << L"-frames:v " << totalFrames;
    if (options.codec == L"hevc_nvenc") {
        // NVENC lossless 4:4:4 keeps the CRT comparison visually lossless
        // while moving the encoding bottleneck off the CPU. Matroska avoids
        // the compatibility restrictions of HEVC-in-MP4.
        stream << L" -c:v hevc_nvenc -preset losslesshp -tune lossless "
               << L"-profile:v rext -pix_fmt yuv444p -r 60/1 -vsync cfr ";
    } else if (options.codec == L"hevc_nvenc_hq") {
        // A practical comparison copy: high-quality 4:4:4 HEVC with a capped
        // rate prevents dense phosphor motion from growing into multi-GB
        // lossless files. Every variant uses the identical encode settings.
        stream << L" -c:v hevc_nvenc -preset p4 -tune hq -profile:v rext "
               << L"-rc vbr_hq -cq 16 -b:v 18M -maxrate 28M -bufsize 56M "
               << L"-rc-lookahead 32 -spatial_aq 1 -aq-strength 8 "
               << L"-pix_fmt yuv444p -r 60/1 -vsync cfr ";
    } else {
        stream << L" -c:v ffv1 -level 3 -coder 1 -context 1 -g 1 "
               << L"-threads 8 -slicecrc 1 "
               << L"-pix_fmt bgra -r 60/1 -vsync cfr ";
    }
    stream << L"-c:a flac -metadata:s:v:0 crt_renderer=goden-crt-render "
           << quote(options.output);
    if (!options.logPath.empty()) {
        stream << L" 2> " << quote(options.logPath);
    }
    command = stream.str();
    auto* pipe = _wpopen(command.c_str(), L"wb");
    if (pipe == nullptr) {
        error = L"Unable to start ffmpeg encoder";
        return nullptr;
    }
    _setmode(_fileno(pipe), _O_BINARY);
    return pipe;
}

bool makeEditingCopy(const Options& options, std::wstring& error)
{
    if (options.editingOutput.empty()) return true;
    std::error_code directoryError;
    fs::create_directories(options.editingOutput.parent_path(), directoryError);
    if (directoryError) {
        error = L"Unable to create editing output directory";
        return false;
    }
    const auto command =
        L"ffmpeg -nostdin -hide_banner -loglevel error -y -i " +
        quote(options.output) +
        L" -map 0:v:0 -map 0:a:0 -c:v prores_ks -profile:v 3 "
        L"-pix_fmt yuv422p10le -r 60/1 -vsync cfr "
        L"-c:a pcm_s24le " + quote(options.editingOutput);
    int exitCode = 0;
    if (!runCommand(command, exitCode)) {
        error = L"ProRes editing encode failed with exit code " +
            std::to_wstring(exitCode);
        return false;
    }
    return true;
}

RenderResult renderOne(const Options& options)
{
    RenderResult result;
    const auto started = std::chrono::steady_clock::now();
    if (options.input.empty() || options.output.empty()) {
        result.error = L"--input and --output are required";
        return result;
    }
    if (options.width == 0 || options.height == 0 ||
        options.width > 16384 || options.height > 16384) {
        result.error = L"Invalid output dimensions";
        return result;
    }
    if (options.tailSeconds < 0.0 || !std::isfinite(options.tailSeconds)) {
        result.error = L"Invalid tail duration";
        return result;
    }
    if (options.codec != L"ffv1" && options.codec != L"hevc_nvenc" &&
        options.codec != L"hevc_nvenc_hq") {
        result.error = L"Codec must be ffv1, hevc_nvenc, or hevc_nvenc_hq";
        return result;
    }

    std::error_code directoryError;
    fs::create_directories(options.output.parent_path(), directoryError);
    if (directoryError) {
        result.error = L"Unable to create output directory";
        return result;
    }
    if (!options.logPath.empty()) {
        fs::create_directories(options.logPath.parent_path(), directoryError);
    }

    DecodedPcm pcm;
    if (!decodePcm(options.input, pcm, result.error)) return result;
    const auto cleanup = [&]() { releaseDecodedPcm(pcm); };
    if (options.startSample >= pcm.frames()) {
        result.error = L"--start-sample lies beyond decoded PCM";
        cleanup();
        return result;
    }
    const auto availableSamples = pcm.frames() - options.startSample;
    result.contentFrames = options.contentFrames != 0
        ? options.contentFrames
        : availableSamples * 60U / pcm.sampleRate;
    const auto contentSamples = std::min<std::uint64_t>(
        availableSamples,
        result.contentFrames * pcm.sampleRate / 60U);
    if (result.contentFrames == 0 || contentSamples == 0 ||
        contentSamples > availableSamples) {
        result.error = L"Requested content interval is empty or exceeds PCM";
        cleanup();
        return result;
    }
    // Exact 60/1 CFR. Tail rounds to the nearest whole frame by contract.
    result.tailFrames = static_cast<std::uint64_t>(
        std::llround(options.tailSeconds * 60.0));
    result.totalFrames = result.contentFrames + result.tailFrames;

    CrtOffscreenRenderer renderer;
    if (!renderer.initialize(
            options.width,
            options.height,
            options.orientation,
            options.forceWarp,
            result.error)) {
        cleanup();
        return result;
    }
    const auto rendererStats = renderer.stats();
    result.warp = rendererStats.softwareRasterizer;
    result.adapter = rendererStats.adapterName;
    result.renderWidth = rendererStats.renderWidth;
    result.renderHeight = rendererStats.renderHeight;

    std::wstring ffmpegCommand;
    auto* encoder = openEncoderPipe(
        options, contentSamples, result.totalFrames,
        ffmpegCommand, result.error);
    if (encoder == nullptr) {
        cleanup();
        return result;
    }

    bool writeFailed = false;
    std::uint64_t writtenFrames = 0;
    const auto writeFrame = [&](const CrtReadbackFrame& frame) {
        if (frame.index != writtenFrames || frame.bgra.empty()) {
            writeFailed = true;
            return;
        }
        const auto written = fwrite(
            frame.bgra.data(), 1, frame.bgra.size(), encoder);
        if (written != frame.bgra.size()) {
            writeFailed = true;
            return;
        }
        ++writtenFrames;
    };

    CrtXyModel model;
    model.reset();
    RealtimeFrameV1 pcmFrame;
    pcmFrame.sampleRate = pcm.sampleRate;
    pcmFrame.channels = 2;
    pcmFrame.flags = nekorox::kRealtimeFrameFlagPcm;
    std::uint64_t previousEnd = options.startSample;
    for (std::uint64_t videoFrame = 0;
         videoFrame < result.totalFrames && !writeFailed;
         ++videoFrame) {
        std::span<const CrtBeamSegment> segments;
        if (videoFrame < result.contentFrames) {
            const auto relativeEnd = std::min<std::uint64_t>(
                contentSamples,
                (videoFrame + 1U) * pcm.sampleRate / 60U);
            const auto absoluteEnd = options.startSample + relativeEnd;
            const auto count = absoluteEnd - previousEnd;
            if (count > nekorox::kRealtimeMaxFrames) {
                result.error = L"Frame PCM interval exceeds realtime model capacity";
                writeFailed = true;
                break;
            }
            pcmFrame.firstFrameIndex = previousEnd;
            pcmFrame.frameCount = static_cast<std::uint32_t>(count);
            pcmFrame.sampleCount = static_cast<std::uint32_t>(count * 2U);
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto source = (previousEnd + index) * 2U;
                pcmFrame.samples[static_cast<std::size_t>(index * 2U)] =
                    pcm.samples[static_cast<std::size_t>(source)];
                pcmFrame.samples[static_cast<std::size_t>(index * 2U + 1U)] =
                    pcm.samples[static_cast<std::size_t>(source + 1U)];
            }
            if (count != 0) {
                segments = model.ingest(
                    pcmFrame, videoFrame == 0, 0.0);
            }
            previousEnd = absoluteEnd;
        }
        // Tail frames intentionally submit no new beam. Their audio is padded
        // with digital silence, leaving only the two phosphor decay layers.
        CrtReadbackFrame completed;
        bool hasCompleted = false;
        if (!renderer.render(
                segments, 1.0 / 60.0,
                completed, hasCompleted, result.error)) {
            writeFailed = true;
            break;
        }
        if (hasCompleted) writeFrame(completed);
    }
    while (!writeFailed && writtenFrames < result.totalFrames) {
        CrtReadbackFrame completed;
        bool hasCompleted = false;
        if (!renderer.drain(completed, hasCompleted, result.error)) {
            writeFailed = true;
            break;
        }
        if (!hasCompleted) break;
        writeFrame(completed);
    }
    if (writtenFrames != result.totalFrames && result.error.empty()) {
        result.error = L"Readback frame count mismatch";
        writeFailed = true;
    }

    const auto encoderExit = _pclose(encoder);
    encoder = nullptr;
    if (encoderExit != 0 && result.error.empty()) {
        result.error = L"ffmpeg encoder failed with exit code " +
            std::to_wstring(encoderExit);
        writeFailed = true;
    }
    const auto finalStats = renderer.stats();
    result.readbackWaits = finalStats.readbackWaits;
    renderer.shutdown();
    cleanup();

    if (writeFailed) return result;
    if (!makeEditingCopy(options, result.error)) return result;
    const auto finished = std::chrono::steady_clock::now();
    result.wallSeconds = std::chrono::duration<double>(finished - started).count();
    const auto mediaSeconds = static_cast<double>(result.totalFrames) / 60.0;
    result.realtimeMultiple = result.wallSeconds > 0.0
        ? mediaSeconds / result.wallSeconds : 0.0;
    result.success = true;
    return result;
}

std::string sha256(std::span<const std::uint8_t> bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD actual = 0;
    std::vector<std::uint8_t> object;
    std::vector<std::uint8_t> digest;
    auto cleanup = [&]() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
            &actual, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
            &actual, 0) < 0) {
        cleanup();
        return {};
    }
    object.resize(objectBytes);
    digest.resize(hashBytes);
    if (BCryptCreateHash(
            algorithm, &hash, object.data(), objectBytes,
            nullptr, 0, 0) < 0 ||
        BCryptHashData(
            hash, const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), hashBytes, 0) < 0) {
        cleanup();
        return {};
    }
    cleanup();
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto value : digest) encoded << std::setw(2) << unsigned(value);
    return encoded.str();
}

bool renderFixture(
    CrtXyOrientation orientation,
    const std::vector<std::vector<CrtBeamSegment>>& fixture,
    std::vector<std::string>& hashes,
    std::vector<std::vector<std::uint8_t>>& frames,
    std::wstring& error)
{
    CrtOffscreenRenderer renderer;
    if (!renderer.initialize(128, 128, orientation, true, error)) return false;
    for (const auto& segments : fixture) {
        CrtReadbackFrame completed;
        bool available = false;
        if (!renderer.render(
                segments, 1.0 / 60.0,
                completed, available, error)) return false;
        if (available) frames.push_back(std::move(completed.bgra));
    }
    while (frames.size() < fixture.size()) {
        CrtReadbackFrame completed;
        bool available = false;
        if (!renderer.drain(completed, available, error)) return false;
        if (!available) break;
        frames.push_back(std::move(completed.bgra));
    }
    if (frames.size() != fixture.size()) {
        error = L"Fixture readback count mismatch";
        return false;
    }
    for (const auto& frame : frames) hashes.push_back(sha256(frame));
    return true;
}

bool runSelfTest(const fs::path& outputPath, std::wstring& error)
{
    constexpr float charge = 1.0f / 48000.0f;
    std::vector<std::vector<CrtBeamSegment>> fixture(14);
    fixture[1].push_back({0.0f, 0.0f, 0.85f, 0.0f, charge, 0.0f});
    for (std::size_t frame = 2; frame < 6; ++frame) {
        for (std::size_t sample = 0; sample < 128; ++sample) {
            const auto phase0 = static_cast<float>(
                (frame * 128 + sample) * 0.071);
            const auto phase1 = phase0 + 0.071f;
            fixture[frame].push_back({
                0.72f * std::sin(phase0),
                0.48f * std::cos(phase0 * 1.13f),
                0.72f * std::sin(phase1),
                0.48f * std::cos(phase1 * 1.13f),
                charge,
                static_cast<float>(128 - sample) / 48000.0f});
        }
    }

    std::vector<std::string> firstHashes;
    std::vector<std::string> secondHashes;
    std::vector<std::vector<std::uint8_t>> firstFrames;
    std::vector<std::vector<std::uint8_t>> secondFrames;
    if (!renderFixture(
            CrtXyOrientation::directLeftRight,
            fixture, firstHashes, firstFrames, error) ||
        !renderFixture(
            CrtXyOrientation::directLeftRight,
            fixture, secondHashes, secondFrames, error)) {
        return false;
    }
    const bool deterministic = firstHashes == secondHashes;
    const bool cleanFrame = !firstHashes.empty() &&
        firstHashes[0] == secondHashes[0];
    const bool impulseVisible = firstHashes.size() > 1 &&
        firstHashes[1] != firstHashes[0];

    std::vector<std::string> msHashes;
    std::vector<std::vector<std::uint8_t>> msFrames;
    if (!renderFixture(
            CrtXyOrientation::midSide,
            fixture, msHashes, msFrames, error)) return false;
    const bool orientationDistinct = msHashes.size() > 5 &&
        msHashes[5] != firstHashes[5];

    auto difference = [&](std::size_t index) {
        std::uint64_t total = 0;
        const auto& clean = firstFrames[0];
        const auto& frame = firstFrames[index];
        for (std::size_t byte = 0; byte < frame.size(); ++byte) {
            total += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(frame[byte]) - static_cast<int>(clean[byte])));
        }
        return total;
    };
    bool decayByTime = true;
    auto previous = difference(6);
    for (std::size_t index = 7; index < firstFrames.size(); ++index) {
        const auto current = difference(index);
        if (current > previous) decayByTime = false;
        previous = current;
    }

    const bool success = deterministic && cleanFrame && impulseVisible &&
        orientationDistinct && decayByTime;
    std::error_code directoryError;
    if (!outputPath.parent_path().empty()) {
        fs::create_directories(outputPath.parent_path(), directoryError);
    }
    std::ofstream report(outputPath, std::ios::binary);
    report << "{\n"
           << "  \"success\": " << (success ? "true" : "false") << ",\n"
           << "  \"backend\": \"warp\",\n"
           << "  \"frame_zero_clean\": " << (cleanFrame ? "true" : "false") << ",\n"
           << "  \"known_impulse_visible\": " << (impulseVisible ? "true" : "false") << ",\n"
           << "  \"lr_ms_distinct\": " << (orientationDistinct ? "true" : "false") << ",\n"
           << "  \"decay_time_monotonic\": " << (decayByTime ? "true" : "false") << ",\n"
           << "  \"two_runs_identical\": " << (deterministic ? "true" : "false") << ",\n"
           << "  \"frame_sha256\": [\n";
    for (std::size_t index = 0; index < firstHashes.size(); ++index) {
        report << "    \"" << firstHashes[index] << "\""
               << (index + 1 == firstHashes.size() ? "\n" : ",\n");
    }
    report << "  ]\n}\n";
    if (!report) {
        error = L"Unable to write self-test report";
        return false;
    }
    if (!success) error = L"One or more deterministic CRT self-tests failed";
    return success;
}

std::optional<std::wstring> jsonString(
    std::wstring_view object,
    std::wstring_view key)
{
    const auto marker = L"\"" + std::wstring(key) + L"\"";
    auto position = object.find(marker);
    if (position == std::wstring_view::npos) return std::nullopt;
    position = object.find(L':', position + marker.size());
    if (position == std::wstring_view::npos) return std::nullopt;
    position = object.find(L'\"', position + 1);
    if (position == std::wstring_view::npos) return std::nullopt;
    ++position;
    std::wstring result;
    bool escape = false;
    for (; position < object.size(); ++position) {
        const auto character = object[position];
        if (escape) {
            switch (character) {
            case L'n': result += L'\n'; break;
            case L'r': result += L'\r'; break;
            case L't': result += L'\t'; break;
            default: result += character; break;
            }
            escape = false;
        } else if (character == L'\\') {
            escape = true;
        } else if (character == L'\"') {
            return result;
        } else {
            result += character;
        }
    }
    return std::nullopt;
}

std::optional<double> jsonNumber(
    std::wstring_view object,
    std::wstring_view key)
{
    const auto marker = L"\"" + std::wstring(key) + L"\"";
    auto position = object.find(marker);
    if (position == std::wstring_view::npos) return std::nullopt;
    position = object.find(L':', position + marker.size());
    if (position == std::wstring_view::npos) return std::nullopt;
    ++position;
    while (position < object.size() && iswspace(object[position])) ++position;
    std::wstring token;
    while (position < object.size() &&
           (iswdigit(object[position]) || object[position] == L'.' ||
            object[position] == L'-' || object[position] == L'+' ||
            object[position] == L'e' || object[position] == L'E')) {
        token += object[position++];
    }
    if (token.empty()) return std::nullopt;
    try { return std::stod(token); } catch (...) { return std::nullopt; }
}

std::vector<std::wstring> manifestJobs(std::wstring_view json)
{
    std::vector<std::wstring> jobs;
    const auto jobsPosition = json.find(L"\"jobs\"");
    if (jobsPosition == std::wstring_view::npos) return jobs;
    auto position = json.find(L'[', jobsPosition);
    if (position == std::wstring_view::npos) return jobs;
    std::size_t objectStart = std::wstring_view::npos;
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (++position; position < json.size(); ++position) {
        const auto character = json[position];
        if (inString) {
            if (escape) escape = false;
            else if (character == L'\\') escape = true;
            else if (character == L'\"') inString = false;
            continue;
        }
        if (character == L'\"') inString = true;
        else if (character == L'{') {
            if (depth++ == 0) objectStart = position;
        } else if (character == L'}' && depth > 0) {
            if (--depth == 0 && objectStart != std::wstring_view::npos) {
                jobs.emplace_back(json.substr(
                    objectStart, position - objectStart + 1));
                objectStart = std::wstring_view::npos;
            }
        } else if (character == L']' && depth == 0) {
            break;
        }
    }
    return jobs;
}

Options optionsFromJob(std::wstring_view job)
{
    Options options;
    if (const auto value = jsonString(job, L"input")) options.input = *value;
    if (const auto value = jsonString(job, L"output")) options.output = *value;
    if (const auto value = jsonString(job, L"editingOutput")) {
        options.editingOutput = *value;
    }
    if (const auto value = jsonString(job, L"log")) options.logPath = *value;
    if (const auto value = jsonString(job, L"codec")) options.codec = *value;
    if (const auto value = jsonString(job, L"orientation")) {
        options.orientation = *value == L"lr"
            ? CrtXyOrientation::directLeftRight
            : CrtXyOrientation::midSide;
    }
    if (const auto value = jsonNumber(job, L"width")) {
        options.width = static_cast<std::uint32_t>(*value);
    }
    if (const auto value = jsonNumber(job, L"height")) {
        options.height = static_cast<std::uint32_t>(*value);
    }
    if (const auto value = jsonNumber(job, L"startSample")) {
        options.startSample = static_cast<std::uint64_t>(*value);
    }
    if (const auto value = jsonNumber(job, L"frames")) {
        options.contentFrames = static_cast<std::uint64_t>(*value);
    }
    if (const auto value = jsonNumber(job, L"tail")) {
        options.tailSeconds = *value;
    }
    if (const auto value = jsonNumber(job, L"seed")) {
        options.seed = static_cast<std::uint64_t>(*value);
    }
    if (const auto value = jsonString(job, L"backend")) {
        options.forceWarp = *value == L"warp";
    }
    return options;
}

void printResult(const Options& options, const RenderResult& result)
{
    std::wcout << L"RESULT {"
               << L"\"success\":" << (result.success ? L"true" : L"false")
               << L",\"input\":\"" << jsonEscape(options.input.wstring()) << L"\""
               << L",\"output\":\"" << jsonEscape(options.output.wstring()) << L"\""
               << L",\"content_frames\":" << result.contentFrames
               << L",\"tail_frames\":" << result.tailFrames
               << L",\"total_frames\":" << result.totalFrames
               << L",\"wall_seconds\":" << std::fixed << std::setprecision(6)
               << result.wallSeconds
               << L",\"x_realtime\":" << std::fixed << std::setprecision(6)
               << result.realtimeMultiple
               << L",\"backend\":\"" << (result.warp ? L"warp" : L"hardware")
               << L"\",\"adapter\":\"" << jsonEscape(result.adapter) << L"\""
               << L",\"render_width\":" << result.renderWidth
               << L",\"render_height\":" << result.renderHeight
               << L",\"readback_waits\":" << result.readbackWaits
               << L",\"error\":\"" << jsonEscape(result.error) << L"\"}"
               << std::endl;
}

bool parseUnsigned(const wchar_t* value, std::uint64_t& result)
{
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (value[consumed] != L'\0') return false;
        result = parsed;
        return true;
    } catch (...) { return false; }
}

bool parseOptions(int argc, wchar_t** argv, Options& options, std::wstring& error)
{
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        const auto value = [&]() -> const wchar_t* {
            if (index + 1 >= argc) return nullptr;
            return argv[++index];
        };
        if (argument == L"--warp") options.forceWarp = true;
        else if (argument == L"--self-test") options.selfTest = true;
        else if (argument == L"--input") {
            const auto* next = value(); if (!next) break; options.input = next;
        } else if (argument == L"--output") {
            const auto* next = value(); if (!next) break; options.output = next;
        } else if (argument == L"--editing-output") {
            const auto* next = value(); if (!next) break; options.editingOutput = next;
        } else if (argument == L"--log") {
            const auto* next = value(); if (!next) break; options.logPath = next;
        } else if (argument == L"--manifest") {
            const auto* next = value(); if (!next) break; options.manifest = next;
        } else if (argument == L"--self-test-output") {
            const auto* next = value(); if (!next) break; options.selfTestOutput = next;
        } else if (argument == L"--orientation") {
            const auto* next = value(); if (!next) break;
            const std::wstring mode = next;
            if (mode == L"ms") options.orientation = CrtXyOrientation::midSide;
            else if (mode == L"lr") {
                options.orientation = CrtXyOrientation::directLeftRight;
            } else {
                error = L"--orientation must be ms or lr"; return false;
            }
        } else if (argument == L"--fps") {
            const auto* next = value(); if (!next) break;
            if (std::wstring(next) != L"60/1" && std::wstring(next) != L"60") {
                error = L"Only exact 60/1 fps is supported"; return false;
            }
        } else if (argument == L"--codec") {
            const auto* next = value(); if (!next) break; options.codec = next;
        } else if (argument == L"--tail") {
            const auto* next = value(); if (!next) break;
            try { options.tailSeconds = std::stod(next); }
            catch (...) { error = L"Invalid --tail"; return false; }
        } else if (argument == L"--width" || argument == L"--height" ||
                   argument == L"--start-sample" || argument == L"--frames" ||
                   argument == L"--seed") {
            const auto* next = value(); if (!next) break;
            std::uint64_t parsed = 0;
            if (!parseUnsigned(next, parsed)) {
                error = L"Invalid numeric argument for " + argument;
                return false;
            }
            if (argument == L"--width") options.width = static_cast<std::uint32_t>(parsed);
            else if (argument == L"--height") options.height = static_cast<std::uint32_t>(parsed);
            else if (argument == L"--start-sample") options.startSample = parsed;
            else if (argument == L"--frames") options.contentFrames = parsed;
            else options.seed = parsed;
        } else {
            error = L"Unknown or incomplete argument: " + argument;
            return false;
        }
    }
    return true;
}

void usage()
{
    std::wcout
        << L"goden-crt-render --input <audio> --output <master.mkv> "
           L"[--editing-output <editing.mov>] [--width 1920] [--height 1080] "
           L"[--fps 60/1] [--orientation ms|lr] [--start-sample N] "
           L"[--frames N] [--tail 2.0] [--codec ffv1] [--seed 1] [--warp]\n"
           L"goden-crt-render --manifest <batch.json>\n"
           L"goden-crt-render --self-test --self-test-output <report.json>\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    Options options;
    std::wstring error;
    if (!parseOptions(argc, argv, options, error)) {
        std::wcerr << error << L"\n";
        usage();
        return 2;
    }
    if (options.selfTest) {
        if (options.selfTestOutput.empty()) {
            options.selfTestOutput = L"goden-crt-render-self-test.json";
        }
        if (!runSelfTest(options.selfTestOutput, error)) {
            std::wcerr << L"Self-test failed: " << error << L"\n";
            return 1;
        }
        std::wcout << L"Self-test passed: "
                   << options.selfTestOutput.wstring() << L"\n";
        return 0;
    }
    if (!options.manifest.empty()) {
        std::ifstream file(options.manifest, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        if (!file && bytes.empty()) {
            std::wcerr << L"Unable to read manifest\n";
            return 2;
        }
        auto json = wide(bytes);
        if (!json.empty() && json.front() == 0xfeff) json.erase(json.begin());
        const auto jobs = manifestJobs(json);
        if (jobs.empty()) {
            std::wcerr << L"Manifest contains no jobs\n";
            return 2;
        }
        bool success = true;
        for (const auto& job : jobs) {
            const auto jobOptions = optionsFromJob(job);
            const auto result = renderOne(jobOptions);
            printResult(jobOptions, result);
            success = success && result.success;
        }
        return success ? 0 : 1;
    }
    if (argc == 1) {
        usage();
        return 2;
    }
    const auto result = renderOne(options);
    printResult(options, result);
    return result.success ? 0 : 1;
}
