// Share：把一个**正常**文件开放成一条可撤销的分享链接
//
// 规格（docx/FMT 开发文档.md §16 / §46-51、技术文档 §7.2）：
//   data/share.json = { version: 1, shares: [ {share_id, file_id,
//                       max_download_count, download_count, expire_time, is_valid} ] }
//   * share_id **唯一且不可预测**，它本身就是外部访问凭证（所以用 CSPRNG，不用递增数）
//   * max_download_count 默认 20；expire_time 默认 7 天，`null` 表示不过期
//   * 过期与次数**相互独立**：7 天内最多 20 次
//   * 一个文件可以有多个 share，各自独立
//
// 「Share 不得绕过 File 状态」（§50）是本模块的中心约束：文件进了回收站或被删掉之后，
// 分享链立刻失效（FMT-503），而不是继续能下载。
//
// **HTTP 下载接口还没做**（用户决定先只做数据面）：`register_download()` 已经把
// §50/§51 那套「全部检查通过才计数、且在业务锁内完成」实现好了，将来 HTTP 端点只管调它。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"
#include "fmt/file/file.hpp"

namespace fmt {

// 默认值（文档冻结）。
inline constexpr int kDefaultShareDownloads = 20;
inline constexpr int kDefaultShareDays = 7;

struct ShareRecord {
    std::string share_id;
    std::string file_id;
    int max_download_count = kDefaultShareDownloads;
    int download_count = 0;
    std::string expire_time;  // "YYYY-MM-DDTHH:MM:SS"；空 = 不过期（JSON 里写 null）
    bool is_valid = true;
};

// 一条 share 现在**能不能用**。get 会把「存在但不可用」如实报出来（§49），
// 而不是伪装成从未存在。
enum class ShareState {
    Ok,
    NotFound,         // share_id 查不到
    Invalid,          // is_valid = false（已撤销）
    Expired,          // 过了 expire_time
    LimitReached,     // 下载次数用尽
    FileUnavailable,  // 关联文件在回收站、或记录已不存在
};

std::string_view share_state_name(ShareState state);
// 不可用状态对应的错误码；Ok 返回 ErrorCode::Ok。
ErrorCode share_state_error(ShareState state);

// 一条 share 的完整视图：记录 + 它指向的文件 + 当前状态。
struct ShareView {
    ShareRecord share;
    FileRecord file;
    ShareState state = ShareState::Ok;
    std::string message;
};

class ShareService {
public:
    ShareService(const PathManager& paths, Config& config, Logger* logger);

    // 生成一条 share。文件必须存在、属于当前用户、且不在回收站（§47）。
    // 参数可以是 `file_id`，也可以是**文件名**（本次改动（未提交））：与 `file get` /
    // `file delete` 同一套定位规则——先当 file_id 查（全局唯一），查不到再当文件名查
    // （当前用户 + 当前 Bucket 的正常文件；同名的在别的桶时报 FMT-002 并点名那个桶）。
    Result<ShareRecord> create(std::string_view file_id_or_name);
    // 按 share_id 查。**过期 / 次数用尽 / 文件进回收站都如实返回状态**，不报「查不到」。
    Result<ShareView> get(std::string_view share_id) const;
    // 列出 share（最近创建在前）。**参数可空**（本次改动（未提交））：
    //   给了 key（file_id 或文件名）→ 只列这个文件的；
    //   空字符串                    → 列出全部（`share list` 不带参数，便于一眼看谁过期/用尽）。
    Result<std::vector<ShareView>> list(std::string_view file_id_or_name = {}) const;
    // 撤销一条 share（删记录）。
    Result<ShareRecord> remove(std::string_view share_id);

    // 下载前记账：全部检查通过才 +1 并落盘，返回要下载的文件（§50/§51）。
    // 由路由层在**业务锁内**调用，所以「19 加两次变 21」不会发生。
    Result<FileRecord> register_download(std::string_view share_id);

    // 把一个「file_id 或文件名」定位到一条文件记录（本次改动（未提交））：
    // 先当 file_id 查（全局唯一），查不到再当文件名查（当前用户 + 当前 Bucket 的正常文件）。
    // `create()` 与 `share list` 都用它，保证两处定位规则**只有一份**。
    Result<FileRecord> resolve(std::string_view file_id_or_name) const;

private:
    Result<std::vector<ShareRecord>> load_shares() const;
    Status save_shares(const std::vector<ShareRecord>& shares) const;
    // 找到记录并算出状态。调用方先确认 share_id 存在（否则报 FMT-500）。
    ShareView view_of(const ShareRecord& share, FileService& files) const;
    static std::string new_share_id();

    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

}  // namespace fmt
