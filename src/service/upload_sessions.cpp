#include "fmt/service/upload_sessions.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include "fmt/common/hash.hpp"
#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/common/validation.hpp"
#include "fmt/core/path.hpp"
#include "fmt/file/file.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt/user/user.hpp"

namespace fmt::service {
namespace {

constexpr std::uint64_t kMinChunkSize = 64ULL * 1024;
constexpr std::uint64_t kMaxChunkSize = 64ULL * 1024 * 1024;
constexpr std::size_t kCopyBuffer = 1024 * 1024;
constexpr int kNameAttempts = 100;

std::string chunk_name(std::uint64_t index) {
    std::ostringstream out;
    out << std::setw(6) << std::setfill('0') << index << ".part";
    return out.str();
}

bool read_exact(std::ifstream& in, char* data, std::size_t length) {
    in.read(data, static_cast<std::streamsize>(length));
    return in.gcount() == static_cast<std::streamsize>(length);
}

std::string md5_file(const std::filesystem::path& path,
                     const std::function<bool()>& keep_going, bool* aborted) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    Md5 hash;
    std::vector<char> buffer(kCopyBuffer);
    int tick = 0;
    while (in) {
        if ((++tick % 64) == 0 && !keep_going()) {
            if (aborted != nullptr) {
                *aborted = true;
            }
            return {};
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0 && !hash.update(buffer.data(), static_cast<std::size_t>(got))) {
            return {};
        }
    }
    return hash.finish();
}

}  // namespace

UploadSessionManager::UploadSessionManager(PathManager paths, Config config, Logger* logger)
    : paths_(std::move(paths)), config_(std::move(config)), logger_(logger) {
    std::error_code ignored;
    std::filesystem::create_directories(sessions_root(), ignored);
    load_existing();
}

UploadSessionManager::~UploadSessionManager() { stop(); }

void UploadSessionManager::set_config(const Config& config) {
    std::lock_guard<std::mutex> guard(mutex_);
    config_ = config;
}

void UploadSessionManager::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& entry : workers_) {
            if (entry.second.joinable()) {
                threads.push_back(std::move(entry.second));
            }
        }
        workers_.clear();
        running_.clear();
    }
    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

std::filesystem::path UploadSessionManager::sessions_root() const {
    return paths_.temp() / "upload-sessions";
}

std::filesystem::path UploadSessionManager::session_dir(const std::string& id) const {
    return sessions_root() / id;
}

std::filesystem::path UploadSessionManager::chunks_dir(const std::string& id) const {
    return session_dir(id) / "chunks";
}

std::filesystem::path UploadSessionManager::chunk_path(const std::string& id,
                                                       std::uint64_t index) const {
    return chunks_dir(id) / chunk_name(index);
}

bool UploadSessionManager::is_expired(const Session& session) const {
    if (session.expires_at.empty()) {
        return false;
    }
    const auto expires = parse_datetime_iso(session.expires_at);
    return expires.has_value() && std::chrono::system_clock::now() >= *expires;
}

void UploadSessionManager::touch(Session& session) const {
    session.updated_at = local_datetime_iso();
    session.expires_at =
        local_datetime_iso_after_seconds(static_cast<std::int64_t>(config_.upload_session_ttl_seconds));
}

nlohmann::json UploadSessionManager::session_json(const Session& session,
                                                  bool include_missing) const {
    nlohmann::json value = nlohmann::json::object();
    value["upload_id"] = session.id;
    value["user"] = session.user;
    value["bucket"] = session.bucket;
    value["source_path"] = session.source_path;
    value["source_mtime"] = session.source_mtime;
    value["file_name"] = session.final_file_name.empty() ? session.file_name : session.final_file_name;
    value["final_file_name"] = session.final_file_name;
    value["size"] = session.size;
    value["chunk_size"] = session.chunk_size;
    value["chunk_count"] = session.chunk_count;
    value["state"] = session.state;
    value["uploaded_bytes"] = session.uploaded_bytes;
    value["md5"] = session.md5;
    value["created_at"] = session.created_at;
    value["updated_at"] = session.updated_at;
    value["expires_at"] = session.expires_at;
    value["final_file_id"] = session.final_file_id;
    // HTTP 的 complete 直接返回这个 JSON；Java 的 FmtUpload.from 读根上的 file_id。
    // 给已完成会话补一份别名，status/list 看到也无害。
    if (!session.final_file_id.empty()) {
        value["file_id"] = session.final_file_id;
    }
    value["error"] = session.error;

    nlohmann::json received = nlohmann::json::array();
    std::uint64_t received_count = 0;
    for (std::uint64_t i = 0; i < session.received.size(); ++i) {
        if (session.received[i]) {
            received.push_back(i);
            ++received_count;
        }
    }
    value["received"] = std::move(received);
    value["received_count"] = received_count;
    nlohmann::json chunk_hashes = nlohmann::json::array();
    std::uint64_t verified_count = 0;
    for (std::uint64_t i = 0; i < session.chunk_hashes.size(); ++i) {
        chunk_hashes.push_back(session.chunk_hashes[i]);
        if (i < session.received.size() && session.received[i] &&
            !session.chunk_hashes[i].empty()) {
            ++verified_count;
        }
    }
    value["chunk_hashes"] = std::move(chunk_hashes);
    value["verified_count"] = verified_count;
    value["percent"] = session.size == 0
                           ? (session.state == "completed" ? 100 : 0)
                           : static_cast<int>((session.uploaded_bytes * 100) / session.size);

    if (include_missing && (session.state == "uploading" || session.state == "paused")) {
        nlohmann::json missing = nlohmann::json::array();
        for (std::uint64_t i = 0; i < session.received.size(); ++i) {
            if (!session.received[i]) {
                missing.push_back(i);
            }
        }
        value["missing"] = std::move(missing);
    }
    return value;
}

