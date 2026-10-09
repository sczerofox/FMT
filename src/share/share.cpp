#include "fmt/share/share.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

constexpr const char* kSharesKey = "shares";

ShareRecord record_from_json(const nlohmann::json& item) {
    ShareRecord record;
    record.share_id = item.value("share_id", std::string{});
    record.file_id = item.value("file_id", std::string{});
    record.max_download_count = item.value("max_download_count", kDefaultShareDownloads);
    record.download_count = item.value("download_count", 0);
    record.is_valid = item.value("is_valid", true);
    // expire_time 为 null 或缺失都表示不过期（JSON 里就是 null）。
    if (const auto expire = item.find("expire_time");
        expire != item.end() && expire->is_string()) {
        record.expire_time = expire->get<std::string>();
    }
    return record;
}

nlohmann::json record_to_json(const ShareRecord& record) {
    nlohmann::json item = nlohmann::json::object();
    item["share_id"] = record.share_id;
    item["file_id"] = record.file_id;
    item["max_download_count"] = record.max_download_count;
    item["download_count"] = record.download_count;
    item["expire_time"] =
        record.expire_time.empty() ? nlohmann::json(nullptr) : nlohmann::json(record.expire_time);
    item["is_valid"] = record.is_valid;
    return item;
}

// 12 位十六进制小写，唯一且不可预测（share_id 本身就是访问凭证）。
// 用系统 CSPRNG（技术文档的库表就指定了 BCryptGenRandom），不用 std::mt19937：
// 后者是可预测的伪随机，拿来当凭证不合格。
std::string make_share_id() {
    static const char* kHex = "0123456789abcdef";
    unsigned char bytes[6] = {};  // 6 字节 = 12 位十六进制，与文档示例同长
    if (BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(sizeof(bytes)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};  // 调用方当「生成失败」处理，绝不退化成可预测的 id
    }

    std::string id;
    id.reserve(12);
    for (const unsigned char byte : bytes) {
        id.push_back(kHex[byte >> 4]);
        id.push_back(kHex[byte & 0x0F]);
    }
    return id;
}

}  // namespace

std::string_view share_state_name(ShareState state) {
    switch (state) {
        case ShareState::Ok:
            return "可用";
        case ShareState::NotFound:
            return "不存在";
        case ShareState::Invalid:
            return "已撤销";
        case ShareState::Expired:
            return "已过期";
        case ShareState::LimitReached:
            return "下载次数已用尽";
        case ShareState::FileUnavailable:
            return "关联文件不可用";
    }
    return "未知";
}

ErrorCode share_state_error(ShareState state) {
    switch (state) {
        case ShareState::Ok:
            return ErrorCode::Ok;
        case ShareState::NotFound:
        case ShareState::Invalid:
            return ErrorCode::ShareNotFound;
        case ShareState::Expired:
            return ErrorCode::ShareExpired;
        case ShareState::LimitReached:
            return ErrorCode::ShareDownloadLimitReached;
        case ShareState::FileUnavailable:
            return ErrorCode::ShareFileUnavailable;
    }
    return ErrorCode::ShareNotFound;
}

ShareService::ShareService(const PathManager& paths, Config& config, Logger* logger)
    : paths_(paths), config_(config), logger_(logger) {}

Result<std::vector<ShareRecord>> ShareService::load_shares() const {
    const std::filesystem::path path = paths_.share_data();
    if (!file_exists(path)) {
        return std::vector<ShareRecord>{};
    }
    Result<nlohmann::json> parsed = read_json_file(path);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& document = std::get<nlohmann::json>(parsed);
    if (const Status version = check_version(document, 1); !ok(version)) {
        return *error_of(version);
    }

    std::vector<ShareRecord> shares;
    const auto items = document.find(kSharesKey);
    if (items == document.end() || !items->is_array()) {
        return shares;
    }
    for (const nlohmann::json& item : *items) {
        if (item.is_object()) {
            shares.push_back(record_from_json(item));
        }
    }
    return shares;
}

Status ShareService::save_shares(const std::vector<ShareRecord>& shares) const {
    nlohmann::json document = make_collection(1, kSharesKey);
    for (const ShareRecord& record : shares) {
        document[kSharesKey].push_back(record_to_json(record));
    }
    return write_json_file(paths_.share_data(), document);
}

