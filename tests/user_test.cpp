// 账号与访问 token 的时效（`data/user.json` + `config.json`）
//
// 这里只测**服务侧**的口径：token 的有效期、过期后能不能用、换发之后旧 token 是否
// 立刻失效，以及「token 轮换不影响密码只能改一次」。CLI 的参数解析交给 e2e。
#include <cstdio>
#include <string>

#include "fmt/config/config.hpp"
#include "fmt/core/app.hpp"
#include "fmt/user/user.hpp"
#include "fmt/user/user_service.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

namespace {

// 把一条现成的账号记录改写成「已经过期」/「没有有效期记录」，再写回文件。
void patch_expiry(const fmt::PathManager& manager, const std::string& expires_at) {
    const auto document = fmt::read_json_file(manager.user_data());
    if (!fmt::ok(document)) {
        return;
    }
    nlohmann::json record = std::get<nlohmann::json>(document);
    record["token_expires_at"] = expires_at;
    fmt::write_json_file(manager.user_data(), record);
}

std::string token_of(const fmt::PathManager& manager) {
    const auto document = fmt::read_json_file(manager.user_data());
    if (!fmt::ok(document)) {
        return {};
    }
    return std::get<nlohmann::json>(document).value("token", std::string{});
}

std::string expires_of(const fmt::PathManager& manager) {
    const auto document = fmt::read_json_file(manager.user_data());
    if (!fmt::ok(document)) {
        return {};
    }
    return std::get<nlohmann::json>(document).value("token_expires_at", std::string{});
}

}  // namespace

FMT_TEST(User, 新建账号的token带有效期且默认七天) {
    fmt_test::TempDir temp("user-token-ttl");
    const auto root = temp / "root";
    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    FMT_CHECK_EQ(config.token_ttl_seconds, 7ULL * 24 * 60 * 60);  // 默认 7 天

    fmt::user::UserService service(manager, config);
    const auto created = service.create("user", fmt::password_digest("pw", fmt::directory_salt("user")));
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    const fmt::UserRecord& record = std::get<fmt::UserRecord>(created);
    FMT_CHECK(!record.token.empty());
    FMT_CHECK(!record.token_issued_at.empty());
    FMT_CHECK(!record.token_expires_at.empty());
    // 刚签发的 token 必须是「还没过期」，否则建完账号当场就用不了。
    FMT_CHECK(!fmt::token_expired(record));
    // 有效期落在签发时间之后（两者都是本地时间字符串，同格式可直接比）。
    FMT_CHECK(record.token_expires_at > record.token_issued_at);

    // 服务侧认这个 token。
    fmt::UserStore store(manager, config);
    const auto found = store.find_by_token(record.token);
    FMT_CHECK(fmt::ok(found));
}

FMT_TEST(User, token过期后服务拒绝) {
    fmt_test::TempDir temp("user-token-expired");
    const auto root = temp / "root";
    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);
    const auto created = service.create("user", fmt::password_digest("pw", fmt::directory_salt("user")));
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    const std::string token = std::get<fmt::UserRecord>(created).token;

    // 把有效期改到过去（模拟「过时」）。
    patch_expiry(manager, "2020-01-01T00:00:00");

    fmt::UserStore store(manager, config);
    const auto found = store.find_by_token(token);
    FMT_CHECK(!fmt::ok(found));
    if (!fmt::ok(found)) {
        // 过期是**独立错误码**：客户端据此知道该去 renew，而不是以为自己抄错了 token。
        FMT_CHECK_EQ(fmt::error_of(found)->code, fmt::ErrorCode::TokenExpired);
    }
}

FMT_TEST(User, 没有有效期记录的旧token按过期处理) {
    fmt_test::TempDir temp("user-token-legacy");
    const auto root = temp / "root";
    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);
    const auto created = service.create("user", fmt::password_digest("pw", fmt::directory_salt("user")));
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    const std::string token = std::get<fmt::UserRecord>(created).token;

    // 老版本签发的 token 没有 `token_expires_at`：不清空它就等于永久有效。
    patch_expiry(manager, "");

    fmt::UserStore store(manager, config);
    const auto found = store.find_by_token(token);
    FMT_CHECK(!fmt::ok(found));
    if (!fmt::ok(found)) {
        FMT_CHECK_EQ(fmt::error_of(found)->code, fmt::ErrorCode::TokenExpired);
    }
}

