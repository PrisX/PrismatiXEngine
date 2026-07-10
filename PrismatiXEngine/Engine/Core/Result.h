#pragma once

#include "Engine/Diagnostics/Diagnostic.h"

#include <optional>
#include <utility>
#include <vector>

namespace px {

class Status {
public:
    Status() = default;

    static Status Ok() { return Status(); }
    static Status Fail(diag::Diagnostic diagnostic) {
        Status status;
        status.m_diagnostics.push_back(std::move(diagnostic));
        return status;
    }
    static Status Fail(std::vector<diag::Diagnostic> diagnostics) {
        Status status;
        status.m_diagnostics = std::move(diagnostics);
        return status;
    }

    [[nodiscard]] bool IsOk() const { return m_diagnostics.empty(); }
    explicit operator bool() const { return IsOk(); }
    [[nodiscard]] const std::vector<diag::Diagnostic>& Diagnostics() const {
        return m_diagnostics;
    }
    void Add(diag::Diagnostic diagnostic) { m_diagnostics.push_back(std::move(diagnostic)); }

private:
    std::vector<diag::Diagnostic> m_diagnostics;
};

template <typename T>
class Result {
public:
    static Result Success(T value) {
        Result result;
        result.m_value.emplace(std::move(value));
        return result;
    }
    static Result Failure(diag::Diagnostic diagnostic) {
        Result result;
        result.m_diagnostics.push_back(std::move(diagnostic));
        return result;
    }
    static Result Failure(std::vector<diag::Diagnostic> diagnostics) {
        Result result;
        result.m_diagnostics = std::move(diagnostics);
        return result;
    }

    [[nodiscard]] bool IsOk() const { return m_value.has_value() && m_diagnostics.empty(); }
    explicit operator bool() const { return IsOk(); }
    [[nodiscard]] T& Value() { return *m_value; }
    [[nodiscard]] const T& Value() const { return *m_value; }
    [[nodiscard]] T&& TakeValue() { return std::move(*m_value); }
    [[nodiscard]] const std::vector<diag::Diagnostic>& Diagnostics() const {
        return m_diagnostics;
    }

private:
    std::optional<T> m_value;
    std::vector<diag::Diagnostic> m_diagnostics;
};

}  // namespace px
