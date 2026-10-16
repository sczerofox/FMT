#include "fmt/user/user.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <cstdio>
#include <system_error>
#include <vector>

#include "fmt/common/hash.hpp"
#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

constexpr int kVersion = 1;  // data/user.json 的版本
// 老形状（多用户时期）的键：根上是一个 `"users": [...]` 数组。
constexpr const char* kLegacyUsersKey = "users";
constexpr int kTokenBytes = 16;  // 32 位十六进制
constexpr int kSaltBytes = 16;

std::string to_hex(const unsigned char* data, std::size_t size) {
    static const char* kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        text.push_back(kDigits[data[i] >> 4]);
        text.push_back(kDigits[data[i] & 0x0F]);
    }
    return text;
}

std::vector<unsigned char> from_hex(std::string_view text) {
    const auto value_of = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };
    std::vector<unsigned char> bytes;
    if (text.size() % 2 != 0) {
        return bytes;
    }
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = value_of(text[i]);
        const int low = value_of(text[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return bytes;
}

UserRecord from_json(const nlohmann::json& item) {
    UserRecord record;
    record.user_id = item.value("user_id", std::string{});
    record.username = item.value("username", std::string{});
    record.password_hash = item.value("password_hash", std::string{});
    record.password_salt = item.value("password_salt", std::string{});
    record.token = item.value("token", std::string{});
    record.current_bucket = item.value("current_bucket", std::string{});
    record.created_at = item.value("created_at", std::string{});
    record.updated_at = item.value("updated_at", std::string{});
    record.last_login_at = item.value("last_login_at", std::string{});
    // 老文件里的 "buckets" 一律**忽略**：桶以 repository/<user>/ 下的真实目录为准。
    return record;
}

nlohmann::json to_json(const UserRecord& record) {
    nlohmann::json item = nlohmann::json::object();
    item["user_id"] = record.user_id;
    item["username"] = record.username;
    item["password_hash"] = record.password_hash;
    item["password_salt"] = record.password_salt;
    item["token"] = record.token;
    item["current_bucket"] = record.current_bucket;
    item["created_at"] = record.created_at;
    item["updated_at"] = record.updated_at;
    item["last_login_at"] = record.last_login_at;
    return item;
}

// 口令盐的固定派生盐：换个字符串就等于换一套盐，改了要当成不兼容变更。
constexpr const char* kDirectorySaltContext = "fmt:user-salt:v1";

// 文件是不是还停在老形状上：多用户时期的 `"users"` 数组，或者更老的 `"buckets"`
// 副本（桶以 repository/<user>/ 下的真实目录为准，这份副本早就不该存在）。
// 只看原文，懒得先解析再比结构——判断「要不要改写一次」够用了。
bool needs_rewrite(const std::string& raw) {
    return raw.find("\"users\"") != std::string::npos ||
           raw.find("\"buckets\"") != std::string::npos;
}

}  // namespace

// 常量时间比较：token / 口令摘要 / 应答值都不该因为比较提前返回而泄漏信息。
bool constant_time_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

std::string random_hex(int bytes) {
    if (bytes <= 0) {
        return {};
    }
    std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes), 0);
    if (BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(buffer.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    return to_hex(buffer.data(), buffer.size());
}

std::string password_hash(std::string_view password, std::string_view salt_hex, int iterations) {
    const std::vector<unsigned char> salt = from_hex(salt_hex);
    if (salt.empty() || iterations <= 0) {
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }

    unsigned char digest[32] = {};
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(iterations), digest,
        static_cast<ULONG>(sizeof(digest)), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        return {};
    }
    return to_hex(digest, sizeof(digest));
}

std::string directory_salt(std::string_view username) {
    // 用用户名当"口令"、固定串当"盐"跑一次 PBKDF2：结果确定、16 字节、十六进制。
    const std::string derived = password_hash(username, md5_hex(kDirectorySaltContext),
                                             kDefaultUserIterations);
    return derived.empty() ? std::string{} : derived.substr(0, kSaltBytes * 2);
}