FMT_TEST(User, 换发token后旧的立刻失效且密码仍只能改一次) {
    fmt_test::TempDir temp("user-token-renew");
    const auto root = temp / "root";
    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("pw", salt);
    const auto created = service.create("user", digest);
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    const std::string old_token = std::get<fmt::UserRecord>(created).token;
    FMT_CHECK(!old_token.empty());

    // 换发：要密码摘要。口令错必须拒绝（否则谁都换得走）。
    const auto denied = service.renew_token(fmt::password_digest("wrong", salt));
    FMT_CHECK(!fmt::ok(denied));
    if (!fmt::ok(denied)) {
        FMT_CHECK_EQ(fmt::error_of(denied)->code, fmt::ErrorCode::Unauthorized);
    }
    FMT_CHECK_EQ(token_of(manager), old_token);  // 没被改

    const auto renewed = service.renew_token(digest);
    FMT_CHECK(fmt::ok(renewed));
    if (!fmt::ok(renewed)) {
        return;
    }
    const std::string new_token = token_of(manager);
    FMT_CHECK(!new_token.empty());
    FMT_CHECK(new_token != old_token);           // 换了
    FMT_CHECK(!expires_of(manager).empty());     // 新有效期写进去了

    fmt::UserStore store(manager, config);
    // 旧 token 立刻失效，新 token 能用。
    FMT_CHECK(!fmt::ok(store.find_by_token(old_token)));
    FMT_CHECK(fmt::ok(store.find_by_token(new_token)));

    // 换 token **不消耗**「密码只能改一次」：现在还能改一次。
    const std::string next = fmt::password_digest("next", salt);
    const auto changed = service.update_password(digest, next);
    FMT_CHECK(fmt::ok(changed));
    // 改完之后（已经用掉那一次）再改必须被拒。
    const auto twice = service.update_password(next, fmt::password_digest("again", salt));
    FMT_CHECK(!fmt::ok(twice));
}

FMT_TEST(User, 登录更新最后登录时间且不影响改密判定) {
    fmt_test::TempDir temp("user-login-stamp");
    const auto root = temp / "root";
    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);
    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("pw", salt);
    const auto created = service.create("user", digest);
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    FMT_CHECK(std::get<fmt::UserRecord>(created).last_login_at.empty());  // 刚建号还没登录过

    // 登录（服务侧在 user.verify 成功后调 record_login）→ 时间戳写进去。
    FMT_CHECK(fmt::ok(service.record_login()));
    const auto after_login = service.current();
    FMT_CHECK(fmt::ok(after_login));
    if (fmt::ok(after_login)) {
        FMT_CHECK(!std::get<fmt::UserRecord>(after_login).last_login_at.empty());
    }

    // **关键**：登录会写 user.json，但指纹只算 password_hash + salt + user_id + created_at，
    // 所以「密码还能改一次」的状态必须原样保留——否则登录一次就把机主锁死了。
    const auto described = service.describe();
    FMT_CHECK(fmt::ok(described));
    if (fmt::ok(described)) {
        FMT_CHECK(!std::get<nlohmann::json>(described).value("password_changed", true));
    }

    // 多登录几次也不影响：还是能改一次密码。
    FMT_CHECK(fmt::ok(service.record_login()));
    FMT_CHECK(fmt::ok(service.record_login()));
    const auto changed = service.update_password(digest, fmt::password_digest("next", salt));
    FMT_CHECK(fmt::ok(changed));
    // 用掉那一次之后，再改必须被拒（登录不会把它"洗回来"）。
    FMT_CHECK(fmt::ok(service.record_login()));
    const auto twice = service.update_password(fmt::password_digest("next", salt),
                                              fmt::password_digest("again", salt));
    FMT_CHECK(!fmt::ok(twice));
    if (!fmt::ok(twice)) {
        FMT_CHECK_EQ(fmt::error_of(twice)->code, fmt::ErrorCode::PermissionDenied);
    }
}

FMT_TEST(User, PBKDF2与Java和标准向量一致) {
    // 与 Java PasswordDigestTest 用同一个向量：PBKDF2-HMAC-SHA256、100000 轮、32 字节，
    // 口令含中文（UTF-8 原始字节）。它同时钉住 C++ 与 Java 两个 jar 的摘要算法一致。
    FMT_CHECK_EQ(fmt::password_digest("密码", "00112233445566778899aabbccddeeff"),
                 std::string("8311414396192b4a109f0f247c08680e8e06f7d0c8871ff7c0faa3582d7a18a0"));
}
