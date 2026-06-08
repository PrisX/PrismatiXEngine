#include "Engine/UI/ScreenManager.h"

#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/UISchema.h"
#include "Engine/Support/Logger.h"

namespace px::ui {

namespace {
const std::string kEmpty;
}

UIStage* ScreenManager::Open(const std::string& vfsPath) {
    auto text = m_vfs.ReadText(vfsPath);
    if (!text) {
        PX_LOG_WARN("ScreenManager: screen not found '{}'", vfsPath);
        return nullptr;
    }
    auto scene = ParsePXUI(*text);
    if (!scene) {
        PX_LOG_WARN("ScreenManager: parse failed '{}'", vfsPath);
        return nullptr;
    }
    auto screen = std::make_unique<Screen>();
    screen->path = vfsPath;
    screen->stage.Load(std::move(*scene));
    screen->stage.TriggerEnter();
    m_stack.push_back(std::move(screen));
    return &m_stack.back()->stage;
}

void ScreenManager::Close() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

void ScreenManager::CloseAll() {
    m_stack.clear();
}

UIStage* ScreenManager::Top() {
    return m_stack.empty() ? nullptr : &m_stack.back()->stage;
}

const std::string& ScreenManager::TopPath() const {
    return m_stack.empty() ? kEmpty : m_stack.back()->path;
}

void ScreenManager::HandleTriggers(const Input& input) {
    for (const auto& [scancode, path] : m_triggers) {
        if (input.KeyPressed(scancode)) {
            if (!m_stack.empty() && m_stack.back()->path == path) {
                Close();
            } else {
                Open(path);
            }
            return;
        }
    }
}

std::optional<UIAction> ScreenManager::Update(const Input& input, float dt) {
    if (m_stack.empty()) {
        return std::nullopt;
    }
    auto action = m_stack.back()->stage.Update(input, dt);
    if (!action) {
        return std::nullopt;
    }
    if (action->type == "back" || action->type == "screen.close") {
        Close();
        return std::nullopt;
    }
    if (action->type == "screen.open" && !action->target.empty()) {
        Open(action->target);
        return std::nullopt;
    }
    return action;
}

void ScreenManager::Render(graphics::Renderer2D& renderer) {
    for (auto& screen : m_stack) {
        screen->stage.Render(renderer);
    }
}

}
