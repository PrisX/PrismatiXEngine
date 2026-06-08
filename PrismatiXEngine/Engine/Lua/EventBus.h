#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::lua {

using EventArgs = std::unordered_map<std::string, std::string>;

class EventBus {
public:
    using Handler = std::function<void(const EventArgs&)>;

    void Subscribe(const std::string& event, Handler handler) {
        m_handlers[event].push_back(std::move(handler));
    }

    void Emit(const std::string& event, const EventArgs& args = {}) const {
        if (auto it = m_handlers.find(event); it != m_handlers.end()) {
            for (const Handler& h : it->second) {
                h(args);
            }
        }
    }

    void Clear() { m_handlers.clear(); }

private:
    std::unordered_map<std::string, std::vector<Handler>> m_handlers;
};

}
