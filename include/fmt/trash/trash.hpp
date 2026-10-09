// 回收站：把**文件级**与**桶级**两级条目合成一份视图
//
// 两级各有权威来源，本模块只做合并与标识解析，不自己存状态：
//   文件级  file.json（is_trash / trash_reason / deleted_at），路径按 file_id 与记录推
//   桶级    trash/<用户>/.original，目录名一律带删除时间戳
//
// 标识解析**不猜**：命中多条就报候选（与 file delete 的歧义处理同一口径）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

// 回收站里的一项。`type` 决定它归谁管：文件级归 FileService，桶级归 BucketService。
struct TrashEntry {
    std::string type;  // "file" / "bucket"
    std::string id;    // 文件：file_id；桶：回收站里的目录名
    std::string name;  // 展示名：文件=文件名；桶=原桶名（未记录时给回收站目录名）
    std::string bucket;  // 文件所属 Bucket；桶级条目为空
    std::string deleted_at;
    std::uintmax_t bytes = 0;
    std::size_t files = 0;   // 桶级：树里的文件数；文件级：1
    bool present = true;     // 数据还在不在（索引有、目录没了就是 false）
    bool restorable = true;  // 能不能单独回退（随桶删除的文件为 false）
    std::string trash_path;  // 相对数据根、正斜杠
    std::string message;     // 不能回退时的原因
};

// 预检结论：说清楚 + 要不要确认 + 能不能靠确认解决。
struct TrashCheck {
    TrashEntry entry;
    bool needs_confirm = false;  // 永久删除：要 y/N
    bool blocked = false;        // 同名冲突 / 随桶删除 / 数据缺失：确认也解决不了
    std::string message;
};

class TrashService {
public:
    TrashService(const PathManager& paths, Config& config, Logger* logger);

    // 合并列表：文件级 + 桶级，按删除时间倒序。
    Result<std::vector<TrashEntry>> list();
    Result<TrashEntry> get(std::string_view identifier);

    // 回退：预检（只读）与执行。执行前的三种阻断由预检给出。
    Result<TrashCheck> check_restore(std::string_view identifier) const;
    Result<TrashEntry> restore(std::string_view identifier);

    // 永久删除：预检（只读，needs_confirm 恒为真）与执行。
    Result<TrashCheck> check_purge(std::string_view identifier) const;
    Result<TrashEntry> purge(std::string_view identifier);

    // 清空回收站：两级条目一次全永久删掉。预检给出「几项、多少字节」；
    // 永久删除**永远**要确认（缺 force 由路由层返回 FMT-016）。
    // 预检与执行都不是 const：它们要列两级条目，而 list() 本身要构造两个服务。
    struct EmptyCheck {
        std::size_t files = 0;
        std::size_t buckets = 0;
        std::uintmax_t bytes = 0;
        std::string message;
    };
    Result<EmptyCheck> check_empty();
    Result<EmptyCheck> empty();

private:
    enum class Kind { None, File, Bucket, Ambiguous };

    struct Resolution {
        Kind kind = Kind::None;
        std::string file_id;
        std::string trashed_name;
        std::vector<TrashEntry> candidates;  // 歧义时把候选摊开
        std::string message;
    };

    // ① 桶的回收站目录名（精确）→ ② file_id → ③ 桶的原名 → ④ 文件名。
    // ③④ 命中多条就是歧义：报候选，让用户用 file_id 或完整的回收站名指定。
    Result<Resolution> resolve(std::string_view identifier) const;

    Result<TrashEntry> entry_of_file(std::string_view file_id) const;
    Result<TrashEntry> entry_of_bucket(std::string_view trashed_name) const;

    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

}  // namespace fmt
