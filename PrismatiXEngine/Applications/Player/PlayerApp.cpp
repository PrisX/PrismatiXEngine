#include "Applications/Player/PlayerApp.h"

#include "Engine/Graphics/Screenshot.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Startup/SplashTypes.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace px::player {

namespace {
const std::string kSaveKey = "prismatix-demo-secret";
constexpr int kAutoSaveSlot = -1;

std::optional<std::string> SafeVfsRelativePath(const std::string& value) {
    return io::VFS::NormalizeVirtualPath(value);
}

std::string LocaleRuntimeIrPath(const std::string_view locale) {
    return "Runtime/Locales/" + std::string(locale) + "/main.pxir";
}

std::string LocaleSourceMapPath(const std::string_view locale) {
    return "Runtime/Locales/" + std::string(locale) + "/main.pxmap";
}

const vn::Command* FindOperation(const vn::Program& program,
                                 const std::string_view operationId) {
    const auto found = std::ranges::find_if(
        program.code, [operationId](const vn::Command& command) {
            return command.operationId == operationId;
        });
    return found == program.code.end() ? nullptr : &*found;
}

std::string LocalizedCommandText(
    const vn::Command& command, const std::string_view field,
    const std::unordered_map<std::string, std::string>& translations,
    const vn::VariableStore& variables) {
    const std::string textId = command.Get("textId", command.sourceId);
    const auto translated = translations.find(textId);
    return variables.Substitute(translated == translations.end()
                                    ? command.Get(field)
                                    : translated->second);
}

bool LocaleProgramsAlign(const vn::Program& current,
                         const vn::Program& candidate) {
    if (current.documentId != candidate.documentId ||
        current.code.size() != candidate.code.size())
        return false;
    for (std::size_t index = 0; index < current.code.size(); ++index) {
        const auto& before = current.code[index];
        const auto& after = candidate.code[index];
        if (before.operationId != after.operationId ||
            before.sourceId != after.sourceId || before.type != after.type)
            return false;
    }
    return true;
}

bool RelocalizeRuntimeState(
    RuntimeSession::GameState& state,
    std::shared_ptr<const vn::Program> candidate,
    const std::string& candidatePath,
    const std::unordered_map<std::string, std::string>& translations,
    const vn::VariableStore& currentVariables) {
    if (!state.runtimeProgram || !candidate ||
        !LocaleProgramsAlign(*state.runtimeProgram, *candidate))
        return false;

    vn::VariableStore variables;
    for (const auto& [name, entry] : currentVariables.Values())
        if (entry.scope == vn::VariableScope::Profile)
            variables.SetValue(name, entry.value.Clone(), entry.scope);
    for (const auto& [name, value] : state.typedVariables)
        variables.SetValue(name, value.Clone(), vn::VariableScope::Session);

    for (auto& choice : state.vm.choices) {
        const vn::Command* command = FindOperation(*candidate,
                                                   choice.operationId);
        if (!command || command->type != "choice") return false;
        choice.text = LocalizedCommandText(*command, "text", translations,
                                           variables);
    }
    for (auto& entry : state.backlog) {
        if (entry.operationId.empty()) continue;  // pre-0.2.0 development save
        const vn::Command* command = FindOperation(*candidate,
                                                   entry.operationId);
        if (!command) return false;
        if (entry.isChoice && command->type == "choice") {
            entry.text = LocalizedCommandText(*command, "text", translations,
                                              variables);
        } else if (!entry.isChoice && command->type == "say") {
            entry.speaker = command->Get("speaker");
            entry.text = LocalizedCommandText(*command, "value", translations,
                                              variables);
        } else {
            return false;
        }
    }

    int current = state.vm.pc;
    if ((state.vm.state == vn::VMState::WaitingClick ||
         state.vm.state == vn::VMState::WaitingTimer ||
         state.vm.state == vn::VMState::WaitingVideo ||
         state.vm.state == vn::VMState::WaitingExternal) &&
        current > 0)
        --current;
    if (current >= 0 && current < static_cast<int>(candidate->code.size())) {
        const auto& command = candidate->code[static_cast<std::size_t>(current)];
        if (command.type == "say" && !state.dialogue.state.fullText.empty()) {
            vn::Dialogue localizedDialogue;
            localizedDialogue.RestoreState(state.dialogue);
            const std::string speaker = command.Get("speaker",
                                                    state.dialogue.state.speaker);
            localizedDialogue.Relocalize(
                speaker, LocalizedCommandText(command, "value", translations,
                                              variables));
            state.dialogue = localizedDialogue.CaptureState();
            state.vm.speaker = speaker;
        }
    }
    state.runtimeProgram = std::move(candidate);
    state.vm.scriptPath = candidatePath;
    return true;
}

int ScancodeFromName(const std::string& name) {
    if (name == "Escape") return SDL_SCANCODE_ESCAPE;
    if (name == "Tab") return SDL_SCANCODE_TAB;
    if (name == "Space") return SDL_SCANCODE_SPACE;
    if (name == "Enter" || name == "Return") return SDL_SCANCODE_RETURN;
    if (name == "Backspace") return SDL_SCANCODE_BACKSPACE;
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        return SDL_SCANCODE_A + (name[0] - 'A');
    }
    if (name.size() >= 2 && name[0] == 'F') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) return SDL_SCANCODE_F1 + (n - 1);
    }
    return SDL_SCANCODE_UNKNOWN;
}

std::optional<std::string> ReadBoundedTextFile(const std::filesystem::path& path,
                                               const std::uintmax_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > limit) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) return std::nullopt;
    return text;
}
std::optional<Variant> JsonVariant(const nlohmann::json& value, const int depth = 0) {
    if (depth > 32) return std::nullopt;
    if (value.is_null()) return Variant{};
    if (value.is_boolean()) return Variant(value.get<bool>());
    if (value.is_number_integer()) return Variant(value.get<std::int64_t>());
    if (value.is_number()) return Variant(value.get<double>());
    if (value.is_string()) return Variant(value.get<std::string>());
    if (value.is_array()) {
        VariantArray result;
        for (const auto& item : value) {
            auto converted = JsonVariant(item, depth + 1);
            if (!converted) return std::nullopt;
            result.push_back(std::move(*converted));
        }
        return Variant(std::move(result));
    }
    if (value.is_object()) {
        VariantObject result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            auto converted = JsonVariant(item.value(), depth + 1);
            if (!converted) return std::nullopt;
            result.emplace(item.key(), std::move(*converted));
        }
        return Variant(std::move(result));
    }
    return std::nullopt;
}
}

PlayerApp::Boot PlayerApp::LoadBootConfig() {
    Boot boot;
    boot.config.title = "PrismatiX Player";
    boot.config.mountDirs.clear();
    const auto text = ReadBoundedTextFile("Package/manifest.json", 4 * 1024 * 1024);
    if (!text) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Fatal,
                                    .code = "PXPLAYER5002",
                                    .category = "Player.Boot",
                                    .message = "Package/manifest.json is missing or unreadable"});
        return boot;
    }
    const auto parsed = sdk::detail::ParsePackageManifest(*text);
    if (!parsed.Valid()) {
        for (const auto& diagnostic : parsed.diagnostics) {
            diag::Emit(diag::Diagnostic{.severity = diag::Severity::Fatal,
                                        .code = diagnostic.code,
                                        .category = "Player.Boot",
                                        .message = diagnostic.message});
        }
        return boot;
    }
    const auto& package = parsed.manifest;
    boot.packaged = true;
    boot.config.title = package.title;
    boot.gameId = package.gameId;
    boot.packageFingerprint = package.packageFingerprint;
    boot.contentVersion = package.contentVersion;
    boot.saveVersion = package.saveVersion;
    for (const auto& migration : package.saveMigrations) {
        boot.saveMigrations.push_back(
            {migration.id, migration.fromContentVersion,
             migration.fromSaveVersion, migration.toContentVersion,
             migration.toSaveVersion, migration.asset});
    }
    boot.extensions = package.extensions;
    boot.graphicsTier = package.graphicsTier;
    boot.config.graphicsTier = package.graphicsTier;
    for (const auto& effect : package.customEffects) {
        graphics::CustomEffectDescriptor descriptor;
        descriptor.id = effect.id;
        descriptor.targetLayer = effect.targetLayer;
        descriptor.samplerCount = effect.samplerCount;
        descriptor.uniformBufferCount = effect.uniformBufferCount;
        for (const auto& uniform : effect.uniforms)
            descriptor.uniforms.push_back(
                {uniform.name, uniform.type, uniform.slot,
                 uniform.defaultValue, uniform.minimum, uniform.maximum});
        for (const auto& artifact : effect.artifacts)
            descriptor.artifacts.push_back(
                {artifact.format, artifact.asset, artifact.fingerprint});
        boot.config.customEffects.push_back(std::move(descriptor));
    }
    for (const auto& archive : package.archives)
        boot.config.mountArchives.push_back(archive.file);
    boot.config.archiveKey = package.archiveKey;
    boot.config.width = boot.config.logicalWidth = package.width;
    boot.config.height = boot.config.logicalHeight = package.height;
    boot.startScript = package.startRuntimeIr;
    boot.sourceMap = package.sourceMap;
    boot.startRoute = package.startRoute;
    for (const auto& route : package.routes)
        boot.routeScenes.emplace(route.id, route.scene);
    boot.saveSecret = package.archiveKey.empty() ? package.packageFingerprint
                                                 : package.archiveKey;
    boot.valid = true;
    PX_LOG_INFO("Packaged build: mounting {} content group(s)",
                boot.config.mountArchives.size());
    return boot;
}

