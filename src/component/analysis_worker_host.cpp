#include "analysis_worker_host.h"

#include "sdk_compat.h"

#include <algorithm>
#include <limits>

namespace nekorox::component {
namespace {

void closeHandle(HANDLE& handle) noexcept
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

std::wstring quoteArgument(std::wstring_view value)
{
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2U + 1U, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

std::filesystem::path workerPath()
{
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const auto length = GetModuleFileNameW(core_api::get_my_instance(),
            buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length + 1U < buffer.size()) {
            const auto component = std::filesystem::path(
                std::wstring(buffer.data(), length));
            return component.parent_path() / L"nekorox-analysis.exe";
        }
        if (buffer.size() >= 32768U) return {};
        buffer.resize(buffer.size() * 2U);
    }
}

bool readExact(HANDLE input, void* destination, std::size_t bytes) noexcept
{
    auto* cursor = static_cast<std::byte*>(destination);
    while (bytes > 0) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes, std::numeric_limits<DWORD>::max()));
        DWORD received = 0;
        if (!ReadFile(input, cursor, chunk, &received, nullptr) || received == 0) {
            return false;
        }
        cursor += received;
        bytes -= received;
    }
    return true;
}

std::wstring windowsError(const wchar_t* operation)
{
    const auto code = GetLastError();
    wchar_t* systemText = nullptr;
    const auto size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&systemText), 0, nullptr);
    std::wstring message = operation;
    message += L" failed";
    if (size > 0 && systemText != nullptr) {
        message += L": ";
        message.append(systemText, size);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    } else {
        message += L" (Win32 ";
        message += std::to_wstring(code);
        message += L')';
    }
    if (systemText != nullptr) LocalFree(systemText);
    return message;
}

} // namespace

