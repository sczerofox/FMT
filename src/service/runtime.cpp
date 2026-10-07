#include "fmt/service/runtime.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/ipc/protocol.hpp"

namespace fmt::service {
namespace {

// 规范化数据根：绝对路径 + 词法归一（去尾部分隔符、折叠 . 与 ..）。
std::string normalize_root(const std::string& root) {
    if (root.empty()) {
        return {};
    }
    std::error_code code;
    std::filesystem::path path = path_from_utf8(root);
    if (!path.is_absolute()) {
        path = std::filesystem::absolute(path, code);
    }
    path = path.lexically_normal();
    std::string text = to_forward_slashes(path_to_utf8(path));
    while (text.size() > 3 && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

bool same_root(const std::string& left, const std::string& right) {
    return iequals(left, right);  // Windows 路径大小写不敏感
}

// 业务操作前缀：阶段 4/5 逐个实现。
bool is_business_operation(const std::string& op) {
    for (const std::string_view prefix :
         {"bucket.", "file.", "share.", "trash.", "config.", "server."}) {
        if (starts_with(op, prefix)) {
            return true;
        }
    }
    return false;
}

}  // namespace

HostResult run_service_host() {
    static std::unique_ptr<ServerRuntime> runtime;
    static std::thread server_thread;

    ServiceCallbacks callbacks;
    callbacks.initialize = []() -> Status {
        runtime = std::make_unique<ServerRuntime>(executable_directory());
        if (const Status status = runtime->start(); !ok(status)) {
            return status;
        }
        server_thread = std::thread([] { runtime->run(); });
        return std::monostate{};
    };
    callbacks.shutdown = [] {
        if (runtime != nullptr) {
            runtime->request_stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    switch (dispatch_service(callbacks)) {
        case DispatcherResult::BecameService:
            return HostResult::BecameService;
        case DispatcherResult::NotService:
            return HostResult::NotService;
        case DispatcherResult::Failed:
            return HostResult::Failed;
    }
    return HostResult::Failed;
}

std::filesystem::path ServerRuntime::state_directory_default() {
    return fmt::service::state_directory();
}

ServerRuntime::ServerRuntime(std::filesystem::path fallback_root,
                             std::filesystem::path state_directory)
    : fallback_root_(std::move(fallback_root)), state_directory_(std::move(state_directory)) {}

ServerRuntime::~ServerRuntime() = default;

Status ServerRuntime::start() {
    std::filesystem::path root = fallback_root_;

    // 上次跑过的数据根优先：开机自启时还没有 CLI 连接，得知道自己该服务哪个根。
    if (Result<ServiceState> state = load_state_from(state_directory_); ok(state)) {
        const ServiceState& recorded = std::get<ServiceState>(state);
        if (!recorded.current_root.empty()) {
            root = path_from_utf8(recorded.current_root);
        }
    }

    Result<std::unique_ptr<AppContext>> context = initialize_service_context(root);
    if (!ok(context)) {
        return *error_of(context);
    }

    std::lock_guard<std::mutex> guard(mutex_);
    context_ = std::move(std::get<std::unique_ptr<AppContext>>(context));

    ServiceState state;
    if (Result<ServiceState> loaded = load_state_from(state_directory_); ok(loaded)) {
        state = std::get<ServiceState>(loaded);
    }
    state.current_root = to_forward_slashes(path_to_utf8(context_->paths->root()));
    if (state.host_path.empty()) {
        state.host_path = to_forward_slashes(path_to_utf8(executable_path()));
    }
    if (const Status saved = save_state_to(state_directory_, state); !ok(saved)) {
        context_->logger->warn("Service", "写入服务状态失败：" + error_of(saved)->message);
    }

    context_->logger->info("Service", "服务已启动，当前数据根：" + state.current_root);
    return std::monostate{};
}

Status ServerRuntime::apply_root(const std::string& requested_root, std::string* effective_root) {
    const std::string target = normalize_root(requested_root);
    if (target.empty()) {
        return make_error(ErrorCode::ConfigError, "没有可用的数据根");
    }

    std::lock_guard<std::mutex> guard(mutex_);

    if (context_ != nullptr) {
        const std::string current =
            to_forward_slashes(path_to_utf8(context_->paths->root()));
        if (same_root(current, target)) {
            if (effective_root != nullptr) {
                *effective_root = current;
            }
            return std::monostate{};
        }
    }

    // 切换：新根初始化失败就保持原根不动（旧根数据也一个字节都不删）。
    Result<std::unique_ptr<AppContext>> created = initialize_service_context(path_from_utf8(target));
    if (!ok(created)) {
        return *error_of(created);
    }

    const std::string previous =
        context_ != nullptr ? to_forward_slashes(path_to_utf8(context_->paths->root())) : std::string{};
    context_ = std::move(std::get<std::unique_ptr<AppContext>>(created));
    context_->logger->info("Main", "数据根切换: " + (previous.empty() ? std::string("(无)") : previous) +
                                       " -> " + target);

    ServiceState state;
    if (Result<ServiceState> loaded = load_state_from(state_directory_); ok(loaded)) {
        state = std::get<ServiceState>(loaded);
    }
    state.current_root = target;
    if (state.host_path.empty()) {
        state.host_path = to_forward_slashes(path_to_utf8(executable_path()));
    }
    if (const Status saved = save_state_to(state_directory_, state); !ok(saved)) {
        context_->logger->warn("Service", "写入服务状态失败：" + error_of(saved)->message);
    }

    if (effective_root != nullptr) {
        *effective_root = target;
    }
    return std::monostate{};
}

ipc::Response ServerRuntime::handle(const ipc::Request& request) {
    ipc::Response response;
    response.id = request.id;

    if (request.op == "hello") {
        std::string effective;
        const Status applied = apply_root(request.root, &effective);
        if (!ok(applied)) {
            response.ok = false;
            response.error = *error_of(applied);
            return response;
        }
        response.ok = true;
        response.data["root"] = effective;
        return response;
    }

    if (is_business_operation(request.op)) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (context_ != nullptr && context_->logger != nullptr) {
                context_->logger->warn("Ipc", "尚未实现的操作：" + request.op);
            }
        }
        response.ok = false;
        response.error = make_error(ErrorCode::ServiceOperationFailed,
                                    "操作尚未实现：" + request.op);
        return response;
    }

    response.ok = false;
    response.error = make_error(ErrorCode::InvalidArgument, "未知操作：" + request.op);
    return response;
}

std::filesystem::path ServerRuntime::current_root() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return context_ != nullptr ? context_->paths->root() : std::filesystem::path{};
}

void ServerRuntime::request_stop() { stop_requested_ = true; }

void ServerRuntime::serve(ipc::PipeConnection connection) {
    while (!stop_requested_.load()) {
        const ipc::RequestOutcome outcome = connection.read_request(1000);
        if (outcome.status == ipc::ReadStatus::Timeout) {
            continue;  // 用户还在敲命令，连接不能断
        }
        if (outcome.status != ipc::ReadStatus::Ok) {
            if (outcome.status == ipc::ReadStatus::Failed) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (context_ != nullptr && context_->logger != nullptr) {
                    context_->logger->warn("Ipc", "读取请求失败：" + outcome.error.message);
                }
            }
            return;
        }

        const ipc::Response response = handle(outcome.request);
        if (const Status written = connection.write_response(response, ipc::kCommandTimeoutMs);
            !ok(written)) {
            return;
        }
    }
}

void ServerRuntime::run() {
    int consecutive_failures = 0;

    while (!stop_requested_.load()) {
        Result<ipc::PipeConnection> accepted = ipc::PipeConnection::accept(500);
        if (!ok(accepted)) {
            // 500 ms 超时是正常路径：回到循环顶部检查停止标志。
            if (++consecutive_failures > 3) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            continue;
        }
        consecutive_failures = 0;
        serve(std::move(std::get<ipc::PipeConnection>(accepted)));
    }
}

}  // namespace fmt::service
