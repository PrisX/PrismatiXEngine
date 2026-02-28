#pragma once
#include <string>
#include <unordered_map>

class ConfigManager {
private:
    static std::unordered_map<std::string, std::string> settings;

public:
    static bool LoadConfig(const std::string& fileName);
    static std::string GetString(const std::string& key, const std::string& defaultValue = "");
    static int GetInt(const std::string& key, int defaultValue = 0);
};