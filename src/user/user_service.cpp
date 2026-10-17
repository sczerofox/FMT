#include "fmt/user/user_service.hpp"

#include <utility>

#include "fmt/common/hash.hpp"
#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt::user {
namespace {

constexpr int kTokenBytes = 16;  // 32 位十六进制，与 UserStore 一致
constexpr std::size_t kMaxUsernameBytes = 32;

// 用户名规则：非空、不超过 32 字节、只允许字母数字与 `_ . -`。
// 它同时会成为 repository/<user>/ 的目录名，所以必须预先挡住路径分隔符与 `..`。
bool username_ok(std::string_view username, std::string* reason) {
    if (username.empty()) {
        *reason = "用户名不能为空";
        return false;
    }
    if (username.size() > kMaxUsernameBytes) {
        *reason = "用户名最长 " + std::to_string(kMaxUsernameBytes) + " 个字节";
        return false;
    }
    for (const char ch : username) {
        const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
        if (!allowed) {
            *reason = "用户名只允许字母、数字与 _ . -（它也会作为目录名）";
            return false;
        }
    }
    if (username == "." || username == "..") {
        *reason = "用户名不能是 . 或 ..";
        return false;
    }
    return true;
}

// 「账号内容」的指纹：config.json 的 uuid 存的就是它，用来判定「密码是否已经改过一次」。
//
// **口径（必须守住）**：指纹要排除 **token 相关的三个字段**。
// token 会因为 `user token renew` 而变，而换 token 与"改没改过密码"是两件事：
//   * 若把 token 算进指纹，一次换 token 就会让 uuid 与当前记录不一致
//     → 密码明明没改，却被判成"已经改过"，用户被永久锁死；
//   * 反过来，若换 token 时重写 uuid，就会把"已改过"的状态抹掉，等于重新放开一次改密。
// 所以：指纹只覆盖**账号本身**（用户名、口令哈希与盐、时间戳、桶快照），不覆盖凭证轮换。
std::string record_md5(const UserRecord& record) {
    UserRecord stable = record;          // 拷一份，只改拷贝
    stable.token.clear();                // ← 排除
    stable.token_issued_at.clear();      // ← 排除
    stable.token_expires_at.clear();     // ← 排除
    return md5_hex(UserStore::record_json_text(stable));
}

// 旧口径的指纹（把整份 user.json 都算进去）。只用于**兼容升级**：
// 老版本写进 uuid 的是它，升级后要能认出"这个账号还没改过密码"。
std::string record_md5_legacy(const UserRecord& record) {
    return md5_hex(UserStore::record_json_text(record));
}

}  // namespace

UserService::UserService(const PathManager& paths, Config& config)
    : paths_(paths), config_(config) {}

Result<UserRecord> UserService::current() const {
    UserStore store(paths_, config_);
    return store.load();
}

std::vector<std::string> UserService::missing_data_files() const {
    const std::pair<const char*, std::filesystem::path> expected[] = {
        {"data/user.json", paths_.user_data()},
        {"data/file.json", paths_.file_data()},
        {"data/share.json", paths_.share_data()},
        {"data/trash.json", paths_.trash_data()},
    };
    std::vector<std::string> missing;
    for (const auto& [label, path] : expected) {
        if (!file_exists(path)) {
            missing.emplace_back(label);
        }
    }
    return missing;
}

std::string UserService::first_run_hint() {
    return "这个数据根还没有账号。请先创建账号：\n"
           "  user create <用户名>\n"
           "随后会提示你输入密码（输入时不回显，只显示 *）。";
}

