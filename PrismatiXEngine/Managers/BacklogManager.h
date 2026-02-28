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
    static std::vector<BacklogEntry> logs;

    static void AddLog(const std::string& speaker, const std::string& text, const std::string& voice = "");
    static void AddChoice(const std::string& text);
    static void Clear();
    static size_t GetCount();
};