bool PlayerApp::LoadLocale(std::string locale, const bool refreshPresentation) {
    if (std::find(m_supportedLocales.begin(), m_supportedLocales.end(), locale) ==
        m_supportedLocales.end()) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                    .code = "PXPLAYER5013",
                                    .category = "Player.Localization",
                                    .message = "Requested locale is not supported",
                                    .details = locale});
        return false;
    }

    const std::string localePath = "Content/Localization/" + locale + ".json";
    const auto langText = m_runtime.VFS().ReadText(localePath);
    if (!langText) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                    .code = "PXPLAYER5012",
                                    .category = "Player.Localization",
                                    .message = "Selected locale document is missing",
                                    .details = localePath});
        return false;
    }
    const auto document = nlohmann::json::parse(*langText, nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        document.value("format", std::string{}) != "PrismatiXLocale" ||
        document.value("schemaRevision", 0) != 2 ||
        document.value("locale", std::string{}) != locale ||
        !document.contains("strings") || !document["strings"].is_object()) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                    .code = "PXPLAYER5010",
                                    .category = "Player.Localization",
                                    .message = "Canonical locale document is invalid",
                                    .details = localePath});
        return false;
    }

    std::unordered_map<std::string, std::string> candidate;
    for (auto it = document["strings"].begin(); it != document["strings"].end(); ++it) {
        if (!it.value().is_string()) {
            diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                        .code = "PXPLAYER5011",
                                        .category = "Player.Localization",
                                        .message = "Locale strings must contain only text values",
                                        .details = it.key()});
            return false;
        }
        candidate.emplace(it.key(), it.value().get<std::string>());
    }
    std::vector<std::string> fontChain;
    if (const auto fonts = document.find("fontChain");
        fonts != document.end()) {
        if (!fonts->is_array() || fonts->size() > 16) {
            diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                        .code = "PXPLAYER5014",
                                        .category = "Player.Localization",
                                        .message = "Locale fontChain must be a bounded array",
                                        .details = localePath});
            return false;
        }
        std::set<std::string> uniqueFonts;
        for (const auto& value : *fonts) {
            if (!value.is_string()) {
                diag::Emit(diag::Diagnostic{
                    .severity = diag::Severity::Error,
                    .code = "PXPLAYER5015",
                    .category = "Player.Localization",
                    .message = "Locale fallback font entries must be paths",
                    .details = localePath});
                return false;
            }
            const auto safe = SafeVfsRelativePath(value.get<std::string>());
            if (!safe || (!safe->ends_with(".ttf") &&
                          !safe->ends_with(".otf")) ||
                !m_runtime.VFS().Exists(*safe) ||
                !uniqueFonts.insert(*safe).second) {
                diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                            .code = "PXPLAYER5015",
                                            .category = "Player.Localization",
                                            .message = "Locale fallback font is missing, unsafe, duplicated, or unsupported",
                                            .details = value.is_string() ? value.get<std::string>() : localePath});
                return false;
            }
            fontChain.push_back(*safe);
        }
    }

    const std::string runtimePath = LocaleRuntimeIrPath(locale);
    const std::string sourceMapPath = LocaleSourceMapPath(locale);
    const auto sourceMapText = m_runtime.VFS().ReadText(sourceMapPath);
    auto runtimeProgram = m_session->PrepareRuntimeIr(runtimePath);
    if (!runtimeProgram || !sourceMapText) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXPLAYER5016",
            .category = "Player.Localization",
            .message = "Locale-specific RuntimeIR or source map is missing",
            .details = runtimePath});
        return false;
    }
    const auto parsedSourceMap = sdk::ParseSourceMap(*sourceMapText);
    if (!parsedSourceMap.Valid() ||
        parsedSourceMap.document.documentId != runtimeProgram->documentId) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXPLAYER5017",
            .category = "Player.Localization",
            .message = "Locale-specific source map is invalid or mismatched",
            .details = sourceMapPath});
        return false;
    }

    std::optional<RuntimeSession::PreparedRestore> preparedRuntime;
    std::vector<vn::BacklogEntry> localizedNvl;
    std::deque<RollbackEntry> localizedRollback = m_rollback;
    if (m_appState == AppState::Game && m_session->RuntimeProgramIdentity()) {
        auto state = m_session->CaptureState();
        if (!RelocalizeRuntimeState(state, runtimeProgram, runtimePath,
                                    candidate, m_session->Variables())) {
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Error,
                .code = "PXPLAYER5018",
                .category = "Player.Localization",
                .message = "Localized Story topology does not match the running Story",
                .details = runtimePath});
            return false;
        }
        auto nvlState = state;
        nvlState.backlog = m_nvlLines;
        if (!RelocalizeRuntimeState(nvlState, runtimeProgram, runtimePath,
                                    candidate, m_session->Variables()))
            return false;
        localizedNvl = std::move(nvlState.backlog);
        for (auto& rollback : localizedRollback) {
            auto& snapshot = rollback.snap;
            RuntimeSession::GameState rollbackState;
            rollbackState.vm = snapshot.vm;
            rollbackState.dialogue = snapshot.dialogue;
            rollbackState.typedVariables = snapshot.typedVariables;
            rollbackState.stage = snapshot.stage;
            rollbackState.audio = snapshot.audio;
            rollbackState.backlog = snapshot.backlog;
            rollbackState.routes = snapshot.routes;
            rollbackState.timelines = snapshot.timelines;
            rollbackState.animationClips = snapshot.animationClips;
            rollbackState.ui = snapshot.ui;
            rollbackState.runtimeProgram = snapshot.runtimeProgram;
            if (!RelocalizeRuntimeState(rollbackState, runtimeProgram,
                                        runtimePath, candidate,
                                        m_session->Variables())) {
                diag::Emit(diag::Diagnostic{
                    .severity = diag::Severity::Error,
                    .code = "PXPLAYER5019",
                    .category = "Player.Localization",
                    .message = "Rollback history cannot be mapped to the requested locale",
                    .details = snapshot.anchor.operationId});
                return false;
            }
            snapshot.vm = rollbackState.vm;
            snapshot.dialogue = rollbackState.dialogue;
            snapshot.backlog = rollbackState.backlog;
            snapshot.runtimeProgram = rollbackState.runtimeProgram;
            snapshot.scriptPath = runtimePath;
            snapshot.pc = rollbackState.vm.pc;
            auto rollbackNvl = rollbackState;
            rollbackNvl.backlog = snapshot.nvlLines;
            if (!RelocalizeRuntimeState(rollbackNvl, runtimeProgram,
                                        runtimePath, candidate,
                                        m_session->Variables()))
                return false;
            snapshot.nvlLines = std::move(rollbackNvl.backlog);
        }
        auto prepared = m_session->PrepareRestore(
            state, m_runtime.GetClock().NowMs());
        if (!prepared) {
            for (const auto& diagnostic : prepared.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
        preparedRuntime = prepared.TakeValue();
    }

    if (preparedRuntime &&
        !m_session->CommitRestore(std::move(*preparedRuntime)))
        return false;
    // Parsing was completed before the runtime transaction, so this commit
    // cannot expose a half-switched program/source-map pair.
    m_session->CommitSourceMap(parsedSourceMap.document, sourceMapPath);

    m_langTable = std::move(candidate);
    m_settings.language = std::move(locale);
    m_script = runtimePath;
    if (!localizedNvl.empty() || m_nvlLines.empty())
        m_nvlLines = std::move(localizedNvl);
    m_rollback = std::move(localizedRollback);
    m_runtime.Renderer().SetTextLocale(m_settings.language,
                                       std::move(fontChain));
    // Locale and font-chain changes invalidate both render and measurement
    // layouts before the next UI transaction.
    m_ui.SetTextRenderer(&m_runtime.Renderer());
    m_session->VM().SetTextFilter(
        [this](const std::string& textId, const std::string& fallback) {
            const auto found = m_langTable.find(textId);
            return found == m_langTable.end() ? fallback : found->second;
        });
    PX_LOG_INFO("Localization: {} entries for '{}'", m_langTable.size(),
                m_settings.language);

    if (!refreshPresentation) return true;
    switch (m_ui.CurrentScreen()) {
        case ui::GalgameUI::Screen::HUD:
            (void)m_ui.ShowHUD(DialogueUI());
            break;
        case ui::GalgameUI::Screen::Backlog:
            (void)m_ui.ShowBacklog(BacklogItems());
            break;
        case ui::GalgameUI::Screen::Save:
            (void)m_ui.ShowSaveLoad(true, SaveItems(true));
            break;
        case ui::GalgameUI::Screen::Load:
            (void)m_ui.ShowSaveLoad(false, SaveItems(false));
            break;
        case ui::GalgameUI::Screen::Gallery:
            (void)m_ui.ShowGallery(GalleryItems());
            break;
        case ui::GalgameUI::Screen::Settings:
            (void)m_ui.ShowSettings(SettingsUI());
            break;
        case ui::GalgameUI::Screen::Title:
            (void)m_ui.ShowTitle();
            break;
        case ui::GalgameUI::Screen::Video:
            break;
    }
    return true;
}

