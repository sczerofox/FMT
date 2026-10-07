// 业务命令路由（服务端内部头）
//
// 管道的 `op` 与 HTTP 路由**共用这一份实现**：浏览器与 CLI 看到的行为必须一致
// （架构文档的冻结规则「CLI 与 HTTP 使用统一业务核心」）。
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"
#include "fmt/core/app.hpp"

namespace fmt::service {

// 执行一条业务命令，成功返回响应信封里的 data 部分。
// operation 形如 `bucket.create`；参数放在 `args.argv`（字符串数组）里，
// 与 CLI 侧的编码一致。
Result<nlohmann::json> execute_business(AppContext& context, const std::string& operation,
                                        const nlohmann::json& args);

// 是否是「已知业务前缀」：已登记但尚未实现的返回 FMT-602，而不是 FMT-001。
bool is_known_business(const std::string& operation);

}  // namespace fmt::service
