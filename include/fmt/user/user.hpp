// 用户与访问令牌（`data/user.json`）
//
// 这是 HTTP 接口的账号来源（docx/FMT 开发文档.md §7 的 `data/user.json`）。
// **一个数据根只有一个账号**，所以这个文件就是「那个用户」，没有 users 数组；
// 形状与 config.json / server.json 一致：版本号打头，用户字段平铺在根上。
//
//   {
//     "version": 1,
//     "user_id": "u-1a2b3c4d",
//     "username": "user",
//     "password_hash": "<PBKDF2-SHA256 的十六进制>",
//     "password_salt": "<16 字节随机盐的十六进制>",
//     "token": "<32 位十六进制，**永久有效**>",
//     "current_bucket": "lazy",     // 快照：config.json 才是运行时权威
//     "created_at": "2026-10-09T20:00:00",
//     "updated_at": "…",
//     "last_login_at": ""           // 还没登录过就是空
//   }
//
// 字段与「多用户」时期**一个没改、一个没删**，只是不再套数组：以后加字段直接平铺
// 在根上。「还没有账号」用**空记录**表示，那时文件里只有 `{"version":1}`
// （见 ensure_root 的补建，账号由 ensure_default() 建出来）。
//
// 老文件照读：`{"version":1,"users":[{…}]}` 取第一条；下一次 ensure_default()
// 顺手把它改写成上面的形状，token / 密码哈希 / 时间戳一个都不动。
// 空的老数组（`"users":[]`）等价于「还没有账号」。
//
// 三条口径：
//   * **密码只存哈希**（PBKDF2-SHA256 + 每用户随机盐）。文件里永远看不到明文；
//     初始密码在创建默认用户时生成，V1 没有登录接口，所以拿不到也不影响。
//   * **token 是管理接口的凭证**：默认永久有效（用户明确要求），随机 32 位十六进制。
//   * **没有「桶列表」字段**：桶以磁盘上真实存在的 `repository/<user>/<bucket>/`
//     为准（用户要求删掉这份副本——第二份事实来源迟早会对不上）。
//     `current_bucket` 保留（以后可能有用），但它也只是快照，运行时权威是 config.json。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
    std::string current_bucket;
    std::string created_at;
    std::string updated_at;
    std::string last_login_at;

    // 空记录 = 这个数据根还没有账号（load() 用它表示「没有用户」）。
    bool empty() const { return user_id.empty() && username.empty(); }
};

// PBKDF2-SHA256 的十六进制摘要；失败返回空串。
std::string password_hash(std::string_view password, std::string_view salt_hex,
                          int iterations = kDefaultUserIterations);

// 随机十六进制串（CSPRNG：BCryptGenRandom）。token 与盐都用它。
std::string random_hex(int bytes);

class UserStore {
public:
    UserStore(const PathManager& paths, Config& config);

    // 读唯一用户；文件不存在、或文件里还没有账号时返回空记录（record.empty()）。
    Result<UserRecord> load() const;
    // 写唯一用户（拒绝写空记录：单用户文件不该存成「没有账号」）。
    Status save(const UserRecord& record) const;

    // 首次运行时建默认用户（用户名取 config.current_user，默认 "user"）：
    // 随机密码 + 随机 token。已经有账号则原样返回，不覆盖。
    Result<UserRecord> ensure_default(std::string* created_password);

    // 按 token 找用户；找不到返回 nullptr 语义的 nullopt。
    Result<UserRecord> find_by_token(std::string_view token) const;
    Result<UserRecord> find_by_name(std::string_view username) const;

    // 校验密码（常量时间比较哈希）。
    Result<bool> verify_password(std::string_view username, std::string_view password) const;

    // 记一次登录：刷新 last_login_at 与 current_bucket 快照。
    Status record_login(std::string_view username);

    // 刷新 current_bucket 快照（桶列表不存副本，以 repository/<user>/ 下的真实目录为准）。
    Status refresh_current_bucket(std::string_view username);

private:
    const PathManager& paths_;
    Config& config_;
};

}  // namespace fmt