std::string password_digest(std::string_view password, std::string_view salt_hex) {
    if (salt_hex.empty()) {
        return {};
    }
    return password_hash(password, salt_hex, kDefaultUserIterations);
}

bool verify_password_digest(std::string_view stored_digest_hex,
                            std::string_view presented_digest_hex) {
    if (stored_digest_hex.empty() || presented_digest_hex.size() != 64) {
        return false;
    }
    return constant_time_equal(stored_digest_hex, presented_digest_hex);
}

UserStore::UserStore(const PathManager& paths, Config& config) : paths_(paths), config_(config) {}

Result<UserRecord> UserStore::load() const {
    const std::filesystem::path path = paths_.user_data();
    if (!file_exists(path)) {
        return UserRecord{};  // 文件都没有 == 还没有账号
    }
    Result<nlohmann::json> parsed = read_json_file(path);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& document = std::get<nlohmann::json>(parsed);
    if (const Status version = check_version(document, kVersion); !ok(version)) {
        return *error_of(version);
    }

    // 新形状：根对象本身就是用户。老形状：`{"version":1,"users":[…]}`，取第一条
    //（单个数据根只可能有一条），老的空数组就是「还没有账号」。
    const nlohmann::json* item = &document;
    if (const auto legacy = document.find(kLegacyUsersKey);
        legacy != document.end() && legacy->is_array()) {
        if (legacy->empty()) {
            return UserRecord{};
        }
        item = &legacy->front();
    }
    if (!item->is_object()) {
        return make_error(ErrorCode::JsonParseError, "data/user.json 里的用户必须是对象");
    }
    return from_json(*item);
}

Status UserStore::save(const UserRecord& record) const {
    if (record.empty()) {
        // 单用户文件不该存成「没有账号」：宁可不写，也不要留一个读不出账号的文件。
        return make_error(ErrorCode::StorageError, "拒绝写入空的用户记录");
    }
    nlohmann::json document = to_json(record);
    document["version"] = kVersion;
    return write_json_file(paths_.user_data(), document);
}

std::string UserStore::record_json_text(const UserRecord& record) {
    nlohmann::json document = to_json(record);
    document["version"] = kVersion;
    return document.dump();
}

Result<UserRecord> UserStore::find_by_token(std::string_view token) const {
    if (token.empty()) {
        return make_error(ErrorCode::Unauthorized, "缺少 token");
    }
    Result<UserRecord> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const UserRecord record = std::get<UserRecord>(loaded);
    // 常量时间比较：token 是凭证，比较不该泄漏前缀信息。
    if (record.empty() || !constant_time_equal(record.token, token)) {
        return make_error(ErrorCode::Unauthorized, "token 无效");
    }
    return record;
}

Result<UserRecord> UserStore::find_by_name(std::string_view username) const {
    Result<UserRecord> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty() || record.username != username) {
        return make_error(ErrorCode::FileNotFound, "用户不存在：" + std::string(username));
    }
    return record;
}

Result<bool> UserStore::verify_password(std::string_view username, std::string_view password) const {
    Result<UserRecord> found = find_by_name(username);
    if (!ok(found)) {
        return *error_of(found);
    }
    const UserRecord& record = std::get<UserRecord>(found);
    if (record.password_hash.empty() || record.password_salt.empty()) {
        return false;
    }
    const std::string computed = password_hash(password, record.password_salt);
    return !computed.empty() && constant_time_equal(computed, record.password_hash);
}

Status UserStore::refresh_current_bucket(std::string_view username) {
    Result<UserRecord> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty() || record.username != username) {
        return make_error(ErrorCode::FileNotFound, "用户不存在：" + std::string(username));
    }

    record.current_bucket = config_.current_bucket;
    record.updated_at = local_datetime_iso();
    return save(record);
}

Status UserStore::record_login(std::string_view username) {
    Result<UserRecord> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    UserRecord record = std::get<UserRecord>(loaded);
    if (record.empty() || record.username != username) {
        return make_error(ErrorCode::FileNotFound, "用户不存在：" + std::string(username));
    }
    record.last_login_at = local_datetime_iso();
    record.updated_at = record.last_login_at;
    return save(record);
}

}  // namespace fmt