Result<UploadSessionManager::Session> UploadSessionManager::load_session(
    const std::filesystem::path& dir) const {
    Result<nlohmann::json> parsed = read_json_file(dir / "session.json");
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& value = std::get<nlohmann::json>(parsed);
    if (!value.is_object()) {
        return make_error(ErrorCode::JsonParseError, "上传会话不是 JSON 对象：" + path_to_utf8(dir));
    }

    Session session;
    session.id = value.value("upload_id", std::string{});
    session.user = value.value("user", std::string{});
    session.bucket = value.value("bucket", std::string{});
    session.source_path = value.value("source_path", std::string{});
    session.source_mtime = value.value("source_mtime", std::int64_t{0});
    session.file_name = value.value("file_name", std::string{});
    session.size = value.value("size", std::uint64_t{0});
    session.chunk_size = value.value("chunk_size", std::uint64_t{0});
    session.chunk_count = value.value("chunk_count", std::uint64_t{0});
    session.md5 = value.value("md5", std::string{});
    session.state = value.value("state", std::string{"uploading"});
    session.uploaded_bytes = value.value("uploaded_bytes", std::uint64_t{0});
    session.created_at = value.value("created_at", std::string{});
    session.updated_at = value.value("updated_at", std::string{});
    session.expires_at = value.value("expires_at", std::string{});
    session.final_file_id = value.value("final_file_id", std::string{});
    session.final_file_name = value.value("final_file_name", std::string{});
    session.error = value.value("error", std::string{});

    if (session.id.empty() || session.chunk_count == 0 && session.size != 0) {
        return make_error(ErrorCode::JsonParseError, "上传会话字段不完整：" + path_to_utf8(dir));
    }
    session.received.assign(static_cast<std::size_t>(session.chunk_count), false);
    if (const auto items = value.find("received"); items != value.end() && items->is_array()) {
        for (const auto& item : *items) {
            if (item.is_number_unsigned()) {
                const std::uint64_t index = item.get<std::uint64_t>();
                if (index < session.received.size()) {
                    session.received[static_cast<std::size_t>(index)] = true;
                }
            }
        }
    }
    session.chunk_hashes.assign(static_cast<std::size_t>(session.chunk_count), std::string{});
    if (const auto items = value.find("chunk_hashes"); items != value.end() && items->is_array()) {
        const std::size_t limit = std::min<std::size_t>(items->size(), session.chunk_hashes.size());
        for (std::size_t i = 0; i < limit; ++i) {
            if ((*items)[i].is_string()) {
                session.chunk_hashes[i] = (*items)[i].get<std::string>();
            }
        }
    }
    return session;
}

Status UploadSessionManager::save_session(const Session& session) const {
    const std::filesystem::path dir = session_dir(session.id);
    if (const Status created = ensure_directory(dir); !ok(created)) {
        return created;
    }
    return write_json_file(dir / "session.json", session_json(session, false));
}

void UploadSessionManager::load_existing() {
    std::error_code code;
    if (!std::filesystem::is_directory(sessions_root(), code)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(sessions_root(), code)) {
        if (code) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        Result<Session> loaded = load_session(entry.path());
        if (!ok(loaded)) {
            if (logger_ != nullptr) {
                logger_->warn("Upload", "跳过损坏的上传会话：" + path_to_utf8(entry.path()));
            }
            continue;
        }
        Session session = std::get<Session>(loaded);
        if (is_expired(session)) {
            std::error_code ignored;
            std::filesystem::remove_all(entry.path(), ignored);
            continue;
        }
        // 服务重启/崩溃时后台线程已经没了；把“传输中”标成暂停，
        // 让 upload resume 能重新拉起，而不是永远卡在 uploading。
        if (session.state == "uploading" || session.state == "completing") {
            session.state = "paused";
            session.error = "服务重启，上传已暂停；执行 upload resume 继续";
            touch(session);
            save_session(session);
        } else if (session.state == "error") {
            // 旧版本失败时可能留下几十 GB 分片；失败不是暂停，启动时清掉。
            cleanup_session_files(session.id);
            session.uploaded_bytes = 0;
            session.received.assign(static_cast<std::size_t>(session.chunk_count), false);
            session.chunk_hashes.assign(static_cast<std::size_t>(session.chunk_count),
                                        std::string{});
            touch(session);
            save_session(session);
        }
        sessions_[session.id] = std::move(session);
    }
}

