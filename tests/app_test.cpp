// core/app 初始化的单元测试
#include "fmt/core/app.hpp"

#include <filesystem>
#include <string>

#include "fmt/common/hash.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt/user/user.hpp"
#include "fmt/user/user_service.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

// 口径（2.0.0 起）：**initialize_root 不再自动建账号**。
// 第一次启动由用户 `user create <用户名>` 自己建（见 FMT 开发文档第 72 条），
// 所以这里断言的是「只建目录与默认 JSON」，账号由 UserService 建出来。
FMT_TEST(App, 初始化数据根不再自动建账号) {
    fmt_test::TempDir temp("app-user");
    const auto root = temp / "root";

    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    // data/user.json 已经建出来了，但里面**只有版本号**：这就是「还没有账号」。
    const auto document = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(document));
    if (!fmt::ok(document)) {
        return;
    }
    const nlohmann::json& empty = std::get<nlohmann::json>(document);
    FMT_CHECK_EQ(empty.value("version", 0), 1);
    FMT_CHECK(!empty.contains("users"));
    FMT_CHECK(!empty.contains("username"));
    FMT_CHECK(!empty.contains("token"));
    FMT_CHECK(!empty.contains("password_hash"));

    // UserService::current() 也如实报告「没有账号」。
    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);
    const auto current = service.current();
    FMT_CHECK(fmt::ok(current));
    if (fmt::ok(current)) {
        FMT_CHECK(std::get<fmt::UserRecord>(current).empty());
    }

    // 建账号：口令摘要在 CLI 侧算好（这里就是 CLI 的那两种写法）。
    const std::string digest = fmt::password_digest("s3cret", fmt::directory_salt("user"));
    FMT_CHECK_EQ(digest.size(), std::size_t{64});
    const auto created = service.create("user", digest);
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    FMT_CHECK_EQ(std::get<fmt::UserRecord>(created).username, std::string("user"));

    const auto after_create = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(after_create));
    if (!fmt::ok(after_create)) {
        return;
    }
    const nlohmann::json& user = std::get<nlohmann::json>(after_create);
    FMT_CHECK(!user.contains("users"));
    FMT_CHECK_EQ(user.value("version", 0), 1);
    FMT_CHECK_EQ(user.value("username", std::string{}), std::string("user"));
    FMT_CHECK(!user.value("user_id", std::string{}).empty());
    // token：32 位十六进制（16 字节 CSPRNG），永久有效
    const std::string token = user.value("token", std::string{});
    FMT_CHECK_EQ(token.size(), std::size_t{32});
    FMT_CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
    // 口令只存摘要（PBKDF2-SHA256 -> 64 位十六进制），盐由用户名派生
    FMT_CHECK_EQ(user.value("password_hash", std::string{}), digest);
    FMT_CHECK_EQ(user.value("password_salt", std::string{}), fmt::directory_salt("user"));
    FMT_CHECK(!user.value("created_at", std::string{}).empty());

    // 再初始化一次：**不覆盖已有账号**（token 与创建时间都不变）。
    const auto again = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(again));
    const auto kept = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(kept));
    if (fmt::ok(kept)) {
        FMT_CHECK_EQ(std::get<nlohmann::json>(kept).value("token", std::string{}), token);
        FMT_CHECK_EQ(std::get<nlohmann::json>(kept).value("created_at", std::string{}),
                     user.value("created_at", std::string{}));
    }

    // 一个数据根一个账号：已有账号时**拒绝**再建（绝不覆盖）。
    const auto second = service.create("other", fmt::password_digest("x", fmt::directory_salt("other")));
    FMT_CHECK(!fmt::ok(second));
    if (!fmt::ok(second)) {
        FMT_CHECK_EQ(fmt::error_of(second)->code, fmt::ErrorCode::InvalidArgument);
    }

    // 与 HTTP 侧同一份口令：verify_password 收的是**明文**，它内部按用户名派生盐算摘要。
    fmt::UserStore store(manager, config);
    const auto verified = store.verify_password("user", "s3cret");
    FMT_CHECK(fmt::ok(verified) && std::get<bool>(verified));
    const auto bad = store.verify_password("user", "wrong");
    FMT_CHECK(fmt::ok(bad) && !std::get<bool>(bad));

    // 跨进程那条路：CLI 本地算摘要，服务只做常量时间比对（明文不过管道）。
    // 服务侧的校验函数认"同一份摘要"，认不出别的——这就是「没有明文也能验口令」。
    FMT_CHECK(fmt::verify_password_digest(digest, fmt::password_digest("s3cret", fmt::directory_salt("user"))));
    FMT_CHECK(!fmt::verify_password_digest(digest, fmt::password_digest("wrong", fmt::directory_salt("user"))));
    // 摘要长度不对 / 空摘要：一律拒绝，绝不"空 == 空"放行
    FMT_CHECK(!fmt::verify_password_digest(digest, "abc"));
    FMT_CHECK(!fmt::verify_password_digest("", digest));
    FMT_CHECK(!fmt::verify_password_digest(digest, ""));

    // UserService 的验密入口（`user delete` 与改密都用它）。
    const auto ok_verify = service.verify_digest(digest);
    FMT_CHECK(fmt::ok(ok_verify));
    if (fmt::ok(ok_verify)) {
        FMT_CHECK_EQ(std::get<fmt::UserRecord>(ok_verify).username, std::string("user"));
    }
    const auto bad_verify = service.verify_digest(fmt::password_digest("wrong", fmt::directory_salt("user")));
    FMT_CHECK(!fmt::ok(bad_verify));
    if (!fmt::ok(bad_verify)) {
        FMT_CHECK_EQ(fmt::error_of(bad_verify)->code, fmt::ErrorCode::Unauthorized);
    }

    // 「密码只能改一次」：config.json 的 uuid 记着 user.json 的指纹。
    //   一致  == 还没改过 → 允许这一次，改完把新指纹写回 uuid；
    //   不一致 == 已经改过 → 拒绝，提示删除用户重建。
    const std::string digest_next = fmt::password_digest("next", fmt::directory_salt("user"));
    const auto changed = service.update_password(digest, digest_next);
    FMT_CHECK(fmt::ok(changed));
    // 第一次成功之后：文件里是新摘要，内存里的 uuid 变成"新记录的指纹"。
    {
        const auto raw = fmt::read_json_file(manager.user_data());
        FMT_CHECK(fmt::ok(raw));
        if (fmt::ok(raw)) {
            FMT_CHECK_EQ(std::get<nlohmann::json>(raw).value("password_hash", std::string{}),
                         digest_next);
        }
    }
    FMT_CHECK_EQ(config.uuid.size(), std::size_t{32});
    FMT_CHECK(!config.uuid.empty());

    // 第二次：同一个实例再改 → uuid 与当前记录的指纹已经不一致 → 必须拒绝。
    const auto twice = service.update_password(digest_next, fmt::password_digest("again", fmt::directory_salt("user")));
    FMT_CHECK(!fmt::ok(twice));
    if (!fmt::ok(twice)) {
        FMT_CHECK_EQ(fmt::error_of(twice)->code, fmt::ErrorCode::PermissionDenied);
    }
    // 改完之后新口令生效、旧口令失效。
    FMT_CHECK(fmt::ok(store.verify_password("user", "next")));
    FMT_CHECK(!std::get<bool>(store.verify_password("user", "s3cret")));
}

