#include "fmt/ipc/protocol.hpp"

#include <cstring>

namespace fmt::ipc {
namespace {

constexpr std::size_t kHeaderBytes = 4;

void append_u32_le(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::uint32_t read_u32_le(const char* data) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[3])) << 24);
}

}  // namespace

nlohmann::json to_json(const Request& request) {
    nlohmann::json value = nlohmann::json::object();
    value["id"] = request.id;
    value["op"] = request.op;
    value["root"] = request.root;
    value["pid"] = request.pid;
    if (request.args.is_object() && !request.args.empty()) {
        value["args"] = request.args;
    }
    return value;
}

nlohmann::json to_json(const Response& response) {
    nlohmann::json value = nlohmann::json::object();
    value["id"] = response.id;
    value["ok"] = response.ok;
    if (response.ok) {
        value["data"] = response.data;
    } else {
        value["error"] = error_to_json(response.error);
    }
    return value;
}

nlohmann::json error_to_json(const Error& error) {
    nlohmann::json value = nlohmann::json::object();
    value["code"] = code_string(error.code);
    value["message"] = error.message;
    return value;
}

Result<Request> request_from_json(const nlohmann::json& value) {
    if (!value.is_object()) {
        return make_error(ErrorCode::JsonParseError, "请求必须是 JSON 对象");
    }

    Request request;
    if (const auto iterator = value.find("id"); iterator != value.end() && iterator->is_number()) {
        request.id = iterator->get<int>();
    }
    if (const auto iterator = value.find("pid"); iterator != value.end() && iterator->is_number()) {
        request.pid = iterator->get<unsigned long>();
    }
    if (const auto iterator = value.find("op"); iterator != value.end() && iterator->is_string()) {
        request.op = iterator->get<std::string>();
    }
    if (const auto iterator = value.find("root"); iterator != value.end() && iterator->is_string()) {
        request.root = iterator->get<std::string>();
    }
    if (const auto iterator = value.find("args");
        iterator != value.end() && iterator->is_object()) {
        request.args = *iterator;
    }

    if (request.op.empty()) {
        return make_error(ErrorCode::InvalidArgument, "请求缺少 op 字段");
    }
    return request;
}

Result<Response> response_from_json(const nlohmann::json& value) {
    if (!value.is_object()) {
        return make_error(ErrorCode::JsonParseError, "响应必须是 JSON 对象");
    }

    Response response;
    if (const auto iterator = value.find("id"); iterator != value.end() && iterator->is_number()) {
        response.id = iterator->get<int>();
    }
    if (const auto iterator = value.find("ok"); iterator != value.end() && iterator->is_boolean()) {
        response.ok = iterator->get<bool>();
    } else {
        return make_error(ErrorCode::JsonParseError, "响应缺少 ok 字段");
    }

    if (response.ok) {
        if (const auto iterator = value.find("data"); iterator != value.end()) {
            response.data = *iterator;
        }
        return response;
    }

    const auto iterator = value.find("error");
    if (iterator == value.end() || !iterator->is_object()) {
        return make_error(ErrorCode::JsonParseError, "失败响应缺少 error 对象");
    }

    std::string code = "FMT-001";
    std::string message;
    if (const auto field = iterator->find("code"); field != iterator->end() && field->is_string()) {
        code = field->get<std::string>();
    }
    if (const auto field = iterator->find("message");
        field != iterator->end() && field->is_string()) {
        message = field->get<std::string>();
    }

    bool known = false;
    response.error.code = code_from_string(code, &known);
    if (!known) {
        response.error.code = ErrorCode::InvalidArgument;
    }
    response.error.message = message.empty() ? std::string(default_message(response.error.code))
                                             : message;
    return response;
}

std::string encode_frame(const nlohmann::json& value) {
    const std::string payload = value.dump();
    std::string frame;
    frame.reserve(kHeaderBytes + payload.size());
    append_u32_le(frame, static_cast<std::uint32_t>(payload.size()));
    frame += payload;
    return frame;
}

Result<bool> take_frame(std::string& buffer, nlohmann::json* out) {
    if (out == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "take_frame 需要输出参数");
    }
    if (buffer.size() < kHeaderBytes) {
        return false;
    }

    const std::uint32_t length = read_u32_le(buffer.data());
    if (length == 0) {
        return make_error(ErrorCode::JsonParseError, "帧长度为 0");
    }
    if (length > kMaxFrameBytes) {
        return make_error(ErrorCode::JsonParseError,
                          "帧长度 " + std::to_string(length) + " 超过上限");
    }
    if (buffer.size() < kHeaderBytes + length) {
        return false;
    }

    const std::string payload = buffer.substr(kHeaderBytes, length);
    buffer.erase(0, kHeaderBytes + length);

    nlohmann::json value;
    try {
        value = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception& error) {
        return make_error(ErrorCode::JsonParseError,
                          std::string("帧内容不是合法 JSON：") + error.what());
    }

    *out = std::move(value);
    return true;
}

}  // namespace fmt::ipc
