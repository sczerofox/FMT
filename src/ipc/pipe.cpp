#include "fmt/ipc/pipe.hpp"

#include <sddl.h>  // ConvertStringSecurityDescriptorToSecurityDescriptorW

#include <utility>

#include "fmt/common/string.hpp"

namespace fmt::ipc {
namespace {

// DACL 授权交互用户 + MIC 标签降到中完整性，一次设置到位（§13.9.2）。
// 只用 SetSecurityInfo 分开设 DACL 与标签很容易漏掉其中一个。
constexpr wchar_t kPipeSddl[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)S:(ML;;NW;;;ME)";

constexpr DWORD kPipeBufferBytes = 64 * 1024;

struct IoOutcome {
    bool ok = false;         // 本次操作成功拿到数据
    bool more_data = false;  // 消息模式下消息未读完（ERROR_MORE_DATA）
    bool timeout = false;
    bool closed = false;     // 对端关闭
    DWORD bytes = 0;
    DWORD error = 0;
};

IoOutcome do_read(HANDLE handle, HANDLE event, char* buffer, DWORD size, int timeout_ms) {
    IoOutcome outcome;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);

    const BOOL started = ReadFile(handle, buffer, size, nullptr, &overlapped);
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA) {
            outcome.error = error;
            outcome.closed = (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                              error == ERROR_HANDLE_EOF);
            return outcome;
        }
        if (error == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeout_ms));
            if (wait == WAIT_TIMEOUT) {
                CancelIoEx(handle, &overlapped);
                WaitForSingleObject(event, 1000);
                DWORD drained = 0;
                GetOverlappedResult(handle, &overlapped, &drained, TRUE);
                outcome.timeout = true;
                outcome.error = ERROR_TIMEOUT;
                return outcome;
            }
            if (wait != WAIT_OBJECT_0) {
                outcome.error = GetLastError();
                return outcome;
            }
        }
    }

    DWORD bytes = 0;
    if (GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
        outcome.ok = true;
        outcome.bytes = bytes;
        return outcome;
    }

    const DWORD error = GetLastError();
    outcome.error = error;
    outcome.bytes = bytes;
    if (error == ERROR_MORE_DATA) {
        // 消息比缓冲区大：这一段是有效的，继续读下一段。
        outcome.ok = true;
        outcome.more_data = true;
    } else if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
               error == ERROR_HANDLE_EOF) {
        outcome.closed = true;
    }
    return outcome;
}

IoOutcome do_write(HANDLE handle, HANDLE event, const char* data, DWORD size, int timeout_ms) {
    IoOutcome outcome;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);

    const BOOL started = WriteFile(handle, data, size, nullptr, &overlapped);
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            outcome.error = error;
            outcome.closed = (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                              error == ERROR_NO_DATA);
            return outcome;
        }
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeout_ms));
        if (wait == WAIT_TIMEOUT) {
            CancelIoEx(handle, &overlapped);
            WaitForSingleObject(event, 1000);
            DWORD drained = 0;
            GetOverlappedResult(handle, &overlapped, &drained, TRUE);
            outcome.timeout = true;
            outcome.error = ERROR_TIMEOUT;
            return outcome;
        }
        if (wait != WAIT_OBJECT_0) {
            outcome.error = GetLastError();
            return outcome;
        }
    }

    DWORD bytes = 0;
    if (GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
        outcome.ok = true;
        outcome.bytes = bytes;
        return outcome;
    }

    const DWORD error = GetLastError();
    outcome.error = error;
    outcome.closed = (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                      error == ERROR_NO_DATA);
    return outcome;
}

Error io_error(const IoOutcome& outcome, std::string_view what) {
    if (outcome.timeout) {
        return make_error(ErrorCode::ServiceOperationFailed, std::string(what) + "超时");
    }
    if (outcome.closed) {
        return make_error(ErrorCode::ServiceNotInstalled, std::string(what) + "：连接已关闭");
    }
    return make_error(ErrorCode::ServiceOperationFailed,
                      std::string(what) + "失败（Win32 " + std::to_string(outcome.error) + "）");
}

}  // namespace

// ---------------------------------------------------------------------------
// PipeConnection（服务端）
// ---------------------------------------------------------------------------

PipeConnection::PipeConnection(PipeConnection&& other) noexcept
    : handle_(other.handle_), event_(other.event_), buffer_(std::move(other.buffer_)) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.event_ = nullptr;
}

PipeConnection& PipeConnection::operator=(PipeConnection&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        event_ = other.event_;
        buffer_ = std::move(other.buffer_);
        other.handle_ = INVALID_HANDLE_VALUE;
        other.event_ = nullptr;
    }
    return *this;
}

