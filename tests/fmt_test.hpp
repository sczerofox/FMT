// FMT 单元测试的最小实现
//
// 不依赖任何第三方库：本项目的构建必须能完全离线完成，因此不用 Catch2
// 之类的框架（那需要在 configure 阶段访问网络）。
//
// 用法：
//     FMT_TEST(ErrorCode, 编号与字符串互转) {
//         FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::PermissionDenied), std::string("FMT-004"));
//     }
//
// 断言失败抛出 AssertionFailure，由运行器捕获：记录失败后继续跑下一个用例，
// 因此一次运行能看到全部失败，而不是第一个就中断。
#pragma once

#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fmt_test {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

// 函数内静态，避免跨编译单元的静态初始化顺序问题。
std::vector<Case>& registry();

struct Registrar {
    Registrar(std::string suite, std::string name, std::function<void()> body);
};

class AssertionFailure {
public:
    explicit AssertionFailure(std::string text) : text_(std::move(text)) {}

    const std::string& text() const { return text_; }

private:
    std::string text_;
};

// 断言失败：带上文件与行号抛出。
void fail(const std::string& text, const char* file, int line);

// ---- 值的打印（不可流输出的类型退化为占位符）----
template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<
    T,
    std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
void print_value(std::ostream& stream, const T& value) {
    if constexpr (is_streamable<T>::value) {
        stream << value;
    } else {
        stream << "<不可打印的类型>";
    }
}

template <typename A, typename B>
void check_equal(const A& actual, const B& expected, const char* expression, const char* file,
                 int line) {
    if (!(actual == expected)) {
        std::ostringstream stream;
        stream << expression << "\n      期望: ";
        print_value(stream, expected);
        stream << "\n      实际: ";
        print_value(stream, actual);
        fail(stream.str(), file, line);
    }
}

// 返回失败数；filters 为空的子串过滤（匹配 suite.name）。
int run_all(const std::vector<std::string>& filters);

}  // namespace fmt_test

#define FMT_TEST(suite, name)                                                   \
    static void fmt_test_body_##suite##_##name();                               \
    static const ::fmt_test::Registrar fmt_test_registrar_##suite##_##name(     \
        #suite, #name, &fmt_test_body_##suite##_##name);                        \
    static void fmt_test_body_##suite##_##name()

#define FMT_CHECK(expression)                              \
    do {                                                   \
        if (!(expression)) {                               \
            ::fmt_test::fail(#expression, __FILE__, __LINE__); \
        }                                                  \
    } while (false)

#define FMT_CHECK_EQ(actual, expected) \
    ::fmt_test::check_equal((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
