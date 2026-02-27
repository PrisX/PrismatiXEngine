#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint> // uint64_t

struct PDXFileEntry {
    std::string archivePath;
    uint64_t offset;
    uint64_t size;
};

class ArchiveManager {
private:
    static std::unordered_map<std::string, PDXFileEntry> globalFileTable;

public:
    static bool MountArchive(const std::string& archivePath);
    static std::vector<char> ExtractFile(const std::string& fileName);
};