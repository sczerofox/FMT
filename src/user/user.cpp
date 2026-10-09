#include "fmt/user/user.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <system_error>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

constexpr const char* kUsersKey = "users";
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
    if (const auto buckets = item.find("buckets"); buckets != item.end() && buckets->is_array()) {
        for (const nlohmann::json& name : *buckets) {
            if (name.is_string()) {
                record.buckets.push_back(name.get<std::string>());
            }
        }
    }
    return record;
}

nlohmann::json to_json(const UserRecord& record) {
    nlohmann::json item = nlohmann::json::object();
    item["user_id"] = record.user_id;
    item["username"] = record.username;
    item["password_hash"] = record.password_hash;
    item["password_salt"] = record.password_salt;
    item["token"] = record.token;
    item["buckets"] = record.buckets;
    item["current_bucket"] = record.current_bucket;
    item["created_at"] = record.created_at;
    item["updated_at"] = record.updated_at;
    item["last_login_at"] = record.last_login_at;
    return item;
}

// 常量时间比较：token / 密码哈希都不该因为比较提前返回而泄漏信息。
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

}  // namespace

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

UserStore::UserStore(const PathManager& paths, Config& config) : paths_(paths), config_(config) {}

Result<std::vector<UserRecord>> UserStore::load() const {
    const std::filesystem::path path = paths_.user_data();
    if (!file_exists(path)) {
        return std::vector<UserRecord>{};
    }
    Result<nlohmann::json> parsed = read_json_file(path);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& document = std::get<nlohmann::json>(parsed);
    if (const Status version = check_version(document, 1); !ok(version)) {
        return *error_of(version);
    }

    std::vector<UserRecord> users;
    const auto items = document.find(kUsersKey);
    if (items == document.end() || !items->is_array()) {
        return users;
    }
    for (const nlohmann::json& item : *items) {
        if (item.is_object()) {
            users.push_back(from_json(item));
        }
    }
    return users;
}

Status UserStore::save(const std::vector<UserRecord>& users) const {
    nlohmann::json document = make_collection(1, kUsersKey);
    for (const UserRecord& record : users) {
        document[kUsersKey].push_back(to_json(record));
    }
    return write_json_file(paths_.user_data(), document);
}

Result<UserRecord> UserStore::ensure_default(std::string* created_password) {
    Result<std::vector<UserRecord>> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<UserRecord> users = std::get<std::vector<UserRecord>>(loaded);

    const std::string wanted =
        config_.current_user.empty() ? std::string("user") : config_.current_user;
    for (const UserRecord& existing : users) {
        if (existing.username == wanted) {
            return existing;  // 已经建过：**不覆盖**（token 与密码都不动）
        }
    }

    UserRecord created;
    created.username = wanted;
    created.user_id = "u-" + random_hex(4);
    created.password_salt = random_hex(kSaltBytes);
    const std::string password = random_hex(8);  // 16 位十六进制
    created.password_hash = password_hash(password, created.password_salt);
    created.token = random_hex(kTokenBytes);
    created.created_at = local_datetime_iso();
    created.updated_at = created.created_at;
    if (created.user_id.empty() || created.token.empty() || created.password_hash.empty()) {
        return make_error(ErrorCode::StorageError, "无法生成默认用户的凭证（CSPRNG 失败）");
    }

    // 桶快照：写记录时刷一次（运行时权威仍是 config.json 与磁盘目录）
    created.current_bucket = config_.current_bucket;
    std::error_code code;
    const std::filesystem::path base = paths_.repository() / path_from_utf8(created.username);
    for (const auto& entry : std::filesystem::directory_iterator(base, code)) {
        std::error_code type_code;
        if (entry.is_directory(type_code)) {
            created.buckets.push_back(path_to_utf8(entry.path().filename()));
        }
    }

    users.push_back(created);
    if (const Status saved = save(users); !ok(saved)) {
        return *error_of(saved);
    }
    if (created_password != nullptr) {
        // 明文只在这里出现一次：调用方负责写进日志，让机主拿得到。
        *created_password = password;
    }
    return created;
}

Result<UserRecord> UserStore::find_by_token(std::string_view token) const {
    if (token.empty()) {
        return make_error(ErrorCode::Unauthorized, "缺少 token");
    }
    Result<std::vector<UserRecord>> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    for (const UserRecord& record : std::get<std::vector<UserRecord>>(loaded)) {
        // 常量时间比较：token 是凭证，比较不该泄漏前缀信息。
        if (constant_time_equal(record.token, token)) {
            return record;
        }
    }
    return make_error(ErrorCode::Unauthorized, "token 无效");
}

Result<UserRecord> UserStore::find_by_name(std::string_view username) const {
    Result<std::vector<UserRecord>> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    for (const UserRecord& record : std::get<std::vector<UserRecord>>(loaded)) {
        if (record.username == username) {
            return record;
        }
    }
    return make_error(ErrorCode::FileNotFound, "用户不存在：" + std::string(username));
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

Status UserStore::refresh_snapshot(std::string_view username) {
    Result<std::vector<UserRecord>> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<UserRecord> users = std::get<std::vector<UserRecord>>(loaded);

    auto found = std::find_if(users.begin(), users.end(), [username](const UserRecord& item) {
        return item.username == username;
    });
    if (found == users.end()) {
        return make_error(ErrorCode::FileNotFound, "用户不存在：" + std::string(username));
    }

    found->buckets.clear();
    std::error_code code;
    const std::filesystem::path base = paths_.repository() / path_from_utf8(found->username);
    for (const auto& entry : std::filesystem::directory_iterator(base, code)) {
        std::error_code type_code;
        if (entry.is_directory(type_code)) {
            found->buckets.push_back(path_to_utf8(entry.path().filename()));
        }
    }
    found->current_bucket = config_.current_bucket;
    found->updated_at = local_datetime_iso();
    return save(users);
}

Status UserStore::record_login(std::string_view username) {
    Result<std::vector<UserRecord>> loaded = load();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<UserRecord> users = std::get<std::vector<UserRecord>>(loaded);

    auto found = std::find_if(users.begin(), users.end(), [username](const UserRecord& item) {
        return item.username == username;
    });
    if (found == users.end()) {
        return make_error(ErrorCode::FileNotFound, "用户不存在：" + std::string(username));
    }
    found->last_login_at = local_datetime_iso();
    found->updated_at = found->last_login_at;
    return save(users);
}

}  // namespace fmt
