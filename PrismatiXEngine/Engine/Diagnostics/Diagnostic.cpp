#include "Engine/Diagnostics/Diagnostic.h"

#include "Engine/Support/Logger.h"

#include <algorithm>
#include <sstream>

namespace px::diag {

const char* ToString(Severity severity) {
    switch (severity) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        case Severity::Fatal: return "fatal";
    }
    return "error";
}

std::string Describe(const Diagnostic& diagnostic) {
    std::ostringstream out;
    out << diagnostic.code << " [" << diagnostic.category << "] " << diagnostic.message;
    if (!diagnostic.source.path.empty()) {
        out << " (" << diagnostic.source.path;
        if (diagnostic.source.line > 0) {
            out << ':' << diagnostic.source.line;
            if (diagnostic.source.column > 0) out << ':' << diagnostic.source.column;
        }
        out << ')';
    }
    if (!diagnostic.source.nodeId.empty()) out << " node=" << diagnostic.source.nodeId;
    if (!diagnostic.source.property.empty()) out << " property=" << diagnostic.source.property;
    if (!diagnostic.details.empty()) out << " — " << diagnostic.details;
    return out.str();
}

void Store::Emit(Diagnostic diagnostic) {
    Listener listener;
    {
        std::lock_guard lock(m_mutex);
        m_items.push_back(diagnostic);
        listener = m_listener;
    }

    const std::string text = Describe(diagnostic);
    switch (diagnostic.severity) {
        case Severity::Info: PX_LOG_INFO("[diagnostic] {}", text); break;
        case Severity::Warning: PX_LOG_WARN("[diagnostic] {}", text); break;
        case Severity::Error: PX_LOG_ERROR("[diagnostic] {}", text); break;
        case Severity::Fatal: PX_LOG_CRITICAL("[diagnostic] {}", text); break;
    }
    if (listener) listener(diagnostic);
}

void Store::Clear() {
    std::lock_guard lock(m_mutex);
    m_items.clear();
}

void Store::ClearCategory(const std::string& category) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_items, [&](const Diagnostic& d) { return d.category == category; });
}

std::vector<Diagnostic> Store::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_items;
}

bool Store::HasBlocking() const {
    std::lock_guard lock(m_mutex);
    return std::any_of(m_items.begin(), m_items.end(),
                       [](const Diagnostic& d) { return d.BlocksBuild(); });
}

void Store::SetListener(Listener listener) {
    std::lock_guard lock(m_mutex);
    m_listener = std::move(listener);
}

Store& Global() {
    static Store store;
    return store;
}

void Emit(Diagnostic diagnostic) { Global().Emit(std::move(diagnostic)); }

}  // namespace px::diag