FMT_TEST(App, 老的多用户文件照读且不受建账号影响) {
    // 复刻线上状态：数据根是 v1.0 建出来的，data/user.json 还是老形状
    // `{"version":1,"users":[{…}]}`。
    //
    // 2.0.0 起的口径：initialize_root **不再改写**它（也不再自动建账号），
    // 老形状由 UserStore::load() 照读（取第一条）；建账号时读到已有账号就拒绝，
    // 所以老账号的 token / 口令摘要 / 创建时间一个都不会动。
    fmt_test::TempDir temp("app-user-legacy");
    const auto root = temp / "root";

    const auto first = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(first));
    if (!fmt::ok(first)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(first);

    // 手写一份老形状：一个 users 数组，里面一条完整记录。
    nlohmann::json record = nlohmann::json::object();
    record["user_id"] = "u-legacy01";
    record["username"] = "olduser";
    record["password_hash"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    record["password_salt"] = "aabbccddeeff00112233445566778899";
    record["token"] = "fedcba9876543210fedcba9876543210";
    record["current_bucket"] = "lazy";
    record["created_at"] = "2026-01-01T00:00:00";
    record["updated_at"] = "2026-01-01T00:00:00";
    record["last_login_at"] = "";
    nlohmann::json legacy = fmt::make_collection(1, "users");
    legacy["users"].push_back(record);
    FMT_CHECK(fmt::ok(fmt::write_json_file(manager.user_data(), legacy)));

    // initialize_root 不碰它
    const auto second = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(second));
    if (!fmt::ok(second)) {
        return;
    }
    const auto untouched = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(untouched));
    if (fmt::ok(untouched)) {
        FMT_CHECK(std::get<nlohmann::json>(untouched).contains("users"));
    }

    // load() 照读第一条
    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);
    const auto loaded = service.current();
    FMT_CHECK(fmt::ok(loaded));
    if (!fmt::ok(loaded)) {
        return;
    }
    const fmt::UserRecord& found = std::get<fmt::UserRecord>(loaded);
    FMT_CHECK_EQ(found.username, std::string("olduser"));
    FMT_CHECK_EQ(found.token, record["token"].get<std::string>());
    FMT_CHECK_EQ(found.password_hash, record["password_hash"].get<std::string>());

    // 建账号被拒（已经有账号），老文件一个字节不动
    const auto created = service.create("other", fmt::password_digest("x", fmt::directory_salt("other")));
    FMT_CHECK(!fmt::ok(created));
    const auto still = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(still));
    if (fmt::ok(still)) {
        FMT_CHECK_EQ(std::get<nlohmann::json>(still)["users"][0]["token"].get<std::string>(),
                     record["token"].get<std::string>());
    }
}

