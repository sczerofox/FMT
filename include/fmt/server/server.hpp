// 浏览器入口：HTTP 服务端
//
// CLI **不走这里**（CLI 走命名管道 \\.\pipe\fmt.control）；HTTP 只给浏览器与
// 调试用。两条入口汇入同一个 service 层，响应信封也是同一个。
#pragma once

#include <memory>
#include <string>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"

namespace fmt::server {

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 绑定端口并起监听线程。port 传 0 表示让系统分配（测试用）。
    // 绑定失败（端口被占用等）返回错误：调用方记 ERROR 日志，但**不中断**其他功能。
    Status start(const std::string& host, int port, std::string data_root, Logger* logger);

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
