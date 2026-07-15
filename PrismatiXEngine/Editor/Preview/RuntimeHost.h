#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/Startup/SplashTypes.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/Lua/LuaHost.h"
#include "Editor/Tools/UIDesigner/DocumentChangeSet.h"
#include "Engine/VN/GameCatalog.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::ui::startup {
class SplashSequencePlayer;
}

namespace px::editor {

class UIPreviewDocumentApplier;

class RuntimeHost {
public:
    enum class Mode { UIScene, Vn, Splash };
    explicit RuntimeHost(SDL_Renderer* editorRenderer);
    ~RuntimeHost();

    void SetProjectRoot(const std::string& root);
    void SetMode(Mode mode) { m_mode = mode; }
    [[nodiscard]] Mode GetMode() const { return m_mode; }
    void LoadUI(const std::string& vfsPath);
    void LoadUIDocument(const resource::TypedDocument& document, const std::string& sourcePath);
    // Exact document changes patch safe properties in place and select the
    // required relayout/style/animation work; structural changes rebuild.
    bool ApplyUIDocumentChange(const resource::TypedDocument& document,
                               const std::string& sourcePath,
                               const DocumentChangeSet& changes);
    Status PreviewUIAnimation(const Uuid& clip, float time, bool playing);
    Status SetUIAnimationParameter(std::string_view parameter, const Variant& value);
    Status PreviewSplashSequence(std::vector<ui::startup::SplashScreenEntry> entries,
                                 bool reducedMotion = false);
    [[nodiscard]] ui::BehaviorRuntimeState UIBehaviorState() const { return m_uiScene.CaptureBehaviorState(); }
    [[nodiscard]] ui::UIAnimationRuntimeState UIAnimationState() const { return m_uiScene.CaptureAnimationState(); }
    void LoadVn(const std::string& script);
    bool LoadVnText(std::string_view text, const std::string& sourcePath);
    void SetGameCatalog(const vn::GameCatalog& catalog) { m_session->VM().SetGameCatalog(catalog); }
    void Reload();
    void SetDisplayScale(float scale, bool pixelExact = false);
    [[nodiscard]] std::optional<Vec2> ImageSize(const std::string& path);
    void Tick(float dt, std::uint64_t nowMs, bool hovered, float localX, float localY, bool click);

    [[nodiscard]] SDL_Texture* Target() const { return m_target; }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }
    [[nodiscard]] const std::string& CurrentUIPath() const { return m_uiPath; }
    [[nodiscard]] const vn::VM& VMRef() const { return m_session->VM(); }
    [[nodiscard]] vn::VM& VMRef() { return m_session->VM(); }
    [[nodiscard]] audio::AudioEngine& AudioRef() { return *m_audio; }
    [[nodiscard]] const vn::VariableStore& Vars() const { return m_session->Variables(); }
    [[nodiscard]] const vn::Dialogue& DialogueRef() const { return m_session->Dialogue(); }

private:
    void EnsureTarget();
    [[nodiscard]] ui::DialoguePresentation DialogueUI() const;

    SDL_Renderer* m_editorRenderer;
    SDL_Texture* m_target = nullptr;
    int m_width = 1280, m_height = 720;
    int m_targetDensity = 1;
    float m_displayScale = 1.0f;
    bool m_pixelExactPreview = false;
    Mode m_mode = Mode::UIScene;
    io::VFS m_vfs;
    std::unique_ptr<graphics::AssetCache> m_assets;
    std::unique_ptr<graphics::Renderer2D> m_renderer;
    std::unique_ptr<audio::AudioEngine> m_audio;
    Input m_input;
    std::unique_ptr<RuntimeSession> m_session;
    lua::LuaServices m_luaServices;
    std::unique_ptr<lua::LuaHost> m_lua;
    ui::UIContext m_uiScene;
    std::unique_ptr<UIPreviewDocumentApplier> m_previewApplier;
    std::unique_ptr<ui::startup::SplashSequencePlayer> m_splash;
    ui::GalgameUI m_hud;
    std::vector<std::string> m_choiceTexts;
    std::string m_uiPath;
    std::string m_vnScript;
    std::unordered_map<std::string, std::string> m_routeScenes;
};

}  // namespace px::editor
