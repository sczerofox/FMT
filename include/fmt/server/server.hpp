// 浏览器入口：HTTP 服务端
//
// CLI **不走这里**（CLI 走命名管道 \\.\pipe\fmt.control）；HTTP 只给浏览器与
// 调试用。两条入口汇入同一个 service 层，响应信封也是同一个。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"

namespace fmt::server {

// 业务命令处理器：由服务的运行体注入。
// 浏览器（HTTP）与 CLI（命名管道）**共用同一份业务实现**：HTTP 层只负责
// 路由、参数与状态码，不认识任何业务对象。
using BusinessHandler =
    std::function<Result<nlohmann::json>(const std::string& operation, const nlohmann::json& args)>;

// token 校验器：传进来的 token 有效就返回**用户名**，无效返回空串。
// 由服务的运行体注入（它才拿得到 data/user.json 与 config）。
// **默认拒绝**：没注入校验器时，管理接口一律 401——HTTP 层不认识账号，
// 但它必须默认是关着的，免得哪天忘了注入就变成裸奔。
using TokenVerifier = std::function<std::string(const std::string& token)>;

// 上传上限的**取值器**：每次上传请求现取一次（不是启动时的快照）。
// 为什么是取值器而不是一个数：上限住在 config.json 里，`config set max_upload_size`
// 只改服务进程里的值、不重启 HTTP 监听；拿快照会让「调大上限」当场失效
//（HTTP 层还按旧的小值中止接收）。0 = 不限（测试可以这样传）。
// HTTP 层判的是**收到的字节**（multipart 的普通字段也算），入库前业务层还会按
// 配置里的值再判一次「文件本身」的大小，所以这一层偏大也不会放过超限文件。
using UploadLimitProvider = std::function<std::uintmax_t()>;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 绑定端口并起监听线程。port 传 0 表示让系统分配（测试用）。
    // 绑定失败（端口被占用等）返回错误：调用方记 ERROR 日志，但**不中断**其他功能。
    Status start(const std::string& host, int port, std::string data_root, Logger* logger,
                 BusinessHandler handler, TokenVerifier verifier,
                 UploadLimitProvider upload_limit = nullptr);

    // 停止监听并等线程结束；可重复调用。
    void stop();

    bool running() const;

    // 实际监听的地址与端口（port 传 0 时读回系统分配的真实端口）。
    const std::string& host() const;
    int port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fmt::server
