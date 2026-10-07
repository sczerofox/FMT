// common/time 的单元测试
#include "fmt/common/time.hpp"

#include <string>

#include "fmt_test.hpp"

FMT_TEST(Time, 时间戳格式) {
    const std::string stamp = fmt::local_timestamp();
    FMT_CHECK_EQ(stamp.size(), std::size_t{19});
    FMT_CHECK_EQ(stamp[4], '-');
    FMT_CHECK_EQ(stamp[7], '-');
    FMT_CHECK_EQ(stamp[10], ' ');
    FMT_CHECK_EQ(stamp[13], ':');
    FMT_CHECK_EQ(stamp[16], ':');
}

FMT_TEST(Time, 日期格式) {
    const std::string compact = fmt::local_date_compact();
    FMT_CHECK_EQ(compact.size(), std::size_t{8});
    for (const char ch : compact) {
        FMT_CHECK(ch >= '0' && ch <= '9');
    }

    const std::string iso = fmt::local_datetime_iso();
    FMT_CHECK_EQ(iso.size(), std::size_t{19});
    FMT_CHECK_EQ(iso[10], 'T');
}

FMT_TEST(Time, 日期片段) {
    const fmt::DateParts parts = fmt::local_date_parts();
    FMT_CHECK_EQ(parts.year.size(), std::size_t{4});
    FMT_CHECK_EQ(parts.month.size(), std::size_t{2});
    FMT_CHECK_EQ(parts.day.size(), std::size_t{2});
    FMT_CHECK_EQ(parts.year, fmt::local_date_compact().substr(0, 4));
}

FMT_TEST(Time, 解析往返一致) {
    const std::string iso = fmt::local_datetime_iso();
    const auto parsed = fmt::parse_datetime_iso(iso);
    FMT_CHECK(parsed.has_value());

    // 秒级精度：重新格式化必须与输入完全相同。
    if (parsed.has_value()) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(*parsed);
        std::tm value{};
        localtime_s(&value, &seconds);
        char buffer[32] = {};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &value);
        FMT_CHECK_EQ(std::string(buffer), iso);
    }
}

FMT_TEST(Time, 空格分隔符也接受) {
    const auto parsed = fmt::parse_datetime_iso("2026-10-05 20:00:00");
    FMT_CHECK(parsed.has_value());

    const auto parsed_t = fmt::parse_datetime_iso("2026-10-05T20:00:00");
    FMT_CHECK(parsed_t.has_value());
    if (parsed.has_value() && parsed_t.has_value()) {
        FMT_CHECK(*parsed == *parsed_t);
    }
}

FMT_TEST(Time, 非法输入返回空) {
    FMT_CHECK(!fmt::parse_datetime_iso("").has_value());
    FMT_CHECK(!fmt::parse_datetime_iso("2026-10-05").has_value());
    FMT_CHECK(!fmt::parse_datetime_iso("2026/10/05 20:00:00").has_value());
    FMT_CHECK(!fmt::parse_datetime_iso("not a time at all").has_value());
}
