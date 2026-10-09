#include "fmt/core/app.hpp"

#include <utility>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt/user/user.hpp"

namespace fmt {
namespace {

// V1 的占位用户名（没有用户系统）。
constexpr std::string_view kDefaultUser = "user";

struct ExpectedFile {
    std::filesystem::path path;
    nlohmann::json value;
};

// 集合型默认文件；与 PathManager 的路径一一对应。
std::vector<ExpectedFile> default_data_files(const PathManager& paths) {
    return {
        {paths.file_data(), make_collection(1, "files")},
        {paths.share_data(), make_collection(1, "shares")},
        {paths.trash_data(), make_collection(1, "trash")},
        {paths.user_data(), make_collection(1, "users")},
    };
}

// 单例型配置文件：默认值的唯一来源在 config 模块，这里只关心「缺不缺」。
std::vector<std::filesystem::path> default_config_files(const PathManager& paths) {
    return {paths.config_file(), paths.server_file()};
}

std::string describe(const std::filesystem::path& path, const Error& error) {
    return to_forward_slashes(path_to_utf8(path)) + "（" + code_string(error.code) + " " +
           error.message + "）";
}

}  // namespace

RootReport check_root(const PathManager& paths) {
    RootReport report;

    for (const std::string& name : PathManager::required_directories()) {
        if (!directory_exists(paths.root() / path_from_utf8(name))) {
            report.missing_directories.push_back(name);
        }
    }

    // 存在就必须读得出来、版本必须受支持——这就是「打开读取确认完整性」。
    const auto inspect = [&report](const std::filesystem::path& path) {
        if (!file_exists(path)) {
            report.missing_files.push_back(to_forward_slashes(path_to_utf8(path)));
            return;
        }

        Result<nlohmann::json> parsed = read_json_file(path);
        if (!ok(parsed)) {
            report.broken_files.push_back(describe(path, *error_of(parsed)));
            return;
        }

        const Status version = check_version(std::get<nlohmann::json>(parsed), 1);
        if (!ok(version)) {
            report.broken_files.push_back(describe(path, *error_of(version)));
        }
    };

    for (const ExpectedFile& expected : default_data_files(paths)) {
        inspect(expected.path);
    }
    for (const std::filesystem::path& path : default_config_files(paths)) {
        inspect(path);
    }

    return report;
}

Result<RootRepair> ensure_root(const PathManager& paths) {
    RootRepair repair;

    for (const std::string& name : PathManager::required_directories()) {
        const std::filesystem::path directory = paths.root() / path_from_utf8(name);
        if (directory_exists(directory)) {
            continue;
        }
        if (const Status status = ensure_directory(directory); !ok(status)) {
            return *error_of(status);
        }
        repair.created_directories.push_back(name);
    }

    for (const ExpectedFile& expected : default_data_files(paths)) {
        if (file_exists(expected.path)) {
            continue;  // 已存在一律不动，哪怕它损坏
        }
        if (const Status status = write_json_file(expected.path, expected.value); !ok(status)) {
            return *error_of(status);
        }
        repair.created_files.push_back(to_forward_slashes(path_to_utf8(expected.path)));
    }

    if (const Status status = ensure_default_config_files(paths, &repair.created_files);
        !ok(status)) {
        return *error_of(status);
    }

    return repair;
}

Result<std::unique_ptr<PathManager>> initialize_root(const std::filesystem::path& root) {
    auto paths = std::make_unique<PathManager>(root);

    if (Result<RootRepair> repaired = ensure_root(*paths); !ok(repaired)) {
        return *error_of(repaired);
    }

    // 配置必须能读出来：损坏时报错让调用方决定怎么办，绝不悄悄重置。
    Result<Config> config = load_config(*paths);
    if (!ok(config)) {
        return *error_of(config);
    }

    // V1 没有用户系统：current_user 为空时用占位名顶上（需求：「不做用户先用 user
    // 代替」）。正式用户系统以后再做，这里只是让 Bucket 有地方落。
    Config loaded = std::get<Config>(config);
    if (loaded.current_user.empty()) {
        loaded.current_user = kDefaultUser;
        if (const Status status = save_config(*paths, loaded); !ok(status)) {
            return *error_of(status);
        }
    }

    // 账号：HTTP 管理接口的 token 来自 data/user.json。首次运行在这里建默认用户
    // （用户名取 current_user，默认 "user"），已存在就原样留着、**不覆盖**。
    // 初始密码不在这里回报：V1 没有登录接口，真正当凭证用的是 token，
    // 而 token 可以用 `config list` 读出来。
    if (const Result<UserRecord> user = UserStore(*paths, loaded).ensure_default(nullptr);
        !ok(user)) {
        return *error_of(user);
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
