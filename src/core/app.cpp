#include "fmt/core/app.hpp"

#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

// 缺失的 JSON 文件补上默认内容；已存在的一律不碰。
Status ensure_default_files(const PathManager& paths) {
    const struct {
        std::filesystem::path path;
        nlohmann::json value;
    } defaults[] = {
        {paths.file_data(), make_collection(1, "files")},
        {paths.share_data(), make_collection(1, "shares")},
        {paths.trash_data(), make_collection(1, "trash")},
        {paths.user_data(), make_collection(1, "users")},
    };

    for (const auto& entry : defaults) {
        if (file_exists(entry.path)) {
            continue;
        }
        if (const Status status = write_json_file(entry.path, entry.value); !ok(status)) {
            return status;
        }
    }
    return std::monostate{};
}

}  // namespace

Result<std::unique_ptr<PathManager>> initialize_root(const std::filesystem::path& root) {
    auto paths = std::make_unique<PathManager>(root);

    for (const std::string& name : PathManager::required_directories()) {
        const Status status = ensure_directory(root / path_from_utf8(name));
        if (!ok(status)) {
            return *error_of(status);
        }
    }

    if (const Status status = ensure_default_files(*paths); !ok(status)) {
        return *error_of(status);
    }

    // 配置文件也走同一套「不存在则写默认」的规则。
    if (auto config = load_config(*paths); !ok(config)) {
        return *error_of(config);
    }
    if (auto server = load_server_config(*paths); !ok(server)) {
        return *error_of(server);
    }

    return paths;
}

Result<std::unique_ptr<AppContext>> initialize_service_context(const std::filesystem::path& root) {
    Result<std::unique_ptr<PathManager>> initialized = initialize_root(root);
    if (!ok(initialized)) {
        return *error_of(initialized);
    }

    auto context = std::make_unique<AppContext>();
    context->paths = std::move(std::get<std::unique_ptr<PathManager>>(initialized));

    Logger::Options options;
    options.info_to_console = false;         // 后台服务没有控制台
    options.warn_error_to_console = false;
    Result<std::unique_ptr<Logger>> logger = Logger::open(context->paths->log(), options);
    if (!ok(logger)) {
        return *error_of(logger);
    }
    context->logger = std::move(std::get<std::unique_ptr<Logger>>(logger));

    Result<Config> config = load_config(*context->paths);
    if (!ok(config)) {
        return *error_of(config);
    }
    context->config = std::get<Config>(config);

    Result<ServerConfig> server = load_server_config(*context->paths);
    if (!ok(server)) {
        return *error_of(server);
    }
    context->server_config = std::get<ServerConfig>(server);

    context->logger->info("Main",
                          "数据根：" + to_forward_slashes(path_to_utf8(context->paths->root())));
    return context;
}

}  // namespace fmt
