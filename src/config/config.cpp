#include "fmt/config/config.hpp"

#include <type_traits>
#include <utility>

#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

nlohmann::json to_json(const Config& config) {
    nlohmann::json value = nlohmann::json::object();
    value["version"] = Config::kVersion;
    value["current_user"] = config.current_user;
    value["current_bucket"] = config.current_bucket;
    value["max_upload_size"] = config.max_upload_size;
    value["size_unit"] = config.size_unit;
    value["language"] = config.language;
    return value;
}

nlohmann::json to_json(const ServerConfig& config) {
    nlohmann::json value = nlohmann::json::object();
    value["version"] = ServerConfig::kVersion;
    value["enabled"] = config.enabled;
    value["host"] = config.host;
    value["port"] = config.port;
    return value;
}

// 读取对象字段：缺失或类型不符时保留默认值，并记下「补过默认值」。
template <typename T>
void read_field(const nlohmann::json& object, const char* key, T& target, bool& patched) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || iterator->is_null()) {
        patched = true;
        return;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (!iterator->is_string()) {
            patched = true;
            return;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!iterator->is_boolean()) {
            patched = true;
            return;
        }
    } else {
        if (!iterator->is_number()) {
            patched = true;
            return;
        }
    }
    target = iterator->get<T>();
}

}  // namespace

Result<Config> load_config(const PathManager& paths) {
    const std::filesystem::path path = paths.config_file();

    if (!file_exists(path)) {
        // 首次运行：写出默认配置。
        const Config defaults;
        if (const Status status = save_config(paths, defaults); !ok(status)) {
            return *error_of(status);
        }
        return defaults;
    }

    Result<nlohmann::json> parsed = read_json_file(path);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& value = std::get<nlohmann::json>(parsed);

    if (const Status version = check_version(value, Config::kVersion); !ok(version)) {
        return *error_of(version);
    }

    Config config;
    bool patched = false;
    read_field(value, "current_user", config.current_user, patched);
    read_field(value, "current_bucket", config.current_bucket, patched);
    read_field(value, "max_upload_size", config.max_upload_size, patched);
    read_field(value, "size_unit", config.size_unit, patched);
    read_field(value, "language", config.language, patched);
    config.version = Config::kVersion;

    if (patched) {
        if (const Status status = save_config(paths, config); !ok(status)) {
            return *error_of(status);
        }
    }

    return config;
}

Status save_config(const PathManager& paths, const Config& config) {
    if (const Status status = ensure_directory(paths.config()); !ok(status)) {
        return status;
    }
    return write_json_file(paths.config_file(), to_json(config));
}

Result<ServerConfig> load_server_config(const PathManager& paths) {
    const std::filesystem::path path = paths.server_file();

    if (!file_exists(path)) {
        const ServerConfig defaults;
        if (const Status status = save_server_config(paths, defaults); !ok(status)) {
            return *error_of(status);
        }
        return defaults;
    }

    Result<nlohmann::json> parsed = read_json_file(path);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& value = std::get<nlohmann::json>(parsed);

    if (const Status version = check_version(value, ServerConfig::kVersion); !ok(version)) {
        return *error_of(version);
    }

    ServerConfig config;
    bool patched = false;
    read_field(value, "enabled", config.enabled, patched);
    read_field(value, "host", config.host, patched);
    read_field(value, "port", config.port, patched);
    config.version = ServerConfig::kVersion;

    if (patched) {
        if (const Status status = save_server_config(paths, config); !ok(status)) {
            return *error_of(status);
        }
    }

    return config;
}

Status save_server_config(const PathManager& paths, const ServerConfig& config) {
    if (const Status status = ensure_directory(paths.config()); !ok(status)) {
        return status;
    }
    return write_json_file(paths.server_file(), to_json(config));
}

}  // namespace fmt