void UploadSessionManager::cleanup_expired_locked() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (is_expired(it->second) && !running_.count(it->first)) {
            std::error_code ignored;
            std::filesystem::remove_all(session_dir(it->first), ignored);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

Result<nlohmann::json> UploadSessionManager::start(const nlohmann::json& args,
                                                   const std::string& user,
                                                   const std::string& bucket) {
    if (user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    if (bucket.empty()) {
        return make_error(ErrorCode::NoCurrentBucket, "未设置当前 Bucket");
    }
    // 大文件开始前先确认桶目录仍在：user delete / 手工删目录后，
    // config.json 里的旧桶名可能已经失效，不能等 7GB 传完才在 commit 报错。
    {
        std::error_code bucket_code;
        const std::filesystem::path bucket_dir =
            paths_.repository() / path_from_utf8(user) / path_from_utf8(bucket);
        if (!std::filesystem::is_directory(bucket_dir, bucket_code)) {
            return make_error(ErrorCode::NoCurrentBucket,
                              "当前 Bucket 的目录不存在：" + bucket +
                                  "；请先 bucket create / bucket use");
        }
    }
    Config request_config = config_;
    request_config.current_user = user;
    request_config.current_bucket = bucket;

    const auto argv = args.find("argv");
    if (argv == args.end() || !argv->is_array() || argv->empty() ||
        !(*argv)[0].is_string()) {
        return make_error(ErrorCode::InvalidArgument, "upload start 需要本地文件路径");
    }
    const std::string source_text = clean_user_path((*argv)[0].get<std::string>());
    std::string name;
    if (argv->size() > 1 && (*argv)[1].is_string()) {
        name = clean_user_path((*argv)[1].get<std::string>());
    }
    const std::filesystem::path source = path_from_utf8(source_text);
    std::error_code code;
    if (!std::filesystem::is_regular_file(source, code)) {
        return make_error(ErrorCode::FileNotFound, "本地文件不存在：" + source_text);
    }
    if (name.empty()) {
        name = clean_user_path(file_name_from_source(source_text));
    }
    if (name.empty()) {
        return make_error(ErrorCode::FileNameEmpty, "无法从路径推断文件名，请显式给文件名");
    }
    // 文件名在**开始上传前**就校验；不要等 7GB 传完到 commit 才报 FMT-10x。
    if (const Status valid = validate_file_name(name); !ok(valid)) {
        return *error_of(valid);
    }

    const std::uintmax_t raw_size = std::filesystem::file_size(source, code);
    if (code) {
        return make_error(ErrorCode::StorageError, "读取文件大小失败：" + source_text);
    }
    const std::uint64_t size = static_cast<std::uint64_t>(raw_size);
    if (size > config_.max_large_upload_size) {
        return make_error(ErrorCode::UploadLimitExceeded,
                          "文件超过大文件上限：" + std::to_string(size) + " > " +
                              std::to_string(config_.max_large_upload_size));
    }

    std::int64_t source_mtime = 0;
    code.clear();
    const auto write_time = std::filesystem::last_write_time(source, code);
    if (!code) {
        source_mtime = std::chrono::duration_cast<std::chrono::seconds>(
                           write_time.time_since_epoch())
                           .count();
    }

    // 秒传快路径：同一个本地文件（路径 + 大小 + mtime）已经有会话时，
    // 直接复用，不再制造一条新的 completed 记录。
    {
        std::lock_guard<std::mutex> guard(mutex_);
        cleanup_expired_locked();
        for (auto& [session_id, candidate] : sessions_) {
            if (!iequals(candidate.user, user) ||
                !iequals(candidate.bucket, bucket) ||
                !iequals(candidate.source_path, source_text) || candidate.size != size) {
                continue;
            }
            if (candidate.source_mtime != 0 && source_mtime != 0 &&
                candidate.source_mtime != source_mtime) {
                continue;
            }
            if (!iequals(name, candidate.file_name) &&
                !iequals(name, candidate.final_file_name)) {
                continue;
            }
            if (candidate.state != "uploading" && candidate.state != "paused" &&
                candidate.state != "completing" && candidate.state != "completed") {
                continue;
            }
            if (candidate.state == "completed") {
                if (candidate.final_file_id.empty()) {
                    continue;
                }
                FileService files(paths_, request_config, logger_);
                Result<FileRecord> existing = files.get_by_id(candidate.final_file_id);
                if (!ok(existing)) {
                    continue;
                }
                const FileRecord& record = std::get<FileRecord>(existing);
                if (record.is_trash || !iequals(record.bucket, bucket)) {
                    continue;
                }
            }
            touch(candidate);
            save_session(candidate);
            nlohmann::json reused = session_json(candidate, false);
            reused["reused"] = true;
            reused["message"] = candidate.state == "completed"
                                    ? "秒传：已有相同文件，复用 upload_id " + session_id
                                    : "该文件已有上传中的会话，复用 upload_id " + session_id;
            return reused;
        }
    }

    std::uint64_t chunk_size = args.value("chunk_size", std::uint64_t{0});
    if (chunk_size == 0) {
        chunk_size = config_.upload_chunk_size;
    }
    chunk_size = std::max(kMinChunkSize, std::min(chunk_size, kMaxChunkSize));
    if (size > 0 && chunk_size > size) {
        chunk_size = size;
    }
    const std::uint64_t chunk_count = size == 0 ? 0 : (size + chunk_size - 1) / chunk_size;

    // 活动会话上限：防止一个账号建无限会话占满磁盘。
    std::size_t active = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        cleanup_expired_locked();
        for (const auto& item : sessions_) {
            if (item.second.user == user &&
                (item.second.state == "uploading" || item.second.state == "paused" ||
                 item.second.state == "completing")) {
                ++active;
            }
        }
    }
    if (active >= config_.upload_max_active_per_user) {
        return make_error(ErrorCode::UploadLimitExceeded,
                          "活动上传会话过多（上限 " +
                              std::to_string(config_.upload_max_active_per_user) + "）");
    }

    // 完成时需要分片 + 拼接临时文件两份空间；先拒绝，避免传到一半才炸。
    const std::filesystem::space_info space = std::filesystem::space(paths_.temp(), code);
    if (!code) {
        const std::uint64_t required = size * 2 + chunk_size;
        if (space.available < required) {
            return make_error(ErrorCode::InsufficientStorage,
                              "磁盘空间不足：需要约 " + std::to_string(required) + " 字节，可用 " +
                                  std::to_string(space.available) + " 字节");
        }
    }

    Session session;
    session.user = user;
    session.bucket = bucket;
    session.source_path = source_text;
    session.source_mtime = source_mtime;
    session.file_name = name;
    session.size = size;
    session.chunk_size = chunk_size;
    session.chunk_count = chunk_count;
    session.received.assign(static_cast<std::size_t>(chunk_count), false);
    session.chunk_hashes.assign(static_cast<std::size_t>(chunk_count), std::string{});
    session.created_at = local_datetime_iso();
    touch(session);

    {
        std::lock_guard<std::mutex> guard(mutex_);
        do {
            session.id = "up-" + random_hex(16);
        } while (sessions_.count(session.id) || std::filesystem::exists(session_dir(session.id)));
        if (const Status saved = save_session(session); !ok(saved)) {
            return *error_of(saved);
        }
        sessions_[session.id] = session;
    }

    spawn_worker(session.id);
    return session_json(session, false);
}

Result<nlohmann::json> UploadSessionManager::init_for(const nlohmann::json& args,
                                                      const std::string& user,
                                                      const std::string& bucket) {
    if (user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    if (bucket.empty()) {
        return make_error(ErrorCode::NoCurrentBucket, "未设置当前 Bucket");
    }
    std::error_code bucket_code;
    const std::filesystem::path bucket_dir =
        paths_.repository() / path_from_utf8(user) / path_from_utf8(bucket);
    if (!std::filesystem::is_directory(bucket_dir, bucket_code)) {
        return make_error(ErrorCode::NoCurrentBucket, "当前 Bucket 的目录不存在：" + bucket);
    }

    std::string name = clean_user_path(args.value("file_name", std::string{}));
    if (name.empty() && args.contains("argv") && args["argv"].is_array() &&
        !args["argv"].empty() && args["argv"][0].is_string()) {
        name = clean_user_path(args["argv"][0].get<std::string>());
    }
    if (name.empty()) {
        return make_error(ErrorCode::FileNameEmpty, "缺少 file_name");
    }
    if (const Status valid = validate_file_name(name); !ok(valid)) {
        return *error_of(valid);
    }

    const std::uint64_t size = args.value("size", std::uint64_t{0});
    if (size > config_.max_large_upload_size) {
        return make_error(ErrorCode::UploadLimitExceeded,
                          "文件超过大文件上限：" + std::to_string(size) + " > " +
                              std::to_string(config_.max_large_upload_size));
    }
    std::uint64_t chunk_size = args.value("chunk_size", std::uint64_t{0});
    if (chunk_size == 0) {
        chunk_size = config_.upload_chunk_size;
    }
    chunk_size = std::max(kMinChunkSize, std::min(chunk_size, kMaxChunkSize));
    if (size > 0 && chunk_size > size) {
        chunk_size = size;
    }
    const std::uint64_t chunk_count = size == 0 ? 0 : (size + chunk_size - 1) / chunk_size;
    const std::string md5 = clean_user_path(args.value("md5", std::string{}));

    // 客户端带 MD5 时可以先查已有内容，命中直接秒传。
    if (!md5.empty()) {
        std::lock_guard<std::mutex> guard(mutex_);
        cleanup_expired_locked();
        Config request_config = config_;
        request_config.current_user = user;
        request_config.current_bucket = bucket;
        FileService files(paths_, request_config, logger_);
        for (const auto& item : sessions_) {
            const Session& candidate = item.second;
            if (!owned(candidate, user, bucket) || candidate.state != "completed" ||
                !iequals(candidate.md5, md5)) {
                continue;
            }
            if (candidate.final_file_id.empty()) {
                continue;
            }
            Result<FileRecord> existing = files.get_by_id(candidate.final_file_id);
            if (!ok(existing)) {
                continue;
            }
            const FileRecord& record = std::get<FileRecord>(existing);
            if (record.is_trash || !iequals(record.bucket, bucket)) {
                continue;
            }
            nlohmann::json reused = session_json(candidate, false);
            reused["reused"] = true;
            reused["message"] = "秒传：已有相同内容，复用 upload_id " + candidate.id;
            return reused;
        }
    }

    std::size_t active = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        cleanup_expired_locked();
        for (const auto& item : sessions_) {
            if (item.second.user == user &&
                (item.second.state == "uploading" || item.second.state == "paused" ||
                 item.second.state == "completing")) {
                ++active;
            }
        }
    }
    if (active >= config_.upload_max_active_per_user) {
        return make_error(ErrorCode::UploadLimitExceeded,
                          "活动上传会话过多（上限 " +
                              std::to_string(config_.upload_max_active_per_user) + "）");
    }

    const std::filesystem::space_info space = std::filesystem::space(paths_.temp(), bucket_code);
    if (!bucket_code) {
        const std::uint64_t required = size * 2 + chunk_size;
        if (space.available < required) {
            return make_error(ErrorCode::InsufficientStorage,
                              "磁盘空间不足：需要约 " + std::to_string(required) + " 字节，可用 " +
                                  std::to_string(space.available) + " 字节");
        }
    }

    Session session;
    session.user = user;
    session.bucket = bucket;
    session.file_name = name;
    session.size = size;
    session.chunk_size = chunk_size;
    session.chunk_count = chunk_count;
    session.md5 = md5;
    session.received.assign(static_cast<std::size_t>(chunk_count), false);
    session.chunk_hashes.assign(static_cast<std::size_t>(chunk_count), std::string{});
    session.created_at = local_datetime_iso();
    touch(session);

    {
        std::lock_guard<std::mutex> guard(mutex_);
        do {
            session.id = "up-" + random_hex(16);
        } while (sessions_.count(session.id) || std::filesystem::exists(session_dir(session.id)));
        if (const Status saved = save_session(session); !ok(saved)) {
            return *error_of(saved);
        }
        sessions_[session.id] = session;
    }
    return session_json(session, false);
}

Result<nlohmann::json> UploadSessionManager::put_chunk_for(const std::string& upload_id,
                                                           std::uint64_t index,
                                                           std::string_view data,
                                                           const std::string& expected_md5,
                                                           const std::string& user,
                                                           const std::string& bucket) {
    std::lock_guard<std::mutex> guard(mutex_);
    cleanup_expired_locked();
    const auto found = sessions_.find(upload_id);
    if (found == sessions_.end() || !owned(found->second, user, bucket)) {
        return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + upload_id);
    }
    Session& session = found->second;
    if (session.state != "uploading") {
        return make_error(ErrorCode::UploadStateInvalid,
                          "当前状态不能写分片：" + session.state);
    }
    if (index >= session.chunk_count) {
        return make_error(ErrorCode::ChunkIndexInvalid, "分片下标越界：" + std::to_string(index));
    }
    const std::uint64_t offset = index * session.chunk_size;
    const std::uint64_t length = std::min(session.chunk_size, session.size - offset);
    if (data.size() != static_cast<std::size_t>(length)) {
        return make_error(ErrorCode::ChunkMismatch, "分片大小不对");
    }
    const std::string actual_md5 = md5_hex(data);
    if (!expected_md5.empty() && !iequals(expected_md5, actual_md5)) {
        return make_error(ErrorCode::ChunkMismatch, "分片 MD5 与 X-Chunk-MD5 不匹配");
    }

    const std::filesystem::path target = chunk_path(upload_id, index);
    const std::filesystem::path temp = target.string() + ".tmp";
    std::error_code code;
    std::filesystem::create_directories(target.parent_path(), code);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::StorageError, "无法写入分片：" + path_to_utf8(temp));
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            std::filesystem::remove(temp, code);
            return make_error(ErrorCode::StorageError, "写入分片失败");
        }
    }
    std::filesystem::rename(temp, target, code);
    if (code) {
        std::filesystem::remove(temp, code);
        return make_error(ErrorCode::StorageError, "分片改名失败：" + path_to_utf8(target));
    }

    if (!session.received[static_cast<std::size_t>(index)]) {
        session.uploaded_bytes += length;
    }
    session.received[static_cast<std::size_t>(index)] = true;
    session.chunk_hashes[static_cast<std::size_t>(index)] = actual_md5;
    touch(session);
    if (const Status saved = save_session(session); !ok(saved)) {
        return *error_of(saved);
    }
    return session_json(session, false);
}

