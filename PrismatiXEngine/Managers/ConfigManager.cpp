#include "ConfigManager.h"
#include "ArchiveManager.h"
#include <sstream>
#include <iostream>

std::unordered_map<std::string, std::string> ConfigManager::settings;

bool ConfigManager::LoadConfig(const std::string& fileName) {
    std::vector<char> buffer = ArchiveManager::ExtractFile(fileName);
    if (buffer.empty()) {
        std::cerr << "Failed to load entry point (" << fileName << "): " << std::endl;
        return false;
    }

    std::string content(buffer.begin(), buffer.end());
    std::stringstream ss(content);
    std::string line;

    while (std::getline(ss, line)) {

        if (!line.empty() && line.back() == '\r') line.pop_back(); // for windows

        if (line.empty() || line.substr(0, 2) == "//" || line[0] == '#') continue;

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);
            settings[key] = value;
        }
    }

    std::cout << "Entry point (" << fileName << ") loaded." << std::endl;
    return true;
}

std::string ConfigManager::GetString(const std::string& key, const std::string& defaultValue) {
    if (settings.find(key) != settings.end()) {
        return settings[key];
    }
    return defaultValue;
}

int ConfigManager::GetInt(const std::string& key, int defaultValue) {
    if (settings.find(key) != settings.end()) {
        try {
            return std::stoi(settings[key]);
        }
        catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}