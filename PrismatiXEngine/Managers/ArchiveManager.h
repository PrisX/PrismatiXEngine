#pragma once
#include <cstdint>  // uint64_t
#include <string>
#include <unordered_map>
#include <vector>

struct PDXFileEntry {
    std::string archivePath;
    uint64_t offset;
    uint64_t size;
};

class ArchiveManager {
private:
    std::unordered_map<std::string, PDXFileEntry> globalFileTable;

public:
    ArchiveManager() = default;
    ~ArchiveManager() = default;

    bool MountArchive(const std::string& archivePath);
    std::vector<char> ExtractFile(const std::string& fileName);
    std::string LoadTextFromArchiveOrDisk(const std::string& path);
};