PipeConnection::~PipeConnection() { reset(); }

void PipeConnection::reset() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        DisconnectNamedPipe(handle_);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (event_ != nullptr) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    buffer_.clear();
}

Result<PipeConnection> PipeConnection::accept(int timeout_ms) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        return make_error(ErrorCode::ServiceOperationFailed,
                          "生成管道安全描述符失败（Win32 " + std::to_string(GetLastError()) + "）");
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    HANDLE handle = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                     PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0,
                                     &attributes);
    const DWORD create_error = GetLastError();
    LocalFree(descriptor);

    if (handle == INVALID_HANDLE_VALUE) {
        return make_error(ErrorCode::ServiceOperationFailed,
                          "创建命名管道失败（Win32 " + std::to_string(create_error) + "）");
    }

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        CloseHandle(handle);
        return make_error(ErrorCode::ServiceOperationFailed, "创建事件对象失败");
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);

    const BOOL connected = ConnectNamedPipe(handle, &overlapped);
    if (!connected) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeout_ms));
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(handle, &overlapped);
                WaitForSingleObject(event, 1000);
                DWORD drained = 0;
                GetOverlappedResult(handle, &overlapped, &drained, TRUE);
                CloseHandle(event);
                CloseHandle(handle);
                return make_error(ErrorCode::ServiceOperationFailed, "等待客户端连接超时");
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
                const DWORD result_error = GetLastError();
                if (result_error != ERROR_PIPE_CONNECTED) {
                    CloseHandle(event);
                    CloseHandle(handle);
                    return make_error(ErrorCode::ServiceOperationFailed,
                                      "等待客户端连接失败（Win32 " +
                                          std::to_string(result_error) + "）");
                }
            }
        } else if (error != ERROR_PIPE_CONNECTED) {
            CloseHandle(event);
            CloseHandle(handle);
            return make_error(ErrorCode::ServiceOperationFailed,
                              "等待客户端连接失败（Win32 " + std::to_string(error) + "）");
        }
    }

    PipeConnection connection;
    connection.handle_ = handle;
    connection.event_ = event;
    return connection;
}

PipeConnection::Chunk PipeConnection::read_some(int timeout_ms) {
    Chunk chunk_outcome;
    char chunk[16 * 1024];
    const IoOutcome outcome = do_read(handle_, event_, chunk, sizeof(chunk), timeout_ms);
    if (!outcome.ok) {
        chunk_outcome.status = outcome.timeout ? ReadStatus::Timeout : ReadStatus::Closed;
        chunk_outcome.error = io_error(outcome, "读取请求");
        return chunk_outcome;
    }
    buffer_.append(chunk, outcome.bytes);
    chunk_outcome.status = ReadStatus::Ok;
    chunk_outcome.data.assign(chunk, outcome.bytes);
    return chunk_outcome;
}

RequestOutcome PipeConnection::read_request(int timeout_ms) {
    RequestOutcome outcome;
    while (true) {
        nlohmann::json value;
        const Result<bool> taken = take_frame(buffer_, &value);
        if (!ok(taken)) {
            outcome.status = ReadStatus::Failed;
            outcome.error = *error_of(taken);
            return outcome;
        }
        if (std::get<bool>(taken)) {
            Result<Request> request = request_from_json(value);
            if (!ok(request)) {
                outcome.status = ReadStatus::Failed;
                outcome.error = *error_of(request);
                return outcome;
            }
            outcome.status = ReadStatus::Ok;
            outcome.request = std::get<Request>(request);
            return outcome;
        }

        const Chunk chunk = read_some(timeout_ms);
        if (chunk.status != ReadStatus::Ok) {
            outcome.status = chunk.status;
            outcome.error = chunk.error;
            return outcome;
        }
        if (chunk.data.empty()) {
            outcome.status = ReadStatus::Closed;
            outcome.error = make_error(ErrorCode::ServiceNotInstalled, "客户端已断开");
            return outcome;
        }
    }
}

Status PipeConnection::write_all(std::string_view data, int timeout_ms) {
    std::size_t written = 0;
    while (written < data.size()) {
        const std::size_t remaining = data.size() - written;
        const DWORD chunk = static_cast<DWORD>(remaining > kPipeBufferBytes ? kPipeBufferBytes
                                                                           : remaining);
        const IoOutcome outcome = do_write(handle_, event_, data.data() + written, chunk, timeout_ms);
        if (!outcome.ok) {
            return io_error(outcome, "写入响应");
        }
        if (outcome.bytes == 0) {
            return make_error(ErrorCode::ServiceOperationFailed, "写入响应时连接中断");
        }
        written += outcome.bytes;
    }
    return std::monostate{};
}

