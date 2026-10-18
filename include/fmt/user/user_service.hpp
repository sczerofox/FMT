// 用户域：账号的创建、查询、改密，以及「密码只能改一次」的判定
//
// 与 bucket / file / trash / share / config 同级：CLI 与 HTTP 共用这一份实现
//（架构文档的冻结规则「CLI 与 HTTP 使用统一业务核心」）。
//
// 三条口径：
//   * **口令明文不跨进程**：CLI 用 `password_digest(用户名, 口令)` 自己算摘要，
//     只把摘要交给这里。明文一个字节都不进命名管道、不进服务日志。
//     （为什么不挑战-应答：服务只存单向摘要，PBKDF2 不可组合，服务方无法对随机
//      nonce 算出同一个应答值；详见 user.hpp 里 `verify_password_digest` 的说明。）
//   * **一个数据根一个账号**：账号就是 data/user.json 里那条记录（单用户文件）。
//   * **密码只能改一次**：config.json 的 `uuid` 存着「账号内容」的指纹。
//     一致 == 还没改过 → 允许改一次，改完把改之前的指纹写回 uuid；
//     不一致 == 已经改过 → 拒绝，提示「删除用户重新创建」。
//     指纹用**规范化文档文本**（UserStore::record_json_text）而不是磁盘原文，
//     并且**排除 token 相关字段**——token 会轮换，轮换不该改变"改没改过密码"。
//   * **token 有时效**：`config.token_ttl_seconds` 决定有效期，过期后 HTTP 一律
//     `FMT-019`；`user token renew`（凭密码）是唯一的续期路径。
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"
#include "fmt/core/path_manager.hpp"
#include "fmt/user/user.hpp"

namespace fmt::user {

// config.json 的 uuid 与「当前账号记录的规范化 JSON」不一致，说明**已经改过一次密码**。
inline constexpr const char* kPasswordAlreadyChanged =
    "密码已经修改过一次，不能再改。要重新设置请先 `user delete` 删除用户，再 `user create` 重建账号。";

class UserService {
public:
    UserService(const PathManager& paths, Config& config);

    // 当前账号；`record.empty()` 表示这个数据根还没有账号（首次启动就是这种状态）。
    Result<UserRecord> current() const;

    // 账号的口令盐（`user salt`）：CLI 拿它算摘要，所以明文不必跨进程。
    // 老账号给的是记录里的**随机盐**，新账号给的是 `directory_salt(用户名)`。
    Result<std::string> password_salt() const;

    // 建账号：用户名 + **口令摘要**（CLI 侧 `password_digest()` 算好的，服务不再看明文）。
    //
    // 前置条件（满足才允许建）：
    //   * 还没有账号——已有账号一律拒绝，**绝不覆盖**；
    //   * 关键数据文件齐全（见 missing_data_files）——缺文件时建账号会掩盖事故。
    // 成功时不只写 user.json，还把它的 MD5 记进 config.json 的 uuid（"第一次"的基准）。
    Result<UserRecord> create(std::string_view username, std::string_view password_digest_hex);

    // 改密：先校验旧口令的摘要，再收**新**口令的摘要。
    // 规则：config.json 的 uuid 必须与当前记录一致（= 还没改过），否则拒绝。
    // 成功后把 uuid 更新成改完之后的 MD5，于是下一次必然被拒。
    Result<std::string> update_password(std::string_view current_digest_hex,
                                        std::string_view new_digest_hex);

    // 口令校验（`user delete` 与改密共用）：把 CLI 算好的摘要与已存摘要做常量时间比对。
    Result<UserRecord> verify_digest(std::string_view presented_digest_hex) const;

    // 登录成功后刷新 `last_login_at`（`user verify` 调用）。
    // 登录痕迹**不参与**「密码只能改一次」的指纹，所以这个写盘不会影响改密判定。
    Status record_login() const;

    // 换发 token：`user token renew`。**必须给密码摘要**——token 过期之后除了这条路
    // 没有别的办法（否则只能 user delete 重建账号，代价是数据全没）。
    //
    // 与「密码只能改一次」互不影响：换 token 不消耗那一次机会（指纹排除了 token 字段，
    // 并且老账号的旧口径指纹会在这里被迁移成新指纹）。
    Result<nlohmann::json> renew_token(std::string_view current_digest_hex);

    // `user get` 的载荷：账号字段 + 「口令是否已经改过一次」。
    //
    // **不含**口令哈希 / 盐 / 访问 token（token 走 `config list`）——与 HTTP 侧
    // `GET /api/config` 的口径一致：读账号信息的接口不该顺手把凭证吐出来。
    Result<nlohmann::json> describe() const;

    // 关键数据文件缺失检查：返回缺失的**相对路径**列表（空 = 齐全）。
    //
    // 涉及 `data/user.json`、`data/file.json`、`data/share.json`、`data/trash.json`。
    // 缺任何一个都不允许继续写任何东西（用户要求）——尤其是 user create：
    // 只补 user.json 会把「丢文件」这件事抹平。
    std::vector<std::string> missing_data_files() const;

    // 首次启动的引导文案（交互模式用）。
    static std::string first_run_hint();

private:
    const PathManager& paths_;
    Config& config_;
};

}  // namespace fmt::user