std::string ShareService::new_share_id() { return make_share_id(); }

Result<ShareRecord> ShareService::create(std::string_view file_id) {
    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    if (file_id.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 file_id");
    }

    // ① 文件必须存在、属于当前用户、且不在回收站（§47 + §50 的中心约束）。
    FileService files(paths_, config_, logger_);
    const Result<FileRecord> found = files.get_by_id(file_id);
    if (!ok(found)) {
        return *error_of(found);
    }
    const FileRecord& record = std::get<FileRecord>(found);
    if (record.user != config_.current_user) {
        return make_error(ErrorCode::PermissionDenied, "这个文件不属于当前用户");
    }
    if (record.is_trash) {
        return make_error(ErrorCode::ShareFileUnavailable,
                          "文件在回收站里，不能被分享：" + record.file_name);
    }

    Result<std::vector<ShareRecord>> loaded = load_shares();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<ShareRecord> shares = std::get<std::vector<ShareRecord>>(loaded);

    // ② 生成不重复的 share_id（随机 12 位；撞了就再摇一次）
    ShareRecord created;
    created.file_id = record.file_id;
    created.max_download_count = kDefaultShareDownloads;
    created.download_count = 0;
    created.expire_time = local_datetime_iso_after_days(kDefaultShareDays);
    created.is_valid = true;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::string candidate = new_share_id();
        const bool taken = std::any_of(shares.begin(), shares.end(),
                                       [&candidate](const ShareRecord& other) {
                                           return other.share_id == candidate;
                                       });
        if (!taken) {
            created.share_id = candidate;
            break;
        }
    }
    if (created.share_id.empty()) {
        return make_error(ErrorCode::StorageError, "无法生成不重复的 share_id");
    }

    shares.push_back(created);
    if (const Status saved = save_shares(shares); !ok(saved)) {
        return *error_of(saved);
    }
    if (logger_ != nullptr) {
        logger_->info("Share", "创建分享：" + created.share_id + " -> " + created.file_id +
                                   "（" + std::to_string(created.max_download_count) +
                                   " 次，到期 " + created.expire_time + "）");
    }
    return created;
}

ShareView ShareService::view_of(const ShareRecord& share, FileService& files) const {
    ShareView view;
    view.share = share;

    // 关联文件：记录不存在、或进了回收站，都算不可用（§49/§50）。
    // get_by_id 连回收站里的记录也查得到，正好用来判断「文件状态」。
    const Result<FileRecord> found = files.get_by_id(share.file_id);
    if (!ok(found)) {
        view.state = ShareState::FileUnavailable;
        view.message = "分享关联的文件已不存在：" + share.file_id;
        return view;
    }
    view.file = std::get<FileRecord>(found);

    // 顺序按 §49：有效性 -> 文件 -> 文件状态 -> 过期 -> 次数。
    if (!view.share.is_valid) {
        view.state = ShareState::Invalid;
        view.message = "分享已撤销：" + view.share.share_id;
        return view;
    }
    if (view.file.is_trash) {
        view.state = ShareState::FileUnavailable;
        view.message = "分享关联的文件在回收站里：" + view.file.file_name;
        return view;
    }
    if (!view.share.expire_time.empty()) {
        const auto expire = parse_datetime_iso(view.share.expire_time);
        if (!expire.has_value()) {
            // 时间戳坏了：当作已过期处理，别放行（安全侧默认可拒）。
            view.state = ShareState::Expired;
            view.message = "分享的到期时间无法解析，按已过期处理：" + view.share.expire_time;
            return view;
        }
        if (std::chrono::system_clock::now() > *expire) {
            view.state = ShareState::Expired;
            view.message = "分享已过期（到期 " + view.share.expire_time + "）";
            return view;
        }
    }
    if (view.share.download_count >= view.share.max_download_count) {
        view.state = ShareState::LimitReached;
        view.message = "分享的下载次数已用尽（" + std::to_string(view.share.download_count) + "/" +
                       std::to_string(view.share.max_download_count) + "）";
        return view;
    }

    view.state = ShareState::Ok;
    view.message = "分享可用：" + view.file.file_name + "（已下载 " +
                   std::to_string(view.share.download_count) + "/" +
                   std::to_string(view.share.max_download_count) + " 次）";
    return view;
}

