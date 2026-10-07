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

Status HttpServer::start(const std::string& host, int port, std::string data_root, Logger* logger) {
    if (impl_->running.load()) {
        return make_error(ErrorCode::InvalidArgument, "HTTP 服务已经在运行");
    }

    impl_->state = std::make_shared<SharedState>();
    impl_->state->data_root = std::move(data_root);
    impl_->state->logger = logger;

    impl_->server = std::make_unique<httplib::Server>();

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

    // 业务路由（bucket / file / share / trash）随阶段 4/5 一起接入。
    impl_->server->Get(R"(/api/.*)", [](const httplib::Request& request,
                                        httplib::Response& response) {
        response.status = 500;
        response.set_content(
            dump(envelope_error(make_error(ErrorCode::ServiceOperationFailed,
                                           "操作尚未实现：" + request.path))),
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
