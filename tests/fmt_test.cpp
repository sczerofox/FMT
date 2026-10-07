#include "fmt_test.hpp"

#include <cstdio>
#include <exception>
#include <iostream>

namespace fmt_test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(std::string suite, std::string name, std::function<void()> body) {
    registry().push_back(Case{std::move(suite), std::move(name), std::move(body)});
}

void fail(const std::string& text, const char* file, int line) {
    std::ostringstream stream;
    stream << "    " << file << ":" << line << "\n    " << text;
    throw AssertionFailure(stream.str());
}

namespace {

bool matches(const std::string& full_name, const std::vector<std::string>& filters) {
    if (filters.empty()) {
        return true;
    }
    for (const std::string& filter : filters) {
        if (full_name.find(filter) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int run_all(const std::vector<std::string>& filters) {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    for (const Case& test_case : registry()) {
        const std::string full_name = test_case.suite + "." + test_case.name;
        if (!matches(full_name, filters)) {
            ++skipped;
            continue;
        }

        try {
            test_case.body();
            ++passed;
            std::cout << "[通过] " << full_name << "\n";
        } catch (const AssertionFailure& failure) {
            ++failed;
            std::cout << "[失败] " << full_name << "\n" << failure.text() << "\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[失败] " << full_name << "\n    未捕获异常: " << error.what() << "\n";
        } catch (...) {
            ++failed;
            std::cout << "[失败] " << full_name << "\n    未捕获的未知异常\n";
        }
    }

    std::cout << "\n共 " << passed + failed << " 项：通过 " << passed << "，失败 " << failed;
    if (skipped > 0) {
        std::cout << "，跳过 " << skipped;
    }
    std::cout << "\n";

    return failed == 0 ? 0 : 1;
}

}  // namespace fmt_test

int main(int argc, char** argv) {
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i) {
        filters.emplace_back(argv[i]);
    }
    return fmt_test::run_all(filters);
}