Result<nlohmann::json> UploadSessionManager::lookup(const std::string& upload_id) {
    return lookup_for(upload_id, config_.current_user, config_.current_bucket);
}

Result<nlohmann::json> UploadSessionManager::lookup_for(const std::string& upload_id,
                                                         const std::string& user,
                                                         const std::string& bucket) {
    return status(upload_id, user, bucket);
}

Result<nlohmann::json> UploadSessionManager::status(const std::string& id, const std::string& user,
                                                    const std::string& bucket) {
    std::lock_guard<std::mutex> guard(mutex_);
    cleanup_expired_locked();
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
    }
    if (!owned(found->second, user, bucket)) {
        return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
    }
    return session_json(found->second, true);
}

Result<nlohmann::json> UploadSessionManager::list(bool all, const std::string& user,
                                                   const std::string& bucket) {
    std::lock_guard<std::mutex> guard(mutex_);
    cleanup_expired_locked();
    nlohmann::json uploads = nlohmann::json::array();
    std::unordered_map<std::string, std::size_t> seen_final;
    std::unordered_map<std::string, std::size_t> seen_md5;
    Config list_config = config_;
    list_config.current_user = user;
    list_config.current_bucket = bucket;
    for (const auto& item : sessions_) {
        const Session& session = item.second;
        if (!owned(session, user, bucket)) {
            continue;
        }
        if (!all && session.state == "canceled") {
            continue;
        }
        if (!all && session.state == "completed") {
            if (session.final_file_id.empty()) {
                continue;
            }
            FileService files(paths_, list_config, logger_);
            Result<FileRecord> record = files.get_by_id(session.final_file_id);
            if (!ok(record)) {
                continue;
            }
            const FileRecord& current = std::get<FileRecord>(record);
            if (current.is_trash || !iequals(current.bucket, bucket)) {
                continue;
            }
            std::optional<std::size_t> visible;
            if (!session.final_file_id.empty()) {
                const auto found = seen_final.find(session.final_file_id);
                if (found != seen_final.end()) {
                    visible = found->second;
                }
            }
            if (!visible.has_value() && !session.md5.empty()) {
                const auto found = seen_md5.find(session.md5);
                if (found != seen_md5.end()) {
                    visible = found->second;
                }
            }
            if (visible.has_value()) {
                uploads[*visible]["duplicate_count"] =
                    uploads[*visible].value("duplicate_count", 1) + 1;
                if (!session.final_file_id.empty()) {
                    seen_final[session.final_file_id] = *visible;
                }
                if (!session.md5.empty()) {
                    seen_md5[session.md5] = *visible;
                }
                continue;
            }
            const std::size_t index = uploads.size();
            if (!session.final_file_id.empty()) {
                seen_final.emplace(session.final_file_id, index);
            }
            if (!session.md5.empty()) {
                seen_md5.emplace(session.md5, index);
            }
        }
        nlohmann::json value = session_json(session, false);
        value["duplicate_count"] = 1;
        uploads.push_back(std::move(value));
    }

    nlohmann::json data = nlohmann::json::object();
    data["uploads"] = std::move(uploads);
    data["count"] = data["uploads"].size();
    data["total_sessions"] = sessions_.size();
    data["all"] = all;
    return data;
}

