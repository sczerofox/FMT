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
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {
struct FileRecord;
}

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

    // HTTP 用：显式带认证用户与请求级 Bucket，不依赖全局 current_user/current_bucket。
    Result<nlohmann::json> handle_for(const std::string& operation, const nlohmann::json& args,
                                      const std::string& user, const std::string& bucket);

    // 给 file delete <upload_id> 用：返回会话（completed 时有 final_file_id）。
    Result<nlohmann::json> lookup(const std::string& upload_id);
    Result<nlohmann::json> lookup_for(const std::string& upload_id, const std::string& user,
                                      const std::string& bucket);

    // trash delete/empty 物理清除后：删掉指向已不存在 file_id 的会话记录。
    // 软删除（文件还在 trash）不会调用它。
    std::size_t prune_missing_files(const std::string& user, const std::string& bucket);

    // 校验 upload_id 的 final_file_id 仍指向同一份内容；防止物理删除后
    // file_id 被新文件复用，导致 file delete <upload_id> 删错文件。
    Result<std::string> verified_file_id(const std::string& upload_id,
                                         const std::string& user, const std::string& bucket);

    // HTTP 初始化（没有本地源文件；客户端逐片上传）。
    Result<nlohmann::json> init_for(const nlohmann::json& args, const std::string& user,
                                    const std::string& bucket);
    // HTTP 写一个分片；data 是原始字节，md5 可空（空则服务端记录）。
    Result<nlohmann::json> put_chunk_for(const std::string& upload_id, std::uint64_t index,
                                         std::string_view data, const std::string& expected_md5,
                                         const std::string& user, const std::string& bucket);

    // 服务停止/换根时调用：让后台线程在分片边界退出并 join。
    void stop();

private:
    struct Session {
        std::string id;
        std::string user;
        std::string bucket;
        std::string origin = "cli";  // cli / http
        std::string source_path;
        std::string file_name;
        std::uint64_t size = 0;
        std::int64_t source_mtime = 0;  // 源文件最后写入时间（秒）；老会话为 0
        std::uint64_t chunk_size = 0;
        std::uint64_t chunk_count = 0;
        std::string md5;
        std::string state = "uploading";  // uploading/paused/completing/completed/canceled/error
        std::vector<bool> received;
        // 每个分片的 MD5（32 位小写十六进制）；未收到的分片为空串。
        std::vector<std::string> chunk_hashes;
        std::uint64_t uploaded_bytes = 0;
        std::string created_at;
        std::string updated_at;
        std::string expires_at;
        std::string final_file_id;
        std::string final_file_name;
        std::string error;
    };

    Result<nlohmann::json> start(const nlohmann::json& args, const std::string& user,
                                 const std::string& bucket);
    Result<nlohmann::json> status(const std::string& id, const std::string& user,
                                  const std::string& bucket);
    Result<nlohmann::json> list(bool all, const std::string& user, const std::string& bucket);
    Result<nlohmann::json> pause(const std::string& id, const std::string& user,
                                 const std::string& bucket);
    Result<nlohmann::json> resume(const std::string& id, const std::string& user,
                                  const std::string& bucket);
    Result<nlohmann::json> cancel(const std::string& id, const std::string& user,
                                  const std::string& bucket);
    Result<nlohmann::json> complete(const std::string& id, const std::string& user,
                                    const std::string& bucket);

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
    bool prepare_target(const std::string& id, const std::string& md5);
    void complete_in_worker(const std::string& id);
    void reap_finished();
    void cleanup_session_files(const std::string& id) const;

    bool all_received(const Session& session) const;
    std::uint64_t next_missing(const Session& session) const;
    bool should_upload_continue(const std::string& id) const;
    bool owned(const Session& session, const std::string& user,
               const std::string& bucket) const;
    static bool matches_file(const Session& session, const FileRecord& record);

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