bool PlayerApp::Init(int argc, char* argv[]) {
    m_boot = LoadBootConfig();
    if (!m_boot.valid) return false;
    if (!m_runtime.Init(m_boot.config)) {
        if (m_boot.graphicsTier == "gpu-effects")
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Fatal,
                .code = "PXPLAYER5010",
                .category = "Player.Graphics",
                .message = "The required SDL_GPU effects tier could not initialize",
                .details = "The device must support D3D12, Metal, or Vulkan and a packaged shader format"});
        PX_LOG_CRITICAL("Failed to initialize runtime.");
        return false;
    }

    // Save files are keyed per project; the fixed default only covers projects
    // that ship neither a key nor a title.
    m_saveKey = crypto::DeriveKey(
        m_boot.saveSecret.empty() ? std::string(kSaveKey) : m_boot.saveSecret + "|px-save");
    progress::LoadGlobalProfile(m_profile, "Save/profile.dat", &m_saveKey);
    m_settings.Load("Save/config.dat", &m_saveKey);
    m_saves.Configure("Save", &m_saveKey);
    PX_LOG_DEBUG("Player boot: persistence configured");
    m_runtime.Audio().SetBGMVolume(m_settings.bgmVolume);
    m_runtime.Audio().SetSEVolume(m_settings.seVolume);
    m_runtime.Audio().SetVoiceVolume(m_settings.voiceVolume);
    if (m_settings.fullscreen) {
        SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), true);
    } else if (m_settings.windowWidth >= 320 && m_settings.windowHeight >= 180) {
        SDL_SetWindowSize(m_runtime.GetWindow().Handle(), m_settings.windowWidth,
                          m_settings.windowHeight);
        SDL_SetWindowPosition(m_runtime.GetWindow().Handle(), SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED);
    }

    m_session = std::make_unique<RuntimeSession>(RuntimeSession::Services{
        m_runtime.VFS(), m_runtime.Audio(), m_runtime.Renderer(), m_runtime.Assets()});
    m_ui.SetTextRenderer(&m_runtime.Renderer());
    if (const Status sourceMap = m_session->LoadSourceMap(m_boot.sourceMap);
        !sourceMap) {
        return false;
    }
    m_session->Variables().SetProfileWriteHandler(
        [this](const std::string_view name, const vn::Value& value) {
            m_profile.SetVariable(std::string(name), value.Clone());
        });
    m_ui.SetBehaviorVariableAccess(
        [this](const std::string_view name)->std::optional<Variant>{
            const auto* value=m_session->Variables().GetValue(name);
            return value?std::optional<Variant>(value->Clone()):std::nullopt;
        },
        [this](const std::string_view name,const Variant& value){
            m_session->Variables().SetValue(std::string(name),value.Clone(),
                                            vn::VariableScope::Session);
            return Status::Ok();
        });
    m_ui.SetExternalAnimationServices(
        [this](const std::string_view path){return m_session->PlayAnimationAsset(std::string(path));},
        [this](const std::uint64_t handle){return m_session->Timeline().Playing(handle);});
    m_ui.SetControlRuntimeConfigurator([this](ui::Control& control){auto* rectangle=dynamic_cast<ui::VideoRect*>(&control);if(!rectangle)return;auto player=std::make_shared<video::VideoPlayer>(m_runtime.Renderer().Handle(),m_runtime.VFS());rectangle->SetPlayback({[player](const std::string_view path){return player->Open(std::string(path));},[player]{player->Close();},[player](const float delta){player->Update(delta);},[player]{return player->Playing();},[player]{return player->Texture();},[player]{return Vec2{static_cast<float>(player->Width()),static_cast<float>(player->Height())};}});});
    m_session->SetUIStateHandler(
        [this] { return m_ui.CaptureGameplayRuntimeState(); },
        [this](const ui::UIRuntimeState& state) { return m_ui.RestoreRuntimeState(state); },
        [this](const ui::UIRuntimeState& state) { return m_ui.ValidateRuntimeState(state); });
    if (const auto projectText=m_runtime.VFS().ReadText("project.pxproject")) {
        const auto project=nlohmann::json::parse(*projectText,nullptr,false);
        if(!project.is_discarded()&&project.is_object()&&
           project.contains("assets")&&project["assets"].is_array()){
            for(const auto& asset:project["assets"]){
                if(!asset.is_object()||!asset.contains("id")||
                   !asset["id"].is_string()||!asset.contains("source")||
                   !asset["source"].is_string())continue;
                const std::string source=asset["source"].get<std::string>();
                if(m_runtime.VFS().Exists(source))
                    m_uiAssets.emplace(
                        asset["id"].get<std::string>(),source);
            }
        }
        if(!project.is_discarded()&&project.is_object()&&
           project.contains("uiComponents")&&
           project["uiComponents"].is_array()){
            for(const auto& component:project["uiComponents"]){
                if(!component.is_object()||!component.contains("id")||
                   !component["id"].is_string()||
                   !component.contains("source")||
                   !component["source"].is_string())continue;
                const auto source=SafeVfsRelativePath(
                    component["source"].get<std::string>());
                if(source&&m_runtime.VFS().Exists(*source))
                    m_uiComponents.emplace(
                        component["id"].get<std::string>(),*source);
            }
        }
    }
    m_ui.SetUiAssetResolver(
        [this](const std::string_view assetId)->std::optional<std::string>{
            const auto found=m_uiAssets.find(std::string(assetId));
            return found==m_uiAssets.end()
                       ?std::nullopt
                       :std::optional<std::string>{found->second};
        });
    m_ui.SetUiComponentLoader(
        [this](const std::string_view componentId)
            ->std::optional<ui::UiComponentSource>{
            const auto found=
                m_uiComponents.find(std::string(componentId));
            if(found==m_uiComponents.end())return std::nullopt;
            const auto source=m_runtime.VFS().ReadText(found->second);
            return source
                       ?std::optional<ui::UiComponentSource>{
                            ui::UiComponentSource{found->second,*source}}
                       :std::nullopt;
        });
    if(m_boot.routeScenes.empty()||!m_boot.routeScenes.contains(m_boot.startRoute)){diag::Diagnostic diagnostic{.severity=diag::Severity::Fatal,.code="PXPLAYER5004",.category="Player.Boot",.message="Route table or startRoute is invalid"};diag::Emit(diagnostic);return false;}
    for(const auto& [route,_]:m_boot.routeScenes){
        const Status registered=m_session->Routes().Register(route,[route](){return Result<std::unique_ptr<ui::Control>>::Success(std::make_unique<ui::Control>(std::string("Route:")+route));});
        if(!registered)return false;
    }
    m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    PX_LOG_DEBUG("Player boot: VN runtime constructed");

    // Canonical locale documents are the only translation authority.
    m_supportedLocales.clear();
    if (const auto projectText = m_runtime.VFS().ReadText("project.pxproject")) {
        const auto project = nlohmann::json::parse(*projectText, nullptr, false);
        if (!project.is_discarded() && project.is_object()) {
            m_defaultLocale = project.value("defaultLocale", m_defaultLocale);
            const auto locales = project.find("supportedLocales");
            if (locales != project.end() && locales->is_array()) {
                for (const auto& locale : *locales)
                    if (locale.is_string()) m_supportedLocales.push_back(locale.get<std::string>());
            }
        }
    }
    if (std::find(m_supportedLocales.begin(), m_supportedLocales.end(),
                  m_settings.language) == m_supportedLocales.end())
        m_settings.language = m_defaultLocale;
    if (!LoadLocale(m_settings.language, false)) return false;

    m_scriptServices.vfs = &m_runtime.VFS();
    m_scriptServices.renderer = &m_runtime.Renderer();
    m_scriptServices.audio = &m_runtime.Audio();
    m_scriptServices.profile = &m_profile;
    m_scriptServices.input = &m_runtime.GetInput();
    m_scriptServices.stage = &m_session->Stage();
    m_scriptServices.variables = &m_session->Variables();
    m_scriptServices.routes = &m_session->Routes();
    m_scriptServices.timeline = &m_session->Timeline();
    PX_LOG_DEBUG("Player boot: constructing script host");
    m_scriptHost = script::CreateScriptHost(m_scriptServices);
    PX_LOG_DEBUG("Player boot: script host constructed backend={}", m_scriptHost->BackendId());
    (void)m_ui.Actions().RegisterProvider(m_scriptHost->CreateActionProvider());
    PX_LOG_DEBUG("Player boot: script Action provider registered");
    m_session->SetExtensionCommandHandler([this](const vn::Command& cmd) {
        // NVL/ADV mode switches are app-level state, handled before extensions.
        const std::string& t = cmd.type;
        if (t == "nvl") {
            m_nvlMode = true;
            if (cmd.Has("clear")) m_nvlLines.clear();
            return true;
        }
        if (t == "adv") {
            m_nvlMode = false;
            m_nvlLines.clear();
            return true;
        }
        if (t == "er" || t == "nvl_clear") {
            m_nvlLines.clear();
            return true;
        }
        if (t == "action") {
            const auto payload =
                nlohmann::json::parse(cmd.Get("value"), nullptr, false);
            if (payload.is_discarded() || !payload.is_object() ||
                !payload.contains("id") || !payload["id"].is_string() ||
                !payload.contains("arguments") ||
                !payload["arguments"].is_object()) {
                return false;
            }
            ui::ActionInvocation invocation;
            invocation.action = payload["id"].get<std::string>();
            invocation.context.sourceScene = m_script;
            invocation.context.preview = false;
            if (const auto* typed = cmd.FindTyped("arguments")) {
                const auto* arguments = typed->AsObject();
                if (!arguments) return false;
                for (const auto& [name, value] : *arguments) {
                    invocation.arguments.emplace(name, value.Clone());
                }
            } else {
                for (auto argument = payload["arguments"].begin();
                     argument != payload["arguments"].end(); ++argument) {
                    auto value = JsonVariant(argument.value());
                    if (!value) return false;
                    invocation.arguments.emplace(argument.key(), std::move(*value));
                }
            }
            const auto started = m_ui.Actions().Start(std::move(invocation));
            if (!started) {
                for (const auto& diagnostic : started.Diagnostics()) {
                    diag::Emit(diagnostic);
                }
                return false;
            }
            if (m_scriptHost->HasPendingAction()) m_session->VM().WaitExternal();
            return true;
        }
        const bool handled=m_scriptHost->InvokeCommand(cmd);
        if(handled&&(m_scriptHost->HasPendingCommand()||m_scriptHost->HasPendingAction()))
            m_session->VM().WaitExternal();
        return handled;
    });
    m_session->SetRoutePresentationHandler(
        [this](std::string_view route, std::string_view operation) {
            PresentRoute(std::string(route), std::string(operation));
        });
    // Load exactly the ordered extension set sealed into the package
    // manifest. No filename convention or working-directory fallback is part
    // of the formal Player path.
    for (const auto& extension : m_boot.extensions) {
        PX_LOG_DEBUG("Player boot: loading extension {}", extension);
        if (!m_scriptHost->LoadExtensionManifest(extension)) {
            diag::Diagnostic diagnostic{
                .severity = diag::Severity::Fatal,
                .code = "PXPLAYER5005",
                .category = "Player.Extensions",
                .message = "Declared extension manifest could not be loaded",
                .details = extension};
            diagnostic.source.path = extension;
            diag::Emit(std::move(diagnostic));
            return false;
        }
    }
    if (const Status ready = m_scriptHost->Emit("engine.ready"); !ready) {
        for (const auto& diagnostic : ready.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    PX_LOG_DEBUG("Player boot: engine.ready emitted");
    m_session->VM().SetUnlockHook([this](const std::string& kind, const std::string& id) {
        if (kind == "cg") m_profile.UnlockCG(id);
        else m_profile.UnlockScene(id);
        progress::SaveGlobalProfile(m_profile, "Save/profile.dat", &m_saveKey);
        PX_LOG_INFO("Player progression unlocked kind={} id={}", kind, id);
    });
    m_session->VM().SetSeenHook([this](const std::string& key) {
        const bool seen = m_profile.HasSeen(key);
        m_profile.MarkSeen(key);
        return seen;
    });
    m_session->VM().SetChoiceSeenHook([this](const std::string& key) {
        m_profile.MarkChoiceSeen(key);
    });
    PX_LOG_DEBUG("Player boot: constructing video player");
    SDL_Renderer* videoRenderer = m_runtime.GetWindow().Renderer();
    PX_LOG_DEBUG("Player boot: video renderer resolved");
    io::VFS& videoVfs = m_runtime.VFS();
    PX_LOG_DEBUG("Player boot: video VFS resolved");
    m_video =
        std::make_unique<video::VideoPlayer>(videoRenderer, videoVfs);
    PX_LOG_DEBUG("Player boot: video player constructed");
    m_session->VM().SetVideoHook([this](const std::string& path, bool skippable) {
        // Deferred: opened on the next frame so we never re-enter VM::Run().
        m_pendingVideo = path;
        m_videoSkippable = skippable;
    });

    m_ui.SetActionSink([this](const ui::GalgameAction& action) { HandleUIAction(action); });
    const auto validateUiAnimation =
        [this](const auto& binding, const Variant& value,
               const ui::UIRuntimeState& state) {
            return m_ui.ValidateAnimationProperty(binding, value, state);
        };
    m_session->SetAnimationTargetHandler(
        animation::TargetKind::UI,
        [this](const auto& binding,const Variant& value){return m_ui.ApplyAnimationProperty(binding,value);},
        validateUiAnimation);
    m_session->SetAnimationTargetHandler(
        animation::TargetKind::Text,
        [this](const auto& binding,const Variant& value){return m_ui.ApplyAnimationProperty(binding,value);},
        validateUiAnimation);
    PX_LOG_DEBUG("Player boot: registering typed UI templates");
    const auto registerTemplate=[this](ui::GalgameUI::Screen screen,const std::string& path){
        auto text=m_runtime.VFS().ReadText(path);if(!text){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXPLAYER5003",.category="Player.UI",.message="Required UI template is missing: "+path};d.source.path=path;diag::Emit(d);return false;}
        const Status status=m_ui.RegisterTemplate(screen,*text,path);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return status.IsOk();
    };
    const auto scene=[this](const char* route)->std::string{const auto found=m_boot.routeScenes.find(route);return found==m_boot.routeScenes.end()?std::string{}:found->second;};
    const auto registerOptional=[&](ui::GalgameUI::Screen screen,const char* route){
        const std::string path=scene(route);
        return path.empty()||registerTemplate(screen,path);
    };
    if(!registerTemplate(ui::GalgameUI::Screen::Title,m_boot.routeScenes.at(m_boot.startRoute))||
       !registerOptional(ui::GalgameUI::Screen::HUD,"hud")||
       !registerOptional(ui::GalgameUI::Screen::Backlog,"backlog")||
       !registerOptional(ui::GalgameUI::Screen::Save,"save")||
       !registerOptional(ui::GalgameUI::Screen::Load,"load")||
       !registerOptional(ui::GalgameUI::Screen::Gallery,"gallery")||
       !registerOptional(ui::GalgameUI::Screen::Settings,"settings")||
       !registerOptional(ui::GalgameUI::Screen::Video,"video"))return false;
    m_accessibilityAdapter = accessibility::CreatePlatformSemanticAdapter(
        m_runtime.GetWindow().Handle());
    if (!m_accessibilityAdapter) {
        diag::Emit(diag::Diagnostic{
            .severity=diag::Severity::Fatal,.code="PXACCESS9001",
            .category="Accessibility.Platform",
            .message=std::string(accessibility::PlatformAccessibilityBackend()) +
                     " could not attach to the Player window"});
        return false;
    }
    m_ui.SetAccessibilityAdapter(m_accessibilityAdapter);
    PX_LOG_DEBUG("Player boot: typed UI templates registered");
    const auto projectManifest =
        m_runtime.VFS().ReadText("project.pxproject");
    if (!projectManifest) {
        diag::Diagnostic diagnostic{.severity=diag::Severity::Fatal,.code="PXPLAYER5001",.category="Player.Boot",
                                    .message="Required project manifest is missing: project.pxproject"};
        diag::Emit(diagnostic); return false;
    }
    bool characterResourcesDeclared = false;
    const Status characterResources = m_catalog.LoadCharacterResources(
        *projectManifest,
        [this](const std::string_view uri) {
            return m_runtime.VFS().ReadText(std::string(uri));
        },
        [this](const std::string_view uri) {
            return m_runtime.VFS().Exists(std::string(uri));
        },
        characterResourcesDeclared);
    if (!characterResources) return false;
    const auto projectJson = nlohmann::json::parse(*projectManifest, nullptr, false);
    const std::string catalogPath =
        projectJson.is_object()
            ? projectJson.value("gameCatalog", std::string{})
            : std::string{};
    const auto runtimeCatalog = catalogPath.empty()
                                    ? std::optional<std::string>{}
                                    : m_runtime.VFS().ReadText(catalogPath);
    if (!runtimeCatalog) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Fatal,
                                    .code = "PXPLAYER5006",
                                    .category = "Player.Boot",
                                    .message = "Canonical gameCatalog is missing",
                                    .details = catalogPath});
        return false;
    }
    if (const Status status = m_catalog.LoadCanonical(
            *runtimeCatalog, *projectManifest,
            [this](const std::string_view uri) {
                return m_runtime.VFS().Exists(std::string(uri));
            },
            catalogPath);
        !status) return false;
    PX_LOG_DEBUG("Player boot: {} character(s) loaded through {}",
                 m_catalog.Characters().size(),
                 characterResourcesDeclared ? "characterResources=2"
                                            : "canonical project without characters");
    PX_LOG_DEBUG(
        "Player boot: GameCatalog resources variables={} bindings={} gallery={}",
        m_catalog.Variables().size(), m_catalog.InputBindings().size(),
        m_catalog.Gallery().size());
    for (const auto& binding : m_catalog.InputBindings()) {
        if (binding.command == "screen.open") {
            const int sc = ScancodeFromName(binding.key);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                m_screenTriggers[sc] = binding.argument;
                PX_LOG_INFO("Player input binding registered key={} route={}",
                            binding.key, binding.argument);
            }
        }
    }
    m_session->VM().SetGameCatalog(m_catalog);

    m_script = argc > 1 ? argv[1]
                        : (m_script.empty() ? m_boot.startScript : m_script);
    m_splash=std::make_unique<ui::startup::SplashSequencePlayer>(
        ui::startup::SplashSequencePlayer::Services{
            .loadScene=[this](const ResourceRefValue& reference)->Result<resource::TypedDocument>{
                const auto text=m_runtime.VFS().ReadText(reference.lastKnownPath);
                if(!text){diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXBOOT1201",.category="Player.Splash",.message="Splash scene is missing",.details=reference.lastKnownPath};diagnostic.source.path=reference.lastKnownPath;return Result<resource::TypedDocument>::Failure(std::move(diagnostic));}
                return resource::ParseTypedDocument(*text,reference.lastKnownPath);
            },
            .playAudio=[this](const ResourceRefValue& reference){
                if(!m_runtime.VFS().Exists(reference.lastKnownPath)){diag::Diagnostic diagnostic{.severity=diag::Severity::Warning,.code="PXBOOT1202",.category="Player.Splash",.message="Splash audio is missing; visual playback will continue",.details=reference.lastKnownPath};diagnostic.source.path=reference.lastKnownPath;return Status::Fail(std::move(diagnostic));}
                m_runtime.Audio().PlaySE(reference.lastKnownPath);return Status::Ok();
            },
            .diagnostics=[](const diag::Diagnostic& diagnostic){diag::Emit(diagnostic);}});
    m_splash->Context().SetTextRenderer(&m_runtime.Renderer());
    m_splash->Context().SetDiagnosticOverlayEnabled(false);
    m_splash->SetCompletionCallback([this]{FinishBootPresentation();});
    m_appState=AppState::BootSplash;
    const Status splashStarted=m_splash->Start(m_boot.splashes,m_settings.reducedMotion);
    if(!splashStarted)return false;
    ConfigureE2EJourney();
    return true;
}

