#pragma once

#include "Engine/Core/Object.h"

#include <filesystem>

namespace px::resource {

class Resource : public Object {
public:
    [[nodiscard]] std::string_view TypeName() const override { return "Resource"; }
    [[nodiscard]] const std::filesystem::path& SourcePath() const { return m_sourcePath; }
    void SetSourcePath(std::filesystem::path path) { m_sourcePath = std::move(path); }

private:
    std::filesystem::path m_sourcePath;
};

}  // namespace px::resource
