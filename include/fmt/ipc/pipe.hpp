// 命名管道：服务端监听 + 客户端连接
//
// 管道名 \\.\pipe\fmt.control，消息模式、多实例、64 KB 缓冲（技术文档 §13.9.1）。
//
// 两个必须处理的 Windows 坑（§13.9.2）：
//   1. 服务以 LocalSystem 运行，默认 DACL 只允许 SYSTEM / Administrators，
//      普通用户连接会直接 ERROR_ACCESS_DENIED —— 必须用 SDDL 授权交互用户；
//   2. 高完整性进程创建的对象带高完整性标签，中完整性 CLI 会被「禁止向上写」
//      挡住 —— 必须在同一个 SDDL 里加 MIC 标签 S:(ML;;NW;;;ME)。
//
// 读写都用重叠 I/O，因为 ConnectNamedPipe / ReadFile 必须能超时返回：
// 服务停止时不能有线程无限期挂在读上。
#pragma once

#include <windows.h>

#include <string>

#include "fmt/common/error.hpp"
#include "fmt/ipc/protocol.hpp"

namespace fmt::ipc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\fmt.control";

// 读一个请求的结果。服务端必须能区分这三种情况：
//   客户端只是还没发命令（Timeout）  -> 继续等，不能断开
//   客户端真的走了（Closed）        -> 结束这条连接
//   其余（Failed）                  -> 记日志后断开
enum class ReadStatus { Ok, Timeout, Closed, Failed };

struct RequestOutcome {
    ReadStatus status = ReadStatus::Failed;
    Request request;
    Error error;
};

// 服务端的一条连接。accept() 会创建一个新实例并阻塞到有客户端连上或超时。
class PipeConnection {
public:
    PipeConnection() = default;
    ~PipeConnection();

    PipeConnection(PipeConnection&& other) noexcept;
    PipeConnection& operator=(PipeConnection&& other) noexcept;
    PipeConnection(const PipeConnection&) = delete;
    PipeConnection& operator=(const PipeConnection&) = delete;

    // 建实例 + 等连接。超时返回 FMT-602。
    static Result<PipeConnection> accept(int timeout_ms);

    // 读一个完整请求。超时不算错误（ReadStatus::Timeout），调用方继续等即可。
    RequestOutcome read_request(int timeout_ms);

    // 写一个响应。
    Status write_response(const Response& response, int timeout_ms);

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE native_handle() const { return handle_; }

private:
    void reset();

    struct Chunk {
        ReadStatus status = ReadStatus::Failed;
        std::string data;
        Error error;
    };

    Chunk read_some(int timeout_ms);
    Status write_all(std::string_view data, int timeout_ms);

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    std::string buffer_;
};

// 客户端。只连一次，之后可以复用这条连接发多条命令。
class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();

    PipeClient(PipeClient&& other) noexcept;
    PipeClient& operator=(PipeClient&& other) noexcept;
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // 连接失败分类（§13.9.4）：
    //   管道不存在   -> FMT-601 服务未安装 / 未运行
    //   实例被占满   -> WaitNamedPipeW 重试一次
    //   权限被拒     -> FMT-004（DACL 或 MIC 不对）
    static Result<PipeClient> connect(int timeout_ms);

    // 发一帧请求。
    Status send(const Request& request, int timeout_ms);

    // 收一帧响应，并校验 id 与请求一致。
    Result<Response> receive(int expected_id, int timeout_ms);

    // send + receive。
    Result<Response> call(const Request& request, int timeout_ms);

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    Result<std::string> read_some(int timeout_ms);
    Status write_all(std::string_view data, int timeout_ms);

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    std::string buffer_;
};

}  // namespace fmt::ipc
