#pragma once

#include "Engine/UI/UiStage.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px {
class Input;
}
namespace px::gfx {
class Renderer2D;
}
namespace px::io {
class Vfs;
}

namespace px::ui {

class ScreenManager {
public:
    explicit ScreenManager(io::Vfs& vfs) : m_vfs(vfs) {}

    UiStage* Open(const std::string& vfsPath);
    void Close();
    void CloseAll();
    [[nodiscard]] bool Empty() const { return m_stack.empty(); }
    [[nodiscard]] UiStage* Top();
    [[nodiscard]] const std::string& TopPath() const;

    void SetTrigger(int scancode, std::string vfsPath) { m_triggers[scancode] = std::move(vfsPath); }
    void HandleTriggers(const Input& input);

    std::optional<UiAction> Update(const Input& input, float dt);
    void Render(gfx::Renderer2D& renderer);

private:
    struct Screen {
        std::string path;
        UiStage stage;
    };
    io::Vfs& m_vfs;
    std::vector<std::unique_ptr<Screen>> m_stack;
    std::unordered_map<int, std::string> m_triggers;
};

}