Result<nlohmann::json> UploadSessionManager::pause(const std::string& id, const std::string& user,
                                                   const std::string& bucket) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
    }
    if (!owned(found->second, user, bucket)) {
        return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
    }
    Session& session = found->second;
    if (session.state == "paused") {
        return session_json(session, true);
    }
    if (session.state != "uploading") {
        return make_error(ErrorCode::UploadStateInvalid,
                          "当前状态不能暂停：" + session.state);
    }
    session.state = "paused";
    touch(session);
    if (const Status saved = save_session(session); !ok(saved)) {
        return *error_of(saved);
    }
    return session_json(session, true);
}

Result<nlohmann::json> UploadSessionManager::resume(const std::string& id, const std::string& user,
                                                    const std::string& bucket) {
    std::thread old_worker;
    bool wait_for_old = false;
    bool has_local_source = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        if (!owned(found->second, user, bucket)) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        Session& session = found->second;
        has_local_source = !session.source_path.empty();
        if (session.state == "completed") {
            return session_json(session, false);
        }
        if (session.state == "uploading") {
            // 正常情况下 worker 还在；如果状态是 uploading 但线程已经没了
            // （异常退出/旧会话），也允许重新拉起。
            if (running_.count(id) > 0) {
                return session_json(session, false);
            }
        } else if (session.state == "paused" || session.state == "error") {
            // 暂停/报错时旧 worker 可能还在收尾。先把线程交出来，锁外 join；
            // 否则 resume 设置 uploading 会让旧 worker 继续跑，或者新 worker
            // 因为 running_ 仍是 true 而根本起不来。
            if (running_.count(id) > 0) {
                const auto worker = workers_.find(id);
                if (worker != workers_.end()) {
                    if (worker->second.joinable()) {
                        old_worker = std::move(worker->second);
                    }
                    workers_.erase(worker);
                }
                wait_for_old = true;
            }
        } else {
            return make_error(ErrorCode::UploadStateInvalid,
                              "当前状态不能继续：" + session.state);
        }
        if (!wait_for_old) {
            session.state = "uploading";
            session.error.clear();
            touch(session);
            if (const Status saved = save_session(session); !ok(saved)) {
                return *error_of(saved);
            }
        }
    }

    if (old_worker.joinable()) {
        old_worker.join();
    }

    if (wait_for_old) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        if (!owned(found->second, user, bucket)) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        Session& session = found->second;
        if (session.state == "canceled") {
            return session_json(session, false);
        }
        if (session.state == "completed") {
            return session_json(session, false);
        }
        session.state = "uploading";
        session.error.clear();
        touch(session);
        if (const Status saved = save_session(session); !ok(saved)) {
            return *error_of(saved);
        }
    }

    if (has_local_source) {
        spawn_worker(id);
    }
    return status(id, user, bucket);
}

