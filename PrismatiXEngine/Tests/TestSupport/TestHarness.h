#pragma once

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace px::test {

inline std::string ProcessSuffix() {
#ifdef _WIN32
    return std::to_string(_getpid());
#else
    return std::to_string(getpid());
#endif
}

class Suite {
public:
    explicit Suite(std::string name) : m_name(std::move(name)) {}

    template <typename Test>
    void Run(std::string name, Test&& test) {
        m_current = std::move(name);
        try {
            std::invoke(std::forward<Test>(test));
        } catch (const std::exception& error) {
            Fail("no exception", error.what(), "uncaught exception");
        } catch (...) {
            Fail("no exception", "unknown exception", "uncaught exception");
        }
    }

    void Expect(bool condition, std::string_view contract,
                std::string_view actual = "contract was false") {
        if (!condition) Fail("contract satisfied", actual, contract);
    }

    void Equal(std::string_view expected, std::string_view actual,
               std::string_view context) {
        if (expected != actual) Fail(expected, actual, context);
    }

    void Fail(std::string_view expected, std::string_view actual,
              std::string_view context) {
        ++m_failures;
        std::cerr << "FAIL: " << m_name << '.' << m_current << '\n'
                  << "  Expected: " << expected << '\n'
                  << "  Actual:   " << actual << '\n'
                  << "  Context:  " << context << '\n';
    }

    [[nodiscard]] int Finish() const {
        if (m_failures == 0) {
            std::cout << "PASS: " << m_name << '\n';
            return 0;
        }
        std::cerr << "FAILED: " << m_name << " (" << m_failures
                  << " contract failures)\n";
        return 1;
    }

private:
    std::string m_name;
    std::string m_current = "setup";
    int m_failures = 0;
};

class TempDirectory {
public:
    explicit TempDirectory(std::string_view name)
        : path(std::filesystem::temp_directory_path() /
               ("prismatix-" + std::string(name) + '-' + ProcessSuffix())) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        error.clear();
        std::filesystem::create_directories(path, error);
        if (error) throw std::filesystem::filesystem_error(
            "create test directory", path, error);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    std::filesystem::path path;
};

}  // namespace px::test
