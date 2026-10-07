// CLI 与服务之间的帧协议
//
// 帧格式：`[4 字节小端长度][UTF-8 JSON]`，长度只算 payload。
// 一请求一响应，靠请求里的 id 配对（docx/FMT 技术文档.md §13.9）。
//
// 这一层是纯数据：不碰管道句柄，因此可以脱离 Windows API 单独测试。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"

namespace fmt::ipc {

// 单帧上限，防止一个坏长度字段把内存吃光。
inline constexpr std::size_t kMaxFrameBytes = 64ULL * 1024 * 1024;

// 连接超时与普通命令超时（§13.9.4）。
inline constexpr int kConnectTimeoutMs = 3000;
inline constexpr int kCommandTimeoutMs = 30000;

// 上传是长任务：下载可能几分钟到几十分钟，**不能**按普通命令的 30 秒算。
// 客户端等这么久才放弃；服务端自己的连接/发送/接收超时另有设定（http_client）。
// 真到了这个上限，CLI 会明确提示「服务端可能仍在处理，用 file list 确认」，
// 不让用户把「等超时」误当成「没入库」。
inline constexpr int kUploadTimeoutMs = 30 * 60 * 1000;

// 请求：{"id":N,"op":"...","root":"...","pid":N}
struct Request {
    int id = 0;
    std::string op;    // hello / bucket.* / file.* / share.* / trash.* / config.*
    std::string root;  // CLI 声明的数据根
    unsigned long pid = 0;
    nlohmann::json args = nlohmann::json::object();
};

// 响应：{"id":N,"ok":true,"data":...} 或 {"id":N,"ok":false,"error":{"code":..,"message":..}}
// 与 HTTP 响应体共用同一个信封。
struct Response {
    int id = 0;
    bool ok = true;
    nlohmann::json data = nlohmann::json::object();
    Error error;
};

nlohmann::json to_json(const Request& request);
nlohmann::json to_json(const Response& response);

// 解析失败返回 JsonParseError；op 为空返回 InvalidArgument。
Result<Request> request_from_json(const nlohmann::json& value);
Result<Response> response_from_json(const nlohmann::json& value);

// 编码一帧（4 字节小端长度 + payload）。
std::string encode_frame(const nlohmann::json& value);

// 从缓冲区头部取出一帧：取到返回 true 并从 buffer 中移除该帧；不足一帧返回
// false 且不动 buffer；长度越界或 JSON 非法返回错误（调用方应断开连接）。
Result<bool> take_frame(std::string& buffer, nlohmann::json* out);

}  // namespace fmt::ipc