void PlayerApp::FinishBootPresentation() {
    if(m_appState!=AppState::BootSplash)return;
    if(!m_session->Routes().Replace(m_boot.startRoute)){m_quitRequested=true;return;}
    const Status title=m_ui.ShowTitle();
    if(!title){for(const auto& diagnostic:title.Diagnostics())diag::Emit(diagnostic);m_quitRequested=true;return;}
    m_appState=AppState::Title;
    PX_LOG_INFO("Player presentation ready route={}", m_boot.startRoute);
}

void PlayerApp::SplashFrame(const float dt) {
    if(!m_splash||m_splash->Completed()){FinishBootPresentation();return;}
    Input& input=m_runtime.GetInput();
    int width=0,height=0;m_runtime.Renderer().GetLogicalSize(width,height);
    const bool skip=input.LeftClick()||input.KeyPressed(SDL_SCANCODE_RETURN)||
        input.KeyPressed(SDL_SCANCODE_ESCAPE)||input.KeyPressed(SDL_SCANCODE_SPACE);
    (void)m_splash->Context().Update(input,width,height,dt);
    m_splash->Update(dt,skip);
    if(!m_splash->Completed())m_splash->Context().Render(m_runtime.Renderer());
}

void PlayerApp::StartGame() {
    m_session->Variables().Reset(false);
    for (const auto& v : m_catalog.Variables()) {
        if (v.scope == vn::CatalogVariable::Scope::Profile) {
            const Variant* stored = m_profile.Variable(v.name);
            m_session->Variables().SetValue(
                v.name, stored ? stored->Clone() : v.typedDefault.Clone(),
                vn::VariableScope::Profile);
        } else {
            m_session->Variables().SetValue(
                v.name, v.typedDefault.Clone(), vn::VariableScope::Session);
        }
    }
    m_session->Backlog().Clear();
    m_autoMode = m_skipMode = m_hudHidden = false;
    m_nvlMode = false;
    m_nvlLines.clear();
    m_rollback.clear();
    m_lastBacklogSize = 0;
    m_playtimeBaseMs = 0;
    m_playtimeStartedAtMs = m_runtime.GetClock().NowMs();
    if (m_script.ends_with(".pxir")) {
        if (!m_session->StartRuntimeIr(m_script, /*resetVariables=*/false)) {
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Fatal,
                .code = "PXPLAYER5006",
                .category = "Player.Boot",
                .message = "Compiled Story Runtime IR could not be loaded",
                .details = m_script});
            m_quitRequested = true;
            return;
        }
    } else {
        m_session->VM().LoadScript(m_script);
    }
    if (m_scriptHost) m_scriptHost->Emit("scenario.started", {{"resource", m_script}});
    m_appState = AppState::Game;
    (void)m_session->Routes().Replace("hud");
    const Status hud = m_ui.ShowHUD(DialogueUI());
    if (!hud) {
        for (const auto& diagnostic : hud.Diagnostics()) diag::Emit(diagnostic);
        m_quitRequested = true;
        return;
    }
    PX_LOG_INFO("Player game started script={}", m_script);
}

