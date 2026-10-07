// 名称校验
//
// Bucket 名会变成目录名（`repository/<user>/<bucket>/`），文件名会变成磁盘上的
// 实体文件名，两者都必须按 Windows 的命名规则挡一遍（开发文档第 25、26、118 节）。
//
// 校验只负责「这个名字能不能用」，不负责「这个名字是否已存在」——后者是业务规则。
#pragma once

#include <string_view>

#include "fmt/common/error.hpp"

namespace fmt {

// Windows 保留设备名：CON / PRN / AUX / NUL / COM1-9 / LPT1-9。
// 带扩展名同样保留（`CON.txt` 也非法）。
bool is_windows_reserved_name(std::string_view name);

// 名字是否与 file_id 同形（`fmt-YYYYMMDD-N`，前缀不区分大小写）。
//
// 这种名字会让「先按 file_id 查、查不到再按名字查」的定位产生歧义：一个文件叫
// `fmt-20261008-0`，而另一个文件的 file_id 恰好就是它——所以上传时直接拒绝（FMT-106）。
bool looks_like_file_id(std::string_view name);

// 名称长度上限（字节）：Windows 单个路径分量的上限是 255。
inline constexpr std::size_t kMaxNameBytes = 255;

// Bucket 名称校验。失败返回 FMT-202 BucketNameInvalid。
Status validate_bucket_name(std::string_view name);

// 文件名校验（上传用）。失败时：
//   空 -> FMT-100；含路径分隔符 / 是 . 或 .. -> FMT-102；
//   含 Windows 非法字符 -> FMT-101；保留设备名 -> FMT-103；超长 -> FMT-104；
//   与 file_id 同形（fmt-YYYYMMDD-N）-> FMT-106。
Status validate_file_name(std::string_view name);

}  // namespace fmt
