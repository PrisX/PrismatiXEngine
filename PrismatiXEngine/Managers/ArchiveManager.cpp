#include "ArchiveManager.h"

#include <fstream>
#include <iostream>

#include "SecretKey.h"

std::unordered_map<std::string, PDXFileEntry> ArchiveManager::globalFileTable;

bool ArchiveManager::MountArchive(const std::string& archivePath) {
    std::ifstream in(archivePath, std::ios::binary);
    if (!in) {
        std::cerr << "Archive not found (" << archivePath << "): " << std::endl;
        return false;
    }

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

        // For patch
        globalFileTable[fileName] = entry;
    }

    std::cout << "Mounted archive (" << archivePath << ") [Including " << fileCount << " files]" << std::endl;
    return true;
}

std::vector<char> ArchiveManager::ExtractFile(const std::string& fileName) {
    if (globalFileTable.find(fileName) == globalFileTable.end()) {
        std::cerr << "File not found (" << fileName << "): " << std::endl;
        return {};
    }

    PDXFileEntry entry = globalFileTable[fileName];

    std::ifstream in(entry.archivePath, std::ios::binary);
    in.seekg(entry.offset, std::ios::beg);

    std::vector<char> buffer(entry.size);
    in.read(buffer.data(), entry.size);
    in.close();

    // XOR decryption
    int keyLen = PDX_SECRET_KEY.length();
    for (size_t i = 0; i < buffer.size(); i++) {
        buffer[i] ^= PDX_SECRET_KEY[i % keyLen];
    }

    return buffer;
}