bool launchAnalysisWorker(std::wstring_view command, std::uint64_t requestId,
    const std::filesystem::path& localTrack, SIZE_T processMemoryLimit,
    AnalysisWorkerHandles& handles, std::wstring& error, bool deferStart)
{
    closeAnalysisWorker(handles);
    error.clear();
    if ((command != L"analyze" && command != L"cost") || requestId == 0 ||
        processMemoryLimit == 0) {
        error = L"Analysis worker launch arguments are invalid.";
        return false;
    }
    std::error_code fileError;
    const auto executable = workerPath();
    if (!std::filesystem::is_regular_file(executable, fileError)) {
        error = L"The native analysis worker is missing.";
        return false;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE pipeWrite = nullptr;
    if (!CreatePipe(&handles.output, &pipeWrite, &security, 256U * 1024U)) {
        error = windowsError(L"Analysis pipe creation");
        return false;
    }
    if (!SetHandleInformation(handles.output, HANDLE_FLAG_INHERIT, 0)) {
        error = windowsError(L"Analysis pipe inheritance");
        closeHandle(pipeWrite);
        closeAnalysisWorker(handles);
        return false;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    handles.job = CreateJobObjectW(nullptr, nullptr);
    if (handles.job == nullptr || nullInput == INVALID_HANDLE_VALUE ||
        nullOutput == INVALID_HANDLE_VALUE) {
        error = windowsError(L"Analysis containment setup");
        closeHandle(pipeWrite);
        closeHandle(nullInput);
        closeHandle(nullOutput);
        closeAnalysisWorker(handles);
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = processMemoryLimit;
    if (!SetInformationJobObject(handles.job,
            JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = windowsError(L"Analysis job limits");
        closeHandle(pipeWrite);
        closeHandle(nullInput);
        closeHandle(nullOutput);
        closeAnalysisWorker(handles);
        return false;
    }
    JOBOBJECT_BASIC_UI_RESTRICTIONS uiRestrictions{};
    uiRestrictions.UIRestrictionsClass =
        JOB_OBJECT_UILIMIT_DESKTOP |
        JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
        JOB_OBJECT_UILIMIT_EXITWINDOWS |
        JOB_OBJECT_UILIMIT_GLOBALATOMS |
        JOB_OBJECT_UILIMIT_READCLIPBOARD |
        JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
        JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!SetInformationJobObject(handles.job, JobObjectBasicUIRestrictions,
            &uiRestrictions, sizeof(uiRestrictions))) {
        error = windowsError(L"Analysis job UI restrictions");
        closeHandle(pipeWrite);
        closeHandle(nullInput);
        closeHandle(nullOutput);
        closeAnalysisWorker(handles);
        return false;
    }

    std::wstring commandLine = quoteArgument(executable.native()) + L' ' +
        std::wstring(command) + L' ' + std::to_wstring(requestId) + L' ' +
        quoteArgument(localTrack.native());
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = pipeWrite;
    startup.StartupInfo.hStdError = nullOutput;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<
        LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    HANDLE inherited[]{nullInput, pipeWrite, nullOutput};
    const auto attributeInitialized = attributeBytes != 0 &&
        InitializeProcThreadAttributeList(
            startup.lpAttributeList, 1, 0, &attributeBytes);
    const auto allowlistInstalled = attributeInitialized &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
            nullptr, nullptr);
    if (!allowlistInstalled) {
        error = windowsError(L"Analysis handle allowlist");
        if (attributeInitialized) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        }
        closeHandle(pipeWrite);
        closeHandle(nullInput);
        closeHandle(nullOutput);
        closeAnalysisWorker(handles);
        return false;
    }
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(executable.c_str(), mutableCommand.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
            EXTENDED_STARTUPINFO_PRESENT,
        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    closeHandle(pipeWrite);
    closeHandle(nullInput);
    closeHandle(nullOutput);
    if (!created) {
        error = windowsError(L"Analysis worker launch");
        closeAnalysisWorker(handles);
        return false;
    }
    handles.process = process.hProcess;
    if (!AssignProcessToJobObject(handles.job, handles.process)) {
        error = windowsError(L"Analysis worker containment");
        TerminateProcess(handles.process, ERROR_ACCESS_DENIED);
        closeHandle(process.hThread);
        closeAnalysisWorker(handles);
        return false;
    }
    if (deferStart) {
        handles.startThread = process.hThread;
        return true;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        error = windowsError(L"Analysis worker resume");
        TerminateJobObject(handles.job, ERROR_CANCELLED);
        closeHandle(process.hThread);
        closeAnalysisWorker(handles);
        return false;
    }
    closeHandle(process.hThread);
    return true;
}

bool readAnalysisWorkerFrame(HANDLE output, std::uint64_t expectedRequestId,
    AnalysisFrameHeaderV1& header, std::vector<std::byte>& payload,
    std::string& error)
{
    payload.clear();
    error.clear();
    if (!readExact(output, &header, sizeof(header))) return false;
    if (!validAnalysisFrameHeader(header) ||
        header.requestId != expectedRequestId) {
        error = "The analysis worker returned an invalid protocol frame.";
        return false;
    }
    payload.resize(header.payloadBytes);
    if (!payload.empty() &&
        !readExact(output, payload.data(), payload.size())) {
        payload.clear();
        error = "The analysis worker ended inside a protocol frame.";
        return false;
    }
    return true;
}

bool cancelAnalysisWorker(
    AnalysisWorkerHandles& handles, DWORD waitMilliseconds) noexcept
{
    bool stopped = true;
    if (handles.process != nullptr && handles.process != INVALID_HANDLE_VALUE) {
        // Give a worker which is already finishing a short grace period.  The
        // Job Object remains the hard containment boundary; closing it first
        // made every cancellation an immediate kill and could strand the UI
        // while its reader thread unwound.
        auto waitResult = WaitForSingleObject(handles.process, waitMilliseconds);
        if (waitResult == WAIT_TIMEOUT &&
            handles.job != nullptr && handles.job != INVALID_HANDLE_VALUE) {
            stopped = TerminateJobObject(handles.job, ERROR_CANCELLED) != FALSE;
            waitResult = WaitForSingleObject(handles.process, 1000);
        }
        stopped = stopped && waitResult == WAIT_OBJECT_0;
    }
    closeHandle(handles.job);
    return stopped;
}

void closeAnalysisWorker(AnalysisWorkerHandles& handles) noexcept
{
    closeHandle(handles.startThread);
    closeHandle(handles.output);
    closeHandle(handles.process);
    closeHandle(handles.job);
}

} // namespace nekorox::component
