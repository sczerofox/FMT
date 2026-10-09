#include "fmt/service/runtime.hpp"
#include <chrono>
#include <thread>
#include <utility>

#include "fmt/bucket/bucket.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/file/file.hpp"
#include "fmt/ipc/protocol.hpp"
#include "fmt/service/commands.hpp"
#include "fmt/user/user.hpp"

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

    // 只清**真的陈旧**的临时文件。
    //
    // 提权副本回传结果的临时文件也叫 `fmt-elev-<pid>.json(.tmp)`，而 `service install`
    // 会在同一次操作里**启动服务**——服务启动就来清 temp/。原来只看前缀 `fmt-`，
    // 于是把父进程正在收的结果文件一起删掉，父进程只好报
    // `FMT-602 提权副本没有返回结果`（服务其实已经装好并启动了，用户看到的是假失败）。
    //
    // 正在回传的结果文件寿命只有几十毫秒，所以按年龄放过新的、只清旧的。
    constexpr auto kStaleAfter = std::chrono::minutes(10);
    const auto now = std::filesystem::file_time_type::clock::now();

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
        std::error_code time_code;
        const std::filesystem::file_time_type modified = entry.last_write_time(time_code);
        if (time_code) {
            continue;  // 读不到时间就别动它
        }
        if (now - modified < kStaleAfter) {
            continue;  // 可能是**正在写**的：放过
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

        // current_bucket 失效时置空（开发文档第 61 节）：启动即校一次。
        refresh_current_bucket_locked();
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

Result<nlohmann::json> ServerRuntime::run_upload(const nlohmann::json& args) {
    std::string source;
    std::string name;
    if (const auto argv = args.find("argv"); argv != args.end() && argv->is_array()) {
        if (argv->size() > 0 && (*argv)[0].is_string()) {
            source = (*argv)[0].get<std::string>();
        }
        if (argv->size() > 1 && (*argv)[1].is_string()) {
            name = (*argv)[1].get<std::string>();
        }
    }
    if (source.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少上传来源（http:// URL 或本地路径）");
    }

    // ① 锁下取快照。之后整段下载都在锁外跑，所以这里只拿必须的东西。
    //
    // 数据根在下载期间不会被换掉：换根只由 hello 触发，而管道的 accept/serve 是串行的，
    // 我们此刻就在 handle() 里面，不会再处理第二个请求（技术文档并发一节）。
    PathManager* paths = nullptr;
    Logger* logger = nullptr;
    std::uintmax_t size_limit = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (context_ == nullptr) {
            return make_error(ErrorCode::ServiceOperationFailed, "服务尚未初始化数据根");
        }
        paths = context_->paths.get();
        logger = context_->logger.get();
        size_limit = context_->config.max_upload_size;
    }

    // ② 锁外：下载或复制到 temp/（长耗时，不能占着业务锁）。
    Result<PreparedUpload> prepared = prepare_upload(*paths, source, name, size_limit, logger);
    if (!ok(prepared)) {
        return *error_of(prepared);
    }
    PreparedUpload upload = std::get<PreparedUpload>(prepared);

    // ③ 锁内：登记。快，一次锁就够。
    std::lock_guard<std::mutex> guard(mutex_);
    if (context_ == nullptr) {
        std::error_code ignored;
        std::filesystem::remove(upload.temp_path, ignored);
        return make_error(ErrorCode::ServiceOperationFailed, "服务尚未初始化数据根");
    }

    FileService files(*context_->paths, context_->config, context_->logger.get());
    // 登记失败时由 commit_upload 负责清掉临时文件（见 file.hpp）。
    Result<FileRecord> record = files.commit_upload(upload);
    if (!ok(record)) {
        return *error_of(record);
    }

    const FileRecord& stored = std::get<FileRecord>(record);
    nlohmann::json data = nlohmann::json::object();
    data["file_id"] = stored.file_id;
    data["file_name"] = stored.file_name;
    data["bucket"] = stored.bucket;
    data["extension"] = stored.extension;
    data["file_type"] = stored.file_type;
    data["size"] = stored.size;
    data["md5"] = stored.md5;
    data["message"] = "文件已入库：" + stored.file_name + "（" + stored.file_id + "）";
    return data;
}

void ServerRuntime::refresh_current_bucket_locked() {
    if (context_ == nullptr || context_->paths == nullptr) {
        return;
    }

    BucketService buckets(*context_->paths, context_->config, context_->logger.get());
    const Status status = buckets.refresh_current_bucket();
    if (!ok(status) && context_->logger != nullptr) {
        context_->logger->warn("Bucket", "校验当前 Bucket 失败：" + error_of(status)->message);
    }
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
        // 上传同样走两段式：HTTP 与管道共用这一份实现。
        if (operation == "file.upload") {
            return run_upload(args);
        }
        std::lock_guard<std::mutex> guard(mutex_);
        if (context_ == nullptr) {
            return make_error(ErrorCode::ServiceOperationFailed, "服务尚未初始化数据根");
        }
        return execute_business(*context_, operation, args);
    };

    auto created = std::make_unique<server::HttpServer>();

    // token 校验：HTTP 层不认识账号，把这件事交给服务——只有它拿得到
    // data/user.json 与 config。找不到用户就返回空串，HTTP 层答 401。
    server::TokenVerifier verifier = [this](const std::string& token) -> std::string {
        std::lock_guard<std::mutex> guard(mutex_);
        if (context_ == nullptr) {
            return {};
        }
        UserStore store(*context_->paths, context_->config);
        const Result<UserRecord> found = store.find_by_token(token);
        return ok(found) ? std::get<UserRecord>(found).username : std::string{};
    };

    const Status started =
        created->start(host, port, root, logger, std::move(handler), std::move(verifier));
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

        // 换根后同样校一次：新根里的 current_bucket 可能指向不存在的 Bucket。
        refresh_current_bucket_locked();
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

    // 上传是唯一的长任务：两段式，下载在锁外，登记在锁内。
    if (request.op == "file.upload") {
        Result<nlohmann::json> uploaded = run_upload(request.args);
        if (ok(uploaded)) {
            response.ok = true;
            response.data = std::get<nlohmann::json>(uploaded);
            return response;
        }
        response.ok = false;
        response.error = *error_of(uploaded);
        std::lock_guard<std::mutex> guard(mutex_);
        if (context_ != nullptr && context_->logger != nullptr) {
            context_->logger->warn("Ipc", "file.upload 失败：" +
                                              code_string(response.error.code) + " " +
                                              response.error.message);
        }
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
    // 管道名只算一次：默认名，或测试用 `FMT_PIPE` 覆盖的值。
    const std::wstring pipe = ipc::pipe_name();

    while (!stop_requested_.load()) {
        Result<ipc::PipeConnection> accepted = ipc::PipeConnection::accept(500, pipe.c_str());
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
