#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace px::diag {

enum class Severity : std::uint8_t { Info, Warning, Error, Fatal };

struct Source {
    std::string resourceId{};
    std::string path{};
    std::string nodeId{};
    std::string property{};
    int line = 0;
    int column = 0;
    int endLine = 0;
    int endColumn = 0;
};

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code{};
    std::string category{};
    std::string message{};
    std::string details{};
    Source source{};
    std::string operationId{};
    std::string quickFix{};
    // Canonical 0.2 diagnostic identity. Legacy Source fields remain for
    // object/property diagnostics; every transport serializes these fields.
    std::string documentId{};
    std::string sourceId{};
    std::string hint{};
    std::string cause{};

    [[nodiscard]] bool BlocksBuild() const {
        return severity == Severity::Error || severity == Severity::Fatal;
    }
};

[[nodiscard]] const char* ToString(Severity severity);
[[nodiscard]] std::string Describe(const Diagnostic& diagnostic);

class Store {
public:
    using Listener = std::function<void(const Diagnostic&)>;

    void Emit(Diagnostic diagnostic);
    void Clear();
    void ClearCategory(const std::string& category);
    [[nodiscard]] std::vector<Diagnostic> Snapshot() const;
    [[nodiscard]] bool HasBlocking() const;
    void SetListener(Listener listener);

private:
    mutable std::mutex m_mutex;
    std::vector<Diagnostic> m_items;
    Listener m_listener;
};

Store& Global();
void Emit(Diagnostic diagnostic);

}  // namespace px::diag