FMT_TEST(App, 老的空users文件等价于还没有账号) {
    // 更老的数据根：`{"version":1,"users":[]}`（那会儿还没有账号功能）。
    // 口径：空数组 == 「还没有账号」，load() 返回空记录；建账号照常可行，
    // 并且会把它写成单用户形状（老容器被替掉）。
    fmt_test::TempDir temp("app-user-existing");
    const auto root = temp / "root";

    const auto first = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(first));
    if (!fmt::ok(first)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(first);
    FMT_CHECK(fmt::ok(fmt::write_json_file(manager.user_data(), fmt::make_collection(1, "users"))));

    // initialize_root 不再改写它
    const auto second = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(second));
    if (!fmt::ok(second)) {
        return;
    }

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);
    const auto current = service.current();
    FMT_CHECK(fmt::ok(current));
    if (!fmt::ok(current)) {
        return;
    }
    FMT_CHECK(std::get<fmt::UserRecord>(current).empty());  // 还没有账号

    // 建账号：写成单用户形状，老容器消失
    const auto created = service.create("user", fmt::password_digest("pw", fmt::directory_salt("user")));
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    const auto document = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(document));
    if (!fmt::ok(document)) {
        return;
    }
    const nlohmann::json& user = std::get<nlohmann::json>(document);
    FMT_CHECK(!user.contains("users"));
    FMT_CHECK_EQ(user.value("username", std::string{}), std::string("user"));
    FMT_CHECK_EQ(user.value("token", std::string{}).size(), std::size_t{32});
}

