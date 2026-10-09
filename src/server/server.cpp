#include "fmt/server/server.hpp"

// winsock2 必须在 windows.h 之前；配合全局的 WIN32_LEAN_AND_MEAN，
// 避免 windows.h 拉进 winsock v1 造成结构体重定义。
#include <winsock2.h>

#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include <cpp-httplib/httplib.h>

#include "fmt/common/envelope.hpp"
#include "fmt/common/string.hpp"
#include "fmt/version.hpp"

namespace fmt::server {
namespace {

constexpr const char* kJsonContentType = "application/json; charset=utf-8";

// 让监听线程与路由处理器共享的只读状态，生命周期不受 HttpServer 成员顺序影响。
struct SharedState {
    std::string data_root;
    Logger* logger = nullptr;
};

std::string dump(const nlohmann::json& value) { return value.dump(2); }

// 错误码 -> HTTP 状态码。映射表见技术文档 12.5：
// 400 参数错误 / 403 权限与不可用 / 404 不存在 / 409 冲突 / 500 内部错误。
int http_status_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidArgument:
        case ErrorCode::ConfirmRequired:
        case ErrorCode::PathTooLong:
        case ErrorCode::PathEscape:
        case ErrorCode::FileNameEmpty:
        case ErrorCode::FileNameInvalidChar:
        case ErrorCode::FileNameSeparator:
        case ErrorCode::FileNameReserved:
        case ErrorCode::FileNameTooLong:
        case ErrorCode::FileNameLikeFileId:
        case ErrorCode::BucketNameInvalid:
        case ErrorCode::UrlInvalid:
        case ErrorCode::SizeLimitExceeded:
        case ErrorCode::HttpRequestInvalid:
        case ErrorCode::PreviewUnsupported:
            return 400;
        case ErrorCode::PermissionDenied:
        case ErrorCode::ShareExpired:
        case ErrorCode::ShareDownloadLimitReached:
        case ErrorCode::ShareFileUnavailable:
            return 403;
        case ErrorCode::FileNotFound:
        case ErrorCode::BucketNotFound:
        case ErrorCode::TrashEntryNotFound:
        case ErrorCode::ShareNotFound:
        case ErrorCode::NoCurrentBucket:
        case ErrorCode::RestoreBucketMissing:
            return 404;
        case ErrorCode::FileAlreadyExists:
        case ErrorCode::FileNameConflict:
        case ErrorCode::BucketAlreadyExists:
        case ErrorCode::BucketInUse:
        case ErrorCode::Md5Duplicate:
        case ErrorCode::RestoreConflict:
            return 409;
        case ErrorCode::RouteNotFound:
            return 404;
        case ErrorCode::Unauthorized:
            return 401;  // 缺 token / token 不对
        case ErrorCode::ServiceOperationFailed:
            // 「服务端还没实现这个接口」是 501，不是 500：500 等于说服务器坏了，
            // 而真相是这个功能还没做（share 就属于这一类）。
            return 501;
        default:
            return 500;
    }
}

// 位置参数：管道与 HTTP 用同一套编码，参数都放在 args.argv 里。
// 路径里的中文会被客户端百分号编码，这里负责解码回 UTF-8。
nlohmann::json args_with_name(std::string name) {
    nlohmann::json args = nlohmann::json::object();
    args["argv"] = nlohmann::json::array({std::move(name)});
    return args;
}

nlohmann::json args_with_encoded_name(const std::string& encoded) {
    return args_with_name(url_decode(encoded));
}

// ---- 访问凭证（token）----

// 不需要 token 的路径：只有健康检查与「别人拿分享链接下载」。
// 分享链接本身就是凭证（share_id 是随机 12 位十六进制），那一步不该再要 token。
bool is_public_path(const std::string& path) {
    if (path == "/api/ping") {
        return true;
    }
    return starts_with(path, "/api/share/") && ends_with(path, "/download");
}

// 接受两种写法：`X-FMT-Token: <token>` 与 `Authorization: Bearer <token>`。
std::string header_token(const httplib::Request& request) {
    if (request.has_header("X-FMT-Token")) {
        return trim(request.get_header_value("X-FMT-Token"));
    }
    if (request.has_header("Authorization")) {
        const std::string value = request.get_header_value("Authorization");
        if (starts_with(value, "Bearer ")) {
            return trim(value.substr(7));
        }
    }
    return {};
}

