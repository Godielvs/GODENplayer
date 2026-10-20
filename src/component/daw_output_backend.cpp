#include "qa_hooks.h"
#include "daw_output_backend.h"

#include <algorithm>
#include <array>
#include <cwchar>

namespace nekorox::component {
namespace {

bool qaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

std::uint32_t readUnsigned(const wchar_t* name, std::uint32_t maximum) noexcept
{
    wchar_t text[32]{};
    const auto length = GetEnvironmentVariableW(
        name, text, static_cast<DWORD>(std::size(text)));
    if (length == 0 || length >= std::size(text)) return 0;
    wchar_t* end = nullptr;
    const auto value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0') return 0;
    return std::min<std::uint32_t>(
        static_cast<std::uint32_t>(value), maximum);
}

} // namespace

DawOutputBackend& DawOutputBackend::instance() noexcept
{
    static DawOutputBackend backend;
    return backend;
}

void DawOutputBackend::refreshQaPolicy() noexcept
{
    fault_ = Fault::none;
    writeDelayMs_ = 0;
    failAfterWrites_ = 0;
    writes_ = 0;
    if (!qaHooksAllowed()) return;

    wchar_t text[32]{};
    const auto length = GetEnvironmentVariableW(
        L"NEKOROX_QA_DAW_FAULT", text, static_cast<DWORD>(std::size(text)));
    if (length != 0 && length < std::size(text)) {
        if (_wcsicmp(text, L"open") == 0) fault_ = Fault::open;
        else if (_wcsicmp(text, L"prepare") == 0) fault_ = Fault::prepare;
        else if (_wcsicmp(text, L"write") == 0) fault_ = Fault::write;
        else if (_wcsicmp(text, L"reset") == 0) fault_ = Fault::reset;
        else if (_wcsicmp(text, L"unprepare") == 0) fault_ = Fault::unprepare;
        else if (_wcsicmp(text, L"close") == 0) fault_ = Fault::close;
        else if (_wcsicmp(text, L"completion-stall") == 0)
            fault_ = Fault::completionStall;
    }
    writeDelayMs_ = readUnsigned(L"NEKOROX_QA_DAW_WRITE_DELAY_MS", 5000U);
    failAfterWrites_ = readUnsigned(
        L"NEKOROX_QA_DAW_FAIL_AFTER_WRITES", 100000U);
}

bool DawOutputBackend::fail(Fault operation) noexcept
{
    if (fault_ != operation) return false;
    if (operation != Fault::write || failAfterWrites_ == 0) return true;
    return writes_ >= failAfterWrites_;
}

MMRESULT DawOutputBackend::open(
    HWAVEOUT* output,
    const WAVEFORMATEX* format,
    HANDLE completionEvent) noexcept
{
    refreshQaPolicy();
    if (fail(Fault::open)) return MMSYSERR_ERROR;
    return waveOutOpen(output, WAVE_MAPPER, format,
        reinterpret_cast<DWORD_PTR>(completionEvent), 0, CALLBACK_EVENT);
}

MMRESULT DawOutputBackend::prepare(
    HWAVEOUT output, WAVEHDR* header) noexcept
{
    if (fail(Fault::prepare)) return MMSYSERR_ERROR;
    return waveOutPrepareHeader(output, header, sizeof(*header));
}

MMRESULT DawOutputBackend::write(HWAVEOUT output, WAVEHDR* header) noexcept
{
    if (writeDelayMs_ != 0) Sleep(writeDelayMs_);
    const auto shouldFail = fail(Fault::write);
    ++writes_;
    if (shouldFail) return MMSYSERR_ERROR;
    return waveOutWrite(output, header, sizeof(*header));
}

bool DawOutputBackend::completed(const WAVEHDR* header) const noexcept
{
    return fault_ != Fault::completionStall && header != nullptr &&
        (header->dwFlags & WHDR_DONE) != 0;
}

MMRESULT DawOutputBackend::reset(HWAVEOUT output) noexcept
{
    const auto result = waveOutReset(output);
    return fail(Fault::reset) ? MMSYSERR_ERROR : result;
}

MMRESULT DawOutputBackend::unprepare(
    HWAVEOUT output, WAVEHDR* header) noexcept
{
    const auto result = waveOutUnprepareHeader(output, header, sizeof(*header));
    return fail(Fault::unprepare) ? MMSYSERR_ERROR : result;
}

MMRESULT DawOutputBackend::close(HWAVEOUT output) noexcept
{
    const auto result = waveOutClose(output);
    return fail(Fault::close) ? MMSYSERR_ERROR : result;
}

} // namespace nekorox::component