bool PlayerApp::LoadSlot(int slot) {
    auto snap = m_saves.Load(slot);
    if (!snap) {
        return false;
    }
    if (snap->gameId != m_boot.gameId) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6111",
            .category = "Persistence.Save",
            .message = "Save belongs to a different game"});
        return false;
    }
    const bool sameContent = snap->contentVersion == m_boot.contentVersion &&
                             snap->saveVersion == m_boot.saveVersion;
    if (sameContent &&
        snap->packageFingerprint != m_boot.packageFingerprint) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6111",
            .category = "Persistence.Save",
            .message = "Package fingerprint changed without a content/save version migration"});
        return false;
    }
    if (!sameContent) {
        auto migrated = progress::MigrateSaveSnapshot(
            *snap,
            {m_boot.gameId, m_boot.packageFingerprint, m_boot.contentVersion,
             m_boot.saveVersion},
            m_boot.saveMigrations,
            [this](const std::string_view asset) {
                return m_runtime.VFS().ReadText(std::string(asset));
            });
        if (!migrated) {
            for (const auto& diagnostic : migrated.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
        *snap = migrated.TakeValue();
    }
    const std::string activeLocaleScript =
        LocaleRuntimeIrPath(m_settings.language);
    const std::string candidateScript =
        m_runtime.VFS().Exists(activeLocaleScript)
            ? activeLocaleScript
            : (snap->scriptPath.empty() ? m_script : snap->scriptPath);
    const auto runtimeProgram = m_session->PrepareRuntimeIr(candidateScript);
    if (!runtimeProgram ||
        runtimeProgram->documentId != snap->anchor.runtimeDocumentId) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6112",
            .category = "Persistence.Save",
            .message = "Save execution document is unavailable"});
        return false;
    }
    const auto anchor = std::find_if(
        runtimeProgram->code.begin(), runtimeProgram->code.end(),
        [&snap](const vn::Command& command) {
            return command.sourceId == snap->anchor.sourceId &&
                   command.operationId == snap->anchor.operationId;
        });
    if (anchor == runtimeProgram->code.end()) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6113",
            .category = "Persistence.Save",
            .message = "Save execution anchor no longer exists"});
        return false;
    }
    const int anchorPc = static_cast<int>(
        std::distance(runtimeProgram->code.begin(), anchor));
    const bool resumesAfterAnchor =
        snap->vm.state == vn::VMState::WaitingClick ||
        snap->vm.state == vn::VMState::WaitingTimer ||
        snap->vm.state == vn::VMState::WaitingVideo ||
        snap->vm.state == vn::VMState::WaitingExternal;
    snap->vm.pc = anchorPc + (resumesAfterAnchor ? 1 : 0);
    snap->vm.scriptPath = candidateScript;
    snap->pc = snap->vm.pc;
    RuntimeSession::GameState state;
    state.vm = snap->vm;
    state.dialogue = snap->dialogue;
    state.variables = snap->variables;
    state.typedVariables = snap->typedVariables;
    state.stage = snap->stage;
    state.audio = snap->audio;
    state.backlog = snap->backlog;
    state.routes = snap->routes;
    state.timelines = snap->timelines;
    state.animationClips = snap->animationClips;
    state.ui = snap->ui;
    state.playtimeMs = snap->playtimeMs;
    state.runtimeProgram = runtimeProgram;
    if (!RelocalizeRuntimeState(state, runtimeProgram, candidateScript,
                                m_langTable, m_session->Variables())) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6114",
            .category = "Persistence.Save",
            .message = "Save Story state cannot be mapped to the active locale",
            .details = candidateScript});
        return false;
    }
    const bool awaitingTimeline = std::any_of(snap->timelines.begin(), snap->timelines.end(),
        [](const animation::PlaybackState& playback) { return playback.playing && playback.awaiting; });
    if ((snap->vm.state == vn::VMState::WaitingExternal) !=
        (!snap->scriptPending.empty() || awaitingTimeline)) {
        diag::Emit(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXSAVE6110",
                                    .category="Persistence.Save",
                                    .message="Save has inconsistent script await state"});
        return false;
    }
    const std::uint64_t restoreNow = m_runtime.GetClock().NowMs();
    auto preparedRuntime = m_session->PrepareRestore(state, restoreNow);
    if (!preparedRuntime) {
        for (const auto& diagnostic : preparedRuntime.Diagnostics())
            diag::Emit(diagnostic);
        return false;
    }
    script::PendingCommandsState previousPending;
    script::PendingActionsState previousActions;
    const ui::UIRuntimeState previousUi = m_ui.CaptureRuntimeState();
    const auto rollbackPresentation = [this, &previousPending, &previousActions,
                                       &previousUi] {
        if (m_scriptHost) {
            (void)m_scriptHost->RestoreCheckpoint(previousPending, previousActions);
        }
        (void)m_ui.RestoreRuntimeState(previousUi);
    };
    if (m_scriptHost) {
        previousPending = m_scriptHost->CapturePending();
        previousActions = m_scriptHost->CapturePendingActions();
        const Status scriptStatus = m_scriptHost->RestoreCheckpoint(
            snap->scriptPending, snap->scriptActions);
        if (!scriptStatus) {
            rollbackPresentation();
            return false;
        }
    }
    if (!m_session->CommitRestore(preparedRuntime.TakeValue())) {
        rollbackPresentation();
        return false;
    }
    m_nvlMode = snap->nvlMode;
    RuntimeSession::GameState nvlState = state;
    nvlState.backlog = snap->nvlLines;
    if (RelocalizeRuntimeState(nvlState, runtimeProgram, candidateScript,
                               m_langTable, m_session->Variables()))
        m_nvlLines = std::move(nvlState.backlog);
    else
        m_nvlLines = snap->nvlLines;
    m_rollback.clear();
    m_lastBacklogSize = m_session->Backlog().Entries().size();
    m_playtimeBaseMs = snap->playtimeMs;
    m_playtimeStartedAtMs = m_runtime.GetClock().NowMs();
    m_script = candidateScript;
    m_appState = AppState::Game;
    m_autoMode = m_skipMode = m_hudHidden = false;
    (void)m_ui.RefreshHUD(DialogueUI());
    if (m_scriptHost) m_scriptHost->Emit("save.loaded", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Loaded slot {}", slot);
    return true;
}

progress::SaveSnapshot PlayerApp::MakeSnapshot(bool includeBacklog) {
    progress::SaveSnapshot snap;
    const std::uint64_t now = m_runtime.GetClock().NowMs();
    snap.playtimeMs = m_playtimeBaseMs +
        (now >= m_playtimeStartedAtMs ? now - m_playtimeStartedAtMs : 0);
    const RuntimeSession::GameState state = m_session->CaptureState(snap.playtimeMs);
    snap.gameId = m_boot.gameId;
    snap.packageFingerprint = m_boot.packageFingerprint;
    snap.contentVersion = m_boot.contentVersion;
    snap.saveVersion = m_boot.saveVersion;
    snap.anchor = {m_session->VM().CurrentDocumentId(),
                   m_session->VM().CurrentSourceId(),
                   m_session->VM().CurrentOperationId()};
    snap.runtimeProgram = state.runtimeProgram;
    snap.scriptPath = state.vm.scriptPath;
    snap.pc = m_session->VM().SavePoint();
    snap.chapter = state.vm.chapter;
    snap.bgmPath = state.vm.currentBgm;
    snap.stage = state.stage;
    snap.audio = state.audio;
    snap.variables = state.variables;
    snap.typedVariables = state.typedVariables;
    snap.vm = state.vm;
    snap.dialogue = state.dialogue;
    snap.routes = state.routes;
    snap.timelines = state.timelines;
    snap.animationClips = state.animationClips;
    snap.ui = state.ui;
    if (m_scriptHost) {
        snap.scriptPending = m_scriptHost->CapturePending();
        snap.scriptActions = m_scriptHost->CapturePendingActions();
    }
    if (includeBacklog) snap.backlog = state.backlog;
    snap.nvlMode = m_nvlMode;
    snap.nvlLines = m_nvlLines;
    snap.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
    return snap;
}

void PlayerApp::SaveSlot(int slot, std::vector<std::uint8_t> thumbnail) {
    progress::SaveSnapshot snap = MakeSnapshot(/*includeBacklog=*/true);
    snap.thumbnailPng = std::move(thumbnail);
    if (!m_saves.Save(slot, snap)) {
        diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXPLAYER6001",.category="Player.Save",
                           .message="Could not save slot "+std::to_string(slot)};diag::Emit(d);return;
    }
    if (m_scriptHost) m_scriptHost->Emit("save.written", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Saved slot {} (thumb {} bytes)", slot, snap.thumbnailPng.size());
}

