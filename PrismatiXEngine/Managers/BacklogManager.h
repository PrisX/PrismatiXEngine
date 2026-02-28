#pragma once
#include <string>
#include <vector>

struct BacklogEntry {
    std::string speaker;
    std::string text;
    std::string voice;
};

class BacklogManager {
public:
    static inline std::vector<BacklogEntry> logs;

    static void AddLog(const std::string& speaker, const std::string& text, const std::string& voice = "") {
        logs.push_back({ speaker, text, voice });
    }

    static void Clear() {
        logs.clear();
    }

    static size_t GetCount() { return logs.size(); }
};