Result<UserRecord> UserService::create(std::string_view username,
                                       std::string_view password_digest_hex) {
    std::string reason;
    if (!username_ok(username, &reason)) {
        return make_error(ErrorCode::InvalidArgument, reason);
    }
    if (password_digest_hex.size() != 64) {
        return make_error(ErrorCode::InvalidArgument, "口令摘要格式不对（应为 64 位十六进制）");
    }

    // 关键数据文件必须先齐全：只有 user.json 缺失时建账号，会把「数据文件丢了」抹平。
    if (const std::vector<std::string> missing = missing_data_files(); !missing.empty()) {
        std::string text;
        for (const std::string& item : missing) {
            text += (text.empty() ? "" : "、") + item;
        }
        return make_error(ErrorCode::StorageError,
                          "关键数据文件缺失：" + text +
                              "。请先恢复这些文件；现在新建账号会把丢失的现场抹平");
    }

    UserStore store(paths_, config_);
    Result<UserRecord> existing = store.load();
    if (!ok(existing)) {
        return *error_of(existing);
    }
    if (!std::get<UserRecord>(existing).empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "这个数据根已经有账号了（" + std::get<UserRecord>(existing).username +
                              "）。一个数据根只能有一个账号；要重建请先 user delete");
    }

    UserRecord record;
    record.username = std::string(username);
    record.user_id = "u-" + random_hex(4);
    record.password_salt = directory_salt(record.username);
    record.password_hash = std::string(password_digest_hex);
    // 按 config 的 token_ttl_seconds 签发 token（带有效期）。
    record.created_at = local_datetime_iso();
    record.updated_at = record.created_at;
    record.current_bucket = config_.current_bucket;  // 快照，运行时权威是 config.json

    if (!issue_token(config_, record)) {
        return make_error(ErrorCode::StorageError, "无法生成账号凭证（CSPRNG 失败）");
    }

    if (const Status saved = store.save(record); !ok(saved)) {
        return *error_of(saved);
    }

    // 「第一次」的基准：把刚写出的 user.json 的 MD5 记进 config.json。
    // current_user 一起跟上——bucket 落在哪个用户名下由它决定。
    config_.uuid = record_md5(record);
    config_.current_user = record.username;
    if (const Status saved = save_config(paths_, config_); !ok(saved)) {
        return *error_of(saved);
    }
    return record;
}

Result<std::string> UserService::password_salt() const {
    UserStore store(paths_, config_);
    Result<UserRecord> loaded = store.load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty()) {
        return make_error(ErrorCode::FileNotFound, "这个数据根还没有账号，请先 user create");
    }
    // 老账号用记录里的随机盐；缺盐（理论上不会）才退回用户名派生。
    const std::string salt =
        record.password_salt.empty() ? directory_salt(record.username) : record.password_salt;
    if (salt.empty()) {
        return make_error(ErrorCode::StorageError, "账号缺少口令盐，无法校验");
    }
    return salt;
}

Result<UserRecord> UserService::verify_digest(std::string_view presented_digest_hex) const {
    UserStore store(paths_, config_);
    Result<UserRecord> loaded = store.load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty()) {
        return make_error(ErrorCode::FileNotFound, "这个数据根还没有账号，请先 user create");
    }
    if (!verify_password_digest(record.password_hash, presented_digest_hex)) {
        return make_error(ErrorCode::Unauthorized, "口令不正确");
    }
    return record;
}

Result<std::string> UserService::update_password(std::string_view current_digest_hex,
                                                 std::string_view new_digest_hex) {
    if (new_digest_hex.size() != 64) {
        return make_error(ErrorCode::InvalidArgument, "口令摘要格式不对（应为 64 位十六进制）");
    }

    UserStore store(paths_, config_);
    Result<UserRecord> loaded = store.load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty()) {
        return make_error(ErrorCode::FileNotFound, "这个数据根还没有账号，请先 user create");
    }

    // 先验旧口令（摘要比对，常量时间），再谈「能不能改」。
    if (!verify_password_digest(record.password_hash, current_digest_hex)) {
        return make_error(ErrorCode::Unauthorized, "当前密码不正确");
    }

    // 判定「是不是第一次改」：uuid 与当前记录的 MD5 一致 == 还没改过。
    // 不一致（= 已经改过一次）→ 拒绝，并说清怎么重来。
    //
    // **这里必须用"改之前"的指纹**：uuid 是「本次改动之前那份文件」的指纹。
    // 曾经写成保存之后再取指纹——那等于把 uuid 设成"改完之后那份文件"的指纹，
    // 而下一次进来加载到的正是它，于是"改之前指纹 == uuid"永远成立，可以无限改
    // （真踩过，被 App.初始化数据根不再自动建账号 这条用例钉住）。
    const std::string fingerprint_before_edit = record_md5(record);
    // **兼容**：老版本把「整份 user.json 的 MD5」写进 uuid。新指纹不再覆盖 token
    // 字段（见 record_md5 的说明），所以老账号两种口径都认——否则一个还没改过密码的
    // 老账号会被判成"已经改过"，机主就被锁死了。
    const bool never_changed =
        config_.uuid.empty() || constant_time_equal(config_.uuid, fingerprint_before_edit) ||
        constant_time_equal(config_.uuid, record_md5_legacy(record));
    if (!never_changed) {
        return make_error(ErrorCode::PermissionDenied, kPasswordAlreadyChanged);
    }

    record.password_hash = std::string(new_digest_hex);
    record.updated_at = local_datetime_iso();
    if (const Status saved = store.save(record); !ok(saved)) {
        return *error_of(saved);
    }

    // 记下**改动之前**那份文件的指纹：它与改动后的文件必然不同，
    // 于是下一次的 fingerprint_before_edit 与它不再一致 → 被上面那道判断拒绝。
    config_.uuid = fingerprint_before_edit;
    if (const Status saved = save_config(paths_, config_); !ok(saved)) {
        return *error_of(saved);
    }
    return std::string("密码已更新。密码只能修改一次；再次修改需要先 user delete 再 user create。");
}