// 请求体：接受 {"name":"工作"}，也接受与管道一致的 {"argv":["工作"]}。
Result<nlohmann::json> args_from_body(const httplib::Request& request) {
    if (request.body.empty()) {
        return make_error(ErrorCode::InvalidArgument, "请求体不能为空");
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(request.body);
    } catch (const nlohmann::json::exception& error) {
        return make_error(ErrorCode::JsonParseError,
                          std::string("请求体不是合法 JSON：") + error.what());
    }
    if (!body.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "请求体必须是 JSON 对象");
    }
    if (const auto iterator = body.find("argv");
        iterator != body.end() && iterator->is_array() && !iterator->empty() &&
        (*iterator)[0].is_string()) {
        return body;
    }
    if (const auto iterator = body.find("name");
        iterator != body.end() && iterator->is_string()) {
        return args_with_name(iterator->get<std::string>());
    }
    return make_error(ErrorCode::InvalidArgument, "请求体缺少 name 字段");
}

// POST /api/file：{"url": "https://…"} 或 {"path": "D:/a.txt"}，可带 "file_name"。
// 语义与管道一致（CLI 传来源，不传文件内容），只是包了一层 HTTP。
Result<nlohmann::json> upload_args_from_body(const httplib::Request& request) {
    if (request.body.empty()) {
        return make_error(ErrorCode::InvalidArgument, "请求体不能为空");
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(request.body);
    } catch (const nlohmann::json::exception& error) {
        return make_error(ErrorCode::JsonParseError,
                          std::string("请求体不是合法 JSON：") + error.what());
    }
    if (!body.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "请求体必须是 JSON 对象");
    }

    std::string source;
    if (const auto url = body.find("url"); url != body.end() && url->is_string()) {
        source = url->get<std::string>();
    } else if (const auto path = body.find("path"); path != body.end() && path->is_string()) {
        source = path->get<std::string>();
    }
    if (source.empty()) {
        return make_error(ErrorCode::InvalidArgument, "请求体需要 url 或 path");
    }

    nlohmann::json args = nlohmann::json::object();
    args["argv"] = nlohmann::json::array({source});
    if (const auto iterator = body.find("file_name");
        iterator != body.end() && iterator->is_string()) {
        args["argv"].push_back(iterator->get<std::string>());
    }
    return args;
}

// DELETE 路由的公共参数：
//   ?dry_run=1  只预检（说清要删什么、有没有冲突），零副作用
//   ?force=1    已确认执行（也接受请求体 {"force":true}）
nlohmann::json delete_args(const httplib::Request& request, const std::string& name) {
    nlohmann::json args = args_with_encoded_name(name);

    const auto flag = [&request](const char* key) {
        if (!request.has_param(key)) {
            return false;
        }
        const std::string value = request.get_param_value(key);
        return value == "1" || iequals(value, "true");
    };

    if (flag("dry_run")) {
        args["dry_run"] = true;
    }
    bool force = flag("force");
    if (!force && !request.body.empty()) {
        try {
            const nlohmann::json body = nlohmann::json::parse(request.body);
            force = body.is_object() && body.value("force", false);
        } catch (const nlohmann::json::exception&) {
            force = false;  // 请求体不是 JSON：当作没确认
        }
    }
    if (force) {
        args["force"] = true;
    }
    return args;
}

void respond(httplib::Response& response, const Result<nlohmann::json>& result) {
    if (ok(result)) {
        response.status = 200;
        response.set_content(dump(envelope_ok(std::get<nlohmann::json>(result))),
                             kJsonContentType);
        return;
    }
    const Error& error = *error_of(result);
    response.status = http_status_for(error.code);
    response.set_content(dump(envelope_error(error)), kJsonContentType);
}