Result<nlohmann::json> UploadSessionManager::cancel(const std::string& id, const std::string& user,
                                                    const std::string& bucket) {
    bool running = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        if (!owned(found->second, user, bucket)) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        Session& session = found->second;
        if (session.state == "completed") {
            return make_error(ErrorCode::UploadStateInvalid, "上传已完成，不能取消");
        }
        session.state = "canceled";
        session.error.clear();
        touch(session);
        if (const Status saved = save_session(session); !ok(saved)) {
            return *error_of(saved);
        }
        running = running_.count(id) > 0;
    }
    if (!running) {
        cleanup_session_files(id);
    }
    return status(id, user, bucket);
}

Result<nlohmann::json> UploadSessionManager::complete(const std::string& id,
                                                      const std::string& user,
                                                      const std::string& bucket) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        if (!owned(found->second, user, bucket)) {
            return make_error(ErrorCode::UploadSessionNotFound, "上传会话不存在：" + id);
        }
        if (!all_received(found->second)) {
            return make_error(ErrorCode::UploadIncomplete, "分片不完整，不能完成");
        }
        if (found->second.state == "completed") {
            return session_json(found->second, false);
        }
        found->second.state = "completing";
        touch(found->second);
        if (const Status saved = save_session(found->second); !ok(saved)) {
            return *error_of(saved);
        }
    }
    complete_in_worker(id);
    return status(id, user, bucket);
}

bool UploadSessionManager::all_received(const Session& session) const {
    return std::all_of(session.received.begin(), session.received.end(),
                       [](bool value) { return value; });
}

bool UploadSessionManager::should_upload_continue(const std::string& id) const {
    if (stopping_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = sessions_.find(id);
    return found != sessions_.end() && found->second.state == "uploading";
}

bool UploadSessionManager::owned(const Session& session, const std::string& user,
                                 const std::string& bucket) const {
    return iequals(session.user, user) && iequals(session.bucket, bucket);
}

std::uint64_t UploadSessionManager::next_missing(const Session& session) const {
    for (std::uint64_t i = 0; i < session.received.size(); ++i) {
        if (!session.received[i]) {
            return i;
        }
    }
    return session.chunk_count;
}

void UploadSessionManager::spawn_worker(const std::string& id) {
    std::thread old;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_.load() || running_.count(id)) {
            return;
        }
        const auto found = workers_.find(id);
        if (found != workers_.end()) {
            if (found->second.joinable()) {
                old = std::move(found->second);
            }
            workers_.erase(found);
        }
        running_.insert(id);
    }
    if (old.joinable()) {
        old.join();
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_.load() || !running_.count(id)) {
            return;
        }
        workers_[id] = std::thread([this, id] { worker(id); });
    }
}

void UploadSessionManager::worker(const std::string& id) {
    // 先算源文件 MD5，再开始分片：
    //   * 若桶里已有相同内容（不管名字），直接复用，不白传 7GB；
    //   * 若同名但内容不同，先把目标名改成 xxx01，再传；
    //   * 最终拼接后还会用这个 MD5 校验一次。
    std::string expected_md5;
    std::string source_path;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            running_.erase(id);
            return;
        }
        expected_md5 = found->second.md5;
        source_path = found->second.source_path;
    }
    if (expected_md5.empty()) {
        bool aborted = false;
        expected_md5 = md5_file(
            path_from_utf8(source_path),
            [this, &id]() { return should_upload_continue(id); }, &aborted);
        if (aborted) {
            std::lock_guard<std::mutex> guard(mutex_);
            running_.erase(id);
            const auto found = sessions_.find(id);
            if (found != sessions_.end() && found->second.state == "canceled") {
                std::filesystem::remove_all(session_dir(id));
                sessions_.erase(found);
            }
            return;
        }
        if (expected_md5.empty()) {
            set_error(id, "无法计算源文件 MD5：" + source_path);
            std::lock_guard<std::mutex> guard(mutex_);
            running_.erase(id);
            return;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            running_.erase(id);
            return;
        }
        found->second.md5 = expected_md5;
        touch(found->second);
        save_session(found->second);
    }

    if (prepare_target(id, expected_md5)) {
        std::lock_guard<std::mutex> guard(mutex_);
        running_.erase(id);
        return;
    }

    bool completed = false;
    while (!stopping_.load()) {
        std::uint64_t index = 0;
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        std::string source;
        bool done = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = sessions_.find(id);
            if (found == sessions_.end()) {
                running_.erase(id);
                return;
            }
            Session& session = found->second;
            if (session.state == "paused" || session.state == "canceled" ||
                session.state == "completed" || session.state == "error") {
                break;
            }
            if (all_received(session)) {
                session.state = "completing";
                touch(session);
                save_session(session);
                done = true;
            } else {
                index = next_missing(session);
                offset = index * session.chunk_size;
                length = std::min(session.chunk_size, session.size - offset);
                source = session.source_path;
            }
        }
        if (done) {
            completed = true;
            break;
        }

        std::ifstream in(path_from_utf8(source), std::ios::binary);
        if (!in) {
            set_error(id, "无法读取源文件：" + source);
            break;
        }
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in) {
            set_error(id, "定位源文件失败：" + source);
            break;
        }

        const std::filesystem::path target = chunk_path(id, index);
        const std::filesystem::path temp = target.string() + ".tmp";
        std::error_code code;
        std::filesystem::create_directories(target.parent_path(), code);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(id, "无法写入分片：" + path_to_utf8(temp));
            break;
        }
        std::vector<char> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(kCopyBuffer, length)));
        std::uint64_t left = length;
        bool failed = false;
        bool hash_failed = false;
        Md5 chunk_hash;
        while (left > 0) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
            if (!read_exact(in, buffer.data(), want)) {
                failed = true;
                break;
            }
            if (!chunk_hash.update(buffer.data(), want)) {
                hash_failed = true;
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(want));
            if (!out) {
                failed = true;
                break;
            }
            left -= want;
        }
        out.close();
        if (failed || hash_failed) {
            std::filesystem::remove(temp, code);
            set_error(id, hash_failed ? "分片 MD5 计算失败" : "读取或写入分片失败，源文件可能已被修改");
            break;
        }
        const std::string chunk_digest = chunk_hash.finish();
        if (chunk_digest.empty()) {
            std::filesystem::remove(temp, code);
            set_error(id, "分片 MD5 计算失败");
            break;
        }
        std::filesystem::rename(temp, target, code);
        if (code) {
            std::filesystem::remove(temp, code);
            set_error(id, "分片改名失败：" + path_to_utf8(target));
            break;
        }

        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = sessions_.find(id);
            if (found == sessions_.end()) {
                running_.erase(id);
                return;
            }
            Session& session = found->second;
            if (!session.received[static_cast<std::size_t>(index)]) {
                session.received[static_cast<std::size_t>(index)] = true;
                session.chunk_hashes[static_cast<std::size_t>(index)] = chunk_digest;
                session.uploaded_bytes += length;
            }
            touch(session);
            save_session(session);
            if (session.state == "paused" || session.state == "canceled") {
                break;
            }
        }
    }

    if (completed) {
        complete_in_worker(id);
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        running_.erase(id);
        const auto found = sessions_.find(id);
        if (found != sessions_.end() && found->second.state == "canceled") {
            std::filesystem::remove_all(session_dir(id));
            sessions_.erase(found);
        }
    }
}

