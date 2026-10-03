#if defined(PRISMATIX_WEB_PLAYER)
#include "Engine/Accessibility/PlatformSemanticAdapter.h"
#include <emscripten/emscripten.h>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
namespace px::accessibility {
namespace {
EM_JS(void, PublishWebSemantics, (const char* json), {
    Module.prismatix.accessibility.publish(JSON.parse(UTF8ToString(json)));
});
EM_JS(char*, WebSemanticActions, (), {
    return stringToNewUTF8(JSON.stringify(Module.prismatix.accessibility.takeActions()));
});
EM_JS(void, ClearWebSemantics, (), { Module.prismatix.accessibility.clear(); });
class WebSemanticAdapter final : public SemanticAdapter {
public:
    ~WebSemanticAdapter() override { ClearWebSemantics(); }
    void Publish(const SemanticTree& tree) override {
        MockSemanticAdapter serialized;
        serialized.Publish(tree);
        PublishWebSemantics(serialized.CanonicalJson().c_str());
    }
    bool NeedsPublish() const override {
        char* json = WebSemanticActions();
        const auto actions = nlohmann::json::parse(json ? json : "[]", nullptr, false);
        std::free(json);
        if (actions.is_array()) for (const auto& action : actions) {
            if (!action.is_object()) continue;
            const auto id = Uuid::Parse(action.value("id", ""));
            if (id) (void)InvokeAction(*id, action.value("action", ""), action.value("value", ""));
        }
        const auto now = SDL_GetTicks();
        if (m_published && now - m_lastPublish < 100) return false;
        m_published = true; m_lastPublish = now;
        return true;
    }
private:
    mutable Uint64 m_lastPublish = 0;
    mutable bool m_published = false;
};
}
std::shared_ptr<SemanticAdapter> CreatePlatformSemanticAdapter(SDL_Window*) {
    return std::make_shared<WebSemanticAdapter>();
}
std::string_view PlatformAccessibilityBackend() { return "Web ARIA"; }
}
#endif
