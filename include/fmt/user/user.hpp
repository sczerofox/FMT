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
// （见 ensure_root 的补建；账号由 `user create` —— UserService::create —— 建出来）。
//
// 老文件照读：`{"version":1,"users":[{…}]}` 取第一条（不再自动改写：账号由用户建，
// 建账号时读到已有账号会拒绝，所以老文件不会被悄悄重写）。
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

// 口令盐由「用户名」定出来（16 字节十六进制）：**新建账号**用它。
//
// 新账号为什么不用随机盐：`config.json` 的 uuid 规则（密码只能改一次）依赖
// 「同一个用户名重建账号后文件内容一致」，随机盐会让每次重建都不一样。
//
// **老账号（v1.0 建出来的）用的是随机盐**，读回来照样能用：口令派生时用的是
// 记录里的 `password_salt`（见 password_digest），所以升级后老口令依然能验证。
std::string directory_salt(std::string_view username);

// 口令摘要 = PBKDF2-SHA256(口令, salt)。**盐由调用方给出**：
//   * 新建账号：用 `directory_salt(用户名)`；
//   * 老账号 / 改密 / 删除：用记录里存着的 `password_salt`（服务通过 `user salt` 给出）。
// 这样明文永远不需要跨进程，而老账号的随机盐也不会被换掉。
std::string password_digest(std::string_view password, std::string_view salt_hex);

// 常量时间比较（token / 口令摘要都用它）。
bool constant_time_equal(std::string_view left, std::string_view right);

// 口令校验的**唯一入口**（服务端）：拿"口令派生出的摘要"与已存摘要做常量时间比较。
//
// 为什么不是挑战-应答：服务只存**单向摘要**，而"对随机 nonce 的应答"要求服务能算
// 出同一个值——PBKDF2 不可组合（拿摘要当输入再跑一遍，得到的不是口令那一支的值），
// 所以只要不存明文、也不存"口令等价值"，服务方就无法验证 nonce 应答。
// 于是口径定为：**CLI 本地把口令派生成摘要，明文一个字节都不进命名管道/日志**，
// 服务只做摘要比对。命名管道本身是本地 IPC，ACL 已限本用户，不需要再加一层传输加密。
bool verify_password_digest(std::string_view stored_digest_hex,
                            std::string_view presented_digest_hex);

class UserStore {
public:
    UserStore(const PathManager& paths, Config& config);

    // 读唯一用户；文件不存在、或文件里还没有账号时返回空记录（record.empty()）。
    Result<UserRecord> load() const;
    // 写唯一用户（拒绝写空记录：单用户文件不该存成「没有账号」）。
    Status save(const UserRecord& record) const;

    // 这份记录**将要写进 data/user.json 的那个文档**的 JSON 文本（含 version，
    // 与 save() 走同一个 to_json，所以两者永远一致）。
    //
    // 用途：config.json 的 `uuid` 存的是 data/user.json 的 MD5，而**读回来的文件文本**
    // 会随后续补字段、格式化变化而漂移；按「规范化文档」算，判定就与写盘细节无关了
    //（见 config/config.hpp 的 uuid 说明）。
    static std::string record_json_text(const UserRecord& record);

    // 找到一条账号记录（唯一用户）。**不再有 ensure_default()**：2.0.0 起账号只能由
    // 用户 `user create <用户名>` 建（见 user_service.hpp 的 UserService::create），
    // 服务启动时不再自动建一个谁也不知道口令的默认账号。

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
