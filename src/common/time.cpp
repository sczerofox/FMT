#include "fmt/common/time.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace fmt {
namespace {

// localtime_s 是 MSVC 的安全版本；本项目的目标平台只有 Windows。
std::tm local_now() {
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    localtime_s(&value, &now);
    return value;
}

std::string format_tm(const std::tm& value, const char* pattern) {
    std::ostringstream stream;
    stream << std::put_time(&value, pattern);
    return stream.str();
}

std::string two_digits(int value) {
    std::ostringstream stream;
    stream << std::setw(2) << std::setfill('0') << value;
    return stream.str();
}

}  // namespace

std::string local_timestamp() {
    return format_tm(local_now(), "%Y-%m-%d %H:%M:%S");
}

std::string local_date_compact() {
    return format_tm(local_now(), "%Y%m%d");
}

std::string local_datetime_iso() {
    return format_tm(local_now(), "%Y-%m-%dT%H:%M:%S");
}

DateParts local_date_parts() {
    const std::tm now = local_now();
    return DateParts{two_digits(now.tm_year + 1900), two_digits(now.tm_mon + 1),
                     two_digits(now.tm_mday)};
}

std::optional<std::chrono::system_clock::time_point> parse_datetime_iso(std::string_view text) {
    if (text.size() < 19) {
        return std::nullopt;
    }

    // 接受 'T' 与空格两种分隔符。
    std::string normalized(text.substr(0, 19));
    if (normalized[10] == ' ') {
        normalized[10] = 'T';
    }
    if (normalized[10] != 'T') {
        return std::nullopt;
    }

    std::tm value{};
    std::istringstream stream(normalized);
    stream >> std::get_time(&value, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return std::nullopt;
    }

    value.tm_isdst = -1;  // 让 mktime 自己判断夏令时
    const std::time_t seconds = std::mktime(&value);
    if (seconds == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(seconds);
}

}  // namespace fmt