// 业务路由表见技术文档 12.3；这里只做「路径 -> op + 参数」的翻译。
void register_business_routes(httplib::Server* server, BusinessHandler handler) {
    const auto run = [handler](const std::string& operation, const nlohmann::json& args,
                               httplib::Response& response) {
        if (!handler) {
            respond(response, make_error(ErrorCode::ServiceOperationFailed,
                                         "HTTP 未接入业务处理"));
            return;
        }
        respond(response, handler(operation, args));
    };

    // 回收站（桶级）：GET /api/trash 列条目、POST /api/trash/<名字>/restore 回退。
    server->Get("/api/trash", [run](const httplib::Request&, httplib::Response& response) {
        run("trash.list", nlohmann::json::object(), response);
    });
    server->Post(R"(/api/trash/([^/]+)/restore)",
                 [run](const httplib::Request& request, httplib::Response& response) {
                     run("trash.restore", args_with_encoded_name(request.matches[1]), response);
                 });
    server->Get(R"(/api/trash/([^/]+))",
                [run](const httplib::Request& request, httplib::Response& response) {
                    run("trash.get", args_with_encoded_name(request.matches[1]), response);
                });
    server->Delete(R"(/api/trash/([^/]+))",
                   [run](const httplib::Request& request, httplib::Response& response) {
                       // 永久删除不可恢复：必须显式确认，?force=1 或请求体 {"force":true}；
                       // ?dry_run=1 只预检（把要删掉的东西说清楚）。
                       nlohmann::json args = delete_args(request, request.matches[1]);
                       run("trash.delete", args, response);
                   });

    // 文件（阶段 5）：upload 是长任务，服务端会走两段式（下载在锁外、登记在锁内）。
    server->Get("/api/file", [run](const httplib::Request&, httplib::Response& response) {
        run("file.list", nlohmann::json::object(), response);
    });
    server->Post("/api/file", [run](const httplib::Request& request,
                                    httplib::Response& response) {
        const Result<nlohmann::json> args = upload_args_from_body(request);
        if (!ok(args)) {
            respond(response, *error_of(args));
            return;
        }
        run("file.upload", std::get<nlohmann::json>(args), response);
    });
    server->Get(R"(/api/file/([^/]+))",
                [run](const httplib::Request& request, httplib::Response& response) {
                    run("file.get", args_with_encoded_name(request.matches[1]), response);
                });
    server->Delete(R"(/api/file/([^/]+))",
                   [run](const httplib::Request& request, httplib::Response& response) {
                       // 跨 Bucket 删除要先确认：?dry_run=1 预检、?force=1 执行。
                       run("file.delete", delete_args(request, request.matches[1]), response);
                   });

    // 分享（数据面）。管理接口都要 token —— 认证在 pre-routing 钩子里统一做，
    // 这里不重复判断（“漏给某条路由加认证”正是这类代码最容易出的事故）。
    // 「别人拿分享链接下载」是唯一公开的一条，属于流式下载，下一步做。
    server->Post("/api/share", [run](const httplib::Request& request,
                                     httplib::Response& response) {
        // 请求体：{"file_id": "fmt-20261009-0"}
        nlohmann::json args = nlohmann::json::object();
        if (!request.body.empty()) {
            try {
                const nlohmann::json body = nlohmann::json::parse(request.body);
                const std::string file_id =
                    body.is_object() ? body.value("file_id", std::string{}) : std::string{};
                if (!file_id.empty()) {
                    args["argv"] = nlohmann::json::array({file_id});
                }
            } catch (const nlohmann::json::exception&) {
                respond(response, make_error(ErrorCode::JsonParseError, "请求体不是合法 JSON"));
                return;
            }
        }
        run("share.create", args, response);
    });
    server->Get("/api/share", [run](const httplib::Request& request,
                                    httplib::Response& response) {
        // 查询串：?file_id=fmt-20261009-0
        nlohmann::json args = nlohmann::json::object();
        if (request.has_param("file_id")) {
            args["argv"] = nlohmann::json::array({request.get_param_value("file_id")});
        }
        run("share.list", args, response);
    });
    server->Get(R"(/api/share/([^/]+))",
                [run](const httplib::Request& request, httplib::Response& response) {
                    run("share.get", args_with_encoded_name(request.matches[1]), response);
                });
    server->Delete(R"(/api/share/([^/]+))",
                   [run](const httplib::Request& request, httplib::Response& response) {
                       run("share.delete", args_with_encoded_name(request.matches[1]), response);
                   });
}

}  // namespace

struct HttpServer::Impl {
    std::unique_ptr<httplib::Server> server;
    std::shared_ptr<SharedState> state;
    std::thread worker;
    std::string host;
    int port = 0;
    std::atomic<bool> running{false};
};

HttpServer::HttpServer() : impl_(std::make_unique<Impl>()) {}

HttpServer::~HttpServer() { stop(); }