Status PipeConnection::write_response(const Response& response, int timeout_ms) {
    return write_all(encode_frame(to_json(response)), timeout_ms);
}

// ---------------------------------------------------------------------------
// PipeClient（CLI 侧）
// ---------------------------------------------------------------------------

PipeClient::PipeClient(PipeClient&& other) noexcept
    : handle_(other.handle_), event_(other.event_), buffer_(std::move(other.buffer_)) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.event_ = nullptr;
}

PipeClient& PipeClient::operator=(PipeClient&& other) noexcept {
    if (this != &other) {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
        handle_ = other.handle_;
        event_ = other.event_;
        buffer_ = std::move(other.buffer_);
        other.handle_ = INVALID_HANDLE_VALUE;
        other.event_ = nullptr;
    }
    return *this;
}

PipeClient::~PipeClient() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (event_ != nullptr) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

Result<PipeClient> PipeClient::connect(int timeout_ms) {
    HANDLE handle = INVALID_HANDLE_VALUE;

    for (int attempt = 0; attempt < 2; ++attempt) {
        handle = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && attempt == 0) {
            // 实例被占满：等 3 秒再来一次（§13.9.4）。
            if (WaitNamedPipeW(kPipeName, static_cast<DWORD>(timeout_ms))) {
                continue;
            }
            return make_error(ErrorCode::ServiceNotInstalled, "无法连接 FMT Service，请先执行 service install");
        }
        if (error == ERROR_FILE_NOT_FOUND) {
            return make_error(ErrorCode::ServiceNotInstalled,
                              "无法连接 FMT Service，请先执行 service install");
        }
        if (error == ERROR_ACCESS_DENIED) {
            return make_error(ErrorCode::PermissionDenied,
                              "连接 FMT Service 被拒绝（管道权限或完整性级别不对）");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "连接命名管道失败（Win32 " + std::to_string(error) + "）");
    }

    if (handle == INVALID_HANDLE_VALUE) {
        return make_error(ErrorCode::ServiceNotInstalled, "无法连接 FMT Service，请先执行 service install");
    }

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        CloseHandle(handle);
        return make_error(ErrorCode::ServiceOperationFailed, "创建事件对象失败");
    }

    PipeClient client;
    client.handle_ = handle;
    client.event_ = event;
    return client;
}

Result<std::string> PipeClient::read_some(int timeout_ms) {
    char chunk[16 * 1024];
    const IoOutcome outcome = do_read(handle_, event_, chunk, sizeof(chunk), timeout_ms);
    if (!outcome.ok) {
        return io_error(outcome, "读取响应");
    }
    buffer_.append(chunk, outcome.bytes);
    return std::string(chunk, outcome.bytes);
}

Status PipeClient::write_all(std::string_view data, int timeout_ms) {
    std::size_t written = 0;
    while (written < data.size()) {
        const std::size_t remaining = data.size() - written;
        const DWORD chunk =
            static_cast<DWORD>(remaining > kPipeBufferBytes ? kPipeBufferBytes : remaining);
        const IoOutcome outcome =
            do_write(handle_, event_, data.data() + written, chunk, timeout_ms);
        if (!outcome.ok) {
            return io_error(outcome, "发送请求");
        }
        written += outcome.bytes;
    }
    return std::monostate{};
}

Status PipeClient::send(const Request& request, int timeout_ms) {
    return write_all(encode_frame(to_json(request)), timeout_ms);
}

Result<Response> PipeClient::receive(int expected_id, int timeout_ms) {
    while (true) {
        nlohmann::json value;
        const Result<bool> taken = take_frame(buffer_, &value);
        if (!ok(taken)) {
            return *error_of(taken);
        }
        if (std::get<bool>(taken)) {
            Result<Response> response = response_from_json(value);
            if (!ok(response)) {
                return *error_of(response);
            }
            if (std::get<Response>(response).id != expected_id) {
                return make_error(ErrorCode::ServiceOperationFailed,
                                  "响应 id 与请求不匹配（期望 " + std::to_string(expected_id) +
                                      "，收到 " + std::to_string(std::get<Response>(response).id) +
                                      "）");
            }
            return response;
        }

        Result<std::string> chunk = read_some(timeout_ms);
        if (!ok(chunk)) {
            return *error_of(chunk);
        }
        if (std::get<std::string>(chunk).empty()) {
            return make_error(ErrorCode::ServiceNotInstalled, "服务已断开");
        }
    }
}

Result<Response> PipeClient::call(const Request& request, int timeout_ms) {
    if (const Status status = send(request, timeout_ms); !ok(status)) {
        return *error_of(status);
    }
    return receive(request.id, timeout_ms);
}

}  // namespace fmt::ipc