bool UploadSessionManager::prepare_target(const std::string& id, const std::string& md5) {
    Session snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return true;
        }
        snapshot = found->second;
    }

    Config commit_config = config_;
    commit_config.current_user = snapshot.user;
    commit_config.current_bucket = snapshot.bucket;
    FileService files(paths_, commit_config, logger_);

    // 同内容验证：当前用户 + 当前 Bucket 里已有相同 MD5，直接复用。
    Result<std::vector<FileRecord>> listed = files.list();
    if (!ok(listed)) {
        set_error(id, "检查已有文件失败：" + error_of(listed)->message);
        return true;
    }
    for (const FileRecord& record : std::get<std::vector<FileRecord>>(listed)) {
        if (!record.md5.empty() && iequals(record.md5, md5)) {
            cleanup_session_files(id);
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = sessions_.find(id);
            if (found != sessions_.end()) {
                found->second.state = "completed";
                found->second.final_file_id = record.file_id;
                found->second.final_file_name = record.file_name;
                found->second.uploaded_bytes = found->second.size;
                touch(found->second);
                save_session(found->second);
            }
            return true;
        }
    }

    // 同名不同内容：在开始传分片前就改成 xxx01，避免传完 7GB 才发现冲突。
    std::string target = snapshot.file_name;
    for (int attempt = 0; attempt < kNameAttempts; ++attempt) {
        Result<FileRecord> existing = files.get_by_name(target);
        if (!ok(existing)) {
            if (error_of(existing)->code == ErrorCode::FileNotFound) {
                break;
            }
            set_error(id, "检查文件名失败：" + error_of(existing)->message);
            return true;
        }
        target = next_name(snapshot.file_name, attempt + 1);
    }
    if (!iequals(target, snapshot.file_name)) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) {
            found->second.file_name = target;
            touch(found->second);
            save_session(found->second);
        }
    }
    return false;
}