FMT_TEST(App, 老账号的随机盐依然能验密与改密) {
    // 真实升级路径：v1.0 建出来的账号是**随机盐**（`password_salt` 是 16 字节随机数），
    // 而 2.0.0 新建的账号用「用户名派生盐」。如果口令派生一律按用户名派生，
    // 老账号的口令会当场验不过——等于把机主锁在门外（真机上那个账号就是随机盐）。
    //
    // 口径：**口令派生用的盐始终取自记录里的 `password_salt`**（服务通过 user.salt 给出），
    // 所以老账号照常能验密、能改密；只有新建账号才用用户名派生盐。
    fmt_test::TempDir temp("app-user-oldsalt");
    const auto root = temp / "root";

    const auto first = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(first));
    if (!fmt::ok(first)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(first);

    // 手写一份「老账号」：随机盐 + 用该盐算出来的摘要。
    const std::string old_salt = "4115f06b9842f8d1d0c5378956106e5d";  // 真机那个账号的盐
    const std::string old_digest = fmt::password_digest("old-pw", old_salt);
    nlohmann::json record = nlohmann::json::object();
    record["user_id"] = "u-oldsalt";
    record["username"] = "user";
    record["password_hash"] = old_digest;
    record["password_salt"] = old_salt;
    record["token"] = "0123456789abcdef0123456789abcdef";
    record["current_bucket"] = "lazy";
    record["created_at"] = "2026-01-01T00:00:00";
    record["updated_at"] = "2026-01-01T00:00:00";
    record["last_login_at"] = "";
    record["version"] = 1;
    FMT_CHECK(fmt::ok(fmt::write_json_file(manager.user_data(), record)));

    fmt::Config config = std::get<fmt::Config>(fmt::load_config(manager));
    fmt::user::UserService service(manager, config);

    // 服务给出的盐必须是记录里那个**随机盐**，不是用户名派生盐。
    const auto salt = service.password_salt();
    FMT_CHECK(fmt::ok(salt));
    if (fmt::ok(salt)) {
        FMT_CHECK_EQ(std::get<std::string>(salt), old_salt);
    }

    // 老口令照常能验（CLI 会拿 user.salt 算摘要）。
    FMT_CHECK(fmt::ok(service.verify_digest(old_digest)));
    FMT_CHECK(!fmt::ok(service.verify_digest(fmt::password_digest("bad", old_salt))));

    // 老账号也能改密一次；改完盐不变、新口令生效、旧口令失效。
    const std::string fresh_digest = fmt::password_digest("new-pw", old_salt);
    const auto changed = service.update_password(old_digest, fresh_digest);
    FMT_CHECK(fmt::ok(changed));
    const auto after_salt = service.password_salt();
    FMT_CHECK(fmt::ok(after_salt));
    if (fmt::ok(after_salt)) {
        FMT_CHECK_EQ(std::get<std::string>(after_salt), old_salt);  // 盐不该被换掉
    }
    FMT_CHECK(fmt::ok(service.verify_digest(fresh_digest)));
    FMT_CHECK(!fmt::ok(service.verify_digest(old_digest)));
}

FMT_TEST(App, 初始化建出六个目录与默认JSON) {
    fmt_test::TempDir temp("app-init");
    const auto root = temp / "FMT";

    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));

    for (const std::string& name : fmt::PathManager::required_directories()) {
        FMT_CHECK(fmt::directory_exists(root / fmt::path_from_utf8(name)));
    }

    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);
    FMT_CHECK(fmt::file_exists(manager.config_file()));
    FMT_CHECK(fmt::file_exists(manager.server_file()));
    FMT_CHECK(fmt::file_exists(manager.file_data()));
    FMT_CHECK(fmt::file_exists(manager.share_data()));
    FMT_CHECK(fmt::file_exists(manager.trash_data()));
    FMT_CHECK(fmt::file_exists(manager.user_data()));

    // 集合文件的默认结构
    const auto files = fmt::read_json_file(manager.file_data());
    FMT_CHECK(fmt::ok(files));
    FMT_CHECK_EQ(std::get<nlohmann::json>(files)["version"].get<int>(), 1);
    FMT_CHECK(std::get<nlohmann::json>(files)["files"].is_array());
}

FMT_TEST(App, 初始化幂等且不碰已有数据) {
    fmt_test::TempDir temp("app-idempotent");
    const auto root = temp / "FMT";
    const fmt::PathManager paths{root};

    {
        const auto first = fmt::initialize_root(root);
        FMT_CHECK(fmt::ok(first));
    }

    // 人为放进「用户数据」与一份已经有内容的配置
    const auto marker = root / "repository" / "小谷.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(marker, "用户数据，不许动")));

    fmt::Config existing;
    existing.current_user = "小谷";
    existing.current_bucket = "工作";
    existing.max_upload_size = 10485760;
    FMT_CHECK(fmt::ok(fmt::save_config(paths, existing)));

    {
        const auto second = fmt::initialize_root(root);
        FMT_CHECK(fmt::ok(second));
    }

    // 用户数据一字不动
    FMT_CHECK(std::filesystem::exists(marker));
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(marker)), std::string("用户数据，不许动"));

    // 已有配置的字段被保留（不是被默认值覆盖回去）
    const auto reloaded = fmt::load_config(paths);
    FMT_CHECK(fmt::ok(reloaded));
    FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).current_user, std::string("小谷"));
    FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).current_bucket, std::string("工作"));
    FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).max_upload_size, std::uint64_t{10485760});
}

