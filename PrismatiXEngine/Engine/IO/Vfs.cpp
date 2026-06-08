#include "Engine/IO/VFS.h"

#include "Engine/Support/Logger.h"

#include <filesystem>
#include <fstream>

namespace px::io {

namespace {
std::filesystem::path Join(const std::string& root, std::string_view rel) {
    return std::filesystem::path(root) / std::filesystem::path(rel);
}
}

void VFS::MountDirectory(const std::string& root) {
    m_dirs.push_back(root);
    PX_LOG_INFO("VFS mounted directory '{}'", root);
}

bool VFS::MountArchive(const std::string& archivePath, const crypto::Key* key) {
    auto archive = std::make_unique<Archive>();
    if (!archive->Open(archivePath, key)) {
        return false;
    }
    m_archives.push_back(std::move(archive));
    return true;
}

void VFS::Clear() {
    m_dirs.clear();
    m_archives.clear();
}

bool VFS::Exists(std::string_view path) const {
    for (const std::string& dir : m_dirs) {
        if (std::filesystem::exists(Join(dir, path))) {
            return true;
        }
    }
    for (const auto& archive : m_archives) {
        if (archive->Contains(path)) {
            return true;
        }
    }
    return false;
}

std::optional<Bytes> VFS::Read(std::string_view path) const {
    for (const std::string& dir : m_dirs) {
        const std::filesystem::path full = Join(dir, path);
        std::ifstream in(full, std::ios::binary | std::ios::ate);
        if (in) {
            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            Bytes data(static_cast<std::size_t>(size));
            if (in.read(reinterpret_cast<char*>(data.data()), size)) {
                return data;
            }
        }
    }
    for (const auto& archive : m_archives) {
        if (auto data = archive->Read(path)) {
            return data;
        }
    }
    return std::nullopt;
}

std::optional<std::string> VFS::ReadText(std::string_view path) const {
    if (auto bytes = Read(path)) {
        return std::string(bytes->begin(), bytes->end());
    }
    return std::nullopt;
}

}
