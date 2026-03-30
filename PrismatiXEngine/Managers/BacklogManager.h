#pragma once
#include <string>
#include <vector>

struct BacklogEntry {
    std::string speaker;
    std::string text;
    std::string voice;
    bool isChoice = false;
};

class BacklogManager {
public:
    BacklogManager() = default;
    ~BacklogManager() = default;

    std::vector<BacklogEntry> logs;

    void AddLog(const std::string& speaker, const std::string& text, const std::string& voice = "");
    void AddChoice(const std::string& text);
    void Clear();
    size_t GetCount() const;
};