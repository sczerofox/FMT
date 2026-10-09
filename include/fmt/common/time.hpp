// 时间格式化与解析
//
// V1 只处理本地时间：日志行、file_id 的日期片段、trash.json 的 deleted_at 都用它。
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace fmt {

// 本地时间 "YYYY-MM-DD HH:MM:SS"，日志每行开头使用。
std::string local_timestamp();

// 本地日期 "YYYYMMDD"，file_id（fmt-YYYYMMDD-N）使用。
std::string local_date_compact();

// 本地时间 "YYYY-MM-DDTHH:MM:SS"，trash.json 的 deleted_at 使用。
std::string local_datetime_iso();

// 当前本地时间 + days 天，格式同 local_datetime_iso()。
// share 的 expire_time 用它（到期判断走 parse_datetime_iso() 解析后比时间点，
// 不靠字符串比较——两种格式只差一个字符，比字符串会踩坑）。
std::string local_datetime_iso_after_days(int days);

// 本地日期片段，用于 repository/<user>/<bucket>/YYYY/MM/DD/ 这样的路径。
struct DateParts {
    std::string year;
    std::string month;
    std::string day;
};

DateParts local_date_parts();

// 把 "YYYY-MM-DDTHH:MM:SS" 或 "YYYY-MM-DD HH:MM:SS" 解析成时间点；失败返回空。
std::optional<std::chrono::system_clock::time_point> parse_datetime_iso(std::string_view text);

}  // namespace fmt