bool PlayerApp::ApplyRollback(const RollbackEntry& entry) {
    const progress::SaveSnapshot& s = entry.snap;
    RuntimeSession::GameState state;
    state.vm = s.vm;
    state.dialogue = s.dialogue;
    state.variables = s.variables;
    state.typedVariables = s.typedVariables;
    state.stage = s.stage;
    state.audio = s.audio;
    state.routes = s.routes;
    state.timelines = s.timelines;
    state.animationClips = s.animationClips;
    state.ui = s.ui;
    state.runtimeProgram = s.runtimeProgram;
    state.backlog = m_session->Backlog().Entries();
    if (state.backlog.size() > entry.backlogSize) state.backlog.resize(entry.backlogSize);
    auto preparedRuntime = m_session->PrepareRestore(
        state, m_runtime.GetClock().NowMs());
    if (!preparedRuntime) {
        for (const auto& diagnostic : preparedRuntime.Diagnostics())
            diag::Emit(diagnostic);
        return false;
    }
    script::PendingCommandsState previousPending;
    script::PendingActionsState previousActions;
    const ui::UIRuntimeState previousUi = m_ui.CaptureRuntimeState();
    const auto rollbackPresentation = [this, &previousPending, &previousActions,
                                       &previousUi] {
        if (m_scriptHost) {
            (void)m_scriptHost->RestoreCheckpoint(previousPending, previousActions);
        }
        (void)m_ui.RestoreRuntimeState(previousUi);
    };
    if (m_scriptHost) {
        previousPending = m_scriptHost->CapturePending();
        previousActions = m_scriptHost->CapturePendingActions();
        const Status scriptStatus = m_scriptHost->RestoreCheckpoint(
            s.scriptPending, s.scriptActions);
        if (!scriptStatus) { rollbackPresentation(); return false; }
    }
    if (!m_session->CommitRestore(preparedRuntime.TakeValue())) {
        rollbackPresentation();
        return false;
    }
    m_nvlMode = s.nvlMode;
    m_nvlLines = s.nvlLines;
    m_lastBacklogSize = m_session->Backlog().Entries().size();
    m_autoMode = m_skipMode = false;
    (void)m_ui.RefreshHUD(DialogueUI());
    return true;
}

void PlayerApp::RollbackOneLine() {
    if (m_rollback.size() < 2) {
        return;  // back() is the line currently on screen
    }
    const RollbackEntry target = m_rollback[m_rollback.size() - 2];
    if (ApplyRollback(target)) m_rollback.pop_back();
}

bool PlayerApp::RollbackToBacklogIndex(std::size_t index) {
    for (std::size_t i = m_rollback.size(); i > 0; --i) {
        const RollbackEntry& entry = m_rollback[i - 1];
        if (entry.backlogSize == index + 1) {
            const RollbackEntry target = entry;
            if (!ApplyRollback(target)) return false;
            m_rollback.erase(m_rollback.begin() + static_cast<std::ptrdiff_t>(i),
                             m_rollback.end());
            return true;
        }
    }
    return false;  // line is older than the rollback window
}

std::vector<ui::GalgameItem> PlayerApp::GalleryItems() {
    std::vector<ui::GalgameItem> items;
    for (const auto& cg : m_catalog.Gallery()) {
        const bool unlocked = m_profile.CGUnlocked(cg.id);
        const std::string thumb = cg.thumbnail.empty() ? cg.image : cg.thumbnail;
        items.push_back({cg.id, unlocked ? cg.title : "？？？", unlocked ? "已解鎖" : "尚未解鎖",
                         unlocked ? thumb : "", !unlocked, "cg.view", cg.image});
    }
    return items;
}

std::vector<ui::GalgameItem> PlayerApp::SaveItems(bool saveMode) {
    std::vector<ui::GalgameItem> items;
    const auto slotItem = [&](int slot, const std::string& prefix, const std::string& action) {
        const progress::SlotInfo info = m_saves.Peek(slot);
        std::string label = "空白存檔";
        std::string subtitle;
        std::string image;
        if (info.exists) {
            label = info.chapter.empty() ? "未命名章節" : info.chapter;
            if (info.timestamp != 0) {
                const std::time_t t = static_cast<std::time_t>(info.timestamp);
                char buf[32];
                std::tm localTime{};
#ifdef _WIN32
                const bool converted = localtime_s(&localTime, &t) == 0;
#else
                const bool converted = localtime_r(&t, &localTime) != nullptr;
#endif
                if (converted && std::strftime(buf, sizeof(buf), "  %m/%d %H:%M", &localTime)) {
                    subtitle = buf;
                }
            }
            if (!info.thumbnailPng.empty()) {
                const std::string key = "mem://save/" + std::to_string(slot);
                if (m_runtime.Assets().RegisterMemoryTexture(key, info.thumbnailPng.data(),
                                                             info.thumbnailPng.size())) {
                    image = key;
                }
            }
        }
        items.push_back({"slot-" + std::to_string(slot), prefix + "  " + label, subtitle, image,
                         !saveMode && !info.exists, action, std::to_string(slot)});
    };

    if (!saveMode) {
        slotItem(kAutoSaveSlot, "AUTO", "load.slot");
    }
    for (int i = 0; i < 6; ++i) {
        slotItem(i, "#" + std::to_string(i + 1), saveMode ? "save.slot" : "load.slot");
    }
    return items;
}

std::vector<ui::GalgameItem> PlayerApp::BacklogItems() {
    std::vector<ui::GalgameItem> items;
    const auto& entries = m_session->Backlog().Entries();
    items.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const std::string speaker = entry.isChoice ? "▶ 選擇" : entry.speaker;
        items.push_back({"backlog-" + std::to_string(i), speaker.empty() ? entry.text : speaker + "　" + entry.text,
                         entry.voice.empty() ? "" : "♪ 點擊重播語音", "", false,
                         entry.voice.empty() ? "backlog.rollback" : "backlog.voice", std::to_string(i)});
    }
    return items;
}

ui::DialoguePresentation PlayerApp::DialogueUI() const {
    ui::DialoguePresentation view;
    view.speaker = m_session->Dialogue().State().speaker;
    view.text = m_session->Dialogue().State().displayText;
    view.chapterTitle = m_session->VM().Chapter();
    view.musicTitle = m_session->VM().CurrentBgm();
    view.effect = m_session->Dialogue().State().effect;
    view.effectProgress = m_session->Dialogue().State().effectProgress;
    view.choices = m_choiceTexts;
    view.nvlMode = m_nvlMode;
    view.autoMode = m_autoMode;
    view.skipMode = m_skipMode;
    view.textScale=m_settings.textScale;view.reducedMotion=m_settings.reducedMotion;
    for (const auto& line : m_nvlLines) view.nvlLines.push_back(line.speaker.empty() ? line.text : "【" + line.speaker + "】" + line.text);
    if (!view.nvlLines.empty() && m_session->Dialogue().State().fullText == m_nvlLines.back().text)
        view.nvlLines.back() = m_session->Dialogue().State().speaker.empty() ? m_session->Dialogue().State().displayText : "【" + m_session->Dialogue().State().speaker + "】" + m_session->Dialogue().State().displayText;
    return view;
}

ui::SettingsPresentation PlayerApp::SettingsUI() const {
    return {m_settings.bgmVolume, m_settings.seVolume, m_settings.voiceVolume,
            m_settings.textSpeedMs, m_settings.skipReadOnly, m_settings.fullscreen,
            m_settings.textScale,m_settings.highContrast,m_settings.reducedMotion,m_settings.selfVoicing,
            m_settings.language,m_supportedLocales};
}

void PlayerApp::OpenScreen(const std::string& route) {
    std::string target=route;if(target=="saveload")target="load";
    const Status routed=m_session->Routes().ShowModal(target);if(!routed){for(const auto& diagnostic:routed.Diagnostics())diag::Emit(diagnostic);return;}
    PresentRoute(target, "modal");
}

void PlayerApp::PresentRoute(const std::string& route, const std::string& operation) {
    std::string target=route;if(target=="saveload")target="load";
    if (operation == "back") {
        if (m_appState == AppState::Title) (void)m_ui.ShowTitle();
        else (void)m_ui.ShowHUD(DialogueUI());
        return;
    }
    if (target == "gallery") {
        auto items = GalleryItems();
        const auto unlocked = static_cast<std::size_t>(std::count_if(
            items.begin(), items.end(), [](const ui::GalgameItem& item) {
                return !item.disabled;
            }));
        const Status status = m_ui.ShowGallery(std::move(items));
        if (!status) {
            for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
            return;
        }
        PX_LOG_INFO("Player gallery presented total={} unlocked={}",
                    m_catalog.Gallery().size(), unlocked);
    }
    else if (target == "save") m_ui.ShowSaveLoad(true, SaveItems(true));
    else if (target == "load") m_ui.ShowSaveLoad(false, SaveItems(false));
    else if (target == "settings") m_ui.ShowSettings(SettingsUI());
    else if (target == "backlog") m_ui.ShowBacklog(BacklogItems());
    else if (target == "title") { m_appState = AppState::Title; (void)m_ui.ShowTitle(); }
    else if (target == "hud" || target == "game") { m_appState = AppState::Game; (void)m_ui.ShowHUD(DialogueUI()); }
}