Status HttpServer::start(const std::string& host, int port, std::string data_root, Logger* logger,
                         BusinessHandler handler, TokenVerifier verifier) {
    if (impl_->running.load()) {
        return make_error(ErrorCode::InvalidArgument, "HTTP 服务已经在运行");
    }

    impl_->state = std::make_shared<SharedState>();
    impl_->state->data_root = std::move(data_root);
    impl_->state->logger = logger;

    impl_->server = std::make_unique<httplib::Server>();

    // **认证只有这一处**：路由之前的钩子。放在这里而不是每个路由里，
    // 是因为「漏给某条路由加认证」正是这类代码最容易出的事故。
    // 没有注入校验器时**一律拒绝**（默认关着），免得哪天忘了注入就裸奔。
    impl_->server->set_pre_routing_handler(
        [verifier](const httplib::Request& request,
                   httplib::Response& response) -> httplib::Server::HandlerResponse {
            if (!starts_with(request.path, "/api/") || is_public_path(request.path)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            const std::string token = header_token(request);
            const std::string username =
                (verifier && !token.empty()) ? verifier(token) : std::string{};
            if (!username.empty()) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            response.status = 401;
            response.set_content(
                dump(envelope_error(make_error(
                    ErrorCode::Unauthorized,
                    "缺少或无效的访问 token：请求头用 X-FMT-Token: <token>，"
                    "或 Authorization: Bearer <token>（token 在 data/user.json 里）"))),
                kJsonContentType);
            return httplib::Server::HandlerResponse::Handled;
        });

    impl_->server->Get("/api/ping", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(dump(envelope_ok(nlohmann::json::object())), kJsonContentType);
    });

    const std::shared_ptr<SharedState> state = impl_->state;
    impl_->server->Get("/api/status",
                       [state](const httplib::Request&, httplib::Response& response) {
                           nlohmann::json data = nlohmann::json::object();
                           data["root"] = state->data_root;
                           data["pid"] = static_cast<unsigned long>(GetCurrentProcessId());
                           data["version"] = std::string(version::STRING);
                           data["channel"] = "http";
                           response.set_content(dump(envelope_ok(data)), kJsonContentType);
                       });

    // 业务路由：与命名管道共用同一份实现（见 register_business_routes）。
    register_business_routes(impl_->server.get(), std::move(handler));

    // 兜底：已经登记的模块里还没实现的操作。
    impl_->server->Get(R"(/api/.*)", [](const httplib::Request& request,
                                        httplib::Response& response) {
        // 兜底路由要分清两件事（原来一律 500，等于告诉调用方「服务器坏了」）：
        //   已知模块但还没实现（例如 /api/share/...）→ 404? 不：**501** Not Implemented
        //   完全没这个路径（例如打错字）            → **404** Not Found
        const std::string& path = request.path;
        // 注意：**bucket 不在里面**。用户明确不要 HTTP 桶接口（桶由 CLI 管），
        // 所以 /api/bucket 走 404「没有这个接口」，而不是 501「还没实现」。
        const bool known_module =
            starts_with(path, "/api/file") || starts_with(path, "/api/trash") ||
            starts_with(path, "/api/share") || starts_with(path, "/api/config") ||
            starts_with(path, "/api/server") || starts_with(path, "/api/preview");
        if (known_module) {
            response.status = 501;
            response.set_content(
                dump(envelope_error(make_error(ErrorCode::ServiceOperationFailed,
                                               "接口尚未实现：" + path))),
                kJsonContentType);
            return;
        }
        response.status = 404;
        response.set_content(
            dump(envelope_error(make_error(ErrorCode::RouteNotFound, "没有这个接口：" + path))),
            kJsonContentType);
    });

    impl_->server->set_error_handler([](const httplib::Request& request,
                                        httplib::Response& response) {
        if (!response.body.empty()) {
            return;
        }
        response.set_content(
            dump(envelope_error(make_error(ErrorCode::FileNotFound, "资源不存在：" + request.path))),
            kJsonContentType);
    });

    // httplib 0.18 的语义：bind_to_port 返回 bool，bind_to_any_port 返回真实端口。
    // 传 0 时用后者，才能把系统分配的端口读回来。
    const int bound = port == 0
                          ? impl_->server->bind_to_any_port(host.c_str())
                          : (impl_->server->bind_to_port(host.c_str(), port) ? port : 0);
    if (bound <= 0) {
        impl_->server.reset();
        return make_error(ErrorCode::ServiceOperationFailed,
                          "无法监听 " + host + ":" + std::to_string(port) + "（地址不可用或端口被占用）");
    }

    impl_->host = host;
    impl_->port = bound;
    impl_->running = true;

    Impl* raw = impl_.get();
    impl_->worker = std::thread([raw] {
        raw->server->listen_after_bind();
        raw->running = false;
    });

    // 必须等监听真正就绪再返回：httplib 的 stop() 通过内部 socket 通知监听循环，
    // 监听还没起来就 stop()，信号会丢掉，随后 join() 永久挂住。
    impl_->server->wait_until_ready();

    return std::monostate{};
}

void HttpServer::stop() {
    if (!impl_ || !impl_->server) {
        return;
    }
    impl_->server->stop();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->running = false;
    impl_->server.reset();
}

bool HttpServer::running() const { return impl_ != nullptr && impl_->running.load(); }

const std::string& HttpServer::host() const { return impl_->host; }

int HttpServer::port() const { return impl_->port; }

}  // namespace fmt::server