void UploadSessionManager::complete_in_worker(const std::string& id) {
    Session snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return;
        }
        snapshot = found->second;
        if (!all_received(snapshot)) {
            set_error(id, "分片不完整，不能完成");
            return;
        }
        found->second.state = "completing";
        touch(found->second);
        save_session(found->second);
    }

    const std::filesystem::path assembled = paths_.temp() / ("fmt-upload-" + id + ".tmp");
    {
        std::ofstream out(assembled, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(id, "无法创建最终临时文件：" + path_to_utf8(assembled));
            return;
        }
        Md5 hash;
        std::vector<char> buffer(kCopyBuffer);
        for (std::uint64_t index = 0; index < snapshot.chunk_count; ++index) {
            std::ifstream in(chunk_path(id, index), std::ios::binary);
            if (!in) {
                out.close();
                std::filesystem::remove(assembled);
                set_error(id, "分片 " + std::to_string(index) +
                                  " 缺失；已清理残片，可 upload resume 重传");
                return;
            }
            Md5 chunk_hash;
            while (in) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = in.gcount();
                if (got > 0) {
                    if (!chunk_hash.update(buffer.data(), static_cast<std::size_t>(got)) ||
                        !hash.update(buffer.data(), static_cast<std::size_t>(got))) {
                        in.close();
                        out.close();
                        std::filesystem::remove(assembled);
                        set_error(id, "MD5 计算失败");
                        return;
                    }
                    out.write(buffer.data(), got);
                }
            }
            const std::string actual_chunk = chunk_hash.finish();
            const std::string& expected_chunk =
                snapshot.chunk_hashes[static_cast<std::size_t>(index)];
            if (actual_chunk.empty()) {
                in.close();
                out.close();
                std::filesystem::remove(assembled);
                set_error(id, "分片 MD5 计算失败");
                return;
            }
            if (!expected_chunk.empty() && !iequals(expected_chunk, actual_chunk)) {
                in.close();
                out.close();
                std::filesystem::remove(assembled);
                set_error(id, "分片 " + std::to_string(index) +
                                  " 校验失败；已清理残片，可 upload resume 重传");
                return;
            }
        }
        out.close();
        if (!out) {
            std::filesystem::remove(assembled);
            set_error(id, "写入最终临时文件失败");
            return;
        }

        std::error_code size_error;
        const std::uintmax_t actual = std::filesystem::file_size(assembled, size_error);
        if (size_error || actual != snapshot.size) {
            std::filesystem::remove(assembled);
            set_error(id, "拼接后大小不一致");
            return;
        }
        std::string md5 = hash.finish();
        if (md5.empty()) {
            std::filesystem::remove(assembled);
            set_error(id, "MD5 计算失败");
            return;
        }
        if (!snapshot.md5.empty() && !iequals(snapshot.md5, md5)) {
            std::filesystem::remove(assembled);
            set_error(id, "整文件 MD5 不匹配");
            return;
        }

        PreparedUpload prepared;
        prepared.temp_path = assembled;
        prepared.file_name = snapshot.file_name;
        prepared.size = snapshot.size;
        prepared.md5 = md5;

        // 会话绑定创建时的 user/bucket；CLI 之后切换当前 Bucket 不能影响它。
        Config commit_config = config_;
        commit_config.current_user = snapshot.user;
        commit_config.current_bucket = snapshot.bucket;
        FileService files(paths_, commit_config, logger_);
        std::string final_name = snapshot.file_name;
        for (int attempt = 0; attempt < kNameAttempts; ++attempt) {
            Result<FileRecord> existing = files.get_by_name(final_name);
            if (!ok(existing) && error_of(existing)->code == ErrorCode::FileNotFound) {
                break;
            }
            if (!ok(existing)) {
                std::filesystem::remove(assembled);
                set_error(id, "检查文件重名失败：" + error_of(existing)->message);
                return;
            }
            const FileRecord& record = std::get<FileRecord>(existing);
            if (!record.md5.empty() && iequals(record.md5, md5)) {
                std::filesystem::remove(assembled);
                cleanup_session_files(id);
                std::lock_guard<std::mutex> guard(mutex_);
                const auto found = sessions_.find(id);
                if (found != sessions_.end()) {
                    found->second.state = "completed";
                    found->second.final_file_id = record.file_id;
                    found->second.final_file_name = record.file_name;
                    found->second.uploaded_bytes = found->second.size;
                    touch(found->second);
                    save_session(found->second);
                }
                return;
            }
            final_name = next_name(snapshot.file_name, attempt + 1);
        }
        prepared.file_name = final_name;

        Result<FileRecord> committed = files.commit_upload(prepared);
        if (!ok(committed)) {
            set_error(id, error_of(committed)->message);
            return;
        }
        const FileRecord& record = std::get<FileRecord>(committed);

        // 先清分片，再把状态置为 completed：状态一旦可见，chunks 目录就已经没了。
        cleanup_session_files(id);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = sessions_.find(id);
            if (found != sessions_.end()) {
                found->second.state = "completed";
                found->second.final_file_id = record.file_id;
                found->second.final_file_name = record.file_name;
                found->second.uploaded_bytes = found->second.size;
                touch(found->second);
                save_session(found->second);
            }
        }
    }
}

void UploadSessionManager::set_error(const std::string& id, const std::string& message) {
    // 失败不是暂停：分片全部清掉，只留 session.json 供 status / resume。
    // resume 会从源文件重新传，所以不会留下几十 GB 的残片。
    cleanup_session_files(id);
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        return;
    }
    found->second.state = "error";
    found->second.error = message;
    found->second.uploaded_bytes = 0;
    found->second.received.assign(found->second.chunk_count, false);
    found->second.chunk_hashes.assign(found->second.chunk_count, std::string{});
    touch(found->second);
    save_session(found->second);
}

std::string UploadSessionManager::next_name(const std::string& original, int sequence) const {
    const std::size_t dot = original.find_last_of('.');
    const std::string base = dot == std::string::npos ? original : original.substr(0, dot);
    const std::string extension = dot == std::string::npos ? std::string{} : original.substr(dot);
    std::ostringstream out;
    out << base << std::setw(2) << std::setfill('0') << sequence << extension;
    return out.str();
}

void UploadSessionManager::cleanup_session_files(const std::string& id) const {
    std::error_code ignored;
    std::filesystem::remove_all(chunks_dir(id), ignored);
}

Result<nlohmann::json> UploadSessionManager::handle(const std::string& operation,
                                                     const nlohmann::json& args) {
    return handle_for(operation, args, config_.current_user, config_.current_bucket);
}

Result<nlohmann::json> UploadSessionManager::handle_for(const std::string& operation,
                                                        const nlohmann::json& args,
                                                        const std::string& user,
                                                        const std::string& bucket) {
    reap_finished();

    const auto argv = args.find("argv");
    const auto first_arg = [&]() -> std::string {
        if (argv != args.end() && argv->is_array() && !argv->empty() && (*argv)[0].is_string()) {
            return (*argv)[0].get<std::string>();
        }
        return {};
    };

    if (operation == "upload.start") {
        return start(args, user, bucket);
    }
    if (operation == "upload.init") {
        return init_for(args, user, bucket);
    }
    if (operation == "upload.list") {
        return list(args.value("all", false), user, bucket);
    }
    if (operation == "upload.status") {
        const std::string id = first_arg();
        if (id.empty()) {
            return make_error(ErrorCode::InvalidArgument, "upload status 需要 upload_id");
        }
        return status(id, user, bucket);
    }
    if (operation == "upload.pause") {
        return pause(first_arg(), user, bucket);
    }
    if (operation == "upload.resume") {
        return resume(first_arg(), user, bucket);
    }
    if (operation == "upload.cancel") {
        return cancel(first_arg(), user, bucket);
    }
    if (operation == "upload.complete") {
        return complete(first_arg(), user, bucket);
    }
    return make_error(ErrorCode::InvalidArgument, "未知的上传操作：" + operation);
}

void UploadSessionManager::reap_finished() {
    std::vector<std::thread> finished;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (!running_.count(it->first)) {
                if (it->second.joinable()) {
                    finished.push_back(std::move(it->second));
                }
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (std::thread& thread : finished) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

}  // namespace fmt::service