// 换发 token：**必须用密码**（用户要求 token 有时效，那就得有一条不依赖 token
// 自身的续期路径——否则 token 一过期，除了删号重建就没别的办法了）。
//
// 与改密的关系：换 token **不消耗**「密码只能改一次」的那一次机会。
// 因此这里不碰 `updated_at`（它属于"账号内容"），指纹又排除了 token 字段，
// 换完之后"还没改过密码"的状态原样保留。
Result<nlohmann::json> UserService::renew_token(std::string_view current_digest_hex) {
    UserStore store(paths_, config_);
    Result<UserRecord> loaded = store.load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty()) {
        return make_error(ErrorCode::FileNotFound, "这个数据根还没有账号，请先 user create");
    }
    if (!verify_password_digest(record.password_hash, current_digest_hex)) {
        return make_error(ErrorCode::Unauthorized, "当前密码不正确");
    }

    // 换之前先记住"还没改过密码"这件事（老账号的 uuid 还是整份文件的 MD5，
    // 换了 token 之后那个 MD5 就变了，所以必须现在就把它迁移成新指纹）。
    const bool never_changed =
        config_.uuid.empty() || constant_time_equal(config_.uuid, record_md5(record)) ||
        constant_time_equal(config_.uuid, record_md5_legacy(record));

    if (!issue_token(config_, record)) {
        return make_error(ErrorCode::StorageError, "无法生成新 token（CSPRNG 失败）");
    }
    if (const Status saved = store.save(record); !ok(saved)) {
        return *error_of(saved);
    }
    if (never_changed) {
        config_.uuid = record_md5(record);
        if (const Status saved = save_config(paths_, config_); !ok(saved)) {
            return *error_of(saved);
        }
    }

    nlohmann::json data = nlohmann::json::object();
    data["username"] = record.username;
    data["token"] = record.token;
    data["token_issued_at"] = record.token_issued_at;
    data["token_expires_at"] = record.token_expires_at;
    data["token_expiry_text"] = token_expiry_text(record);
    data["path"] = "data/user.json";
    // 旧 token 立刻失效（同一个字段被覆盖），要把话说清楚，别让人拿着旧的到处试。
    data["message"] = "新 token 已签发，旧 token 立即失效";
    return data;
}

Result<nlohmann::json> UserService::describe() const {
    UserStore store(paths_, config_);
    Result<UserRecord> loaded = store.load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty()) {
        return make_error(ErrorCode::FileNotFound, "这个数据根还没有账号，请先 user create");
    }

    const std::string fingerprint = record_md5(record);
    const bool changed = !config_.uuid.empty() && !constant_time_equal(config_.uuid, fingerprint);

    nlohmann::json data = nlohmann::json::object();
    data["username"] = record.username;
    data["user_id"] = record.user_id;
    data["current_bucket"] = config_.current_bucket;
    data["created_at"] = record.created_at;
    data["updated_at"] = record.updated_at;
    data["last_login_at"] = record.last_login_at;
    data["path"] = "data/user.json";
    // 「口令是否已经改过一次」如实报出来，省得用户改到一半才发现。
    // **不给 message**：CLI 已经按 password_changed 打成「密码状态：…」，
    // 再附一句"账号信息（密码还可以修改一次）"只是把同一件事说两遍
    // （真机测出来的重复行）。
    data["password_changed"] = changed;
    // token 的**有效期**照实报（这不是凭证，是"什么时候要去换"的信息）。
    // token 本身不给：要看走 `config list --show-token`。
    data["token_issued_at"] = record.token_issued_at;
    data["token_expires_at"] = record.token_expires_at;
    data["token_expired"] = token_expired(record);
    data["token_expiry_text"] = token_expiry_text(record);
    // 刻意**不含** password_hash / password_salt / token：读账号信息不该顺手吐凭证。
    return data;
}

}  // namespace fmt::user
