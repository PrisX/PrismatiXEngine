#pragma once

#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::io {

class Vfs {
public:
    void MountDirectory(const std::string& root);
    bool MountArchive(const std::string& archivePath, const crypto::Key* key = nullptr);
    void Clear();

    [[nodiscard]] bool Exists(std::string_view path) const;
    [[nodiscard]] std::optional<Bytes> Read(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> ReadText(std::string_view path) const;

private:
    std::vector<std::string> m_dirs;
    std::vector<std::unique_ptr<Archive>> m_archives;
};

}