void PlayerApp::HandleUIAction(const ui::GalgameAction& action) {
    const std::string& t = action.command;
    auto& audio = m_runtime.Audio();
    if (t == "load.slot") {
        LoadSlot(std::atoi(action.argument.c_str()));
    } else if (t == "save.slot") {
        SaveSlot(std::atoi(action.argument.c_str()), m_menuThumb);
        m_ui.ShowSaveLoad(true, SaveItems(true));
    } else if (t == "set.bgm.value") {
        m_settings.bgmVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.se.value") {
        m_settings.seVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.voice.value") {
        m_settings.voiceVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.speed.value") {
        m_settings.textSpeedMs=std::clamp(std::atoi(action.argument.c_str()),0,120);m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.skipread.value") {
        m_settings.skipReadOnly=action.argument=="true";
    } else if (t == "set.fullscreen.value") {
        m_settings.fullscreen=action.argument=="true";SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(),m_settings.fullscreen);
    } else if(t=="set.textscale.value") { m_settings.textScale=std::clamp(std::atoi(action.argument.c_str())/100.0f,.75f,2.0f);
    } else if(t=="set.highcontrast.value") { m_settings.highContrast=action.argument=="true";
    } else if(t=="set.reducedmotion.value") { m_settings.reducedMotion=action.argument=="true";
    } else if(t=="set.selfvoicing.value") { m_settings.selfVoicing=action.argument=="true";if(!m_settings.selfVoicing)m_speech.Stop();
    } else if(t=="set.language.value") { if(LoadLocale(action.argument,true))(void)m_settings.Save("Save/config.dat",&m_saveKey);
    } else if (t == "set.bgm.up") {
        m_settings.bgmVolume = std::min(128, m_settings.bgmVolume + 8);
        audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.bgm.down") {
        m_settings.bgmVolume = std::max(0, m_settings.bgmVolume - 8);
        audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.se.up") {
        m_settings.seVolume = std::min(128, m_settings.seVolume + 8);
        audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.se.down") {
        m_settings.seVolume = std::max(0, m_settings.seVolume - 8);
        audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.voice.up") {
        m_settings.voiceVolume = std::min(128, m_settings.voiceVolume + 8);
        audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.voice.down") {
        m_settings.voiceVolume = std::max(0, m_settings.voiceVolume - 8);
        audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.speed.up") {
        m_settings.textSpeedMs = std::min(120, m_settings.textSpeedMs + 4);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.speed.down") {
        m_settings.textSpeedMs = std::max(0, m_settings.textSpeedMs - 4);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "gallery.open") {
        OpenScreen("gallery");
    } else if (t == "load.open") {
        m_slotSaveMode = false;
        OpenScreen("load");
    } else if (t == "save.open") {
        m_slotSaveMode = true;
        m_pendingSaveScreen = true;
    } else if (t == "settings.open") {
        OpenScreen("settings");
    } else if (t == "cg.view") {
        m_viewingCG = action.argument;
    } else if (t == "set.skipread.toggle") {
        m_settings.skipReadOnly = !m_settings.skipReadOnly;
        m_ui.ShowSettings(SettingsUI());
    } else if (t == "set.fullscreen.toggle") {
        m_settings.fullscreen = !m_settings.fullscreen;
        SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), m_settings.fullscreen);
        m_ui.ShowSettings(SettingsUI());
    } else if (t == "game.start") {
        StartGame();
    } else if (t == "choice.select") {
        m_session->SelectChoice(std::atoi(action.argument.c_str()));
    } else if (t == "mode.auto") {
        m_autoMode = !m_autoMode; if (m_autoMode) m_skipMode = false;
    } else if (t == "mode.skip") {
        m_skipMode = !m_skipMode; if (m_skipMode) m_autoMode = false;
    } else if (t == "backlog.open") {
        OpenScreen("backlog");
    } else if (t == "backlog.voice" || t == "backlog.rollback") {
        const std::size_t index = static_cast<std::size_t>(std::max(0, std::atoi(action.argument.c_str())));
        if (index < m_session->Backlog().Entries().size()) {
            const auto& entry = m_session->Backlog().Entries()[index];
            if (t == "backlog.voice" && !entry.voice.empty())
                m_runtime.Audio().PlayVoice(entry.voice.find('/') != std::string::npos ? entry.voice : m_session->VM().Config().voiceDir + entry.voice);
            else RollbackToBacklogIndex(index);
        }
    } else if (t == "overlay.close") {
        (void)m_session->Routes().CloseModal();
        m_settings.Save("Save/config.dat", &m_saveKey); progress::SaveGlobalProfile(m_profile, "Save/profile.dat", &m_saveKey);
        if (m_appState == AppState::Title) m_ui.ShowTitle(); else m_ui.ShowHUD(DialogueUI());
        PX_LOG_INFO("Player overlay closed");
    } else if (t == "app.quit") {
        m_quitRequested = true;
    }
    if (t.ends_with(".up") || t.ends_with(".down")) m_ui.ShowSettings(SettingsUI());
}

void PlayerApp::ScreensFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0; m_runtime.Renderer().GetLogicalSize(w, h);
    (void)m_ui.Update(input, w, h,dt);
    if (m_appState == AppState::Game) m_session->Stage().Render();
    m_ui.Render(m_runtime.Renderer());
    CaptureE2EGalleryFrame();
}

void PlayerApp::ConfigureE2EJourney() {
    const char* journey=SDL_getenv("PRISMATIX_E2E_JOURNEY");
    if(!journey||std::string_view(journey)!="catalog")return;
    m_e2eStage=E2EStage::AwaitTitle;
    m_e2eStartedAt=SDL_GetTicks();
    PX_LOG_INFO("Player E2E journey armed name=catalog");
}

void PlayerApp::DriveE2EJourney() {
    if(m_e2eStage==E2EStage::Disabled||m_e2eStage==E2EStage::Complete)
        return;
    if(SDL_GetTicks()-m_e2eStartedAt>30'000){
        PX_LOG_ERROR("Player E2E journey timed out stage={}",
                     static_cast<int>(m_e2eStage));
        m_e2eFailed=true;
        m_e2eStage=E2EStage::Complete;
        m_quitRequested=true;
        return;
    }
    auto& input=m_runtime.GetInput();
    switch(m_e2eStage){
        case E2EStage::AwaitTitle:
            if(m_appState==AppState::Title){
                input.InjectAction(InputAction::FocusNext);
                input.InjectAction(InputAction::Accept);
                m_e2eStage=E2EStage::AwaitGameReady;
            }
            break;
        case E2EStage::AwaitGameReady:
            if(m_appState==AppState::Game&&m_profile.CGUnlocked("ending-rin")&&
               m_scriptHost&&!m_scriptHost->HasPendingCommand()&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Dialogue().Active()){
                if(!m_e2eLocaleSwitched&&m_supportedLocales.size()>1){
                    const std::string target=m_supportedLocales[1];
                    if(!LoadLocale(target,true)){
                        PX_LOG_ERROR("Player E2E locale switch failed locale={}",target);
                        m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                        m_quitRequested=true;break;
                    }
                    m_e2eLocaleSwitched=true;
                    PX_LOG_INFO("Player E2E locale switched locale={} text={}",
                                target,m_session->Dialogue().State().fullText);
                    break;
                }
                input.InjectKeyPress(SDL_SCANCODE_G);
                m_e2eStage=E2EStage::AwaitGallery;
            }
            break;
        case E2EStage::AwaitGallery:
            if(m_ui.IsOverlay()&&
               m_ui.CurrentScreen()==ui::GalgameUI::Screen::Gallery)
                m_e2eStage=E2EStage::CaptureGallery;
            break;
        case E2EStage::CloseGallery:
            input.InjectAction(InputAction::Cancel);
            input.InjectKeyPress(SDL_SCANCODE_ESCAPE);
            m_e2eStage=E2EStage::AwaitOverlayClosed;
            break;
        case E2EStage::AwaitOverlayClosed:
            if(!m_ui.IsOverlay()){
                PX_LOG_INFO("Player E2E journey complete name=catalog");
                m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;
            }
            break;
        default:break;
    }
}

void PlayerApp::CaptureE2EGalleryFrame() {
    if(m_e2eStage!=E2EStage::CaptureGallery)return;
    const auto capture=graphics::CaptureFrameSummary(
        m_runtime.Renderer().Handle());
    PX_LOG_INFO("Player E2E gallery frame width={} height={} colors={} hash={}",
                capture.width,capture.height,capture.sampledColors,capture.hash);
    if(!capture.Valid()||capture.width<640||capture.height<360||
       capture.sampledColors<8){
        PX_LOG_ERROR("Player E2E gallery frame is blank or undersized");
        m_e2eFailed=true;
        m_e2eStage=E2EStage::Complete;
        m_quitRequested=true;
        return;
    }
    m_e2eStage=E2EStage::CloseGallery;
}

void PlayerApp::TitleFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0; m_runtime.Renderer().GetLogicalSize(w, h);
    (void)m_ui.Update(input, w, h,dt);
    m_ui.Render(m_runtime.Renderer());
}

bool PlayerApp::VideoFrame(float dt) {
    if (m_session->VM().State() != vn::VMState::WaitingVideo) {
        return false;
    }
    px::Input& input = m_runtime.GetInput();

    if (!m_pendingVideo.empty()) {
        const float volume = static_cast<float>(m_settings.bgmVolume) / 128.0f;
        if (!m_video->Open(m_pendingVideo, volume)) {
            m_pendingVideo.clear();
            m_session->VM().NotifyVideoDone();
            return false;
        }
        m_pendingVideo.clear();
        // Swallow this frame's input: the click that advanced the script into
        // [video] must not instantly skip the movie.
        int w = 0, h = 0;
        m_runtime.Renderer().GetLogicalSize(w, h);
        m_runtime.Renderer().DrawRect(
            Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) }, Color{ 0, 0, 0, 255 });
        return true;
    }
    if (!m_video->Playing()) {
        m_session->VM().NotifyVideoDone();  // safety: never leave the VM stuck
        return false;
    }

    m_video->Update(dt);
    const bool skipRequested =
        m_videoSkippable &&
        (input.LeftClick() || input.KeyPressed(SDL_SCANCODE_RETURN) ||
         input.KeyPressed(SDL_SCANCODE_ESCAPE) || input.KeyPressed(SDL_SCANCODE_SPACE));
    if (skipRequested || m_video->Finished()) {
        m_video->Close();
        m_session->VM().NotifyVideoDone();
        return false;
    }

    int w = 0, h = 0;
    m_runtime.Renderer().GetLogicalSize(w, h);
    m_runtime.Renderer().DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
                                  Color{ 0, 0, 0, 255 });
    m_video->Render(w, h);
    if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) m_ui.ShowVideoOverlay(m_videoSkippable);
    (void)m_ui.Update(input, w, h,dt);
    m_ui.Render(m_runtime.Renderer());
    return true;
}