Result<ShareView> ShareService::get(std::string_view share_id) const {
    if (share_id.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 share_id");
    }
    Result<std::vector<ShareRecord>> loaded = load_shares();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }

    // 完全查不到 share_id：这是**真错误**（FMT-500，退出码 3）。
    // 「存在但过期 / 次数用尽 / 文件进回收站」不是错误，而是**如实返回状态**（§49）。
    const std::vector<ShareRecord>& shares = std::get<std::vector<ShareRecord>>(loaded);
    const auto found = std::find_if(shares.begin(), shares.end(), [share_id](const ShareRecord& item) {
        return item.share_id == share_id;
    });
    if (found == shares.end()) {
        return make_error(ErrorCode::ShareNotFound, "分享不存在：" + std::string(share_id));
    }

    FileService files(paths_, config_, logger_);
    return view_of(*found, files);
}

Result<std::vector<ShareView>> ShareService::list(std::string_view file_id) const {
    if (file_id.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 file_id");
    }
    Result<std::vector<ShareRecord>> loaded = load_shares();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }

    FileService files(paths_, config_, logger_);
    // share.json 里是**追加**顺序，也就是创建顺序；最近创建的排前面。
    const std::vector<ShareRecord>& shares = std::get<std::vector<ShareRecord>>(loaded);
    std::vector<ShareView> views;
    for (auto it = shares.rbegin(); it != shares.rend(); ++it) {
        if (it->file_id != file_id) {
            continue;
        }
        views.push_back(view_of(*it, files));
    }
    return views;
}

Result<ShareRecord> ShareService::remove(std::string_view share_id) {
    if (share_id.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 share_id");
    }
    Result<std::vector<ShareRecord>> loaded = load_shares();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<ShareRecord> shares = std::get<std::vector<ShareRecord>>(loaded);

    const auto found = std::find_if(shares.begin(), shares.end(), [share_id](const ShareRecord& item) {
        return item.share_id == share_id;
    });
    if (found == shares.end()) {
        return make_error(ErrorCode::ShareNotFound, "分享不存在：" + std::string(share_id));
    }
    const ShareRecord removed = *found;
    shares.erase(found);

    if (const Status saved = save_shares(shares); !ok(saved)) {
        return *error_of(saved);
    }
    if (logger_ != nullptr) {
        logger_->info("Share", "撤销分享：" + removed.share_id);
    }
    return removed;
}

Result<FileRecord> ShareService::register_download(std::string_view share_id) {
    Result<ShareView> view = get(share_id);
    if (!ok(view)) {
        return *error_of(view);
    }
    const ShareView& current = std::get<ShareView>(view);
    if (current.state != ShareState::Ok) {
        return make_error(share_state_error(current.state), current.message);
    }

    Result<std::vector<ShareRecord>> loaded = load_shares();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<ShareRecord> shares = std::get<std::vector<ShareRecord>>(loaded);

    auto found = std::find_if(shares.begin(), shares.end(), [share_id](const ShareRecord& item) {
        return item.share_id == share_id;
    });
    if (found == shares.end()) {
        return make_error(ErrorCode::ShareNotFound, "分享不存在：" + std::string(share_id));
    }
    // 再查一次上限：get() 与这里之间没有别人能插进来（调用方持业务锁），
    // 但把判断放在**改数之前、同一把锁内**，§51 的「19 + 两次 = 21」就不可能发生。
    if (found->download_count >= found->max_download_count) {
        return make_error(ErrorCode::ShareDownloadLimitReached,
                          "分享的下载次数已用尽（" + std::to_string(found->download_count) + "/" +
                              std::to_string(found->max_download_count) + "）");
    }
    ++found->download_count;

    if (const Status saved = save_shares(shares); !ok(saved)) {
        return *error_of(saved);  // 计数没落盘就**不能**放行下载：否则会超发
    }
    if (logger_ != nullptr) {
        logger_->info("Share", "下载计数：" + std::string(share_id) + " = " +
                                   std::to_string(found->download_count) + "/" +
                                   std::to_string(found->max_download_count));
    }
    return current.file;
}

}  // namespace fmt
