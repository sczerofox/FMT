// 流式 HTTP(S) 下载客户端（Windows WinHTTP + Schannel）
//
// **为什么不用 cpp-httplib 的 Client**：它要 OpenSSL 才能走 https，而本项目
// 用 `/MT` 静态 CRT、构建只依赖 vendored 单头文件（不引 Perl/NASM）。WinHTTP
// 是系统组件，TLS 走 Schannel（系统证书库），**不分发任何 DLL**，还自带
// **系统代理**支持——访问国内 https 站点基本都要走代理。
//
// 这里只做「下载需要的那点事」：GET、跟随重定向、超时、流式回调。
// 浏览器入口（HTTP 服务端）仍然用 cpp-httplib。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fmt/common/error.hpp"

namespace fmt {

// 是不是需要走网络的来源（http:// 或 https://）。
bool is_remote_url(std::string_view source);

struct HttpDownloadRequest {
    std::string url;
    std::string user_agent = "FileManagerTool/1.0";
    int connect_timeout_ms = 10000;
    int send_timeout_ms = 30000;
    int receive_timeout_ms = 300000;  // 单次读取的空闲超时，不是总时长
    bool follow_redirects = true;
};

struct HttpDownloadResult {
    int status = 0;
    std::uintmax_t bytes = 0;                 // 实际交给 sink 的字节数
    std::uintmax_t content_length = 0;        // 服务器声明的长度
    bool has_content_length = false;
    std::string content_type;
    bool aborted = false;                     // sink 主动中止（例如超过大小上限）
};

// 流式下载。只有 2xx 的响应体会交给 sink；非 2xx 的响应体直接丢弃，
// 状态码照实返回（由调用方决定报什么错）。
//
// sink 返回 false 表示调用方要求中止：函数返回成功且 aborted = true。
// 网络/协议层的失败返回 ErrorCode::DownloadFailed 或 DownloadTimeout。
Result<HttpDownloadResult> http_download(
    const HttpDownloadRequest& request,
    const std::function<bool(const char* data, std::size_t size)>& sink);

}  // namespace fmt
