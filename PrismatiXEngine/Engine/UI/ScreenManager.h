#pragma once

#include "Engine/UI/UIStage.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px {
class Input;
}
namespace px::graphics {
class Renderer2D;
}
namespace px::io {
class VFS;
}

namespace px::ui {

class ScreenManager {
public:
    explicit ScreenManager(io::VFS& vfs) : m_vfs(vfs) {}

    UIStage* Open(const std::string& vfsPath);
    void Close();
    void CloseAll();
    [[nodiscard]] bool Empty() const { return m_stack.empty(); }
    [[nodiscard]] UIStage* Top();
    [[nodiscard]] const std::string& TopPath() const;

    void SetTrigger(int scancode, std::string vfsPath) { m_triggers[scancode] = std::move(vfsPath); }
    void HandleTriggers(const Input& input);

    std::optional<UIAction> Update(const Input& input, float dt);
    void Render(graphics::Renderer2D& renderer);

private:
    struct Screen {
        std::string path;
        UIStage stage;
    };
    io::VFS& m_vfs;
    std::vector<std::unique_ptr<Screen>> m_stack;
    std::unordered_map<int, std::string> m_triggers;
};

}
