#pragma once

#include <windows.h>
#include <winhttp.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>

namespace nekorox::component {

// One issuing thread per handle. Callbacks only publish bounded completion
// records; they never chain WinHTTP operations or touch the client's policy.
// Cancellation closes AFTER the issuing API has returned, then drains the
// final HANDLE_CLOSING before freeing context or an in-flight caller buffer.
class DiscordHttpIo final {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    explicit DiscordHttpIo(bool websocket = false) : websocket_(websocket) {}
    ~DiscordHttpIo() { close(); }
    DiscordHttpIo(const DiscordHttpIo&) = delete;
    DiscordHttpIo& operator=(const DiscordHttpIo&) = delete;

    bool attach(HINTERNET handle, bool inheritedCallback = false)
    {
        handle_ = handle;
        if (!handle_) return false;
        // CompleteUpgrade inherits the request's callback and receives this
        // socket's distinct context, even if later configuration fails.
        closingCallback_ = inheritedCallback;
        DWORD_PTR context = contextValue();
        if (!WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE,
                &context, sizeof(context))) return false;
        if (WinHttpSetStatusCallback(handle_, callback,
                WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                    WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
                WINHTTP_INVALID_STATUS_CALLBACK) return false;
        closingCallback_ = true;
        return true;
    }

    [[nodiscard]] HINTERNET handle() const noexcept { return handle_; }
    [[nodiscard]] DWORD_PTR contextValue() noexcept
    { return reinterpret_cast<DWORD_PTR>(this); }
    [[nodiscard]] DWORD bytes() const noexcept { return bytes_; }
    [[nodiscard]] WINHTTP_WEB_SOCKET_BUFFER_TYPE type() const noexcept { return type_; }
    std::array<std::uint8_t, 4096> buffer{};

    static DWORD accepted(BOOL result) noexcept
    { return result ? ERROR_SUCCESS : GetLastError(); }

    template<class Issue>
    bool perform(DWORD completion, std::stop_token token,
        Deadline deadline, Issue&& issue)
    {
        if (!handle_ || token.stop_requested() ||
                std::chrono::steady_clock::now() >= deadline) return false;
        {
            std::lock_guard lock(mutex_);
            expected_ = completion;
            completed_ = false;
            error_ = ERROR_SUCCESS;
            bytes_ = 0;
        }
        // A completion may run inline here. Never hold the callback mutex
        // across WinHTTP, and never issue another API before this returns.
        const auto result = issue();
        bool succeeded = false;
        if (result == ERROR_SUCCESS || result == ERROR_IO_PENDING) {
            std::unique_lock lock(mutex_);
            const auto ready = [&] { return completed_ || closed_; };
            if (deadline == Deadline::max()) changed_.wait(lock, token, ready);
            else changed_.wait_until(lock, token, deadline, ready);
            succeeded = completed_ && !closed_ && error_ == ERROR_SUCCESS &&
                !token.stop_requested() &&
                std::chrono::steady_clock::now() < deadline;
        }
        // Caller-owned send bytes may leave scope as soon as we return.
        // A failed/cancelled operation must drain before that can happen.
        if (!succeeded) close();
        return succeeded;
    }

    void beginClose() noexcept
    {
        const auto handle = std::exchange(handle_, nullptr);
        if (!handle) return;
        closeInitiated_ = true;
        WinHttpCloseHandle(handle);
    }

    void close() noexcept
    {
        beginClose();
        if (closingCallback_ && closeInitiated_) {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return closed_; });
        }
    }

    bool readResponse(std::string& output, std::stop_token token, Deadline deadline,
        std::size_t maximumBytes)
    {
        output.clear();
        for (;;) {
            if (!perform(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, token, deadline, [&] {
                    return accepted(WinHttpReadData(handle_, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr));
                })) return false;
            if (bytes_ == 0) return true;
            if (bytes_ > buffer.size() ||
                    bytes_ > maximumBytes - output.size()) return false;
            output.append(reinterpret_cast<const char*>(buffer.data()), bytes_);
        }
    }

private:
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status,
        void* information, DWORD length) noexcept
    {
        auto* self = reinterpret_cast<DiscordHttpIo*>(context);
        if (!self) return;
        std::lock_guard lock(self->mutex_);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            self->closed_ = true;
        } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            self->error_ = ERROR_INVALID_DATA;
            if (information && length >= sizeof(WINHTTP_ASYNC_RESULT)) {
                self->error_ = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
                if (self->error_ == ERROR_SUCCESS) self->error_ = ERROR_INVALID_DATA;
            }
            self->completed_ = true;
        } else if (status == self->expected_) {
            if (self->websocket_ && (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ||
                    status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)) {
                if (information && length >= sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
                    const auto* value = static_cast<WINHTTP_WEB_SOCKET_STATUS*>(information);
                    self->bytes_ = value->dwBytesTransferred;
                    self->type_ = value->eBufferType;
                } else self->error_ = ERROR_INVALID_DATA;
            } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
                self->bytes_ = length;
                if (length > self->buffer.size() ||
                        (length && information != self->buffer.data())) {
                    self->error_ = ERROR_INVALID_DATA;
                }
            }
            self->completed_ = true;
        }
        // The final callback's waiter may free this object after unlock.
        // Notify while locked and perform no context access afterward.
        self->changed_.notify_all();
    }

    HINTERNET handle_ = nullptr;
    bool websocket_ = false;
    bool closingCallback_ = false;
    bool closeInitiated_ = false;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    DWORD expected_ = 0;
    DWORD error_ = ERROR_SUCCESS;
    DWORD bytes_ = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type_{};
    bool completed_ = false;
    bool closed_ = false;
};

} // namespace nekorox::component
