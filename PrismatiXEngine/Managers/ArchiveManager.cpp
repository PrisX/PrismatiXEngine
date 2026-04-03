#include "ArchiveManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "SecretKey.h"

namespace fs = std::filesystem;

ArchiveManager::ArchiveManager() {
    ScanDirectory("Data");
    ScanDirectory("Engine");
}

void ArchiveManager::ScanDirectory(const std::string& root) {
    if (!fs::exists(root) || !fs::is_directory(root)) return;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::string fileName = entry.path().filename().string();
            std::string fullPath = entry.path().string();
            std::replace(fullPath.begin(), fullPath.end(), '\\', '/');

            diskFileMap[fileName] = fullPath;

            std::string relativePath = fs::relative(entry.path(), root).string();
            std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
            diskFileMap[relativePath] = fullPath;
        }
    }
}

bool ArchiveManager::MountArchive(const std::string& archivePath) {
    if (!fs::exists(archivePath)) {
        return true;
    }

    std::ifstream in(archivePath, std::ios::binary);
    if (!in) return false;

    char magic[4];
    in.read(magic, 4);
    if (strncmp(magic, "PDX!", 4) != 0) return false;

    uint32_t fileCount;
    in.read(reinterpret_cast<char*>(&fileCount), sizeof(fileCount));

    for (uint32_t i = 0; i < fileCount; ++i) {
        uint16_t nameLen;
        in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        std::string fileName(nameLen, '\0');
        in.read(&fileName[0], nameLen);

        PDXFileEntry entry;
        entry.archivePath = archivePath;
        in.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));
        in.read(reinterpret_cast<char*>(&entry.size), sizeof(entry.size));
        globalFileTable[fileName] = entry;
    }
    return true;
}

std::vector<char> ArchiveManager::ExtractFile(const std::string& fileName) {
    std::string normalizedName = fileName;
    std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');

    // 1. Disk mapping
    if (diskFileMap.count(normalizedName)) {
        std::string actualPath = diskFileMap[normalizedName];
        std::ifstream in(actualPath, std::ios::binary | std::ios::ate);
        if (in) {
            size_t size = in.tellg();
            std::vector<char> buffer(size);
            in.seekg(0, std::ios::beg);
            in.read(buffer.data(), size);
            return buffer;
        }
    }

    // 2. Archive
    auto it = globalFileTable.find(normalizedName);
    if (it == globalFileTable.end()) return {};

    PDXFileEntry entry = it->second;
    std::ifstream in(entry.archivePath, std::ios::binary);
    if (!in) return {};

    in.seekg(entry.offset, std::ios::beg);
    std::vector<char> buffer(entry.size);
    in.read(buffer.data(), entry.size);
    in.close();

    int keyLen = (int)PDX_SECRET_KEY.length();
    if (keyLen > 0) {
        for (size_t i = 0; i < buffer.size(); i++) {
            buffer[i] ^= PDX_SECRET_KEY[i % keyLen];
        }
    }

    return buffer;
}

std::string ArchiveManager::LoadTextFromArchiveOrDisk(const std::string& path) {
    std::vector<char> buffer = ExtractFile(path);
    if (buffer.empty()) return "";
    return std::string(buffer.begin(), buffer.end());
}
