// 服务端运行体：当前数据根 + CLI 连接处理
//
// 服务是唯一写 data/*.json 的进程。CLI 连上来先发 hello 声明数据根，服务比较
// 后按需切换（切换不删旧根数据），之后的命令都在该根下执行。
//
// V1 串行处理连接：单实例互斥体保证同时只有一个 CLI 窗口，一条连接上可以连续
// 发多条命令，所以串行够用，也免去了并发切换数据根的麻烦。
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "fmt/common/error.hpp"
#include "fmt/core/app.hpp"
#include "fmt/ipc/pipe.hpp"
#include "fmt/service/service.hpp"

namespace fmt::service {

// 服务形态的入口结果。
enum class HostResult {
    BecameService,  // 本进程被 SCM 启动，已经跑完整个服务生命周期
    NotService,     // 不是 SCM 启动（用户双击 / 命令行）→ 调用方走 CLI
    Failed,         // 无法接入服务控制管理器（FMT-602 / 退出码 8）
};

// 交给 SCM：被启动时在内部建好 ServerRuntime 并进入服务主循环。
// 必须在 wmain 开头尽早调用（SCM 只等 30 秒）。
HostResult run_service_host();

class ServerRuntime {
public:
    // state_directory 默认为 %ProgramData%\FMT；测试可以换成临时目录。
    explicit ServerRuntime(std::filesystem::path fallback_root,
                           std::filesystem::path state_directory = state_directory_default());

    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 确定数据根（service.json 的记录值优先，否则 fallback_root）并做幂等初始化。
    Status start();

    // 阻塞式 accept 循环，直到 request_stop()。
    void run();

    void request_stop();

    // 处理一个请求（可以脱离管道单独测试）。
    ipc::Response handle(const ipc::Request& request);

    std::filesystem::path current_root() const;

private:
    static std::filesystem::path state_directory_default();

    Status apply_root(const std::string& requested_root, std::string* effective_root);
    void serve(ipc::PipeConnection connection);

    mutable std::mutex mutex_;
    std::unique_ptr<AppContext> context_;
    std::filesystem::path fallback_root_;
    std::filesystem::path state_directory_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace fmt::service