FMT_TEST(App, 检查能报出缺失的目录与文件) {
    fmt_test::TempDir temp("app-check");
    const fmt::PathManager paths{temp / "FMT"};

    const fmt::RootReport report = fmt::check_root(paths);
    FMT_CHECK(!report.complete());
    FMT_CHECK_EQ(report.missing_directories.size(), std::size_t{6});
    // 四个集合文件 + config.json + server.json
    FMT_CHECK_EQ(report.missing_files.size(), std::size_t{6});
    FMT_CHECK(report.broken_files.empty());
}

FMT_TEST(App, 补齐只补缺失且幂等) {
    fmt_test::TempDir temp("app-ensure");
    const auto root = temp / "FMT";
    const fmt::PathManager paths{root};

    const auto first = fmt::ensure_root(paths);
    FMT_CHECK(fmt::ok(first));
    FMT_CHECK_EQ(std::get<fmt::RootRepair>(first).created_directories.size(), std::size_t{6});
    FMT_CHECK_EQ(std::get<fmt::RootRepair>(first).created_files.size(), std::size_t{6});
    FMT_CHECK(fmt::check_root(paths).complete());

    // 再补一次：什么都不该再建
    const auto second = fmt::ensure_root(paths);
    FMT_CHECK(fmt::ok(second));
    FMT_CHECK(std::get<fmt::RootRepair>(second).created_directories.empty());
    FMT_CHECK(std::get<fmt::RootRepair>(second).created_files.empty());

    // 用户自己放的文件不受影响
    const auto mine = root / fmt::path_from_utf8("说明.txt");
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(mine, "别删我")));
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));
    FMT_CHECK(fmt::file_exists(mine));
}

FMT_TEST(App, 损坏的JSON只报告不修复) {
    fmt_test::TempDir temp("app-broken-check");
    const auto root = temp / "FMT";
    const fmt::PathManager paths{root};
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));

    const std::string broken = "{\"version\":1,\"files\":[}";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(paths.file_data(), broken)));

    const fmt::RootReport report = fmt::check_root(paths);
    FMT_CHECK(!report.complete());
    FMT_CHECK(report.missing_files.empty());
    FMT_CHECK_EQ(report.broken_files.size(), std::size_t{1});
    FMT_CHECK(report.broken_files[0].find("FMT-006") != std::string::npos);

    // 补齐不会碰它：冻结规则是「损坏的 JSON 不静默重置」
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(paths.file_data())), broken);
    FMT_CHECK_EQ(fmt::check_root(paths).broken_files.size(), std::size_t{1});
}

FMT_TEST(App, 服务上下文打开日志并加载配置) {
    fmt_test::TempDir temp("app-service");
    const auto root = temp / "FMT";

    const auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));

    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);
    FMT_CHECK(app.paths != nullptr);
    FMT_CHECK(app.logger != nullptr);
    FMT_CHECK_EQ(app.config.language, std::string("zh-CN"));
    FMT_CHECK_EQ(app.server_config.port, 4122);

    // 日志目录与文件都已建立，并且写入了数据根信息
    FMT_CHECK(fmt::file_exists(app.paths->log() / "fmt.log"));
    const auto log = fmt::read_text_file(app.paths->log() / "fmt.log");
    FMT_CHECK(fmt::ok(log));
    FMT_CHECK(std::get<std::string>(log).find("数据根") != std::string::npos);
}

FMT_TEST(App, 损坏的配置让初始化失败) {
    fmt_test::TempDir temp("app-bad-config");
    const auto root = temp / "FMT";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(root / "config")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(root / "config" / "config.json", "{ broken")));

    const auto context = fmt::initialize_service_context(root);
    FMT_CHECK(!fmt::ok(context));
    FMT_CHECK(fmt::error_of(context)->code == fmt::ErrorCode::JsonParseError);
}