void PlayerApp::GameFrame(float dt, std::uint64_t now) {
    px::Input& input = m_runtime.GetInput();

    if (input.KeyPressed(SDL_SCANCODE_F9)) LoadSlot(0);
    if (input.KeyPressed(SDL_SCANCODE_F2)) {
        m_profile.RegisterClear("demo");
        m_session->Variables().Reset(true);
        progress::SaveGlobalProfile(m_profile, "Save/profile.dat", &m_saveKey);
        PX_LOG_INFO("NG+ clear count {}", m_profile.ClearCount());
    }
    if (input.KeyPressed(SDL_SCANCODE_F5)) m_saveRequested = true;
    if (input.KeyPressed(SDL_SCANCODE_F6)) m_pendingSaveScreen = true;

    // Mode toggles (KAG conventions: A=auto, S=skip toggle, Ctrl=skip held).
    if (input.KeyPressed(SDL_SCANCODE_A)) {
        m_autoMode = !m_autoMode;
        if (m_autoMode) m_skipMode = false;
    }
    if (input.KeyPressed(SDL_SCANCODE_S)) {
        m_skipMode = !m_skipMode;
        if (m_skipMode) m_autoMode = false;
    }
    if (input.KeyPressed(SDL_SCANCODE_B) || input.WheelY() > 0.0f) {
        OpenScreen("backlog");
        return;
    }
    if (input.KeyPressed(SDL_SCANCODE_PAGEUP)) {
        RollbackOneLine();
    }
    if (input.RightClick()) {
        m_hudHidden = !m_hudHidden;
    }

    m_choiceTexts.clear();
    if (m_session->VM().State() == vn::VMState::WaitingChoice) {
        for (const auto& c : m_session->VM().Choices()) m_choiceTexts.push_back(c.text);
        m_skipMode = false;  // Skip always stops at choices.
    }

    // Enter/Space advance like a click (KAG convention). Alt+Enter stays the
    // fullscreen toggle handled in MainLoop.
    const bool advanceKey =
        (input.KeyPressed(SDL_SCANCODE_RETURN) && !input.KeyDown(SDL_SCANCODE_LALT) &&
         !input.KeyDown(SDL_SCANCODE_RALT)) ||
        input.KeyPressed(SDL_SCANCODE_SPACE);

    bool advancedByClick = false;
    if (m_hudHidden) {
        if (input.LeftClick() || advanceKey) m_hudHidden = false;
    } else {
        m_ui.RefreshHUD(DialogueUI());
        int uiW = 0, uiH = 0; m_runtime.Renderer().GetLogicalSize(uiW, uiH);
        const bool uiConsumed = m_ui.Update(input, uiW, uiH,dt);
        if (m_ui.IsOverlay()) { m_session->Stage().Render(); m_ui.Render(m_runtime.Renderer()); return; }
        if (!uiConsumed && (input.LeftClick() || advanceKey) &&
               m_session->VM().State() != vn::VMState::WaitingChoice) {
            m_session->Advance();
            advancedByClick = true;
            m_autoMode = false;
            m_skipMode = false;
        }
    }

    // Skip-read-only (既讀スキップ): S-toggle skip stops at unread text;
    // holding Ctrl always force-skips.
    const bool ctrlSkip = input.KeyDown(SDL_SCANCODE_LCTRL) || input.KeyDown(SDL_SCANCODE_RCTRL);
    if (m_skipMode && m_settings.skipReadOnly && !m_session->VM().CurrentLineSeen()) {
        m_skipMode = false;
    }
    const bool skipping = m_skipMode || ctrlSkip;
    if (skipping && !advancedByClick &&
        (m_session->VM().State() == vn::VMState::WaitingClick ||
         m_session->VM().State() == vn::VMState::WaitingTimer)) {
        if (!m_session->Dialogue().Finished()) m_session->Dialogue().ShowAll();
        m_session->Advance();
        m_autoTimerStart = 0;
    } else if (m_autoMode && m_session->VM().State() == vn::VMState::WaitingClick &&
               m_session->Dialogue().Finished()) {
        if (m_autoTimerStart == 0) {
            m_autoTimerStart = now;
        } else if (now - m_autoTimerStart >=
                   static_cast<std::uint64_t>(std::max(0, m_settings.autoWaitMs))) {
            m_session->Advance();
            m_autoTimerStart = 0;
        }
    } else {
        m_autoTimerStart = 0;
    }

    m_session->Update(now, dt);
    if(m_scriptHost){m_scriptHost->Emit("frame.update",{{"delta",std::to_string(dt)}});const bool hadPending=m_scriptHost->HasPendingCommand()||m_scriptHost->HasPendingAction();m_scriptHost->Update(dt);if(hadPending&&!m_scriptHost->HasPendingCommand()&&!m_scriptHost->HasPendingAction())m_session->VM().NotifyExternalDone();}

    // New dialogue lines feed the NVL page and the rollback ring.
    const std::size_t backlogSize = m_session->Backlog().Entries().size();
    if (backlogSize > m_lastBacklogSize) {
        for (std::size_t i = m_lastBacklogSize; i < backlogSize; ++i) {
            const vn::BacklogEntry& e = m_session->Backlog().Entries()[i];
            if (m_nvlMode && !e.isChoice) m_nvlLines.push_back(e);
            if (m_settings.selfVoicing && !e.isChoice)
                m_speech.Speak(e.speaker.empty() ? e.text : e.speaker + ". " + e.text);
        }
        RollbackEntry entry;
        entry.snap = MakeSnapshot(/*includeBacklog=*/false);
        entry.backlogSize = backlogSize;
        m_rollback.push_back(std::move(entry));
        if (m_rollback.size() > 64) m_rollback.pop_front();
        m_lastBacklogSize = backlogSize;
    } else if (backlogSize < m_lastBacklogSize) {
        m_lastBacklogSize = backlogSize;
    }

    m_session->Stage().Render();

    // Autosave once per choice prompt; the stage is rendered, so the thumbnail
    // shows the scene the player was deciding on.
    if (m_session->VM().State() == vn::VMState::WaitingChoice) {
        if (!m_autoSavedChoice) {
            SaveSlot(kAutoSaveSlot,
                     graphics::CaptureThumbnailPng(m_runtime.Renderer().Handle(), 256, 144));
            m_autoSavedChoice = true;
        }
    } else {
        m_autoSavedChoice = false;
    }

    if (m_pendingSaveScreen) {
        m_menuThumb = graphics::CaptureThumbnailPng(m_runtime.Renderer().Handle(), 256, 144);
        m_slotSaveMode = true;
        OpenScreen("save");
        m_pendingSaveScreen = false;
    }

    if (!m_hudHidden) {
        m_ui.RefreshHUD(DialogueUI());
        m_ui.Render(m_runtime.Renderer());
    }

    if (m_saveRequested) {
        SaveSlot(0, graphics::CaptureThumbnailPng(m_runtime.Renderer().Handle(), 256, 144));
        m_saveRequested = false;
    }
}

void PlayerApp::MainLoop() {
    while (!m_quitRequested && m_runtime.BeginFrame()) {
        px::Input& input = m_runtime.GetInput();
        const float dt = m_runtime.GetClock().DeltaSeconds();
        const std::uint64_t now = m_runtime.GetClock().NowMs();
        DriveE2EJourney();

        if (input.KeyPressed(SDL_SCANCODE_RETURN) &&
            (input.KeyDown(SDL_SCANCODE_LALT) || input.KeyDown(SDL_SCANCODE_RALT))) {
            m_settings.fullscreen = !m_settings.fullscreen;
            SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), m_settings.fullscreen);
        }

        if(m_appState==AppState::BootSplash){
            SplashFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        // Fullscreen CG viewer sits above everything; any click closes it.
        if (!m_viewingCG.empty()) {
            if (input.LeftClick() || input.RightClick() ||
                input.KeyPressed(SDL_SCANCODE_ESCAPE)) {
                m_viewingCG.clear();
            } else {
                int w = 0, h = 0;
                m_runtime.Renderer().GetLogicalSize(w, h);
                m_runtime.Renderer().DrawRect(
                    Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
                    Color{ 0, 0, 0, 255 });
                m_runtime.Renderer().DrawImageAuto(m_viewingCG, graphics::DisplayMode::Fit, 255);
                m_runtime.EndFrame();
                continue;
            }
        }

        if (m_appState == AppState::Game && !m_ui.IsOverlay()) {
            for (const auto& [scancode, route] : m_screenTriggers)
                if (input.KeyPressed(scancode)) OpenScreen(route);
        }

        if (m_ui.IsOverlay() && m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) {
            if (input.RightClick() || input.KeyPressed(SDL_SCANCODE_ESCAPE)) {
                HandleUIAction({"overlay.close", {}});
            }
            ScreensFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        if (m_appState == AppState::Title) {
            TitleFrame(dt);
            // A title action may replace the UI root. End the frame here so
            // the click that activated Start cannot also advance the VN HUD.
            m_runtime.EndFrame();
            continue;
        }

        if (VideoFrame(dt)) {
            m_runtime.EndFrame();
            continue;
        }

        GameFrame(dt, now);
        m_runtime.EndFrame();
    }
}

void PlayerApp::Shutdown() {
    if (m_scriptHost) m_scriptHost->Emit("engine.shutdown");
    if (!m_settings.fullscreen) {
        int w = 0, h = 0;
        SDL_GetWindowSize(m_runtime.GetWindow().Handle(), &w, &h);
        if (w >= 320 && h >= 180) {
            m_settings.windowWidth = w;
            m_settings.windowHeight = h;
        }
    }
    m_settings.Save("Save/config.dat", &m_saveKey);
    progress::SaveGlobalProfile(m_profile, "Save/profile.dat", &m_saveKey);
    // Subsystems referencing the runtime must go before Runtime::Shutdown.
    m_scriptHost.reset();
    m_splash.reset();
    m_session.reset();
    m_runtime.Shutdown();
}

int PlayerApp::Run(int argc, char* argv[]) {
    if (!Init(argc, argv)) {
        return 1;
    }
    MainLoop();
    Shutdown();
    return m_e2eFailed?2:0;
}

}
