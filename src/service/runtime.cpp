#include "fmt/service/runtime.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/ipc/protocol.hpp"
#include "fmt/service/commands.hpp"

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

// temp/ 是随时可清空的目录：服务启动时清掉上次遗留的临时文件。
// 只删我们自己前缀（fmt-）的文件，用户手放进去的东西一律不动。
int clean_temp_directory(const PathManager& paths) {
    std::error_code code;
    if (!std::filesystem::is_directory(paths.temp(), code)) {
        return 0;
    }

    int removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths.temp(), code)) {
        if (code) {
            break;
        }
        std::error_code type_code;
        if (!entry.is_regular_file(type_code)) {
            continue;
        }
        if (!starts_with(path_to_utf8(entry.path().filename()), "fmt-")) {
            continue;
        }
        std::error_code ignored;
        if (std::filesystem::remove(entry.path(), ignored)) {
            ++removed;
        }
    }
    return removed;
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
    callbacks.on_control = [](const char* event) {
        if (runtime != nullptr) {
            runtime->log_event("Service", event);
        }
    };
    callbacks.shutdown = [] {
        if (runtime != nullptr) {
            runtime->log_event("Service", "正在停止：停 HTTP、等在途操作");
            runtime->request_stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (runtime != nullptr) {
            runtime->log_event("Service", "服务已停止");
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

    PathManager* paths = nullptr;
    Logger* logger = nullptr;
    std::string root_text;

    {
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

        paths = context_->paths.get();
        logger = context_->logger.get();
        root_text = state.current_root;
    }

    // 上次异常退出可能留下提权结果之类的临时文件，启动时顺手清掉。
    if (const int cleaned = clean_temp_directory(*paths); cleaned > 0) {
        logger->info("Service", "清理 temp/ 中 " + std::to_string(cleaned) + " 个遗留临时文件");
    }

    // 必须在锁外：restart_http 会 join HTTP 工作线程（见其注释）。
    restart_http();

    logger->info("Service", "服务已启动，当前数据根：" + root_text);
    return std::monostate{};
}

void ServerRuntime::restart_http() {
    // 先把配置与旧实例摘出来（持锁），停旧实例放到锁外。
    std::unique_ptr<server::HttpServer> previous;
    std::string host;
    std::string root;
    int port = 0;
    bool enabled = false;
    Logger* logger = nullptr;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        previous = std::move(http_);
        if (context_ != nullptr) {
            host = context_->server_config.host;
            port = context_->server_config.port;
            enabled = context_->server_config.enabled;
            root = to_forward_slashes(path_to_utf8(context_->paths->root()));
            logger = context_->logger.get();
        }
    }

    // stop() 会 join HTTP 工作线程，而处理器要拿 mutex_ —— 必须在锁外做。
    if (previous != nullptr) {
        previous->stop();
    }
    if (!enabled) {
        return;
    }

    // 浏览器与 CLI 走同一份业务实现。
    server::BusinessHandler handler =
        [this](const std::string& operation, const nlohmann::json& args) -> Result<nlohmann::json> {
        std::lock_guard<std::mutex> guard(mutex_);
        if (context_ == nullptr) {
            return make_error(ErrorCode::ServiceOperationFailed, "服务尚未初始化数据根");
        }
        return execute_business(*context_, operation, args);
    };

    auto created = std::make_unique<server::HttpServer>();
    const Status started = created->start(host, port, root, logger, std::move(handler));
    if (!ok(started)) {
        // 端口被占用等：记 ERROR 日志，但**不中断**服务的其他功能。
        if (logger != nullptr) {
            logger->error("Http", error_of(started)->message);
        }
        return;
    }
    if (logger != nullptr) {
        logger->info("Http", "HTTP 监听 " + created->host() + ":" +
                                 std::to_string(created->port()));
    }

    std::lock_guard<std::mutex> guard(mutex_);
    http_ = std::move(created);
}

Status ServerRuntime::apply_root(const std::string& requested_root, std::string* effective_root,
                                 std::string* previous_root, bool* switched) {
    if (switched != nullptr) {
        *switched = false;
    }

    const std::string target = normalize_root(requested_root);
    if (target.empty()) {
        return make_error(ErrorCode::ConfigError, "没有可用的数据根");
    }

    std::string previous;
    {
        std::lock_guard<std::mutex> guard(mutex_);

        if (context_ != nullptr) {
            const std::string current = to_forward_slashes(path_to_utf8(context_->paths->root()));
            if (same_root(current, target)) {
                if (effective_root != nullptr) {
                    *effective_root = current;
                }
                return std::monostate{};
            }
        }

        // 切换：新根初始化失败就保持原根不动（旧根数据也一个字节都不删）。
        Result<std::unique_ptr<AppContext>> created =
            initialize_service_context(path_from_utf8(target));
        if (!ok(created)) {
            return *error_of(created);
        }

        previous = context_ != nullptr ? to_forward_slashes(path_to_utf8(context_->paths->root()))
                                       : std::string{};
        context_ = std::move(std::get<std::unique_ptr<AppContext>>(created));
        context_->logger->info("Main", "数据根切换: " +
                                           (previous.empty() ? std::string("(无)") : previous) +
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
    }

    // 换根后重新评估 server.json；必须在锁外（见 restart_http 注释）。
    restart_http();

    if (effective_root != nullptr) {
        *effective_root = target;
    }
    if (previous_root != nullptr) {
        *previous_root = previous;
    }
    if (switched != nullptr) {
        *switched = true;
    }
    return std::monostate{};
}

ipc::Response ServerRuntime::handle(const ipc::Request& request) {
    ipc::Response response;
    response.id = request.id;

    if (request.op == "hello") {
        std::string effective;
        std::string previous;
        bool switched = false;

        const Status applied = apply_root(request.root, &effective, &previous, &switched);
        if (!ok(applied)) {
            response.ok = false;
            response.error = *error_of(applied);
            return response;
        }
        response.ok = true;
        response.data["root"] = effective;
        response.data["switched"] = switched;
        if (switched) {
            // CLI 据此打印「数据根已切换：旧 -> 新」，双击时就能看见服务跟过来了。
            response.data["previous_root"] = previous;
        }
        return response;
    }

    if (!is_known_business(request.op)) {
        response.ok = false;
        response.error = make_error(ErrorCode::InvalidArgument, "未知操作：" + request.op);
        return response;
    }

    // 业务命令在 runtime 的锁下串行执行：同一时刻只有服务在写数据根，
    // 命令之间也不会互相踩（V1 只有一个 CLI 窗口，串行足够）。
    std::lock_guard<std::mutex> guard(mutex_);
    if (context_ == nullptr) {
        response.ok = false;
        response.error = make_error(ErrorCode::ServiceOperationFailed, "服务尚未初始化数据根");
        return response;
    }

    Result<nlohmann::json> result = execute_business(*context_, request.op, request.args);
    if (ok(result)) {
        response.ok = true;
        response.data = std::get<nlohmann::json>(result);
        return response;
    }

    response.ok = false;
    response.error = *error_of(result);
    if (context_->logger != nullptr) {
        context_->logger->warn("Ipc", request.op + " 失败：" + code_string(response.error.code) +
                                          " " + response.error.message);
    }
    return response;
}

std::filesystem::path ServerRuntime::current_root() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return context_ != nullptr ? context_->paths->root() : std::filesystem::path{};
}

void ServerRuntime::log_event(std::string_view module, const std::string& message) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (context_ != nullptr && context_->logger != nullptr) {
        context_->logger->info(module, message);
    }
}

void ServerRuntime::request_stop() {
    stop_requested_ = true;

    // 停止接受新请求后立刻停 HTTP。**必须在锁外**：HTTP 的请求处理器要拿
    // mutex_，持锁去 join 它的工作线程会互相等待。
    std::unique_ptr<server::HttpServer> http;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        http = std::move(http_);
    }
    if (http != nullptr) {
        http->stop();
    }
}

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
