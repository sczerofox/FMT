// 分片上传会话（CLI 先用；HTTP 后续复用同一份存储与完成逻辑）。
//
// 会话目录：<数据根>/temp/upload-sessions/<upload_id>/
//   session.json
//   chunks/000000.part ...
//
// CLI 的 upload.start 由本类起后台线程直接读本地源文件；HTTP 的逐片上传
// 后续只复用 init/chunk/complete 这套会话接口，不重复设计存储。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt::service {

class UploadSessionManager {
public:
    UploadSessionManager(PathManager paths, Config config, Logger* logger);
    ~UploadSessionManager();

    UploadSessionManager(const UploadSessionManager&) = delete;
    UploadSessionManager& operator=(const UploadSessionManager&) = delete;

    // CLI 的当前账号/Bucket 可能在会话之间切换；每次 upload 请求前由 runtime 注入。
    void set_config(const Config& config);

    // 处理 upload.start / status / list / pause / resume / cancel / complete。
    Result<nlohmann::json> handle(const std::string& operation, const nlohmann::json& args);

    // 服务停止/换根时调用：让后台线程在分片边界退出并 join。
    void stop();

private:
    struct Session {
        std::string id;
        std::string user;
        std::string bucket;
        std::string source_path;
        std::string file_name;
        std::uint64_t size = 0;
        std::uint64_t chunk_size = 0;
        std::uint64_t chunk_count = 0;
        std::string md5;
        std::string state = "uploading";  // uploading/paused/completing/completed/canceled/error
        std::vector<bool> received;
        std::uint64_t uploaded_bytes = 0;
        std::string created_at;
        std::string updated_at;
        std::string expires_at;
        std::string final_file_id;
        std::string final_file_name;
        std::string error;
    };

    Result<nlohmann::json> start(const nlohmann::json& args);
    Result<nlohmann::json> status(const std::string& id);
    Result<nlohmann::json> list();
    Result<nlohmann::json> pause(const std::string& id);
    Result<nlohmann::json> resume(const std::string& id);
    Result<nlohmann::json> cancel(const std::string& id);
    Result<nlohmann::json> complete(const std::string& id);

    Result<Session> find_session(const std::string& id) const;
    Result<Session> load_session(const std::filesystem::path& dir) const;
    Status save_session(const Session& session) const;
    void load_existing();
    void cleanup_expired_locked();
    bool is_expired(const Session& session) const;
    void touch(Session& session) const;

    std::filesystem::path sessions_root() const;
    std::filesystem::path session_dir(const std::string& id) const;
    std::filesystem::path chunks_dir(const std::string& id) const;
    std::filesystem::path chunk_path(const std::string& id, std::uint64_t index) const;

    void spawn_worker(const std::string& id);
    void worker(const std::string& id);
    void complete_in_worker(const std::string& id);
    void reap_finished();
    void cleanup_session_files(const std::string& id) const;

    bool all_received(const Session& session) const;
    std::uint64_t next_missing(const Session& session) const;

    nlohmann::json session_json(const Session& session, bool include_missing) const;
    std::string next_name(const std::string& original, int sequence) const;

    void set_error(const std::string& id, const std::string& message);

    PathManager paths_;
    Config config_;
    Logger* logger_ = nullptr;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<std::string, std::thread> workers_;
    std::unordered_set<std::string> running_;
    std::atomic<bool> stopping_{false};
};

}  // namespace fmt::service
