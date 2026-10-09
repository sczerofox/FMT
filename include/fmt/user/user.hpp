// 用户与访问令牌（`data/user.json`）
//
// 这是 HTTP 接口的账号来源（docx/FMT 开发文档.md §7 的 `data/user.json`）：
//
//   {
//     "version": 1,
//     "users": [
//       {
//         "user_id": "u-000001",
//         "username": "user",
//         "password_hash": "<PBKDF2-SHA256 的十六进制>",
//         "password_salt": "<16 字节随机盐的十六进制>",
//         "token": "<32 位十六进制，**永久有效**>",
//         "buckets": ["lazy"],          // 快照：写记录时刷新
//         "current_bucket": "lazy",     // 快照：config.json 才是运行时权威
//         "created_at": "2026-10-09T20:00:00",
//         "updated_at": "…",
//         "last_login_at": ""           // 还没登录过就是空
//       }
//     ]
//   }
//
// 三条口径：
//   * **密码只存哈希**（PBKDF2-SHA256 + 每用户随机盐）。文件里永远看不到明文；
//     初始密码在创建默认用户时写进日志**一次**，之后改不了（V1 没有改密命令）。
//   * **token 是管理接口的凭证**：默认永久有效（用户明确要求），随机 32 位十六进制。
//   * `buckets` / `current_bucket` 只是**快照**：运行时权威仍是 `config.json`
//     （`current_bucket`）与 `repository/<user>/` 下的目录。写记录时顺手刷新，
//     免得变成第二份事实来源。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

inline constexpr int kDefaultUserIterations = 100000;

struct UserRecord {
    std::string user_id;
    std::string username;
    std::string password_hash;
    std::string password_salt;
    std::string token;
    std::vector<std::string> buckets;
    std::string current_bucket;
    std::string created_at;
    std::string updated_at;
    std::string last_login_at;
};

// PBKDF2-SHA256 的十六进制摘要；失败返回空串。
std::string password_hash(std::string_view password, std::string_view salt_hex,
                          int iterations = kDefaultUserIterations);

// 随机十六进制串（CSPRNG：BCryptGenRandom）。token 与盐都用它。
std::string random_hex(int bytes);

class UserStore {
public:
    UserStore(const PathManager& paths, Config& config);

    // 读全部用户（文件不存在返回空）。
    Result<std::vector<UserRecord>> load() const;
    Status save(const std::vector<UserRecord>& users) const;

    // 首次运行时建默认用户（用户名取 config.current_user，默认 "user"）：
    // 随机密码 + 随机 token。已经存在则原样返回，不覆盖。
    Result<UserRecord> ensure_default(std::string* created_password);

    // 按 token 找用户；找不到返回 nullptr 语义的 nullopt。
    Result<UserRecord> find_by_token(std::string_view token) const;
    Result<UserRecord> find_by_name(std::string_view username) const;

    // 校验密码（常量时间比较哈希）。
    Result<bool> verify_password(std::string_view username, std::string_view password) const;

    // 记一次登录：刷新 last_login_at / buckets / current_bucket 快照。
    Status record_login(std::string_view username);

    // 刷新某个用户的桶快照（桶列表与当前桶）。桶名从 repository/<user>/ 下扫。
    Status refresh_snapshot(std::string_view username);

private:
    const PathManager& paths_;
    Config& config_;
};

}  // namespace fmt
