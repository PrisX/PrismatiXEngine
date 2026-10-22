#include "Applications/Player/PlayerApp.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
EM_ASYNC_JS(void, WaitWebFrame, (), {
    await new Promise(resolve => {
        let frame;
        const finish = () => {
            if (frame) cancelAnimationFrame(frame);
            document.removeEventListener("visibilitychange", finish);
            resolve();
        };
        document.addEventListener("visibilitychange", finish, {once: true});
        if (document.hidden) setTimeout(finish, 16);
        else frame = requestAnimationFrame(finish);
    });
});
#if defined(PRISMATIX_WEB_PLAYER)
EM_JS(int, TakeWebPersistenceRequest, (), {
    if (!Module.prismatix.flushRequested) return 0;
    Module.prismatix.flushActive = Module.prismatix.flushRequested;
    Module.prismatix.flushRequested = null;
    return 1;
});
EM_JS(void, FinishWebPersistenceRequest, (int saved), {
    const request = Module.prismatix.flushActive;
    Module.prismatix.flushActive = null;
    request?.resolve(!!saved);
});
#endif
#endif
#include "Applications/Player/PlayerControls.h"
#include "Applications/Player/PlayerSupport.h"
#include "Applications/Player/QaHooks.h"
#include "Applications/Player/QaIpc.h"

#include "Engine/Platform/PlatformInfo.h"
#include "Engine/Graphics/Screenshot.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Diagnostics/FrameProfiler.h"
#include "Engine/IO/PackageVerifier.h"
#include "Engine/Progression/AwaitedWork.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/UserDataLocation.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Script/ScriptFrameScheduler.h"
#include "Engine/Script/VnModelSnapshots.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/Support/Logger.h"
#include "Engine/Support/Utf8Path.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Startup/SplashTypes.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace px::player {

PlayerApp::~PlayerApp() = default;

namespace {
// A window of the requested size, shrunk (keeping its shape) to fit the
// monitor's usable area with room for the title bar, and centred. A window
// as large as the monitor would read as borderless fullscreen.
bool ShowWindowed(SDL_Window* window, int width, int height) {
    width = std::max(320, width);
    height = std::max(180, height);
    SDL_Rect usable{};
    if (const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        display != 0 && SDL_GetDisplayUsableBounds(display, &usable) &&
        usable.w > 0 && usable.h > 0) {
        const double scale = std::min({1.0, usable.w * 0.9 / width, usable.h * 0.9 / height});
        width = static_cast<int>(width * scale);
        height = static_cast<int>(height * scale);
    }
    SDL_RestoreWindow(window);
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return SDL_SyncWindow(window);
}

const std::string kSaveKey = "prismatix-demo-secret";
// One line of the NVL page: the speaker in brackets, then the text.
std::string NvlLineText(const std::string& speaker, const std::string& text) {
    return speaker.empty() ? text : "【" + speaker + "】" + text;
}

bool ProfileFlagEnabled(const Variant* value) {
    if (!value) return false;
    if (const auto* integer = value->TryGet<std::int64_t>()) return *integer == 1;
    if (const auto* boolean = value->TryGet<bool>()) return *boolean;
    return false;
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

std::string ProductText(
    const std::unordered_map<std::string, std::string>& translations,
    const std::string_view id, const std::string_view fallback) {
    const auto found = translations.find(std::string(id));
    return found == translations.end() ? std::string(fallback)
                                       : found->second;
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

    const auto replacePath = [](std::string& value,
                                const std::string_view before,
                                const std::string_view after) {
        if (!before.empty() && value == before) value = after;
    };
    for (std::size_t index = 0; index < state.runtimeProgram->code.size();
         ++index) {
        const auto& before = state.runtimeProgram->code[index];
        const auto& after = candidate->code[index];
        if (before.type == "bg") {
            const std::string oldPath = before.Get("file");
            const std::string newPath = after.Get("file", oldPath);
            replacePath(state.stage.background, oldPath, newPath);
            replacePath(state.stage.previousBackground, oldPath, newPath);
            replacePath(state.stage.ruleOldBackground, oldPath, newPath);
            replacePath(state.stage.ruleNewBackground, oldPath, newPath);
        } else if (before.type == "char") {
            const std::string oldPath = before.Get("file");
            const std::string newPath = after.Get("file", oldPath);
            for (auto& actor : state.stage.actors) {
                replacePath(actor.imagePath, oldPath, newPath);
                replacePath(actor.previousImagePath, oldPath, newPath);
            }
            for (auto& layer : state.stage.layers)
                replacePath(layer.imagePath, oldPath, newPath);
        } else if (before.type == "bgm") {
            const std::string oldPath = before.Get("file");
            const std::string newPath = after.Get("file", oldPath);
            replacePath(state.audio.music.path, oldPath, newPath);
            replacePath(state.audio.pendingMusicLoop, oldPath, newPath);
        } else if (before.type == "se") {
            const std::string oldPath = before.Get("file");
            const std::string newPath = after.Get("file", oldPath);
            for (auto& channel : state.audio.seChannels)
                replacePath(channel.track.path, oldPath, newPath);
        } else if (before.type == "voice") {
            const std::string oldPath = before.Get("file");
            const std::string newPath = after.Get("file", oldPath);
            replacePath(state.audio.voice.path, oldPath, newPath);
            for (auto& entry : state.backlog)
                replacePath(entry.voice, oldPath, newPath);
        }
    }

    for (auto& choice : state.vm.choices) {
        const vn::Command* command = FindOperation(*candidate,
                                                   choice.operationId);
        if (!command || command->type != "choice") return false;
        choice.text = LocalizedCommandText(*command, "text", translations,
                                           variables);
    }
    for (auto& entry : state.backlog) {
        if (entry.operationId.empty()) continue;  // pre-release development save
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

// Package files come from APK assets on Android, so read through the package
// FileSource rather than std::ifstream.
std::optional<std::string> ReadBoundedTextFile(const std::string& path,
                                               const std::uintmax_t limit) {
    auto text = io::ReadTextFile(*io::PackageFileSource(), path, limit);
    if (!text || text->empty()) return std::nullopt;
    return text;
}
}

PlayerApp::Boot PlayerApp::LoadBootConfig(const bool allowOverlay,
                                         const bool allowOptionalPack) {
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
    auto effectivePackage = parsed.manifest;
    std::string savePackageFingerprint = parsed.manifest.packageFingerprint;
    if (allowOverlay) {
        std::string enabledOptionalId;
        const auto userData = progress::ResolvePackagedUserDataDirectory(
            parsed.manifest.gameId);
        if (userData && allowOptionalPack) {
            const auto selected = progress::ReadEnabledOptionalPackId(
                userData.Value());
            if (selected) enabledOptionalId = selected.Value();
            else
                for (auto diagnostic : selected.Diagnostics()) {
                    diagnostic.severity = diag::Severity::Warning;
                    diag::Emit(std::move(diagnostic));
                }
        }
        const auto chain = io::ResolveSignedPackageOverlayChain(
            std::filesystem::current_path(),
            std::filesystem::path("Package/updates"), enabledOptionalId);
        if (!chain.valid) {
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Warning,
                .code = "PXPLAYER5014",
                .category = "Player.Boot",
                .message = "Single-file update was rejected; starting the base package",
                .details = chain.error});
        } else {
            if (!chain.mounts.empty()) {
                const auto target = sdk::detail::ParsePackageManifest(
                    chain.targetManifestJson);
                if (target.Valid()) {
                    effectivePackage = target.manifest;
                    savePackageFingerprint = chain.mainPackageFingerprint;
                    boot.compatibleSaveFingerprints =
                        chain.compatibleSaveFingerprints;
                    boot.compatibleSaveFingerprints.insert(
                        boot.compatibleSaveFingerprints.end(),
                        chain.mainCompatibleSaveFingerprints.begin(),
                        chain.mainCompatibleSaveFingerprints.end());
                    boot.legacyPersistenceSecrets.insert(
                        boot.legacyPersistenceSecrets.end(),
                        chain.mainLegacyPersistenceSecrets.begin(),
                        chain.mainLegacyPersistenceSecrets.end());
                    for (const auto& mount : chain.mounts)
                        boot.config.overlays.push_back(
                            {PathToUtf8(mount.path), mount.tombstones});
                    boot.overlayApplied = true;
                    if (!chain.optionalPackId.empty())
                        PX_LOG_INFO("Player optional pack enabled id={}",
                                    chain.optionalPackId);
                    boot.optionalPackApplied = !chain.optionalPackId.empty();
                }
            }
            if (!chain.ignoredFiles.empty()) {
                std::string ignored;
                for (const auto& name : chain.ignoredFiles) {
                    if (!ignored.empty()) ignored += ", ";
                    ignored += name;
                }
                diag::Emit(diag::Diagnostic{
                    .severity = diag::Severity::Warning,
                    .code = "PXPLAYER5014",
                    .category = "Player.Boot",
                    .message = "Single-file update was rejected or did not apply",
                    .details = ignored});
            }
        }
    }
    const auto& package = effectivePackage;
    boot.packaged = true;
    boot.config.title = package.title;
    boot.gameId = package.gameId;
    boot.packageFingerprint = package.packageFingerprint;
    if (!savePackageFingerprint.empty())
        boot.packageFingerprint = savePackageFingerprint;
    boot.compatibleSaveFingerprints.insert(
        boot.compatibleSaveFingerprints.end(),
        parsed.manifest.compatibleSaveFingerprints.begin(),
        parsed.manifest.compatibleSaveFingerprints.end());
    std::ranges::sort(boot.compatibleSaveFingerprints);
    boot.compatibleSaveFingerprints.erase(
        std::unique(boot.compatibleSaveFingerprints.begin(),
                    boot.compatibleSaveFingerprints.end()),
        boot.compatibleSaveFingerprints.end());
    std::erase(boot.compatibleSaveFingerprints, boot.packageFingerprint);
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
        descriptor.schemaRevision = effect.schemaRevision;
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
    for (const auto& archive : parsed.manifest.archives)
        boot.config.mountArchives.push_back(archive.file);
    boot.config.archiveKey = parsed.manifest.archiveKey;
    boot.config.width = boot.config.logicalWidth = package.width;
    boot.config.height = boot.config.logicalHeight = package.height;
    boot.startScript = package.startRuntimeIr;
    boot.sourceMap = package.sourceMap;
    boot.startRoute = package.startRoute;
    for (const auto& route : package.routes)
        boot.routeScenes.emplace(route.id, route.scene);
    if (package.persistenceKeyId.empty()) {
        boot.saveSecret = parsed.manifest.archiveKey.empty()
                              ? parsed.manifest.packageFingerprint
                              : parsed.manifest.archiveKey;
    } else {
        boot.persistenceDataId = package.persistenceDataId;
        boot.persistenceKeyId = package.persistenceKeyId;
        for (const auto& reader : parsed.manifest.legacyPersistenceReaders)
            boot.legacyPersistenceSecrets.push_back(reader.secret);
        std::ranges::sort(boot.legacyPersistenceSecrets);
        boot.legacyPersistenceSecrets.erase(
            std::unique(boot.legacyPersistenceSecrets.begin(),
                        boot.legacyPersistenceSecrets.end()),
            boot.legacyPersistenceSecrets.end());
    }
    boot.valid = true;
    PX_LOG_INFO("Packaged build: mounting {} content group(s), signed overlay={} count={}",
                boot.config.mountArchives.size(), boot.overlayApplied,
                boot.config.overlays.size());
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
        document.value("schemaRevision", 0) != 1 ||
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
    // Do not mutate deferred source-map state while an earlier locale is still
    // compiling on the preload worker. Keep a matching ready future so New
    // Game can consume it; discard a stale locale before starting its
    // replacement below.
    if (m_runtimePreload.valid()) {
        m_runtimePreload.wait();
        if (m_runtimePreloadPath != runtimePath) {
            try {
                (void)m_runtimePreload.get();
            } catch (const std::exception& error) {
                PX_LOG_WARN("Discarding failed Runtime IR preload: {}",
                            error.what());
            }
            m_runtimePreloadPath.clear();
        }
    }
    // Packaging validates the Runtime IR/source-map pair exhaustively. The
    // title and splash presentation do not execute Story code, so compile it
    // in the background before New Game (or synchronously when a running game
    // changes language and must preserve its exact state).
    if (!m_runtime.VFS().Exists(runtimePath) ||
        !m_runtime.VFS().Exists(sourceMapPath)) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXPLAYER5016",
            .category = "Player.Localization",
            .message = "Locale-specific RuntimeIR or source map is missing",
            .details = runtimePath});
        return false;
    }

    const bool replacingRunningProgram =
        m_appState == AppState::Game && m_session->RuntimeProgramIdentity();
    std::shared_ptr<const vn::Program> runtimeProgram;
    if (replacingRunningProgram) {
        // Clear any previously materialized locale map before compiling the
        // replacement program so diagnostics never resolve through stale
        // metadata.
        m_session->DeferSourceMap(sourceMapPath);
        runtimeProgram = m_session->PrepareRuntimeIr(runtimePath);
        if (!runtimeProgram) return false;
    }

    std::optional<RuntimeSession::PreparedRestore> preparedRuntime;
    std::vector<vn::BacklogEntry> localizedNvl;
    std::deque<RollbackEntry> localizedRollback = m_rollback;
    if (replacingRunningProgram) {
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
    m_session->DeferSourceMap(
        sourceMapPath,
        runtimeProgram ? runtimeProgram->documentId : std::string{});

    m_langTable = std::move(candidate);
    PublishLocaleStrings();
    m_settings.language = std::move(locale);
    m_session->VM().SetCharacterLocale(m_settings.language);
    m_script = runtimePath;
    if (!replacingRunningProgram && m_appState == AppState::Title &&
        m_runtimePreloadPath != m_script)
        BeginRuntimeIrPreload();
    if (!localizedNvl.empty() || m_nvlLines.empty())
        m_nvlLines = std::move(localizedNvl);
    m_rollback = std::move(localizedRollback);
    // Controls without a font use the locale's first font unless the project
    // chose one (player.uiFont), so a game need not ship the stock UI font.
    if (!fontChain.empty() && !m_uiFontAuthored) (void)m_ui.SetDefaultFont(fontChain.front());
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
            (void)m_ui.ShowBacklog(BacklogEntries(), BacklogItems());
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
        case ui::GalgameUI::Screen::Custom:
            break;
    }
    return true;
}

void PlayerApp::RecordScriptVideoTerminal(const std::uint64_t handle,
                                          std::string status,
                                          std::string error) {
    if (handle == 0) return;
    constexpr std::size_t kRetainedTerminalVideoHandles = 256;
    m_scriptVideoStates[handle] = std::move(status);
    if (error.empty()) m_scriptVideoErrors.erase(handle);
    else m_scriptVideoErrors[handle] = std::move(error);
    m_scriptVideoTerminalOrder.push_back(handle);
    while (m_scriptVideoTerminalOrder.size() > kRetainedTerminalVideoHandles) {
        const std::uint64_t expired = m_scriptVideoTerminalOrder.front();
        m_scriptVideoTerminalOrder.pop_front();
        m_scriptVideoStates.erase(expired);
        m_scriptVideoErrors.erase(expired);
    }
}

const std::string& PlayerApp::TitleBgmPath() const {
    static const std::string none;
    const auto* path = progress::FirstMatchingChoice(m_titleBgmRules, ProfileFlags());
    return path ? *path : none;
}

progress::ProfileFlagReader PlayerApp::ProfileFlags() const {
    return [this](const std::string& name) { return m_profile.Variable(name); };
}

// player.profileUiData: title-screen states and the like, recomputed from
// the global profile whenever the title is shown.
void PlayerApp::RefreshTitleProgressUi() {
    const auto flags = ProfileFlags();
    for (const auto& datum : m_profileUiData) {
        const std::int64_t value = datum.Evaluate(flags);
        if (datum.condition) {
            (void)m_ui.UpdateUiData(datum.key, Variant(value != 0));
            continue;
        }
        (void)m_ui.UpdateUiData(datum.key, Variant(value));
        for (const std::int64_t candidate : datum.Values())
            (void)m_ui.UpdateUiData(datum.key + ".is" + std::to_string(candidate),
                                    Variant(candidate == value));
    }
}

// Locale strings are UI data too: a TSX `textKey="save.error"` binds its text
// to strings.save.error, and switching language updates every bound label.
void PlayerApp::PublishLocaleStrings() {
    for (const auto& [key, text] : m_langTable) {
        const std::string path = "strings." + key;
        if (m_uiStringPaths.contains(path)) (void)m_ui.UpdateUiData(path, Variant(text));
        else if (m_ui.DefineUiData(path, Variant(text))) m_uiStringPaths.insert(path);
    }
    for (const auto& path : m_uiStringPaths)
        if (!m_langTable.contains(path.substr(std::string_view("strings.").size())))
            (void)m_ui.UpdateUiData(path, Variant(std::string{}));
}

bool PlayerApp::Init(int argc, char* argv[]) {
#if PRISMATIX_QA_HOOKS
    PX_LOG_WARN("PrismatiXPlayerQA serves PRISMATIX_QA_IPC test requests; "
                "package games with PrismatiXPlayer");
#endif
    m_boot = LoadBootConfig();
    if (!m_boot.valid) return false;
    if (m_boot.packaged) {
        const auto userData = progress::ResolvePackagedUserDataDirectory(
            m_boot.gameId);
        if (!userData) {
            for (const auto& diagnostic : userData.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
        const auto imported = progress::ImportLegacyPortableSaves(
            std::filesystem::current_path(), userData.Value());
        if (!imported) {
            for (const auto& diagnostic : imported.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
        m_saveDirectory = userData.Value() / "Save";
        PX_LOG_INFO("Player user data directory={} legacySavesImported={}",
                    PathToUtf8(userData.Value()), imported.Value());
    }
    bool runtimeReady = m_runtime.Init(m_boot.config);
    if (!runtimeReady && m_boot.optionalPackApplied) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Warning,
            .code = "PXPLAYER5015",
            .category = "Player.Boot",
            .message = "Optional pack could not start; retrying signed updates"});
        m_boot = LoadBootConfig(true, false);
        runtimeReady = m_boot.valid && m_runtime.Init(m_boot.config);
    }
    if (!runtimeReady && m_boot.overlayApplied) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Warning,
            .code = "PXPLAYER5015",
            .category = "Player.Boot",
            .message = "Signed update could not start; retrying the base package"});
        m_boot = LoadBootConfig(false);
        runtimeReady = m_boot.valid && m_runtime.Init(m_boot.config);
    }
    if (!runtimeReady) {
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
    // Android/iOS may kill a backgrounded app without warning: the Runtime
    // calls this before pausing media. It reuses the autosave slot and the
    // normal settings/profile writers, and waits for them to finish.
    m_runtime.SetLifecyclePersistHandler([this](platform::PersistReason) {
        if (m_appState == AppState::Game && m_session &&
            !m_gameStartTransitionActive)
            SaveSlotInBackground(kAutoSaveSlot);
        HarvestBackgroundSave(true);
        if (m_saveDirectory.empty()) return;
        (void)SaveSettingsIfWritable();
        (void)SaveProfileIfWritable();
    });

    // Current packages use stable game-data identity. Manifests produced before
    // that contract remain self-consistent and use their historical key, while
    // upgraded packages carry those old secrets as read-only fallback keys.
    if (m_boot.persistenceKeyId == progress::kGameDataPersistenceKeyId) {
        m_saveKey = progress::DeriveGameDataPersistenceKey(
            m_boot.persistenceDataId);
        for (const auto& secret : m_boot.legacyPersistenceSecrets)
            m_saveFallbackKeys.push_back(
                progress::DeriveLegacyPersistenceKey(secret));
    } else {
        m_saveKey = progress::DeriveLegacyPersistenceKey(
            m_boot.saveSecret.empty() ? std::string(kSaveKey)
                                      : m_boot.saveSecret);
    }
    const auto profileLoad = progress::LoadGlobalProfileWithFallback(
        m_profile, PathToUtf8(m_saveDirectory / "profile.dat"),
        &m_saveKey, m_saveFallbackKeys);
    const auto settingsLoad = m_settings.LoadWithInputBindings(
        PathToUtf8(m_saveDirectory / "config.dat"),
        m_runtime.GetInput(), &m_saveKey, m_saveFallbackKeys);
    m_profileWritable = profileLoad != progress::PersistLoadStatus::Failed;
    m_settingsWritable = settingsLoad != progress::PersistLoadStatus::Failed;
    m_settingsMissing = settingsLoad == progress::PersistLoadStatus::Missing;
    m_saves.Configure(PathToUtf8(m_saveDirectory), &m_saveKey,
                      m_saveFallbackKeys);
    PX_LOG_DEBUG("Player boot: persistence configured");
    m_runtime.Audio().SetMainVolume(m_settings.masterMuted ? 0 : m_settings.masterVolume);
    m_runtime.Audio().SetBGMVolume(m_settings.bgmMuted ? 0 : m_settings.bgmVolume);
    m_runtime.Audio().SetSEVolume(m_settings.seMuted ? 0 : m_settings.seVolume);
    m_runtime.Audio().SetVoiceVolume(m_settings.voiceMuted ? 0 : m_settings.voiceVolume);
    m_runtime.Audio().SetVoiceDuckingEnabled(m_settings.duckBgmOnVoice);
    for (const auto& [profile, volume] : m_settings.voiceProfileVolumes)
        m_runtime.Audio().SetVoiceProfileVolume(profile,
            m_settings.voiceProfileMuted[profile] ? 0 : volume);
    for (const auto& [profile, muted] : m_settings.voiceProfileMuted)
        if (muted && !m_settings.voiceProfileVolumes.contains(profile))
            m_runtime.Audio().SetVoiceProfileVolume(profile, 0);
    // A phone or tablet has no windowed mode; a desktop settings file or a
    // stale toggle must not leave the game in a window there.
    if (CurrentPlatform().mobile) m_settings.fullscreen = true;
    if (m_settings.fullscreen) {
        if (!SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), true) ||
            !SDL_SyncWindow(m_runtime.GetWindow().Handle()))
            PX_LOG_WARN("Player startup fullscreen request failed: {}", SDL_GetError());
    } else if (m_settings.windowWidth >= 320 && m_settings.windowHeight >= 180) {
        if (!ShowWindowed(m_runtime.GetWindow().Handle(), m_settings.windowWidth,
                          m_settings.windowHeight))
            PX_LOG_WARN("Player startup windowed size request failed: {}", SDL_GetError());
    }

    m_session = std::make_unique<RuntimeSession>(RuntimeSession::Services{
        m_runtime.VFS(), m_runtime.Audio(), m_runtime.Renderer(), m_runtime.Assets()});
    m_ui.SetTextRenderer(&m_runtime.Renderer());
    m_session->Variables().SetProfileWriteHandler(
        [this](const std::string_view name, const vn::Value& value) {
            m_profile.SetVariable(std::string(name), value.Clone());
            if (const Status updated = m_ui.UpdateUiData(
                    "profileFlags." + std::string(name),
                    Variant(ProfileFlagEnabled(&value))); !updated)
                for (const auto& diagnostic : updated.Diagnostics())
                    diag::Emit(diagnostic);
            RefreshTitleProgressUi();
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
    m_ui.SetMotionAudioSink([this](const sdk::MotionAudioMarker& marker){
        PlayMotionMarker(marker);
    });
    m_ui.SetInputBindingService(&m_runtime.GetInput(), [this] {
        (void)SaveSettingsIfWritable();
    });
    m_ui.SetControlRuntimeConfigurator([this](ui::Control& control){
        if(auto* rectangle=dynamic_cast<ui::VideoRect*>(&control)){
            auto player=std::make_shared<video::VideoPlayer>(m_runtime.Renderer().Handle(),m_runtime.VFS());
            rectangle->SetPlayback({[player](const std::string_view path){return player->Open(std::string(path));},[player]{player->Close();},[player](const float delta){player->Update(delta);},[player]{return player->Playing();},[player]{return player->Texture();},[player]{return Vec2{static_cast<float>(player->Width()),static_cast<float>(player->Height())};}});
        }
        // Any control may sound as it appears or starts to hide. The handler
        // lives on the control, so it goes with it; a tree configured again
        // keeps the one it has.
        if(!control.ShowSound().Empty()&&!control.HasSignalHandlers("showSound"))
            (void)control.ConnectSignal("showSound",[this,&control](const ui::Control::SignalArguments&){PlayUiControlSound(control,true);});
        if(!control.HideSound().Empty()&&!control.HasSignalHandlers("hideSound"))
            (void)control.ConnectSignal("hideSound",[this,&control](const ui::Control::SignalArguments&){PlayUiControlSound(control,false);});
        if(dynamic_cast<ui::Button*>(&control)){
            const std::string hover=control.HoverSoundPath().empty()
                ? m_defaultUiHoverSound : control.HoverSoundPath();
            // Back/"no" controls default to the cancel sound when the
            // project provides one; an authored activateSoundPath wins.
            const std::string& actionId=static_cast<ui::Button&>(control).ActivationActionId();
            const bool cancels=actionId=="overlay.close"||actionId=="route.back"||
                actionId.ends_with(".cancel");
            const std::string activate=!control.ActivateSoundPath().empty()
                ? control.ActivateSoundPath()
                : cancels&&!m_defaultUiCancelSound.empty()
                    ? m_defaultUiCancelSound : m_defaultUiActivateSound;
            if(!hover.empty()) (void)control.ConnectSignal("pointerEntered",[this,hover](const ui::Control::SignalArguments&){m_runtime.Audio().PlaySE(hover);});
            if(!activate.empty()) (void)control.ConnectSignal("activated",[this,activate](const ui::Control::SignalArguments&){m_runtime.Audio().PlaySE(activate);});
        }
    });
    m_session->SetUIStateHandler(
        [this] { return m_ui.CaptureRuntimeState(); },
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
        if (!project.is_discarded() && project.is_object()) {
            const auto settings = project.find("settings");
            if (settings != project.end() && settings->is_object() &&
                !ApplyPlayerSettings(*settings))
                return false;
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
    m_ui.SetUiMotionLoader([this](const std::string_view path) {
        return m_runtime.VFS().ReadText(std::string(path));
    });
    if(m_boot.routeScenes.empty()||!m_boot.routeScenes.contains(m_boot.startRoute)){diag::Diagnostic diagnostic{.severity=diag::Severity::Fatal,.code="PXPLAYER5004",.category="Player.Boot",.message="Route table or startRoute is invalid"};diag::Emit(diagnostic);return false;}
    std::set<std::string> runtimeRoutes;
    for (const auto& [route, _] : m_boot.routeScenes)
        runtimeRoutes.insert(route);
    for (const std::string_view route :
         {"title", "hud", "backlog", "save", "load", "gallery",
          "settings", "video"})
        runtimeRoutes.emplace(route);
    for(const auto& route:runtimeRoutes){
        const Status registered=m_session->Routes().Register(route,[route](){return Result<std::unique_ptr<ui::Control>>::Success(std::make_unique<ui::Control>(std::string("Route:")+route));});
        if(!registered){
            for(const auto& diagnostic:registered.Diagnostics())diag::Emit(diagnostic);
            return false;
        }
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

    PX_LOG_DEBUG("Player boot: constructing video player");
    m_video = std::make_unique<video::VideoPlayer>(
        m_runtime.GetWindow().Renderer(), m_runtime.VFS());
    m_scriptServices.vfs = &m_runtime.VFS();
    m_scriptServices.renderer = &m_runtime.Renderer();
    m_scriptServices.audio = &m_runtime.Audio();
    m_scriptServices.profile = &m_profile;
    m_scriptServices.input = &m_runtime.GetInput();
    m_scriptServices.stage = &m_session->Stage();
    m_scriptServices.variables = &m_session->Variables();
    m_scriptServices.routes = &m_session->Routes();
    m_scriptServices.timeline = &m_session->Timeline();
    m_scriptServices.loadAnimationAsset = [this](const std::string& path) {
        auto loaded = m_session->LoadAnimationAsset(path);
        if (!loaded)
            return Result<std::string>::Failure(loaded.Diagnostics());
        return Result<std::string>::Success(loaded.Value().ToString());
    };
    m_scriptServices.requestSave = [this](const int slot) {
        if (m_appState != AppState::Game || slot < -1 || slot > 999)
            return false;
        m_pendingScriptSave = slot;
        return true;
    };
    m_scriptServices.requestLoad = [this](const int slot) {
        if (slot < 0 || slot > 999 || !m_saves.Peek(slot).exists)
            return false;
        m_pendingScriptLoad = slot;
        return true;
    };
    m_scriptServices.deleteSave = [this](const int slot) {
        return slot >= 0 && slot <= 999 && m_saves.Delete(slot);
    };
    m_scriptServices.querySave = [this](const int slot) {
        const progress::SlotInfo info = m_saves.Peek(slot);
        VariantObject value;
        value.emplace("slot", Variant(static_cast<std::int64_t>(slot)));
        value.emplace("exists", Variant(info.exists));
        value.emplace("chapter", Variant(info.chapter));
        value.emplace("timestamp",
                      Variant(static_cast<std::int64_t>(std::min<std::uint64_t>(
                          info.timestamp,
                          static_cast<std::uint64_t>(
                              (std::numeric_limits<std::int64_t>::max)())))));
        value.emplace("hasThumbnail", Variant(!info.thumbnailPng.empty()));
        return Variant(std::move(value));
    };
    m_scriptServices.listSaves = [this](const int count) {
        VariantArray values;
        const auto slots = m_saves.List(count);
        values.reserve(slots.size());
        for (std::size_t index = 0; index < slots.size(); ++index) {
            const auto& info = slots[index];
            VariantObject value;
            value.emplace("slot",
                          Variant(static_cast<std::int64_t>(index)));
            value.emplace("exists", Variant(info.exists));
            value.emplace("chapter", Variant(info.chapter));
            value.emplace(
                "timestamp",
                Variant(static_cast<std::int64_t>(std::min<std::uint64_t>(
                    info.timestamp,
                    static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int64_t>::max)())))));
            value.emplace("hasThumbnail", Variant(!info.thumbnailPng.empty()));
            values.emplace_back(std::move(value));
        }
        return Variant(std::move(values));
    };
    m_scriptServices.queryDialogue = [this] {
        return script::BuildDialogueModel(m_session->Dialogue());
    };
    m_scriptServices.presentScriptSay = [this](const vn::Command& command) {
        return m_session->VM().PresentScriptSay(command);
    };
    m_scriptServices.queryChoices = [this] {
        return script::BuildChoiceModels(m_session->VM());
    };
    m_scriptServices.queryBacklog = [this] {
        return script::BuildBacklogModels(m_session->Backlog());
    };
    m_scriptServices.querySettings = [this] {
        VariantArray locales;
        locales.reserve(m_supportedLocales.size());
        for (const auto& locale : m_supportedLocales)
            locales.emplace_back(locale);
        return Variant(VariantObject{
            {"bgmVolume", Variant(m_settings.bgmVolume)},
            {"seVolume", Variant(m_settings.seVolume)},
            {"voiceVolume", Variant(m_settings.voiceVolume)},
            {"textSpeedMs", Variant(m_settings.textSpeedMs)},
            {"autoWaitMs", Variant(m_settings.autoWaitMs)},
            {"skipReadOnly", Variant(m_settings.skipReadOnly)},
            {"fullscreen", Variant(m_settings.fullscreen)},
            {"windowWidth", Variant(m_settings.windowWidth)},
            {"windowHeight", Variant(m_settings.windowHeight)},
            {"language", Variant(m_settings.language)},
            {"textScale", Variant(m_settings.textScale)},
            {"highContrast", Variant(m_settings.highContrast)},
            {"reducedMotion", Variant(m_settings.reducedMotion)},
            {"selfVoicing", Variant(m_settings.selfVoicing)},
            {"supportedLocales", Variant(std::move(locales))}});
    };
    m_scriptServices.advanceDialogue = [this] {
        if (m_appState != AppState::Game ||
            m_session->VM().State() == vn::VMState::WaitingChoice)
            return false;
        m_session->Advance();
        // As a click does: a Story [auto on] section (possibly started by
        // this very advance) keeps reading; the player's own auto stops.
        m_autoMode = m_storyAuto.has_value();
        m_skipMode = false;
        return true;
    };
    m_scriptServices.selectChoice = [this](const int index) {
        const auto& choices = m_session->VM().Choices();
        if (m_session->VM().State() != vn::VMState::WaitingChoice ||
            index < 0 || static_cast<std::size_t>(index) >= choices.size())
            return false;
        m_session->SelectChoice(index);
        return true;
    };
    m_scriptServices.replayBacklogVoice =
        [this](const std::uint64_t sequence) {
            const auto* entry = m_session->Backlog().Find(sequence);
            if (!entry || entry->voice.empty()) return false;
            const std::string path =
                entry->voice.find('/') != std::string::npos
                    ? entry->voice
                    : m_session->VM().Config().voiceDir + entry->voice;
            m_runtime.Audio().PlayVoice(path);
            return true;
        };
    m_scriptServices.rollbackBacklog =
        [this](const std::uint64_t sequence) {
            return RollbackToBacklogSequence(sequence);
        };
    m_scriptServices.updateSetting =
        [this](const std::string_view name, const Variant& value) {
            const auto integer = [&value]() -> std::optional<int> {
                if (const auto* exact = value.TryGet<std::int64_t>()) {
                    if (*exact < (std::numeric_limits<int>::min)() ||
                        *exact > (std::numeric_limits<int>::max)())
                        return std::nullopt;
                    return static_cast<int>(*exact);
                }
                if (const auto* number = value.TryGet<double>(); number &&
                    std::isfinite(*number) && std::floor(*number) == *number &&
                    *number >= (std::numeric_limits<int>::min)() &&
                    *number <= (std::numeric_limits<int>::max)())
                    return static_cast<int>(*number);
                return std::nullopt;
            };
            const auto number = [&value]() -> std::optional<double> {
                if (const auto* exact = value.TryGet<double>(); exact &&
                    std::isfinite(*exact))
                    return *exact;
                if (const auto* exact = value.TryGet<std::int64_t>())
                    return static_cast<double>(*exact);
                return std::nullopt;
            };
            bool changed = false;
            if (name == "bgmVolume") {
                if (const auto v = integer()) {
                    m_settings.bgmVolume = std::clamp(*v, 0, 128);
                    m_runtime.Audio().SetBGMVolume(m_settings.bgmVolume);
                    changed = true;
                }
            } else if (name == "seVolume") {
                if (const auto v = integer()) {
                    m_settings.seVolume = std::clamp(*v, 0, 128);
                    m_runtime.Audio().SetSEVolume(m_settings.seVolume);
                    changed = true;
                }
            } else if (name == "voiceVolume") {
                if (const auto v = integer()) {
                    m_settings.voiceVolume = std::clamp(*v, 0, 128);
                    m_runtime.Audio().SetVoiceVolume(m_settings.voiceVolume);
                    changed = true;
                }
            } else if (name == "textSpeedMs") {
                if (const auto v = integer()) {
                    m_settings.textSpeedMs = std::clamp(*v, 0, 120);
                    m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
                    changed = true;
                }
            } else if (name == "autoWaitMs") {
                if (const auto v = integer()) {
                    m_settings.autoWaitMs = std::clamp(*v, 0, 60'000);
                    changed = true;
                }
            } else if (name == "windowWidth" || name == "windowHeight") {
                if (const auto v = integer()) {
                    if (name == "windowWidth")
                        m_settings.windowWidth = std::clamp(*v, 320, 16'384);
                    else
                        m_settings.windowHeight = std::clamp(*v, 180, 16'384);
                    if (!m_settings.fullscreen)
                        SDL_SetWindowSize(m_runtime.GetWindow().Handle(),
                                          m_settings.windowWidth,
                                          m_settings.windowHeight);
                    changed = true;
                }
            } else if (name == "textScale") {
                if (const auto v = number()) {
                    m_settings.textScale =
                        static_cast<float>(std::clamp(*v, 0.75, 2.0));
                    changed = true;
                }
            } else if (name == "language") {
                if (const auto* v = value.TryGet<std::string>())
                    changed = LoadLocale(*v, true);
            } else {
                const auto* v = value.TryGet<bool>();
                if (!v) return false;
                if (name == "skipReadOnly") m_settings.skipReadOnly = *v;
                else if (name == "fullscreen") {
                    return ApplyFullscreenSetting(*v);
                } else if (name == "highContrast") m_settings.highContrast = *v;
                else if (name == "reducedMotion") m_settings.reducedMotion = *v;
                else if (name == "selfVoicing") {
                    m_settings.selfVoicing = *v;
                    if (!*v) m_speech.Stop();
                } else return false;
                changed = true;
            }
            return changed && SaveSettingsIfWritable();
        };
    m_scriptServices.playVideo =
        [this](const std::string& path, const float volume,
               const bool skippable) -> std::uint64_t {
        if (!m_video || !m_runtime.VFS().Exists(path)) return 0;
        if (m_scriptVideoHandle != 0) {
            m_video->Stop();
            RecordScriptVideoTerminal(m_scriptVideoHandle, "stopped");
            m_scriptVideoHandle = 0;
        }
        const std::uint64_t handle = m_nextScriptVideoHandle++;
        if (handle == 0 || !m_video->Open(path, volume)) {
            RecordScriptVideoTerminal(handle, "failed", m_video->LastError());
            return 0;
        }
        m_scriptVideoHandle = handle;
        m_videoSkippable = skippable;
        m_scriptVideoStates[handle] = "playing";
        return handle;
    };
    m_scriptServices.pauseVideo = [this](const std::uint64_t handle) {
        if (!m_video || handle != m_scriptVideoHandle || !m_video->Pause())
            return false;
        m_scriptVideoStates[handle] = "paused";
        return true;
    };
    m_scriptServices.resumeVideo = [this](const std::uint64_t handle) {
        if (!m_video || handle != m_scriptVideoHandle || !m_video->Resume())
            return false;
        m_scriptVideoStates[handle] = "playing";
        return true;
    };
    m_scriptServices.stopVideo = [this](const std::uint64_t handle) {
        if (!m_video || handle != m_scriptVideoHandle) return false;
        m_video->Stop();
        RecordScriptVideoTerminal(handle, "stopped");
        m_scriptVideoHandle = 0;
        return true;
    };
    m_scriptServices.skipVideo = [this](const std::uint64_t handle) {
        if (!m_video || handle != m_scriptVideoHandle || !m_videoSkippable)
            return false;
        m_video->Skip();
        RecordScriptVideoTerminal(handle, "stopped");
        m_scriptVideoHandle = 0;
        return true;
    };
    m_scriptServices.videoStatus = [this](const std::uint64_t handle) {
        const auto found = m_scriptVideoStates.find(handle);
        return found == m_scriptVideoStates.end() ? std::string("unknown")
                                                  : found->second;
    };
    m_scriptServices.videoError = [this](const std::uint64_t handle) {
        const auto found = m_scriptVideoErrors.find(handle);
        return found == m_scriptVideoErrors.end() ? std::string{} : found->second;
    };
    PX_LOG_DEBUG("Player boot: constructing script host");
    m_scriptHost = script::CreateScriptHost(m_scriptServices);
    m_session->SetDialogueAdvanceHandler([this](vn::Dialogue& dialogue) {
        return m_scriptHost && m_scriptHost->AdvanceDialogueWait(dialogue);
    });
    script::BindTimelineMarkerEvents(
        m_session->Timeline(), [this] { return m_scriptHost.get(); });
    PX_LOG_DEBUG("Player boot: script host constructed backend={}", m_scriptHost->BackendId());
    (void)m_ui.Actions().RegisterProvider(m_scriptHost->CreateActionProvider());
    PX_LOG_DEBUG("Player boot: script Action provider registered");
    m_session->SetExtensionCommandHandler([this](const vn::Command& cmd) {
        // NVL/ADV mode switches are app-level state, handled before extensions.
        const std::string& t = cmd.type;
        if (t == "hud_visibility") {
            m_storyHudVisible = cmd.Get("visible") == "true";
            m_hudFadeSeconds = cmd.Has("time")
                ? std::clamp(std::atoi(cmd.Get("time").c_str()), 0, 10'000) / 1000.0f
                : kHudFadeSeconds;
            return true;
        }
        if (t == "actor_action" && cmd.Get("id") == "$window") {
            vn::ActorActionRequest request;
            request.preset = cmd.Get("preset");
            request.size = static_cast<float>(std::atof(cmd.Get("size", "0").c_str()));
            request.timeMs = std::atoi(cmd.Get("time", "0").c_str());
            request.loop = std::atoi(cmd.Get("loop", "1").c_str());
            request.ease = cmd.Get("ease");
            m_windowAction = request.preset == "stop" ? std::nullopt
                                                      : vn::ExpandActorAction(request);
            m_windowActionStart = std::chrono::steady_clock::now();
            return true;
        }
        if (t == "shake" && cmd.Get("target") == "window") {
            // Mirrors the Stage shake curve on the HUD layer only.
            m_windowShakeDuration = std::max(0, std::atoi(cmd.Get("ms", "400").c_str())) / 1000.0f;
            m_windowShakeAmplitude = static_cast<float>(std::atof(cmd.Get("amp", "12").c_str()));
            m_windowShakeStart = std::chrono::steady_clock::now();
            m_windowQuake.reset();
            if (const std::string dir = cmd.Get("dir"); !dir.empty()) {
                const int count = std::clamp(std::atoi(cmd.Get("count", "10").c_str()), 1, 100);
                m_windowQuake = vn::QuakeCurve{dir.front() == 'h' || dir.front() == 'v' ? dir.front() : 'r',
                    m_windowShakeAmplitude, m_windowShakeDuration / (count + 0.5), count};
            }
            return true;
        }
        if (t == "text_input") {
            BeginTextInput(cmd);
            return true;
        }
        if (t == "auto_mode") {
            if (cmd.Get("mode") == "off") SetStoryAuto(std::nullopt);
            else {
                const auto number = [&](const char* key, const int high) {
                    return cmd.Has(key) ? std::clamp(std::atoi(cmd.Get(key).c_str()), 0, high) : -1;
                };
                SetStoryAuto(StoryAuto{number("speed", 1000), number("wait", 60'000),
                                       cmd.Get("lock") == "true"});
            }
            return true;
        }
        if (t == "skip_control") {
            const std::string op = cmd.Get("op");
            if (op == "disable") m_skipDisabled = true;
            else if (op == "enable") m_skipDisabled = false;
            if (op != "enable") m_skipMode = false;
            return true;
        }
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
        // Scripts repeat unlocks whenever an event image is shown again; only
        // a new unlock touches the UI model or the profile file.
        if (kind == "cg") {
            if (!m_profile.UnlockCG(id)) return;
        } else if (kind == "scene") {
            if (!m_profile.UnlockScene(id)) return;
            if (const Status updated = m_ui.UpdateUiData(
                    "sceneUnlocked." + id, Variant(true)); !updated)
                for (const auto& diagnostic : updated.Diagnostics())
                    diag::Emit(diagnostic);
        } else if (kind == "music") {
            if (!m_profile.UnlockMusic(id)) return;
            if (const Status updated = m_ui.UpdateUiData(
                    "musicUnlocked." + id, Variant(true)); !updated)
                for (const auto& diagnostic : updated.Diagnostics())
                    diag::Emit(diagnostic);
        } else return;
        MarkProfileDirty(true);
        PX_LOG_INFO("Player progression unlocked kind={} id={}", kind, id);
    });
    m_session->VM().SetSeenHook([this](const std::string& key) {
        const bool firstRead = m_profile.MarkSeen(key);
        if (firstRead) MarkProfileDirty(false);
        return !firstRead;
    });
    m_session->VM().SetChoiceSeenHook([this](const std::string& key) {
        m_profile.MarkChoiceSeen(key);
        MarkProfileDirty(false);
    });
    m_session->VM().SetVideoHook([this](const std::string& path, bool skippable) {
        // Deferred: opened on the next frame so we never re-enter VM::Run().
        m_pendingVideo = path;
        m_videoSkippable = skippable;
    });

    m_ui.SetActionSink([this](const ui::GalgameAction& action) { HandleUIAction(action); });
    m_ui.SetOptionSource([this](const std::string_view name)
                             -> std::optional<ui::GalgameUI::OptionRange> {
        const auto found = m_optionSpecs.find(name);
        if (found == m_optionSpecs.end()) return std::nullopt;
        return ui::GalgameUI::OptionRange{OptionValue(name), found->second.min,
                                          found->second.max};
    });
    m_ui.SetImagePreloadSink([this](const std::string_view path) {
        m_runtime.Assets().PreloadTexture(
            std::string(path), graphics::AssetPreloadPriority::Normal);
    });
    if (!RegisterUiActions()) return false;
    // The music room lists exactly player.musicTracks; nothing is guessed
    // from a folder of one sample project's file names.
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
    m_ui.SetBuiltinCanvas(m_boot.config.logicalWidth, m_boot.config.logicalHeight);
    PX_LOG_DEBUG("Player boot: registering typed UI templates");
    const auto registerTemplate=[this](const std::string& route,const std::string& path){
        auto text=m_runtime.VFS().ReadText(path);if(!text){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXPLAYER5003",.category="Player.UI",.message="Required UI template is missing: "+path};d.source.path=path;diag::Emit(d);return false;}
        const Status status=m_ui.RegisterRouteTemplate(route,*text,path);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return status.IsOk();
    };
    for (const auto& [route, path] : m_boot.routeScenes)
        if (!registerTemplate(route, path)) return false;
    m_titlePreloadBatch = 0;
    auto titleImages = m_ui.RouteImageDependencies(m_boot.startRoute);
    if (!titleImages) {
        for (const auto& diagnostic : titleImages.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    std::vector<graphics::AssetPreloadRequest> titleRequests;
    titleRequests.reserve(titleImages.Value().size());
    for (const auto& path : titleImages.Value())
        titleRequests.push_back({path, graphics::AssetPreloadPriority::High});
    m_titlePreloadBatch = m_runtime.Assets().BeginTexturePreload(std::move(titleRequests));
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
        "Player boot: GameCatalog resources variables={} bindings={} actions={} gallery={}",
        m_catalog.Variables().size(), m_catalog.InputBindings().size(),
        m_catalog.InputActions().size(),
        m_catalog.Gallery().size());
    for (const auto& variable : m_catalog.Variables()) {
        if (variable.scope != vn::CatalogVariable::Scope::Profile) continue;
        const Variant* stored = m_profile.Variable(variable.name);
        const Status defined = m_ui.DefineUiData(
            "profileFlags." + variable.name,
            Variant(ProfileFlagEnabled(stored ? stored : &variable.typedDefault)));
        if (!defined) {
            for (const auto& diagnostic : defined.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
    }
    for (const auto& path : m_musicTracks) {
        const std::string id = px::PathStemUtf8(path);
        if (const Status defined = m_ui.DefineUiData(
                "musicUnlocked." + id, Variant(m_profile.MusicUnlocked(id))); !defined) {
            for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    for (int number = 1; number <= 24; ++number) {
        const std::string id = std::format("scene{:02}", number);
        if (const Status defined = m_ui.DefineUiData(
                "sceneUnlocked." + id, Variant(m_profile.SceneUnlocked(id))); !defined) {
            for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    for (int page = 1; page <= 4; ++page) {
        if (const Status defined = m_ui.DefineUiData(
                "replay.page" + std::to_string(page), Variant(page == 1)); !defined) {
            for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
        if (const Status defined = m_ui.DefineUiData(
                "settings.page" + std::to_string(page), Variant(page == 1)); !defined) {
            for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    if (const Status defined = DefineOptionData(); !defined) {
        for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    if (const Status defined = DefineDialogData(); !defined) {
        for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    ApplyAllOptions();
    PublishMuteData();
    for (int page = 1; page <= m_savePages; ++page) {
        if (const Status defined = m_ui.DefineUiData(
                "saveLoad.page" + std::to_string(page), Variant(page == 1)); !defined) {
            for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    RefreshTitleProgressUi();
    for (const auto& binding : m_catalog.InputBindings()) {
        if (binding.command == "screen.open" && !binding.argument.empty()) {
            const auto sc = Input::KeyCode(binding.key);
            if (sc) {
                const std::string action = "screen.open." + binding.argument;
                auto& input = m_runtime.GetInput();
                if (!input.HasAction(action)) (void)input.RegisterAction(action);
                (void)input.SetActionContext(action, "game");
                (void)input.SetDefaultKeyboardBindings(action, {*sc});
                if (!m_settings.keyboardBindings.contains(action))
                    (void)input.BindKey(action, *sc);
                m_screenTriggers[action] = binding.argument;
                PX_LOG_INFO("Player input binding registered key={} route={}",
                            binding.key, binding.argument);
            }
        }
    }
    for (const auto& action : m_catalog.InputActions()) {
        auto& input = m_runtime.GetInput();
        std::vector<int> keys;
        keys.reserve(action.keys.size());
        for (const auto& key : action.keys) {
            const auto scancode = Input::KeyCode(key);
            if (!scancode) {
                diag::Emit(diag::Diagnostic{.severity = diag::Severity::Fatal,
                                            .code = "PXPLAYER5022",
                                            .category = "Player.Input",
                                            .message = "Unknown input key name",
                                            .details = action.id + ": " + key});
                return false;
            }
            keys.push_back(*scancode);
        }
        if ((!input.HasAction(action.id) && !input.RegisterAction(action.id)) ||
            !input.SetActionContext(action.id, action.context) ||
            (action.keysSpecified &&
             !input.SetDefaultKeyboardBindings(action.id, keys))) {
            diag::Emit(diag::Diagnostic{.severity = diag::Severity::Fatal,
                                        .code = "PXPLAYER5021",
                                        .category = "Player.Input",
                                        .message = "Game input action could not be registered",
                                        .details = action.id});
            return false;
        }
        if (!m_settings.keyboardBindings.contains(action.id) &&
            action.keysSpecified) {
            if (keys.empty()) {
                if (!input.ClearKeyboardBindings(action.id)) return false;
            } else {
                if (input.RebindKey(action.id, keys.front(), true).status !=
                    KeyRebindStatus::Applied) return false;
                for (std::size_t index = 1; index < keys.size(); ++index)
                    if (!input.BindKey(action.id, keys[index])) return false;
            }
        }
        if (!action.wheelDirection.empty()) {
            const WheelDirection direction = action.wheelDirection == "up"
                ? WheelDirection::Up
                : action.wheelDirection == "down" ? WheelDirection::Down
                                                   : WheelDirection::None;
            if (!input.SetDefaultWheelBinding(action.id, direction,
                                              action.wheelThreshold) ||
                (!m_settings.wheelBindings.contains(action.id) &&
                 !input.SetWheelBinding(action.id, direction,
                                        action.wheelThreshold)))
                return false;
        }
        if (!action.route.empty()) m_screenTriggers[action.id] = action.route;
        if (!action.uiCommand.empty())
            m_uiActionTriggers[action.id] = {action.uiCommand, action.uiArgument};
    }
    m_session->VM().SetGameCatalog(m_catalog);

    m_script = argc > 1 ? argv[1]
                        : (m_script.empty() ? m_boot.startScript : m_script);
    if (m_runtimePreloadPath != m_script) BeginRuntimeIrPreload();
    m_splash=std::make_unique<ui::startup::SplashSequencePlayer>(
        ui::startup::SplashSequencePlayer::Services{
            .loadScene=[this](const ResourceRefValue& reference)->Result<sdk::UiDocument>{
                const auto text=m_runtime.VFS().ReadText(reference.lastKnownPath);
                if(!text){diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXBOOT1201",.category="Player.Splash",.message="Splash scene is missing",.details=reference.lastKnownPath};diagnostic.source.path=reference.lastKnownPath;return Result<sdk::UiDocument>::Failure(std::move(diagnostic));}
                const auto parsed=sdk::ParseUi(*text);
                if(parsed.Valid())return Result<sdk::UiDocument>::Success(parsed.document);
                std::vector<diag::Diagnostic> diagnostics;
                for(const auto& error:parsed.diagnostics){diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=error.code,.category="Player.Splash",.message=error.message};diagnostic.source.path=reference.lastKnownPath;diagnostics.push_back(std::move(diagnostic));}
                return Result<sdk::UiDocument>::Failure(std::move(diagnostics));
            },
            .resolveAsset=[this](const std::string_view assetId)->std::optional<std::string>{
                const auto found=m_uiAssets.find(std::string(assetId));
                return found==m_uiAssets.end()?std::nullopt:std::optional<std::string>{found->second};
            },
            .loadComponent=[this](const std::string_view componentId)->std::optional<ui::UiComponentSource>{
                const auto found=m_uiComponents.find(std::string(componentId));
                if(found==m_uiComponents.end())return std::nullopt;
                const auto source=m_runtime.VFS().ReadText(found->second);
                return source?std::optional<ui::UiComponentSource>{ui::UiComponentSource{found->second,*source}}:std::nullopt;
            },
            .loadMotion=[this](const std::string_view path){
                return m_runtime.VFS().ReadText(std::string(path));
            },
            .preloadVisuals=[this](const std::vector<std::string>& paths)->std::function<bool()>{
                std::vector<graphics::AssetPreloadRequest> requests;
                requests.reserve(paths.size());
                for(const auto& path:paths)
                    requests.push_back({path,graphics::AssetPreloadPriority::Critical});
                const auto batch=m_runtime.Assets().BeginTexturePreload(std::move(requests));
                return [this,batch]{
                    const auto progress=m_runtime.Assets().TexturePreloadProgress(batch);
                    return !progress.valid||progress.Finished();
                };
            },
            .completionVisualsReady=[this]{return TitleVisualsReady();},
            .playAudio=[this](const ResourceRefValue& reference){
                if(!m_runtime.VFS().Exists(reference.lastKnownPath)){diag::Diagnostic diagnostic{.severity=diag::Severity::Warning,.code="PXBOOT1202",.category="Player.Splash",.message="Splash audio is missing; visual playback will continue",.details=reference.lastKnownPath};diagnostic.source.path=reference.lastKnownPath;return Status::Fail(std::move(diagnostic));}
                if (reference.lastKnownPath.starts_with("Assets/Audio/Voice/"))
                    m_runtime.Audio().PlayVoice(reference.lastKnownPath);
                else
                    m_runtime.Audio().PlaySE(reference.lastKnownPath);
                PX_LOG_INFO("Player splash audio started path={}",
                            reference.lastKnownPath);
                return Status::Ok();
            },
            .playMotionMarker=[this](const sdk::MotionAudioMarker& marker){
                PlayMotionMarker(marker);
            },
            .diagnostics=[](const diag::Diagnostic& diagnostic){diag::Emit(diagnostic);}});
    m_splash->Context().SetTextRenderer(&m_runtime.Renderer());
    m_splash->Context().SetDiagnosticOverlayEnabled(false);
    m_splash->SetCompletionCallback([this]{FinishBootPresentation();});
    m_appState=AppState::BootSplash;
    const Status splashStarted=m_splash->Start(m_boot.splashes,m_settings.reducedMotion);
    if(!splashStarted)return false;
    StartQaIpc();
    return true;
}

void PlayerApp::PlayMotionMarker(const sdk::MotionAudioMarker& marker) {
    if (!m_runtime.VFS().Exists(marker.audio)) {
        diag::Diagnostic diagnostic{.severity=diag::Severity::Warning,
            .code="PXPLAYERMOTION1001", .category="Player.Motion",
            .message="Motion audio marker resource is missing: "+marker.audio};
        diagnostic.source.path=marker.audio;
        diag::Emit(diagnostic);
        return;
    }
    if (marker.bus == sdk::MotionAudioBus::Voice)
        m_runtime.Audio().PlayVoice(marker.audio);
    else
        m_runtime.Audio().PlaySE(marker.audio);
}

bool PlayerApp::SaveProfileIfWritable() {
    diag::FrameProfiler::Instance().Note("profile-write");
    if (!m_profileWritable) {
        PX_LOG_ERROR(
            "Profile write suppressed because the existing file failed to load");
        return false;
    }
    if (m_profileWrite.valid()) m_profileTrusted = m_profileWrite.get() || m_profileTrusted;
    const std::string path = PathToUtf8(m_saveDirectory / "profile.dat");
    // The first write verifies that an existing file is ours before replacing
    // it; afterwards this process owns the file and can overwrite directly.
    const bool saved = m_profileTrusted
        ? progress::SaveGlobalProfile(m_profile, path, &m_saveKey)
        : progress::SaveGlobalProfilePreservingExisting(
              m_profile, path, &m_saveKey, m_saveFallbackKeys);
    if (saved) {
        m_profileTrusted = true;
        m_profileDirty = false;
        m_profileUrgent = false;
    }
    return saved;
}

void PlayerApp::MarkProfileDirty(const bool urgent) {
    if (!m_profileDirty) m_profileDirtySince = SDL_GetTicks();
    m_profileDirty = true;
    m_profileUrgent = m_profileUrgent || urgent;
}

void PlayerApp::FlushProfileIfDue() {
    if (m_profileWrite.valid()) {
        if (m_profileWrite.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
            return;
        if (m_profileWrite.get()) m_profileTrusted = true;
        else MarkProfileDirty(false);  // retried on the normal schedule
    }
    if (!m_profileDirty || !m_profileWritable) return;
    const std::uint64_t age = SDL_GetTicks() - m_profileDirtySince;
    if (age < (m_profileUrgent ? 2'000u : 30'000u)) return;
    if (!m_profileTrusted) {
        // The first write checks that an existing file is ours. It runs on the
        // persistence worker too: done inline it froze a frame for ~100 ms,
        // often in the middle of a screen fade.
        m_profileDirty = false;
        m_profileUrgent = false;
        m_profileWrite = m_persistenceWrites.Submit(
            [profile = m_profile, path = PathToUtf8(m_saveDirectory / "profile.dat"),
             key = m_saveKey, fallbacks = m_saveFallbackKeys] {
                return progress::SaveGlobalProfilePreservingExisting(
                    profile, path, &key, fallbacks);
            });
        return;
    }
    nlohmann::json encoded;
    if (!m_profile.TryToJson(encoded)) return;
    m_profileDirty = false;
    m_profileUrgent = false;
    m_profileWrite = m_persistenceWrites.Submit(
        [path = PathToUtf8(m_saveDirectory / "profile.dat"),
         text = encoded.dump(), key = m_saveKey] {
            return progress::SaveJsonText(path, text, &key);
        });
}

bool PlayerApp::SaveSettingsIfWritable() {
    if (!m_settingsWritable) {
        PX_LOG_ERROR(
            "Settings write suppressed because the existing file failed to load");
        return false;
    }
    if (m_settingsWrite.valid()) (void)m_settingsWrite.get();
    return m_settings.SaveWithInputBindings(
        PathToUtf8(m_saveDirectory / "config.dat"),
        m_runtime.GetInput(), &m_saveKey, m_saveFallbackKeys);
}

void PlayerApp::SaveSettingsInBackground() {
    if (!m_settingsWritable) return;
    if (m_settingsWrite.valid()) (void)m_settingsWrite.get();
    // Input bindings are captured here, on the thread that owns Input.
    progress::GameSettings snapshot = m_settings;
    snapshot.CaptureInputBindings(m_runtime.GetInput());
    m_settingsWrite = m_persistenceWrites.Submit(
        [snapshot = std::move(snapshot),
         path = PathToUtf8(m_saveDirectory / "config.dat"), key = m_saveKey,
         fallback = m_saveFallbackKeys] {
            return snapshot.SavePreservingExisting(path, &key, fallback);
        });
}

bool PlayerApp::ApplyFullscreenSetting(bool fullscreen) {
    if (CurrentPlatform().mobile) fullscreen = true;
    SDL_Window* window = m_runtime.GetWindow().Handle();
    const bool actuallyFullscreen =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (m_settings.fullscreen == fullscreen && actuallyFullscreen == fullscreen)
        return true;
    if (!SDL_SetWindowFullscreen(window, fullscreen) || !SDL_SyncWindow(window)) {
        PX_LOG_WARN("Player fullscreen change rejected: {}", SDL_GetError());
        return false;
    }
    if (!fullscreen) {
        // SDL restores the size from before entering fullscreen. The Player is
        // initially created at the game's logical resolution, so that size can
        // itself cover the entire monitor. Restore the saved windowed size.
        if (!ShowWindowed(window, m_settings.windowWidth, m_settings.windowHeight)) {
            PX_LOG_WARN("Player windowed size change rejected: {}", SDL_GetError());
            return false;
        }
    }
    if (((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) != fullscreen) {
        PX_LOG_WARN("Player fullscreen change was not applied by the window manager");
        return false;
    }
    m_settings.fullscreen = fullscreen;
    (void)m_ui.UpdateUiData("settings.fullscreen", Variant(fullscreen));
    (void)m_ui.UpdateUiData("settings.windowed", Variant(!fullscreen));
    (void)SaveSettingsIfWritable();
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    PX_LOG_INFO("Player display mode applied fullscreen={} size={}x{}",
                fullscreen, width, height);
    return true;
}

void PlayerApp::BeginRuntimeIrPreload() {
    if (!m_session || !m_script.ends_with(".pxir")) return;
    if (m_runtimePreload.valid()) {
        m_runtimePreload.wait();
        try {
            (void)m_runtimePreload.get();
        } catch (const std::exception& error) {
            PX_LOG_WARN("Discarding failed Runtime IR preload: {}",
                        error.what());
        }
    }
    m_runtimePreloadPath = m_script;
    PX_LOG_INFO("Player Runtime IR preload started script={}", m_script);
    try {
        auto prepare = [session = m_session.get(), path = m_script] {
                const auto started = std::chrono::steady_clock::now();
                auto program = session->PrepareRuntimeIr(path);
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
                PX_LOG_INFO(
                    "Player Runtime IR preload finished script={} ms={} operations={}",
                    path, elapsed.count(), program ? program->code.size() : 0);
                return program;
            };
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        std::promise<std::shared_ptr<const vn::Program>> prepared;
        m_runtimePreload = prepared.get_future();
        prepared.set_value(prepare());
#else
        m_runtimePreload = std::async(std::launch::async, std::move(prepare));
#endif
    } catch (const std::exception& error) {
        m_runtimePreloadPath.clear();
        PX_LOG_WARN(
            "Player Runtime IR preload could not start; New Game will load synchronously: {}",
            error.what());
    }
}

bool PlayerApp::TitleVisualsReady() {
    if (!m_titlePreloadBatch) return true;
    const auto progress = m_runtime.Assets().TexturePreloadProgress(m_titlePreloadBatch);
    return !progress.valid || progress.Finished();
}

void PlayerApp::FinishBootPresentation() {
    if(m_appState!=AppState::BootSplash)return;
    if(!TitleVisualsReady())return;
    BeginPresentationTransition(m_titleTransition);
    if(!m_session->Routes().Replace(m_boot.startRoute)){m_quitRequested=true;return;}
    const auto routeStatus = m_session->Routes().SetFallbackTransition(
        {m_routeTransition.preset, m_routeTransition.duration});
    if (!routeStatus) {
        for (const auto& diagnostic : routeStatus.Diagnostics()) diag::Emit(diagnostic);
        m_quitRequested = true;
        return;
    }
    for (const auto& [pair, transition] : m_routeTransitionOverrides) {
        const auto separator = pair.find('\n');
        if (separator == std::string::npos) continue;
        const auto status = m_session->Routes().SetDefaultTransition(
            std::string_view(pair).substr(0, separator),
            std::string_view(pair).substr(separator + 1),
            {transition.preset, transition.duration});
        if (!status) {
            for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
            m_quitRequested = true;
            return;
        }
    }
    m_appState=AppState::Title;
    if (!TitleBgmPath().empty()) {
        const auto handle=m_runtime.Audio().PlayBGM(TitleBgmPath(),true,500);
        if(m_runtime.Audio().Status(handle)==audio::PlaybackStatus::Failed)
            PX_LOG_ERROR("Player title BGM failed path={}",TitleBgmPath());
        else
            PX_LOG_INFO("Player title BGM started path={} handle={}",TitleBgmPath(),handle.value);
    }
    PX_LOG_INFO("Player presentation ready route={}", m_boot.startRoute);
    // While the title idles, decode the screens players open first so their
    // first appearance does not wait on image decoding.
    for (const std::string_view route : {"hud", "save", "load", "settings", "backlog"}) {
        if (!m_boot.routeScenes.contains(std::string(route))) continue;
        auto images = m_ui.RouteImageDependencies(route);
        if (!images) continue;
        for (const auto& path : images.Value())
            m_runtime.Assets().PreloadTexture(path, graphics::AssetPreloadPriority::Low);
    }
}

void PlayerApp::PlayUiControlSound(const ui::Control& control, const bool show) {
    const ui::Control::Sound& sound = show ? control.ShowSound() : control.HideSound();
    // Variants replace the path; one available file is picked each time.
    std::vector<std::string> available;
    for (const auto& variant : sound.variants)
        if (const auto safe = SafeVfsRelativePath(variant); safe && m_runtime.VFS().Exists(*safe))
            available.push_back(*safe);
    std::string path;
    if (!available.empty()) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = available[static_cast<std::size_t>(tick) % available.size()];
    } else if (!sound.path.empty()) {
        if (const auto safe = SafeVfsRelativePath(sound.path); safe && m_runtime.VFS().Exists(*safe))
            path = *safe;
    }
    const char* phase = show ? "show" : "hide";
    if (path.empty()) {
        PX_LOG_WARN("Player UI {} sound is missing control={} path={}", phase, control.Name(),
                    sound.path.empty() && !sound.variants.empty() ? sound.variants.front() : sound.path);
        return;
    }
    m_runtime.Audio().PlaySE(path, sound.volume);
    const float after = control.VisibleSeconds();
    PX_LOG_INFO("Player UI {} sound path={} control={} after={:.3f}s", phase, path, control.Name(), after);
    m_uiSoundsPlayed.push_back({phase, path, control.Name(), after});
    while (m_uiSoundsPlayed.size() > 32) m_uiSoundsPlayed.pop_front();
}

void PlayerApp::BeginPresentationTransition(
    const PresentationTransition& transition) {
    if (m_uiTransitionStartedThisFrame || !m_uiEffects) return;
    const auto handle = m_runtime.Renderer().PlayScreenEffect(
        transition.preset, transition.duration, nullptr, true);
    if (handle) m_uiTransitionStartedThisFrame = true;
}

void PlayerApp::SplashFrame(const float dt) {
    if(!m_splash||m_splash->Completed()){FinishBootPresentation();return;}
    Input& input=m_runtime.GetInput();
    int width=0,height=0;m_runtime.Renderer().GetLogicalSize(width,height);
    const bool skip=input.LeftClick()||input.KeyPressed(SDL_SCANCODE_RETURN)||
        input.KeyPressed(SDL_SCANCODE_ESCAPE)||input.KeyPressed(SDL_SCANCODE_SPACE);
    (void)m_splash->Context().Update(input,width,height,dt);
    const std::uint64_t previousPresentation = m_splash->PresentationGeneration();
    m_splash->Update(dt,skip);
    if(!m_splash->Completed()){
        if (m_splash->PresentationGeneration() != previousPresentation) {
            if (previousPresentation > 0) {
                // The new scene installs only after its texture batch is ready;
                // capture the preceding presented frame for the crossfade.
                const std::size_t index = m_splash->CurrentIndex();
                PresentationTransition transition = m_splashTransition;
                if (index == 1 && !m_splashTransitionConfigured)
                    transition.duration = 0.5f;
                if (index < m_splashTransitionOverrides.size()) {
                    const auto& authored = m_splashTransitionOverrides[index];
                    if (authored.preset) transition.preset = *authored.preset;
                    if (authored.duration) transition.duration = *authored.duration;
                }
                BeginPresentationTransition(transition);
            }
            Input neutral;
            neutral.InjectFrame(-1000.0f,-1000.0f,false);
            (void)m_splash->Context().Update(neutral,width,height,0.0f);
        }
        m_splash->Context().Render(m_runtime.Renderer());
        return;
    }
    // The route callback runs during Update(). Arrange and draw the title in
    // this same frame so the clear colour never flashes between splash/title.
    FinishBootPresentation();
    Input neutral;
    neutral.InjectFrame(-1000.0f,-1000.0f,false);
    (void)m_ui.Update(neutral,width,height,0.0f);
    RenderUIWithScripts();
}

void PlayerApp::StartGame() {
    if(m_appState!=AppState::Title||m_gameStartTransitionActive)return;
    m_gameStartTransitionActive=true;
    m_gameStartTransitionCommitted=false;
    m_gameStartTransitionElapsed=0.0f;
    if(!TitleBgmPath().empty())m_runtime.Audio().StopBGM(1000);
}

bool PlayerApp::CommitStartGame() {
    // An authored title button can remain alive until the HUD route commits.
    // Ignore a duplicate click/action delivered during that transition rather
    // than resetting an already-started runtime program.
    if (m_appState != AppState::Title) return false;
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
    m_storyEndHandled = false;
    m_textInput.reset();
    (void)m_ui.UpdateUiData("input.visible", Variant(false));
    SetStoryAuto(std::nullopt);
    m_skipDisabled = false;
    m_hudFadeSnap = true;
    m_storyHudVisible = false;
    m_nvlMode = false;
    m_nvlLines.clear();
    m_rollback.clear();
    m_lastBacklogSequence = m_session->Backlog().LastSequence();
    m_playtimeBaseMs = 0;
    m_playtimeStartedAtMs = m_runtime.GetClock().NowMs();
    if (m_script.ends_with(".pxir")) {
        bool started = false;
        if (m_runtimePreload.valid() && m_runtimePreloadPath == m_script) {
            const auto waiting = std::chrono::steady_clock::now();
            try {
                auto program = m_runtimePreload.get();
                const auto waited =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - waiting);
                PX_LOG_INFO(
                    "Player Runtime IR preload consumed script={} waitMs={}",
                    m_script, waited.count());
                started = m_session->StartPreparedRuntimeIr(
                    std::move(program), m_script, /*resetVariables=*/false);
            } catch (const std::exception& error) {
                PX_LOG_ERROR("Player Runtime IR preload failed: {}", error.what());
                started = m_session->StartRuntimeIr(
                    m_script, /*resetVariables=*/false);
            }
        } else {
            started = m_session->StartRuntimeIr(
                m_script, /*resetVariables=*/false);
        }
        if (!started) {
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Fatal,
                .code = "PXPLAYER5006",
                .category = "Player.Boot",
                .message = "Compiled Story Runtime IR could not be loaded",
                .details = m_script});
            m_quitRequested = true;
            return false;
        }
    } else {
        m_session->VM().LoadScript(m_script);
    }
    if (!m_pendingStoryEntry.empty()) {
        std::string entry = std::exchange(m_pendingStoryEntry, {});
        // A Story project keeps each scene's `*top` label as `<scene>:top`;
        // let authors and the replay screens name a scene by its id.
        if (m_session->VM().CurrentProgram().LabelIndex(entry) < 0 &&
            m_session->VM().CurrentProgram().LabelIndex(entry + ":top") >= 0)
            entry += ":top";
        if (m_session->VM().CurrentProgram().LabelIndex(entry) >= 0) {
            m_session->Stage().ClearAll();
            m_session->Dialogue().Clear();
            m_session->Backlog().Clear();
            m_runtime.Audio().StopVoice();
            m_runtime.Audio().StopAmbience(0);
            m_runtime.Audio().StopBGM(0);
            if (!m_session->VM().RestartAtLabel(entry)) {
                PX_LOG_ERROR("Player could not start story entry label={}", entry);
                return false;
            } else {
                PX_LOG_INFO("Player started story entry label={}", entry);
            }
        } else {
            PX_LOG_ERROR("Player story entry label does not exist label={}", entry);
            return false;
        }
    }
    if (m_scriptHost) m_scriptHost->Emit("scenario.started", {{"resource", m_script}});
    m_appState = AppState::Game;
    if (const Status routed = m_session->Routes().Replace(
            "hud", {.preset = "none"}); !routed) {
        for (const auto& diagnostic : routed.Diagnostics()) diag::Emit(diagnostic);
        m_quitRequested = true;
        return false;
    }
    PX_LOG_INFO("Player game started script={}", m_script);
    return true;
}

void PlayerApp::GameStartTransitionFrame(const float dt,
                                         const std::uint64_t now) {
    constexpr float duration=0.8f;
    // Capped like screen effects: the frame that commits the start loads the
    // story and its first images, and must not swallow the fade-in.
    m_gameStartTransitionElapsed+=std::clamp(dt,0.0f,1.0f/30.0f);
    int width=0,height=0;
    m_runtime.Renderer().GetLogicalSize(width,height);
    float alpha=1.0f;
    if(!m_gameStartTransitionCommitted){
        // Keep the authored title intact while it fades to black. Input is
        // intentionally not updated, so a second click cannot leak through.
        RenderUIWithScripts();
        alpha=std::clamp(m_gameStartTransitionElapsed/duration,0.0f,1.0f);
        if(m_gameStartTransitionElapsed>=duration){
            if (CommitStartGame()) {
                m_gameStartTransitionCommitted=true;
                m_gameStartTransitionElapsed=0.0f;
                alpha=1.0f;
            } else {
                // A replay/flowchart target can become stale after content is
                // rebuilt. Return to the still-mounted title route instead of
                // silently starting the story from its first instruction.
                m_gameStartTransitionActive=false;
                m_gameStartTransitionCommitted=false;
                m_gameStartTransitionElapsed=0.0f;
                alpha=0.0f;
                if (!m_quitRequested && !TitleBgmPath().empty()) {
                    (void)m_runtime.Audio().PlayBGM(TitleBgmPath(),true,300);
                }
            }
        }
    }else{
        GameFrame(dt,now);
        alpha=1.0f-std::clamp(m_gameStartTransitionElapsed/duration,0.0f,1.0f);
        if(m_gameStartTransitionElapsed>=duration){
            m_gameStartTransitionActive=false;
            m_gameStartTransitionCommitted=false;
            m_gameStartTransitionElapsed=0.0f;
            alpha=0.0f;
        }
    }
    if(alpha>0.0f){
        const float eased=alpha*alpha*(3.0f-2.0f*alpha);
        m_runtime.Renderer().DrawRect(
            Rect{0,0,static_cast<float>(width),static_cast<float>(height)},
            Color{0,0,0,static_cast<std::uint8_t>(std::lround(eased*255.0f))});
    }
}

bool PlayerApp::LoadSlot(int slot) {
    HarvestBackgroundSave(true);
    auto snap = m_saves.Load(slot);
    if (!snap) {
        return false;
    }
    m_hudFadeSnap = true;
    // A save taken at the end answers afterStoryEnd again once it is loaded.
    m_storyEndHandled = false;
    if (snap->gameId != m_boot.gameId) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6115",
            .category = "Persistence.Save",
            .message = "Save belongs to a different game"});
        return false;
    }
    const bool sameContent = snap->contentVersion == m_boot.contentVersion &&
                             snap->saveVersion == m_boot.saveVersion;
    if (sameContent &&
        snap->packageFingerprint != m_boot.packageFingerprint &&
        std::ranges::find(m_boot.compatibleSaveFingerprints,
                          snap->packageFingerprint) ==
            m_boot.compatibleSaveFingerprints.end()) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSAVE6111",
            .category = "Persistence.Save",
            .message = "Package fingerprint changed without a content/save version migration",
            .hint = "Pack releases with the project's .prismatix-release/lineage.json, or "
                    "advance contentVersion/saveVersion with a save migration"});
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
    if (!progress::HasConsistentAwaitState(*snap)) {
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
    script::ExtensionStates previousExtensionState;
    const ui::UIRuntimeState previousUi = m_ui.CaptureRuntimeState();
    const auto rollbackPresentation = [this, &previousPending, &previousActions,
                                       &previousExtensionState, &previousUi] {
        if (m_scriptHost) {
            (void)m_scriptHost->RestoreCheckpoint(previousPending, previousActions,
                                                  std::nullopt);
            (void)m_scriptHost->RestoreExtensionState(previousExtensionState);
        }
        (void)m_ui.RestoreRuntimeState(previousUi);
    };
    if (m_scriptHost) {
        previousPending = m_scriptHost->CapturePending();
        previousActions = m_scriptHost->CapturePendingActions();
        auto capturedExtensionState = m_scriptHost->CaptureExtensionState();
        if (!capturedExtensionState) {
            for (const auto& diagnostic : capturedExtensionState.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
        previousExtensionState = capturedExtensionState.TakeValue();
        const Status scriptStatus = m_scriptHost->RestoreCheckpoint(
            snap->scriptPending, snap->scriptActions,
            script::CancellationReason::Load);
        if (!scriptStatus) {
            rollbackPresentation();
            return false;
        }
        const Status extensionStatus =
            m_scriptHost->RestoreExtensionState(snap->extensionState);
        if (!extensionStatus) {
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
    m_lastBacklogSequence = m_session->Backlog().LastSequence();
    m_playtimeBaseMs = snap->playtimeMs;
    m_playtimeStartedAtMs = m_runtime.GetClock().NowMs();
    m_script = candidateScript;
    m_appState = AppState::Game;
    m_autoMode = m_skipMode = m_hudHidden = false;
    m_textInput.reset();
    (void)m_ui.UpdateUiData("input.visible", Variant(false));
    m_skipDisabled = snap->skipDisabled;
    SetStoryAuto(snap->storyAuto ? std::optional<StoryAuto>(StoryAuto{
        snap->storyAutoSpeedMs, snap->storyAutoWaitMs, snap->storyAutoLocked}) : std::nullopt);
    m_storyHudVisible = true;
    if (m_ui.CurrentRoute() == "hud" || m_ui.CurrentRoute() == "game")
        (void)m_ui.RefreshHUD(DialogueUI());
    if (m_scriptHost) m_scriptHost->Emit("save.loaded", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Loaded slot {}", slot);
    return true;
}

std::optional<progress::SaveSnapshot> PlayerApp::MakeSnapshot(
    bool includeBacklog) {
    progress::SaveSnapshot snap;
    const std::uint64_t now = m_runtime.GetClock().NowMs();
    snap.playtimeMs = m_playtimeBaseMs +
        (now >= m_playtimeStartedAtMs ? now - m_playtimeStartedAtMs : 0);
    const RuntimeSession::GameState state =
        m_session->CaptureState(snap.playtimeMs, includeBacklog);
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
    // The save picker is a transient tool surface. Preserve the exact surface
    // and route stack beneath it (including authored custom routes), rather
    // than serializing its dynamically generated slot controls.
    if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::Save && m_beforeSavePresentation) {
        snap.ui = m_beforeSavePresentation->ui;
        snap.routes = m_beforeSavePresentation->routes;
    }
    if (m_scriptHost) {
        snap.scriptPending = m_scriptHost->CapturePending();
        snap.scriptActions = m_scriptHost->CapturePendingActions();
        auto extensionState = m_scriptHost->CaptureExtensionState();
        if (!extensionState) {
            for (const auto& diagnostic : extensionState.Diagnostics())
                diag::Emit(diagnostic);
            return std::nullopt;
        }
        snap.extensionState = extensionState.TakeValue();
    }
    if (includeBacklog) snap.backlog = state.backlog;
    snap.nvlMode = m_nvlMode;
    snap.nvlLines = m_nvlLines;
    snap.storyAuto = m_storyAuto.has_value();
    if (m_storyAuto) {
        snap.storyAutoSpeedMs = m_storyAuto->textSpeedMs;
        snap.storyAutoWaitMs = m_storyAuto->waitMs;
        snap.storyAutoLocked = m_storyAuto->locked;
    }
    snap.skipDisabled = m_skipDisabled;
    snap.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
    return snap;
}

void PlayerApp::SaveSlotInBackground(const int slot) {
    diag::FrameProfiler::Instance().Note("save-slot-async");
    HarvestBackgroundSave(true);
    auto captured = MakeSnapshot(/*includeBacklog=*/true);
    if (!captured) return;
    SDL_Surface* frame =
        graphics::CaptureFrameSurface(m_runtime.Renderer().Handle());
    m_saveWriteSlot = slot;
    m_saveWrite = m_persistenceWrites.Submit(
        [this, slot, frame, snap = std::move(*captured)]() mutable {
            snap.thumbnailPng = graphics::EncodeThumbnailPng(frame, 256, 144);
            return m_saves.Save(slot, snap);
        });
}

void PlayerApp::HarvestBackgroundSave(const bool wait) {
    if (!m_saveWrite.valid()) return;
    if (!wait && m_saveWrite.wait_for(std::chrono::seconds(0)) !=
                     std::future_status::ready)
        return;
    const int slot = m_saveWriteSlot;
    if (!m_saveWrite.get()) {
        diag::Emit({.severity = diag::Severity::Error, .code = "PXPLAYER6001",
                    .category = "Player.Save",
                    .message = "Could not save slot " + std::to_string(slot)});
        return;
    }
    if (m_scriptHost) m_scriptHost->Emit("save.written", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Saved slot {}", slot);
}

bool PlayerApp::SaveSlot(int slot, std::vector<std::uint8_t> thumbnail) {
    diag::FrameProfiler::Instance().Note("save-slot");
    HarvestBackgroundSave(true);
    auto captured = MakeSnapshot(/*includeBacklog=*/true);
    if (!captured) return false;
    captured->thumbnailPng = std::move(thumbnail);
    return WriteSaveSlot(slot, std::move(*captured));
}

bool PlayerApp::WriteSaveSlot(const int slot, progress::SaveSnapshot snap) {
    HarvestBackgroundSave(true);
    if (!m_saves.Save(slot, snap)) {
        diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXPLAYER6001",.category="Player.Save",
                           .message="Could not save slot "+std::to_string(slot)};diag::Emit(d);return false;
    }
    if (m_scriptHost) m_scriptHost->Emit("save.written", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Saved slot {} (thumb {} bytes)", slot, snap.thumbnailPng.size());
    return true;
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
    const auto rollbackLine = std::ranges::find_if(
        state.backlog, [&entry](const vn::BacklogEntry& line) {
            return line.sequence == entry.backlogSequence;
        });
    if (rollbackLine == state.backlog.end()) return false;
    state.backlog.erase(std::next(rollbackLine), state.backlog.end());
    auto preparedRuntime = m_session->PrepareRestore(
        state, m_runtime.GetClock().NowMs());
    if (!preparedRuntime) {
        for (const auto& diagnostic : preparedRuntime.Diagnostics())
            diag::Emit(diagnostic);
        return false;
    }
    script::PendingCommandsState previousPending;
    script::PendingActionsState previousActions;
    script::ExtensionStates previousExtensionState;
    const ui::UIRuntimeState previousUi = m_ui.CaptureRuntimeState();
    const auto rollbackPresentation = [this, &previousPending, &previousActions,
                                       &previousExtensionState, &previousUi] {
        if (m_scriptHost) {
            (void)m_scriptHost->RestoreCheckpoint(previousPending, previousActions,
                                                  std::nullopt);
            (void)m_scriptHost->RestoreExtensionState(previousExtensionState);
        }
        (void)m_ui.RestoreRuntimeState(previousUi);
    };
    if (m_scriptHost) {
        previousPending = m_scriptHost->CapturePending();
        previousActions = m_scriptHost->CapturePendingActions();
        auto capturedExtensionState = m_scriptHost->CaptureExtensionState();
        if (!capturedExtensionState) return false;
        previousExtensionState = capturedExtensionState.TakeValue();
        const Status scriptStatus = m_scriptHost->RestoreCheckpoint(
            s.scriptPending, s.scriptActions,
            script::CancellationReason::Rollback);
        if (!scriptStatus) { rollbackPresentation(); return false; }
        const Status extensionStatus =
            m_scriptHost->RestoreExtensionState(s.extensionState);
        if (!extensionStatus) { rollbackPresentation(); return false; }
    }
    if (!m_session->CommitRestore(preparedRuntime.TakeValue())) {
        rollbackPresentation();
        return false;
    }
    m_nvlMode = s.nvlMode;
    m_nvlLines = s.nvlLines;
    m_lastBacklogSequence = m_session->Backlog().LastSequence();
    m_autoMode = m_skipMode = false;
    m_skipDisabled = s.skipDisabled;
    SetStoryAuto(s.storyAuto ? std::optional<StoryAuto>(StoryAuto{
        s.storyAutoSpeedMs, s.storyAutoWaitMs, s.storyAutoLocked}) : std::nullopt);
    if (m_ui.CurrentRoute() == "hud" || m_ui.CurrentRoute() == "game")
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

bool PlayerApp::RollbackToBacklogSequence(const std::uint64_t sequence) {
    for (std::size_t i = m_rollback.size(); i > 0; --i) {
        const RollbackEntry& entry = m_rollback[i - 1];
        if (entry.backlogSequence == sequence) {
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
        const std::string title = ProductText(
            m_langTable, "gallery." + cg.id + ".title", cg.title);
        items.push_back({cg.id,
                         unlocked ? title
                                  : ProductText(m_langTable,
                                                "player.gallery.lockedTitle",
                                                "？？？"),
                         unlocked ? ProductText(m_langTable,
                                                "player.gallery.unlocked",
                                                "已解鎖")
                                  : ProductText(m_langTable,
                                                "player.gallery.locked",
                                                "尚未解鎖"),
                         unlocked ? thumb : "", !unlocked, "cg.view", cg.image});
    }
    return items;
}

std::vector<ui::GalgameItem> PlayerApp::SaveItems(bool saveMode) {
    std::vector<ui::GalgameItem> items;
    const auto slotItem = [&](int slot, const std::string& action) {
        const progress::SlotInfo info = m_saves.Peek(slot);
        std::string label;
        std::string subtitle;
        std::string image;
        if (info.exists) {
            label = info.chapter.empty()
                        ? ProductText(m_langTable, "player.save.untitled",
                                      "未命名章節")
                        : info.chapter;
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
        // While a copy or move waits for its target, every other slot can
        // receive it, in either screen.
        const bool picking = m_slotOperation && slot != kAutoSaveSlot;
        items.push_back({"slot-" + std::to_string(slot), label, subtitle, image,
                         !picking && ((saveMode && m_appState != AppState::Game) ||
                                      (!saveMode && !info.exists)),
                         action, std::to_string(slot), info.comment,
                         m_slotOperation && m_slotOperation->slot == slot});
    };

    if (!saveMode && m_savePages == 1 && m_saveFirstSlot == 0) {
        slotItem(kAutoSaveSlot, "load.slot");
    }
    for (int row = 0; row < m_saveSlotsPerPage; ++row) {
        const int ordinal = m_saveSlotsPerPage == 8
            ? (row % 2) * 4 + row / 2 : row;
        const int slot = m_saveFirstSlot +
            (m_saveLoadPage - 1) * m_saveSlotsPerPage + ordinal;
        slotItem(slot, saveMode ? "save.slot" : "load.slot");
    }
    return items;
}

std::vector<ui::GalgameItem> PlayerApp::BacklogItems() {
    std::vector<ui::GalgameItem> items;
    const auto& entries = m_session->Backlog().Entries();
    items.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const std::string speaker = entry.isChoice
                                        ? ProductText(m_langTable,
                                                      "player.backlog.choice",
                                                      "▶ 選擇")
                                        : entry.speaker;
        items.push_back({"backlog-" + std::to_string(entry.sequence), speaker.empty() ? entry.text : speaker + "　" + entry.text,
                         entry.voice.empty()
                             ? ""
                             : ProductText(m_langTable,
                                           "player.backlog.replayVoice",
                                           "♪ 點擊重播語音"), "", false,
                         entry.voice.empty() ? "backlog.rollback" : "backlog.voice",
                         std::to_string(entry.sequence)});
    }
    return items;
}

std::vector<ui::BacklogEntryPresentation> PlayerApp::BacklogEntries() {
    std::vector<ui::BacklogEntryPresentation> entries;
    const auto& lines = m_session->Backlog().Entries();
    entries.reserve(lines.size());
    for (const auto& entry : lines)
        entries.push_back({entry.sequence,
                           entry.isChoice ? ProductText(m_langTable, "player.backlog.choice",
                                                        "▶ 選擇")
                                          : entry.speaker,
                           entry.text, !entry.voice.empty(), entry.isChoice});
    return entries;
}

ui::DialoguePresentation PlayerApp::DialogueUI() const {
    ui::DialoguePresentation view;
    const auto& dialogue = m_session->Dialogue().State();
    view.speaker = dialogue.speaker;
    view.text = dialogue.displayText;
    view.fullText = dialogue.fullText;
    // Imported KAG prose retains its literal speaker prefix so existing
    // authored operation identities and saves remain stable. Present that
    // prefix through the SDK HUD's distinct name field, including while the
    // typewriter has only revealed part of the line.
    constexpr std::string_view open = "【";
    constexpr std::string_view close = "】";
    if (view.speaker.empty() && dialogue.fullText.starts_with(open)) {
        const auto end = dialogue.fullText.find(close, open.size());
        if (end != std::string::npos) {
            const auto prefixLength = end + close.size();
            view.speaker = dialogue.fullText.substr(0, prefixLength);
            view.text = dialogue.displayText.size() > prefixLength
                ? dialogue.displayText.substr(prefixLength) : "";
            view.fullText = dialogue.fullText.substr(prefixLength);
        }
    }
    view.chapterTitle = m_session->VM().Chapter();
    const std::string currentBgm=m_session->VM().CurrentBgm();
    if(!currentBgm.empty()){
        const std::string key="bgm."+px::PathStemUtf8(currentBgm);
        if(const auto translated=m_langTable.find(key);translated!=m_langTable.end())
            view.musicTitle=translated->second;
        else
            view.musicTitle=px::PathStemUtf8(currentBgm);
    }
    view.effect = m_session->Dialogue().State().effect;
    view.effectProgress = m_session->Dialogue().State().effectProgress;
    view.choices = m_choiceTexts;
    view.nvlMode = m_nvlMode;
    view.face = MessageFacePath();
    if (!view.speaker.empty() && !dialogue.faceCharacter.empty())
        if (const auto* character = m_catalog.FindCharacter(dialogue.faceCharacter);
            character && character->nameColor.size() >= 7) {
            const auto channel = [&](const std::size_t at) {
                return static_cast<std::uint8_t>(std::stoi(character->nameColor.substr(at, 2), nullptr, 16));
            };
            view.speakerColor = Color{channel(1), channel(3), channel(5),
                                      character->nameColor.size() == 9 ? channel(7) : std::uint8_t{255}};
        }
    view.autoMode = m_autoMode;
    view.skipMode = m_skipMode;
    view.waiting = dialogue.finished && !dialogue.fullText.empty() &&
                   m_choiceTexts.empty() && !m_skipMode;
    view.textScale=m_settings.textScale;view.reducedMotion=m_settings.reducedMotion;
    view.windowOpacity = m_messageWindowOpacity;
    view.dimReadText = m_dimReadText && m_session->VM().CurrentLineSeen();
    for (const auto& line : m_nvlLines) view.nvlLines.push_back(NvlLineText(line.speaker, line.text));
    // The newest line types out like ADV text.
    if (!view.nvlLines.empty() && m_session->Dialogue().State().fullText == m_nvlLines.back().text)
        view.nvlLines.back() = NvlLineText(m_session->Dialogue().State().speaker,
                                           m_session->Dialogue().State().displayText);
    return view;
}

void PlayerApp::SetStoryAuto(std::optional<StoryAuto> value) {
    m_storyAuto = std::move(value);
    m_autoMode = m_storyAuto.has_value();
    if (m_storyAuto) m_skipMode = false;
    m_autoTimerStart = 0;
    if (m_session)
        m_session->VM().SetDefaultTextSpeed(m_storyAuto && m_storyAuto->textSpeedMs >= 0
                                                ? m_storyAuto->textSpeedMs
                                                : m_settings.textSpeedMs);
}

// The speaker's face follows the expression shown on stage, else the
// character's default; a line's `face=` overrides both.
std::string PlayerApp::MessageFacePath() const {
    const auto& state = m_session->Dialogue().State();
    if (m_messageFaceMode == 0 || state.faceCharacter.empty() || state.faceExpression == "none")
        return {};
    const auto& catalog = m_catalog;
    const vn::CatalogCharacter* character = catalog.FindCharacter(state.faceCharacter);
    if (!character) return {};
    const auto onStage = m_session->Stage().CharacterImage(character->id);
    if (m_messageFaceMode == 2 && onStage) return {};
    const auto byId = [&](const std::string& id) -> const vn::CatalogCharacterExpression* {
        for (const auto& expression : character->expressions)
            if (expression.id == id) return &expression;
        return nullptr;
    };
    const vn::CatalogCharacterExpression* expression =
        state.faceExpression.empty() ? nullptr : byId(state.faceExpression);
    if (!expression && state.faceExpression.empty() && onStage) {
        for (const auto& candidate : character->expressions) {
            const std::string& image = candidate.image.lastKnownPath;
            if (!image.empty() && (*onStage == image || onStage->ends_with("/" + image))) {
                expression = &candidate;
                break;
            }
        }
    }
    if (!expression && state.faceExpression.empty()) expression = byId(character->defaultExpression);
    return expression ? expression->facePath : std::string{};
}

Status PlayerApp::DefineDialogData() {
    if (const Status defined = m_ui.DefineUiData("dialog.visible", Variant(false)); !defined)
        return defined;
    if (const Status defined = m_ui.DefineUiData("dialog.skipNext", Variant(false)); !defined)
        return defined;
    for (const std::string& kind : m_dialogKinds)
        if (const Status defined = m_ui.DefineUiData(
                "dialog.kind." + kind, Variant(false)); !defined)
            return defined;
    return Status::Ok();
}

void PlayerApp::PublishDialog() {
    for (const std::string& kind : m_dialogKinds)
        (void)m_ui.UpdateUiData("dialog.kind." + kind,
                                Variant(m_dialog && m_dialog->kind == kind));
    (void)m_ui.UpdateUiData("dialog.skipNext", Variant(m_dialogSkipNext));
    (void)m_ui.UpdateUiData("dialog.visible", Variant(m_dialog.has_value()));
}

// Whether `operation` would show the authored dialog: the project lists it
// and the mounted route has a ConfirmDialog to ask with.
bool PlayerApp::AsksBefore(const std::string_view operation) const {
    return m_confirmations.contains(operation) && m_ui.HasControlNamed("ConfirmDialog");
}

// Runs `accept` now when the project does not ask before `operation`, the
// player chose not to be asked again or the mounted route has no dialog to
// ask with; otherwise shows the dialog and runs it on "yes".
void PlayerApp::RequestConfirm(const std::string_view operation, std::function<void()> accept) {
    if (m_dialog) return;
    const auto confirm = m_confirmations.find(operation);
    if (!AsksBefore(operation) ||
        (!confirm->second.dontAskOption.empty() &&
         OptionValue(confirm->second.dontAskOption) != 0)) {
        accept();
        return;
    }
    m_dialog = PendingDialog{confirm->second.kind, confirm->second.dontAskOption,
                             std::move(accept)};
    m_dialogSkipNext = false;
    PublishDialog();
    // Land the cursor on the yes button when the player asked for it.
    if (m_dialogCursorWarp)
        if (const auto* yes = FindUiControlNamed<ui::Control>(m_ui.Root(), "DialogYes")) {
            const auto area = yes->LayoutRect();
            if (area.w > 0.0f && area.h > 0.0f)
                WarpCursorTo(area.x + area.w / 2.0f, area.y + area.h / 2.0f);
        }
}

int PlayerApp::OptionValue(const std::string_view name) const {
    const auto spec = m_optionSpecs.find(name);
    if (spec == m_optionSpecs.end()) return 0;
    const auto stored = m_settings.options.find(name);
    return stored == m_settings.options.end()
        ? spec->second.def : std::clamp(stored->second, spec->second.min, spec->second.max);
}

void PlayerApp::PublishMuteData() {
    (void)m_ui.UpdateUiData("settings.muted.master", Variant(m_settings.masterMuted));
    (void)m_ui.UpdateUiData("settings.muted.bgm", Variant(m_settings.bgmMuted));
    (void)m_ui.UpdateUiData("settings.muted.se", Variant(m_settings.seMuted));
    (void)m_ui.UpdateUiData("settings.muted.voice", Variant(m_settings.voiceMuted));
    for (const std::string& id : m_voiceProfileIds) {
        const auto found = m_settings.voiceProfileMuted.find(id);
        (void)m_ui.UpdateUiData("settings.profileMuted." + std::string(id),
            Variant(found != m_settings.voiceProfileMuted.end() && found->second));
    }
}

void PlayerApp::UpdateCursorVisibility(const std::uint64_t now) {
    auto& input = m_runtime.GetInput();
    if (input.MouseX() != m_lastMouseX || input.MouseY() != m_lastMouseY ||
        input.LeftClick() || input.RightClick()) {
        m_lastMouseX = input.MouseX();
        m_lastMouseY = input.MouseY();
        m_cursorActiveAt = now;
    }
    const int hideAfter = m_cursorHideDelayMs;
    const bool hide = hideAfter > 0 && now - m_cursorActiveAt >= static_cast<std::uint64_t>(hideAfter);
    if (hide != m_cursorHidden) {
        m_cursorHidden = hide;
        if (hide) SDL_HideCursor(); else SDL_ShowCursor();
    }
}

void PlayerApp::WarpCursorTo(const float logicalX, const float logicalY) {
    int logicalW = 0, logicalH = 0, windowW = 0, windowH = 0;
    m_runtime.Renderer().GetLogicalSize(logicalW, logicalH);
    SDL_Window* window = m_runtime.GetWindow().Handle();
    if (logicalW <= 0 || logicalH <= 0 || !SDL_GetWindowSize(window, &windowW, &windowH)) return;
    SDL_WarpMouseInWindow(window, logicalX * static_cast<float>(windowW) / static_cast<float>(logicalW),
                          logicalY * static_cast<float>(windowH) / static_cast<float>(logicalH));
}

void PlayerApp::PublishDock() {
    const auto& input = m_runtime.GetInput();
    // An unpinned toolbar shows while the pointer is over the HUD control
    // named DockArea (usually an invisible rectangle), or player.dockArea.
    bool valid = m_dockArea.valid;
    float x = m_dockArea.x, y = m_dockArea.y, width = m_dockArea.width, height = m_dockArea.height;
    if (!m_dockPinned)
        if (const auto* region = FindUiControlNamed<ui::Control>(m_ui.Root(), "DockArea")) {
            const Rect rect = region->LayoutRect();
            valid = true;
            x = rect.x; y = rect.y; width = rect.w; height = rect.h;
        }
    const bool inside = valid &&
        input.MouseX() >= x && input.MouseX() < x + width &&
        input.MouseY() >= y && input.MouseY() < y + height;
    const bool shown = m_dockPinned || !m_choiceTexts.empty() || inside;
    if (shown == m_dockShown) return;
    m_dockShown = shown;
    (void)m_ui.UpdateUiData("hud.dockShown", Variant(shown));
}

Status PlayerApp::DefineOptionData() {
    if (const Status defined = m_ui.DefineUiData("hud.dockShown", Variant(true)); !defined)
        return defined;
    for (const std::string_view bus : {"master", "bgm", "se", "voice"})
        if (const Status defined = m_ui.DefineUiData(
                "settings.muted." + std::string(bus), Variant(false)); !defined)
            return defined;
    for (const std::string& id : m_voiceProfileIds)
        if (const Status defined = m_ui.DefineUiData(
                "settings.profileMuted." + std::string(id), Variant(false)); !defined)
            return defined;
    for (const auto& [name, spec] : m_optionSpecs) {
        const std::string path = "options." + name;
        const int value = OptionValue(name);
        if (const Status defined = m_ui.DefineUiData(path, Variant(std::to_string(value))); !defined)
            return defined;
        for (const int choice : spec.choices)
            if (const Status defined = m_ui.DefineUiData(
                    path + ".is" + std::to_string(choice), Variant(choice == value)); !defined)
                return defined;
    }
    return Status::Ok();
}

void PlayerApp::PublishOption(const std::string_view name) {
    const auto spec = m_optionSpecs.find(name);
    if (spec == m_optionSpecs.end()) return;
    const std::string path = "options." + std::string(name);
    const int value = OptionValue(name);
    (void)m_ui.UpdateUiData(path, Variant(std::to_string(value)));
    for (const int choice : spec->second.choices)
        (void)m_ui.UpdateUiData(path + ".is" + std::to_string(choice), Variant(choice == value));
}

bool PlayerApp::SetOption(const std::string_view name, int value) {
    const auto spec = m_optionSpecs.find(name);
    if (spec == m_optionSpecs.end()) return false;
    value = std::clamp(value, spec->second.min, spec->second.max);
    if (!spec->second.choices.empty() &&
        std::ranges::find(spec->second.choices, value) == spec->second.choices.end())
        return false;
    if (value == spec->second.def) {
        if (const auto stored = m_settings.options.find(name); stored != m_settings.options.end())
            m_settings.options.erase(stored);
    } else {
        m_settings.options[std::string(name)] = value;
    }
    PublishOption(name);
    ApplyOption(name, value);
    return true;
}

std::optional<PlayerApp::OptionBinding> PlayerApp::ParseOptionBinding(const std::string_view name) {
    static constexpr std::pair<std::string_view, OptionBinding> kBindings[] = {
        {"textSpeed", OptionBinding::TextSpeed},
        {"autoWait", OptionBinding::AutoWait},
        {"skipUnread", OptionBinding::SkipUnread},
        {"messageWindowOpacity", OptionBinding::MessageWindowOpacity},
        {"dimReadText", OptionBinding::DimReadText},
        {"uiEffects", OptionBinding::UiEffects},
        {"font", OptionBinding::Font},
        {"keepSkipAfterChoice", OptionBinding::KeepSkipAfterChoice},
        {"keepAutoAfterChoice", OptionBinding::KeepAutoAfterChoice},
        {"cursorHideDelay", OptionBinding::CursorHideDelay},
        {"dialogCursorWarp", OptionBinding::DialogCursorWarp},
        {"dockPinned", OptionBinding::DockPinned},
        {"messageFace", OptionBinding::MessageFace},
    };
    for (const auto& [key, binding] : kBindings)
        if (key == name) return binding;
    return std::nullopt;
}

// Hands a bound option to the behaviour it names, as `value * scale + offset`
// in that behaviour's unit; switches are on when the result is non-zero.
void PlayerApp::ApplyOption(const std::string_view name, const int value) {
    const auto spec = m_optionSpecs.find(name);
    if (spec == m_optionSpecs.end() || spec->second.binding == OptionBinding::None) return;
    const double mapped = value * spec->second.scale + spec->second.offset;
    const int bound = static_cast<int>(std::clamp(std::round(mapped), -1.0e9, 1.0e9));
    const bool on = bound != 0;
    switch (spec->second.binding) {
    case OptionBinding::TextSpeed:
        // Milliseconds per character; 0 shows the line at once.
        m_settings.textSpeedMs = std::clamp(bound, 0, 120);
        if (m_session) m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
        break;
    case OptionBinding::AutoWait:
        m_settings.autoWaitMs = std::clamp(bound, 0, 60'000);
        break;
    case OptionBinding::SkipUnread:
        m_settings.skipReadOnly = !on;
        (void)m_ui.UpdateUiData("settings.skipReadOnly", Variant(!on));
        (void)m_ui.UpdateUiData("settings.skipAll", Variant(on));
        break;
    case OptionBinding::MessageWindowOpacity:
        m_messageWindowOpacity = static_cast<float>(std::clamp(bound, 0, 100)) / 100.0f;
        break;
    case OptionBinding::DimReadText: m_dimReadText = on; break;
    case OptionBinding::UiEffects: m_uiEffects = on; break;
    case OptionBinding::Font:
        // 1-based index into `player.fonts`; anything else keeps the theme font.
        if (bound >= 1 && static_cast<std::size_t>(bound) <= m_fontChoices.size())
            (void)m_ui.SetDefaultFont(m_fontChoices[static_cast<std::size_t>(bound) - 1]);
        break;
    case OptionBinding::KeepSkipAfterChoice: m_keepSkipAfterChoice = on; break;
    case OptionBinding::KeepAutoAfterChoice: m_keepAutoAfterChoice = on; break;
    case OptionBinding::CursorHideDelay:
        m_cursorHideDelayMs = std::clamp(bound, 0, 3'600'000);
        break;
    case OptionBinding::DialogCursorWarp: m_dialogCursorWarp = on; break;
    case OptionBinding::DockPinned: m_dockPinned = on; break;
    case OptionBinding::MessageFace: m_messageFaceMode = std::clamp(bound, 0, 2); break;
    case OptionBinding::None: break;
    }
}

void PlayerApp::ApplyAllOptions() {
    for (const auto& [name, spec] : m_optionSpecs) {
        PublishOption(name);
        ApplyOption(name, OptionValue(name));
    }
}

ui::SettingsPresentation PlayerApp::SettingsUI() const {
    ui::SettingsPresentation presentation;
    presentation.master = m_settings.masterVolume;
    presentation.bgm = m_settings.bgmVolume;
    presentation.se = m_settings.seVolume;
    presentation.voice = m_settings.voiceVolume;
    presentation.voiceProfiles = m_settings.voiceProfileVolumes;
    presentation.voiceProfileMuted = m_settings.voiceProfileMuted;
    presentation.masterMuted = m_settings.masterMuted;
    presentation.bgmMuted = m_settings.bgmMuted;
    presentation.seMuted = m_settings.seMuted;
    presentation.voiceMuted = m_settings.voiceMuted;
    presentation.duckBgmOnVoice = m_settings.duckBgmOnVoice;
    presentation.continueVoiceOnAdvance = m_settings.continueVoiceOnAdvance;
    presentation.textSpeedMs = m_settings.textSpeedMs;
    presentation.skipReadOnly = m_settings.skipReadOnly;
    presentation.fullscreen = m_settings.fullscreen;
    presentation.textScale = m_settings.textScale;
    presentation.highContrast = m_settings.highContrast;
    presentation.reducedMotion = m_settings.reducedMotion;
    presentation.selfVoicing = m_settings.selfVoicing;
    presentation.language = m_settings.language;
    presentation.languages = m_supportedLocales;
    return presentation;
}

void PlayerApp::OpenScreen(const std::string& route) {
    // Slot pages list files on disk; a pending autosave must land first.
    HarvestBackgroundSave(true);
    std::string target=route;if(target=="saveload")target="load";
    const auto current = m_session->Routes().CurrentModalRoute();
    const auto extraMode = [](const std::string_view value) {
        return value == "gallery" || value == "replay" || value == "music";
    };
    if (extraMode(target) && extraMode(current)) {
        if (current == target) return;
        if (current == "music" && m_musicTrackSelected && !TitleBgmPath().empty()) {
            m_runtime.Audio().PlayBGM(TitleBgmPath(), true, 300);
            m_musicTrackSelected = false;
        }
        if (!m_session->Routes().CloseModal()) return;
    }
    if ((target == "save" || target == "load") &&
        (current == "save" || current == "load") && current != target) {
        if (!m_session->Routes().CloseModal()) return;
    }
    if (target == "replay") {
        m_replayPage = 1;
        for (int page = 1; page <= 4; ++page)
            (void)m_ui.UpdateUiData("replay.page" + std::to_string(page),
                                    Variant(page == 1));
    }
    if (target == "save" || target == "load") {
        // A copy, move or comment edit does not outlive its screen.
        m_slotOperation.reset();
        if (m_commentSlot) FinishSlotComment(false);
        for (const char* field : {"visible", "copy", "move"})
            (void)m_ui.UpdateUiData(std::string("saveLoad.pending.") + field, Variant(false));
        (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(false));
        for (int page = 1; page <= m_savePages; ++page)
            (void)m_ui.UpdateUiData("saveLoad.page" + std::to_string(page),
                                    Variant(page == m_saveLoadPage));
    }
    if (target == "settings" && m_boot.routeScenes.contains("settings")) {
        for (int page = 1; page <= 4; ++page)
            (void)m_ui.UpdateUiData("settings.page" + std::to_string(page),
                                    Variant(page == m_settingsPage));
    }
    if (target == "save" && m_ui.CurrentScreen() != ui::GalgameUI::Screen::Save)
        m_beforeSavePresentation = SavePresentationState{
            m_ui.CaptureRuntimeState(), m_session->Routes().CaptureState()};
    const Status routed=m_session->Routes().ShowModal(target);if(!routed){for(const auto& diagnostic:routed.Diagnostics())diag::Emit(diagnostic);return;}
}

void PlayerApp::BeginRouteVisualPreload(const std::string_view route) {
    if (m_routePreloadBatch) {
        (void)m_runtime.Assets().CancelTexturePreload(m_routePreloadBatch);
        m_routePreloadBatch = 0;
        m_routePreloadEffect = 0;
    }
    if (m_routePreloadHold) {
        m_runtime.Renderer().EndPresentationHold();
        m_routePreloadHold = false;
    }
    m_routePreloadPaths.clear();
    const auto& transition = m_session->Routes().ActiveTransition();
    if (!transition)
        m_routePreloadHold = m_runtime.Renderer().BeginPresentationHold();
    if (!m_boot.routeScenes.contains(std::string(route))) return;
    auto images = m_ui.RouteImageDependencies(route);
    if (!images) {
        for (const auto& diagnostic : images.Diagnostics()) diag::Emit(diagnostic);
        return;
    }
    m_routePreloadPaths = images.TakeValue();
}

void PlayerApp::FinishRouteVisualPreload(const std::string_view route) {
    const auto& transition = m_session->Routes().ActiveTransition();
    if (!transition && !m_routePreloadHold) return;
    int width = 0, height = 0;
    m_runtime.Renderer().GetLogicalSize(width, height);
    px::Input neutral;
    neutral.InjectFrame(-1000.0f, -1000.0f, false);
    (void)m_ui.Update(neutral, width, height, 0.0f);
    auto mounted = m_ui.MountedImageDependencies();
    m_routePreloadPaths.insert(m_routePreloadPaths.end(),
                               mounted.begin(), mounted.end());
    std::set<std::string> seen;
    std::erase_if(m_routePreloadPaths, [&seen](const std::string& path) {
        return path.empty() || !seen.insert(path).second;
    });
    if (m_routePreloadPaths.empty()) {
        if (m_routePreloadHold) {
            m_runtime.Renderer().EndPresentationHold();
            m_routePreloadHold = false;
        }
        return;
    }
    std::vector<graphics::AssetPreloadRequest> requests;
    requests.reserve(m_routePreloadPaths.size());
    for (const auto& path : m_routePreloadPaths)
        requests.push_back({path, graphics::AssetPreloadPriority::Critical});
    m_routePreloadPaths.clear();
    const auto batch = m_runtime.Assets().BeginTexturePreload(std::move(requests));
    const auto progress = m_runtime.Assets().TexturePreloadProgress(batch);
    if (progress.Finished()) {
        if (m_routePreloadHold) {
            m_runtime.Renderer().EndPresentationHold();
            m_routePreloadHold = false;
        }
        return;
    }
    if (transition &&
        !m_runtime.Renderer().PauseScreenEffect(transition->handle, true))
        return;
    m_routePreloadBatch = batch;
    m_routePreloadEffect = transition ? transition->handle : 0;
    PX_LOG_INFO("Player route visuals preloading route={} images={}",
                route, progress.total);
}

void PlayerApp::UpdateRouteVisualPreload() {
    if (!m_routePreloadBatch) return;
    if ((m_routePreloadEffect &&
         !m_runtime.Renderer().ScreenEffectPlaying(m_routePreloadEffect)) ||
        (m_routePreloadHold &&
         !m_runtime.Renderer().PresentationHoldActive())) {
        (void)m_runtime.Assets().CancelTexturePreload(m_routePreloadBatch);
        m_routePreloadBatch = 0;
        m_routePreloadEffect = 0;
        m_routePreloadHold = false;
        return;
    }
    const auto progress = m_runtime.Assets().TexturePreloadProgress(m_routePreloadBatch);
    if (progress.valid && !progress.Finished()) return;
    if (m_routePreloadEffect)
        (void)m_runtime.Renderer().PauseScreenEffect(m_routePreloadEffect, false);
    if (m_routePreloadHold) m_runtime.Renderer().EndPresentationHold();
    PX_LOG_INFO("Player route visuals ready loaded={} failed={}",
                progress.succeeded, progress.failed);
    m_routePreloadBatch = 0;
    m_routePreloadEffect = 0;
    m_routePreloadHold = false;
}

void PlayerApp::PresentRoute(const std::string& route,
                             const std::string& /*operation*/) {
    std::string target=route;if(target=="saveload")target="load";
    BeginRouteVisualPreload(target);
    (void)m_ui.SetTitleConfirmation(false);
    m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
    (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
    m_pendingSaveLoadSlot.reset();
    const auto present = [this](const Status& status) {
        if (status) return true;
        for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
        m_quitRequested = true;
        return false;
    };
    if (target == "gallery") {
        auto items = GalleryItems();
        const auto unlocked = static_cast<std::size_t>(std::count_if(
            items.begin(), items.end(), [](const ui::GalgameItem& item) {
                return !item.disabled;
            }));
        const Status status = m_ui.ShowGallery(std::move(items));
        if (!present(status)) return;
        PX_LOG_INFO(
            "Player gallery presented total={} unlocked={} base={} modal={} ui={}",
            m_catalog.Gallery().size(), unlocked,
            m_session->Routes().CurrentRoute(),
            m_session->Routes().CurrentModalRoute(), m_ui.CurrentRoute());
    }
    else if (target == "save") (void)present(m_ui.ShowSaveLoad(true, SaveItems(true)));
    else if (target == "load") (void)present(m_ui.ShowSaveLoad(false, SaveItems(false)));
    else if (target == "settings") (void)present(m_ui.ShowSettings(SettingsUI()));
    else if (target == "backlog")
        (void)present(m_ui.ShowBacklog(BacklogEntries(), BacklogItems()));
    else if (target == "title") {
        const bool fromBootSplash = m_appState == AppState::BootSplash;
        m_appState = AppState::Title;
        if (!present(m_ui.ShowTitle())) return;
        // Initial boot starts the title music after its visual preload. Every
        // later return must restore it, including returns from a game menu or
        // from a Music screen that selected a different track.
        if (!fromBootSplash && !TitleBgmPath().empty()) {
            const auto music = m_runtime.Audio().CaptureState().music;
            if (!music.playing || music.path != TitleBgmPath())
                (void)m_runtime.Audio().PlayBGM(TitleBgmPath(), true, 300);
            m_musicTrackSelected = false;
        }
        if (!m_titleVoices.empty()) {
            const auto tick = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            const std::string& voice = m_titleVoices[
                static_cast<std::size_t>(tick) % m_titleVoices.size()];
            m_runtime.Audio().PlayVoice(voice);
            PX_LOG_INFO("Player title system voice started path={}", voice);
        }
    }
    else if (target == "hud" || target == "game") { m_appState = AppState::Game; (void)present(m_ui.ShowHUD(DialogueUI())); }
    else {
        if (!present(m_ui.ShowRoute(target))) return;
        if (target == "flowchart" || target.starts_with("flowchart-")) {
            if (auto* jump = FindUiControlNamed<ui::Button>(
                    m_ui.Root(), "scenejumpButton"))
                jump->SetEnabled(!m_selectedFlowEntry.empty());
        }
        if (target == "music" || target == "extra") {
            if (auto* volume = FindUiControlNamed<ui::Slider>(
                    m_ui.Root(), "MusicVolume")) {
                volume->SetRange(0, 128, 4);
                volume->SetValue(m_settings.bgmVolume);
                volume->SetOnChanged([this](const double value) {
                    m_settings.bgmVolume = std::clamp(
                        static_cast<int>(std::lround(value)), 0, 128);
                    m_runtime.Audio().SetBGMVolume(m_settings.bgmVolume);
                });
            }
        }
    }
    if (!m_quitRequested) FinishRouteVisualPreload(target);
}

void PlayerApp::RenderStageWithScripts() {
    const diag::ScopedFramePhase profiled(diag::FramePhase::StageRender);
    if (m_scriptHost)
        m_scriptHost->RenderRetained(script::RenderPhase::BeforeStage);
    m_session->Stage().Render();
    if (m_scriptHost)
        m_scriptHost->RenderRetained(script::RenderPhase::AfterStage);
    QaStageRendered();
}

void PlayerApp::RenderUIWithScripts(const float opacity) {
    const diag::ScopedFramePhase profiled(diag::FramePhase::UiRender);
    if (m_scriptHost)
        m_scriptHost->RenderRetained(script::RenderPhase::BeforeUI);
    float shakeX = 0.0f;
    float shakeY = 0.0f;
    if (m_windowShakeDuration > 0.0f &&
        (m_ui.CurrentRoute() == "hud" || m_ui.CurrentRoute() == "game")) {
        const float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_windowShakeStart).count();
        if (elapsed >= m_windowShakeDuration) {
            m_windowShakeDuration = 0.0f;
        } else if (m_windowQuake) {
            std::tie(shakeX, shakeY) = vn::SampleQuake(*m_windowQuake, elapsed);
        } else {
            const float amplitude = m_windowShakeAmplitude *
                (1.0f - elapsed / m_windowShakeDuration);
            const float phase = elapsed * 55.0f;
            shakeX = std::sin(phase) * amplitude;
            shakeY = std::cos(phase * 1.3f) * amplitude * 0.6f;
        }
    }
    vn::ActorPose pose;
    if (m_windowAction &&
        (m_ui.CurrentRoute() == "hud" || m_ui.CurrentRoute() == "game")) {
        bool finished = false;
        pose = vn::SampleActorAction(
            *m_windowAction,
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          m_windowActionStart).count(),
            finished);
        if (finished) {
            m_windowAction.reset();
            pose = {};
        }
    }
    const bool shaking = shakeX != 0.0f || shakeY != 0.0f || pose.x != 0.0f ||
                         pose.y != 0.0f || pose.scale != 1.0f || pose.rotation != 0.0f ||
                         pose.scaleY != 1.0f;
    if (shaking) {
        // Actions scale and rotate about the bottom centre, where the message
        // window sits.
        int width = 0, height = 0;
        m_runtime.Renderer().GetLogicalSize(width, height);
        const float pivotX = static_cast<float>(width) * 0.5f;
        const float pivotY = static_cast<float>(height);
        const float radians = pose.rotation * 0.017453292519943295f;
        const float a = std::cos(radians) * pose.scale;
        const float b = std::sin(radians) * pose.scale;
        const float c = -std::sin(radians) * pose.scale * pose.scaleY;
        const float d = std::cos(radians) * pose.scale * pose.scaleY;
        m_runtime.Renderer().PushAffineTransform(
            {a, b, c, d, pivotX - (a * pivotX + c * pivotY) + pose.x + shakeX,
             pivotY - (b * pivotX + d * pivotY) + pose.y + shakeY});
    }
    const bool faded = opacity < 0.999f;
    if (faded) {
        const auto alpha = static_cast<std::uint8_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
        m_runtime.Renderer().PushTransform({0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f,
                                           Color{255, 255, 255, alpha});
    }
    m_ui.Render(m_runtime.Renderer());
    if (faded) m_runtime.Renderer().PopTransform();
    if (shaking) m_runtime.Renderer().PopTransform();
    if (m_scriptHost)
        m_scriptHost->RenderRetained(script::RenderPhase::AfterUI);

}

void PlayerApp::ScreensFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0; m_runtime.Renderer().GetLogicalSize(w, h);
    if (m_routePreloadBatch) {
        px::Input neutral;
        neutral.InjectFrame(-1000.0f, -1000.0f, false);
        (void)m_ui.Update(neutral, w, h, 0.0f);
    } else {
        (void)m_ui.Update(input, w, h, dt);
    }
    if (m_appState == AppState::Game) RenderStageWithScripts();
    RenderUIWithScripts();
}

void PlayerApp::TitleFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0; m_runtime.Renderer().GetLogicalSize(w, h);
    (void)m_ui.Update(input, w, h,dt);
    RenderUIWithScripts();

}

bool PlayerApp::VideoFrame(float dt) {
    const auto restorePresentation = [this] {
        if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) return;
        const auto& routes = m_session->Routes();
        const std::string route(routes.CurrentModalRoute().empty()
                                    ? routes.CurrentRoute()
                                    : routes.CurrentModalRoute());
        // Video temporarily replaces the UI surface without changing the
        // route stack. Restore that route before the VM resumes, so subsequent
        // Story commands can still choose a different presentation.
        PresentRoute(route, "video-ended");
        PX_LOG_INFO("Player video presentation restored route={}", route);
    };
    const bool vmWaiting =
        m_session->VM().State() == vn::VMState::WaitingVideo;
    if (!vmWaiting && m_scriptVideoHandle == 0) {
        restorePresentation();
        return false;
    }
    px::Input& input = m_runtime.GetInput();

    if (!m_pendingVideo.empty()) {
        diag::FrameProfiler::Instance().Note("video-open");
        const float volume = static_cast<float>(m_settings.bgmVolume) / 128.0f;
        if (!m_video->OpenAsync(m_pendingVideo, volume)) {
            m_pendingVideo.clear();
            restorePresentation();
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

    m_video->Update(dt);
    // Text skip (including held Ctrl) waits for movies to finish. Only a
    // fresh advance/cancel action skips a movie that permits manual skipping.
    const bool skipRequested =
        m_videoSkippable &&
        (input.ActionPressed(InputAction::Advance) ||
         input.ActionPressed(InputAction::Cancel));
    if (skipRequested) m_video->Skip();

    const video::PlaybackState state = m_video->State();
    if (state == video::PlaybackState::Finished ||
        state == video::PlaybackState::Stopped ||
        state == video::PlaybackState::Failed ||
        state == video::PlaybackState::Idle) {
        if (m_scriptVideoHandle != 0) {
            std::string status = "stopped";
            if (state == video::PlaybackState::Finished) status = "completed";
            else if (state == video::PlaybackState::Failed) status = "failed";
            RecordScriptVideoTerminal(
                m_scriptVideoHandle, std::move(status),
                state == video::PlaybackState::Failed ? m_video->LastError()
                                                      : std::string{});
            m_scriptVideoHandle = 0;
        }
        m_video->Close();
        restorePresentation();
        if (vmWaiting) m_session->VM().NotifyVideoDone();
        return false;
    }

    int w = 0, h = 0;
    m_runtime.Renderer().GetLogicalSize(w, h);
    m_runtime.Renderer().DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
                                  Color{ 0, 0, 0, 255 });
    m_video->Render(w, h);
    if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) m_ui.ShowVideoOverlay(m_videoSkippable);
    (void)m_ui.Update(input, w, h,dt);
    RenderUIWithScripts();
    return true;
}

void PlayerApp::ProcessScriptSafePoint() {
    // Loads are exclusive: preserving their restored continuation and route
    // state takes precedence over a save requested by the outgoing state.
    if (m_pendingScriptLoad) {
        const int slot = *m_pendingScriptLoad;
        m_pendingScriptLoad.reset();
        (void)LoadSlot(slot);
        return;
    }
    if (m_pendingScriptSave) {
        const int slot = *m_pendingScriptSave;
        m_pendingScriptSave.reset();
        (void)SaveSlot(
            slot, graphics::CaptureThumbnailPng(
                      m_runtime.Renderer().Handle(), 256, 144));
    }
}

void PlayerApp::UpdateScriptScheduler(const float dt) {
    const diag::ScopedFramePhase profiled(diag::FramePhase::Script);
    if (m_scriptHost) {
        script::AdvanceScriptFrame(
            *m_scriptHost, dt,
            [this] { ProcessScriptSafePoint(); },
            [this] {
                if (m_session->VM().State() == vn::VMState::WaitingExternal)
                    m_session->VM().NotifyExternalDone();
            });
        return;
    }
    ProcessScriptSafePoint();
}

void PlayerApp::GameFrame(float dt, std::uint64_t now) {
    px::Input& input = m_runtime.GetInput();

    if (m_dialog && m_session->Routes().CurrentModalRoute().empty()) {
        // A confirmation is modal: only its own buttons, Escape and a right
        // click answer it, and the story waits behind it.
        if (input.RightClick() || input.KeyPressed(SDL_SCANCODE_ESCAPE))
            HandleUIAction({"dialog.cancel", {}});
        if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::HUD)
            m_ui.RefreshHUD(DialogueUI());
        int dialogW = 0, dialogH = 0;
        m_runtime.Renderer().GetLogicalSize(dialogW, dialogH);
        (void)m_ui.Update(input, dialogW, dialogH, dt);
        RenderStageWithScripts();
        RenderUIWithScripts();
        return;
    }

    // A locked [auto on lock=true] section plays by itself: no input, no menus.
    const bool locked = m_storyAuto && m_storyAuto->locked;
    if (m_dialog == std::nullopt && !locked) {
        if (input.ActionPressed(InputAction::QuickLoad))
            RequestConfirm("quickLoad", [this] { LoadSlot(0); });
        else if (input.ActionPressed(InputAction::QuickSave))
            RequestConfirm("quickSave", [this] { m_saveRequested = true; });
    }
    if (!locked && input.ActionPressed(InputAction::OpenSave)) m_pendingSaveScreen = true;

    // Mode toggles (KAG conventions: A=auto, S=skip toggle, Ctrl=skip held).
    if (!locked && !m_storyAuto && input.ActionPressed(InputAction::ToggleAuto)) {
        m_autoMode = !m_autoMode;
        if (m_autoMode) m_skipMode = false;
    }
    if (!locked && !m_skipDisabled && input.ActionPressed(InputAction::ToggleSkip)) {
        m_skipMode = !m_skipMode;
        if (m_skipMode) m_autoMode = m_storyAuto.has_value();
    }
    if (!locked && input.ActionPressed(InputAction::Backlog)) {
        OpenScreen("backlog");
        return;
    }
    if (!locked && input.ActionPressed(InputAction::Rollback)) {
        RollbackOneLine();
    }
    // A right click hides the message window unless player.navigation asks
    // for nothing or for a menu instead.
    const bool rightClickHides = m_navigation.RightClickHidesHud();
    if (!locked && m_storyHudVisible &&
        ((rightClickHides && input.RightClick()) || input.ActionPressed(InputAction::ToggleUi))) {
        m_hudHidden = !m_hudHidden;
    }
    if (!locked && !m_textInput && input.RightClick() &&
        m_navigation.gameRightClick.kind == NavigationTarget::Kind::Route) {
        OpenScreen(m_navigation.gameRightClick.route);
        return;
    }

    const auto syncChoiceTexts = [this] {
        m_choiceTexts.clear();
        if (m_session->VM().State() == vn::VMState::WaitingChoice) {
            for (const auto& choice : m_session->VM().Choices())
                m_choiceTexts.push_back(choice.text);
            // Choices live on the message window, so they bring it back.
            m_storyHudVisible = true;
            m_hudHidden = false;
            if (!m_choiceWasPending) {
                // Config: keep skipping/auto-reading after the choice is made.
                m_resumeSkipAfterChoice = m_skipMode && m_keepSkipAfterChoice;
                m_resumeAutoAfterChoice = m_autoMode && m_keepAutoAfterChoice;
            }
            m_choiceWasPending = true;
            // A choice ends a Story auto section (Artemis autoskip_stop), so a
            // locked one can never leave the choice unanswerable.
            if (m_storyAuto) SetStoryAuto(std::nullopt);
            m_skipMode = false;  // Skip always stops at choices.
            m_autoMode = false;
        } else if (m_choiceWasPending) {
            m_choiceWasPending = false;
            if (m_resumeSkipAfterChoice) m_skipMode = true;
            else if (m_resumeAutoAfterChoice) m_autoMode = true;
            m_resumeSkipAfterChoice = m_resumeAutoAfterChoice = false;
        }
    };
    syncChoiceTexts();

    // Enter/Space advance like a click (KAG convention). Alt+Enter stays the
    // fullscreen toggle handled in MainLoop.
    const bool advanceKey =
        input.ActionPressed(InputAction::Advance) &&
        !input.KeyDown(SDL_SCANCODE_LALT) &&
        !input.KeyDown(SDL_SCANCODE_RALT);
    const bool nonKeyboardAdvance =
        input.ActionPressedWithoutKeyboard(InputAction::Advance);
    const auto advanceDialogue = [this] {
        if (!m_settings.continueVoiceOnAdvance &&
            m_session->VM().State() == vn::VMState::WaitingClick &&
            m_session->Dialogue().Finished())
            m_runtime.Audio().StopVoice();
        const std::uint64_t started = SDL_GetTicksNS();
        m_session->Advance();
        if (const std::uint64_t ms = (SDL_GetTicksNS() - started) / 1'000'000;
            ms >= 4 && diag::FrameProfiler::Instance().Enabled())
            diag::FrameProfiler::Instance().Note("advance:" + std::to_string(ms) + "ms");
    };

    bool advancedByClick = false;
    if (locked) {
        // Keep the presentation current; the section advances by itself.
        if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::HUD)
            m_ui.RefreshHUD(DialogueUI());
    } else if (!m_storyHudVisible && m_session->Routes().CurrentModalRoute().empty()) {
        if ((input.LeftClick() || advanceKey || nonKeyboardAdvance) &&
            m_session->VM().State() != vn::VMState::WaitingChoice) {
            advanceDialogue();
            advancedByClick = true;
            // A Story-driven auto section keeps reading after a click.
            m_autoMode = m_storyAuto.has_value();
            m_skipMode = false;
        }
    } else if (m_hudHidden) {
        if (input.LeftClick() || advanceKey || nonKeyboardAdvance) m_hudHidden = false;
    } else {
        // A modal may have replaced the HUD since the previous frame. Keep
        // its authored scene installed while still updating dialogue when the
        // HUD itself is active.
        if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::HUD)
            m_ui.RefreshHUD(DialogueUI());
        int uiW = 0, uiH = 0; m_runtime.Renderer().GetLogicalSize(uiW, uiH);
        const bool uiConsumed = m_ui.Update(input, uiW, uiH,dt);
        // UI actions may select a branch synchronously. Do not render the
        // previous choice snapshot for the remainder of this frame.
        syncChoiceTexts();
        if (!m_session->Routes().CurrentModalRoute().empty()) {
            RenderStageWithScripts();
            RenderUIWithScripts();
            return;
        }
        if (!uiConsumed && !m_textInput && (input.LeftClick() || advanceKey || nonKeyboardAdvance) &&
               m_session->VM().State() != vn::VMState::WaitingChoice) {
            advanceDialogue();
            advancedByClick = true;
            m_autoMode = m_storyAuto.has_value();
            m_skipMode = false;
        }
    }

    // Skip-read-only (既讀スキップ): S-toggle skip stops at unread text;
    // holding Ctrl always force-skips.
    const bool ctrlSkip = input.KeyDown(SDL_SCANCODE_LCTRL) || input.KeyDown(SDL_SCANCODE_RCTRL);
    if (m_skipMode && m_settings.skipReadOnly && !m_session->VM().CurrentLineSeen()) {
        m_skipMode = false;
    }
    const bool skipping = !locked && !m_skipDisabled && (m_skipMode || ctrlSkip);
    m_session->VM().SetFastForward(skipping);
    // Skip passes over a Story command that holds the Story: the command's
    // handler is cancelled ("skip") and the Story moves on.
    if (skipping && m_scriptHost &&
        m_session->VM().State() == vn::VMState::WaitingExternal &&
        m_scriptHost->CancelPendingCommands(script::CancellationReason::Skip) &&
        !m_scriptHost->HasPendingCommand() && !m_scriptHost->HasPendingAction())
        m_session->VM().NotifyExternalDone();
    const auto skippableWait = [this] {
        return m_session->VM().State() == vn::VMState::WaitingClick ||
               m_session->VM().State() == vn::VMState::WaitingTimer;
    };
    if (skipping && !advancedByClick && skippableWait()) {
        // Skip is not bound to one line per displayed frame. Advance through
        // as many lines as fit a small slice of the frame; only the last one
        // is ever presented, and fast-forward keeps voices and transitions
        // from being started for lines nobody sees.
        constexpr int kMaxSkipLinesPerFrame = 12;
        constexpr std::uint64_t kSkipBudgetNs = 4'000'000;
        const diag::ScopedFramePhase profiled(diag::FramePhase::Logic);
        const std::uint64_t skipStart = SDL_GetTicksNS();
        for (int line = 0; line < kMaxSkipLinesPerFrame; ++line) {
            if (!m_session->Dialogue().Finished()) m_session->Dialogue().ShowAll();
            advanceDialogue();
            if (!skippableWait() ||
                !m_session->Routes().CurrentModalRoute().empty() ||
                SDL_GetTicksNS() - skipStart >= kSkipBudgetNs)
                break;
            if (!ctrlSkip && m_settings.skipReadOnly &&
                !m_session->VM().CurrentLineSeen()) {
                m_skipMode = false;
                break;
            }
        }
        m_autoTimerStart = 0;
    } else if (m_autoMode && m_session->VM().State() == vn::VMState::WaitingClick &&
               m_session->Dialogue().Finished()) {
        if (m_autoTimerStart == 0) {
            m_autoTimerStart = now;
        } else if (now - m_autoTimerStart >= static_cast<std::uint64_t>(std::max(0,
                       m_storyAuto && m_storyAuto->waitMs >= 0 ? m_storyAuto->waitMs
                                                               : m_settings.autoWaitMs))) {
            advanceDialogue();
            m_autoTimerStart = 0;
        }
    } else {
        m_autoTimerStart = 0;
    }

    {
        const diag::ScopedFramePhase profiled(diag::FramePhase::Logic);
        m_session->Update(now, dt);
    }
    const std::string& storyBgm = m_session->VM().CurrentBgm();
    if (storyBgm != m_lastStoryBgm) {
        m_lastStoryBgm = storyBgm;
        if (!storyBgm.empty() &&
            std::ranges::find(m_musicTracks, storyBgm) != m_musicTracks.end()) {
            const std::string id = px::PathStemUtf8(storyBgm);
            if (m_profile.UnlockMusic(id)) {
                (void)m_ui.UpdateUiData("musicUnlocked." + id, Variant(true));
                MarkProfileDirty(true);
            }
        }
    }
    syncChoiceTexts();

    // The Story reached its end: player.navigation.afterStoryEnd decides what
    // follows (by default nothing, and the last line stays on screen).
    if (m_session->VM().State() != vn::VMState::Finished) {
        m_storyEndHandled = false;
    } else if (!m_storyEndHandled) {
        m_storyEndHandled = true;
        if (FollowStoryEnd()) {
            if (m_appState == AppState::Game) RenderStageWithScripts();
            RenderUIWithScripts();
            return;
        }
    }

    // New dialogue lines feed the NVL page and the rollback ring.
    const std::uint64_t backlogSequence = m_session->Backlog().LastSequence();
    if (backlogSequence > m_lastBacklogSequence) {
        m_storyHudVisible = true;
        const vn::BacklogEntry* latestEntry = nullptr;
        for (const vn::BacklogEntry& e :
             m_session->Backlog().EntriesAfter(m_lastBacklogSequence)) {
            latestEntry = &e;
            if (m_nvlMode && !e.isChoice) {
                // A line that no longer fits the page starts the next one.
                std::vector<std::string> page;
                for (const auto& line : m_nvlLines) page.push_back(NvlLineText(line.speaker, line.text));
                page.push_back(NvlLineText(e.speaker, e.text));
                if (!m_nvlLines.empty() && !m_ui.NvlPageFits(page)) m_nvlLines.clear();
                m_nvlLines.push_back(e);
            }
            if (m_settings.selfVoicing && !e.isChoice)
                m_speech.Speak(e.speaker.empty() ? e.text : e.speaker + ". " + e.text);
        }
        if (latestEntry) {
            const diag::ScopedFramePhase profiled(diag::FramePhase::Logic);
            diag::FrameProfiler::Instance().Note("rollback-snapshot");
            if (auto snapshot = MakeSnapshot(/*includeBacklog=*/false)) {
                RollbackEntry entry;
                entry.snap = std::move(*snapshot);
                entry.backlogSequence = latestEntry->sequence;
                m_rollback.push_back(std::move(entry));
                if (m_rollback.size() > 64) m_rollback.pop_front();
            }
        }
        m_lastBacklogSequence = backlogSequence;
    }

    RenderStageWithScripts();

    // Autosave once per choice prompt; the stage is rendered, so the thumbnail
    // shows the scene the player was deciding on.
    if (m_session->VM().State() == vn::VMState::WaitingChoice) {
        if (!m_autoSavedChoice) {
            SaveSlotInBackground(kAutoSaveSlot);
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

    PublishDock();
    const bool hudWanted = (m_storyHudVisible && !m_hudHidden) ||
                           !m_session->Routes().CurrentModalRoute().empty();
    {
        const float target = hudWanted ? 1.0f : 0.0f;
        if (m_hudFadeSnap || !m_uiEffects || m_hudFadeSeconds <= 0.0f) {
            m_hudFade = target;
            m_hudFadeSnap = false;
        } else {
            const float step = std::min(dt, 1.0f / 30.0f) / m_hudFadeSeconds;
            m_hudFade += std::clamp(target - m_hudFade, -step, step);
        }
        if (m_hudFade == target) m_hudFadeSeconds = kHudFadeSeconds;
    }
    if (hudWanted || m_hudFade > 0.001f) {
        // Opening Save above changes the active surface during this frame.
        // RefreshHUD installs HUD when another screen is active, so doing it
        // unconditionally immediately erased the menu the player just opened.
        if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::HUD)
            m_ui.RefreshHUD(DialogueUI());
        RenderUIWithScripts(m_hudFade);
    }

    if (m_saveRequested) {
        SaveSlotInBackground(0);
        m_saveRequested = false;
    }

}

void PlayerApp::UpdateSettingsPreview(const std::uint64_t now) {
    if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Settings) {
        m_previewStart = 0;
        return;
    }
    const std::string sample = ProductText(m_langTable, "player.settings.preview",
        "文字以這個速度出現，自動模式會在句尾停留這麼久。");
    std::vector<std::size_t> ends;  // byte end of each code point
    for (std::size_t index = 0; index < sample.size(); ++index)
        if (index + 1 == sample.size() ||
            (static_cast<unsigned char>(sample[index + 1]) & 0xC0) != 0x80)
            ends.push_back(index + 1);
    const int speed = std::max(0, m_settings.textSpeedMs);
    // A hold this short would only flicker; Auto never waits less per line.
    const int wait = std::max(300, m_settings.autoWaitMs);
    // A changed speed or wait restarts the line, so the player sees it at once.
    if (m_previewStart == 0 || speed != m_previewSpeedMs || wait != m_previewWaitMs) {
        m_previewStart = now;
        m_previewSpeedMs = speed;
        m_previewWaitMs = wait;
    }
    const std::uint64_t typing = static_cast<std::uint64_t>(speed) * ends.size();
    if (now - m_previewStart >= typing + static_cast<std::uint64_t>(wait)) m_previewStart = now;
    const std::uint64_t elapsed = now - m_previewStart;
    const std::size_t shown = speed == 0 ? ends.size()
        : std::min<std::size_t>(ends.size(), static_cast<std::size_t>(elapsed / speed));
    std::string text = sample.substr(0, shown == 0 ? 0 : ends[shown - 1]);
    const bool waiting = shown == ends.size();
    if (text != m_previewShown) {
        m_previewShown = std::move(text);
        (void)m_ui.UpdateUiData("settings.preview", Variant(m_previewShown));
    }
    if (waiting != m_previewWaiting) {
        m_previewWaiting = waiting;
        (void)m_ui.UpdateUiData("settings.previewWaiting", Variant(waiting));
    }
}

void PlayerApp::WatchOwnerProcess() {
    const char* owner = SDL_getenv("PRISMATIX_EXIT_WITH_PROCESS");
    if (!owner || !*owner) return;
    char* end = nullptr;
    const unsigned long long pid = std::strtoull(owner, &end, 10);
    if (!end || *end != '\0' || pid == 0) {
        PX_LOG_WARN("PRISMATIX_EXIT_WITH_PROCESS is not a process id: {}", owner);
        return;
    }
    m_ownerProcessWatch = platform::ProcessExitWatch::Start(
        platform::ProcessAliveProbe(pid), [this] { m_ownerProcessExited = true; });
    if (!m_ownerProcessWatch)
        PX_LOG_WARN("Cannot watch owner process {}; the game will not close with it", pid);
}

void PlayerApp::MainLoop() {
    while (!m_quitRequested) {
        if (m_ownerProcessExited) {
            PX_LOG_INFO("The process that started this game exited; closing");
            m_quitRequested = true;
            break;
        }
#if defined(__EMSCRIPTEN__)
        // Yield between complete frames. Asyncify also lets remote PDX reads
        // await fetch without freezing input or the browser's loading screen.
        WaitWebFrame();
#endif
        if (!m_runtime.BeginFrame()) break;
        m_uiTransitionStartedThisFrame = false;
        UpdateRouteVisualPreload();
        px::Input& input = m_runtime.GetInput();
        if (const auto touch = input.TakeTouchActiveChange())
            (void)m_ui.UpdateUiData("platform.touch", Variant(*touch));
        const float dt = m_runtime.GetClock().DeltaSeconds();
        const std::uint64_t now = m_runtime.GetClock().NowMs();
        UpdateCursorVisibility(now);
        UpdateSettingsPreview(now);
        const std::string_view inputRoute = m_session->Routes().CurrentRoute();
        const bool gameInputContext = m_appState == AppState::Game &&
            !m_gameStartTransitionActive && m_viewingCG.empty() &&
            m_session->Routes().CurrentModalRoute().empty() &&
            (inputRoute == "hud" || inputRoute == "game");
        (void)input.SetBaseContext(gameInputContext ? "game" : "ui");
        PumpQaIpc();

        if (input.KeyPressed(SDL_SCANCODE_RETURN) &&
            (input.KeyDown(SDL_SCANCODE_LALT) || input.KeyDown(SDL_SCANCODE_RALT))) {
            (void)ApplyFullscreenSetting(!m_settings.fullscreen);
        }

        // Script jobs and accepted save/load requests have one application
        // clock and safe point, independent of the currently presented route.
        UpdateScriptScheduler(dt);
        FlushProfileIfDue();
        HarvestBackgroundSave(false);
#if defined(PRISMATIX_WEB_PLAYER)
        if (TakeWebPersistenceRequest()) {
            // Run this in the existing Asyncify frame, never from a DOM/SDL
            // callback while another WASM stack is suspended.
            bool slotSaved = true;
            if (m_appState == AppState::Game && !m_gameStartTransitionActive &&
                m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) {
                SDL_Surface* frame = graphics::CaptureFrameSurface(m_runtime.Renderer().Handle());
                slotSaved = SaveSlot(kAutoSaveSlot, graphics::EncodeThumbnailPng(frame, 256, 144));
            }
            HarvestBackgroundSave(true);
            const bool settingsSaved = SaveSettingsIfWritable();
            const bool profileSaved = SaveProfileIfWritable();
            FinishWebPersistenceRequest(slotSaved && settingsSaved && profileSaved);
        }
#endif

        if(m_appState==AppState::BootSplash){
            SplashFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        if(m_gameStartTransitionActive){
            GameStartTransitionFrame(dt,now);
            m_runtime.EndFrame();
            continue;
        }

        if (m_routePreloadBatch) {
            ScreensFrame(0.0f);
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

        // Declared UI shortcuts use the same command sink as authored buttons.
        // Input contexts suppress game-only bindings while a modal is open.
        bool handledUiShortcut = false;
        if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) {
            for (const auto& [action, command] : m_uiActionTriggers)
                if (input.ActionPressed(action)) {
                    for (const auto& event : input.FrameEvents())
                        if (event.type == InputEventType::Action &&
                            event.action == action)
                            (void)input.SuppressDefault(event.id);
                    HandleUIAction(command);
                    PX_LOG_INFO("Player UI shortcut dispatched action={} command={}",
                                action, command.command);
                    handledUiShortcut = true;
                    break;
                }
        }
        if (m_gameStartTransitionActive) {
            GameStartTransitionFrame(dt, now);
            m_runtime.EndFrame();
            continue;
        }

        bool modalOpen =
            !m_session->Routes().CurrentModalRoute().empty();
        const std::string_view currentRoute =
            m_session->Routes().CurrentRoute();
        const bool gameplayRoute =
            currentRoute == "hud" || currentRoute == "game";

        bool openedByShortcut = false;
        if (!handledUiShortcut && m_appState == AppState::Game &&
            !modalOpen && gameplayRoute) {
            // Keys and gamepad buttons open these screens; a mouse button's
            // default action (a right click is also `menu`) does not:
            // player.navigation.gameRightClick decides what a right click does.
            for (const auto& [action, route] : m_screenTriggers)
                if (input.ActionPressedWithoutPointer(action)) {
                    OpenScreen(route);
                    openedByShortcut = true;
                    break;
                }
            // An input binding may have opened a modal during this frame. Do
            // not let the gameplay HUD refresh replace its authored scene.
            modalOpen = !m_session->Routes().CurrentModalRoute().empty();
        }

        if (modalOpen &&
            m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) {
            // The key that opened a menu this frame (Escape bound to `menu`
            // with a route) does not also close it.
            if (!handledUiShortcut && !openedByShortcut &&
                (input.RightClick() || input.KeyPressed(SDL_SCANCODE_ESCAPE))) {
                HandleUIAction({"overlay.close", {}});
            }
            ScreensFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        // A non-modal custom route (phone, replay selector, custom menu, ...)
        // owns presentation while the story domain remains paused. The shared
        // script scheduler above intentionally continues to run.
        if (m_appState == AppState::Game && !gameplayRoute) {
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

        // One physical key can be bound to a UI command and a legacy game
        // action. The UI command owns this frame; do not also advance or save.
        if (handledUiShortcut) {
            ScreensFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        GameFrame(dt, now);
        SyncTextInput();
        m_runtime.EndFrame();
    }
}

void PlayerApp::Shutdown() {
    if (m_routePreloadHold) m_runtime.Renderer().EndPresentationHold();
    if (m_scriptHost) m_scriptHost->Emit("engine.shutdown");
    if (!m_settings.fullscreen) {
        int w = 0, h = 0;
        SDL_GetWindowSize(m_runtime.GetWindow().Handle(), &w, &h);
        if (w >= 320 && h >= 180) {
            m_settings.windowWidth = w;
            m_settings.windowHeight = h;
        }
    }
    SaveSettingsIfWritable();
    SaveProfileIfWritable();
    // Subsystems referencing the runtime must go before Runtime::Shutdown.
    m_scriptHost.reset();
    m_splash.reset();
    HarvestBackgroundSave(true);
    // The video player owns an SDL audio stream and textures; it must close
    // before Runtime::Shutdown quits SDL.
    m_video.reset();
    if (m_runtimePreload.valid()) m_runtimePreload.wait();
    m_session.reset();
    m_runtime.Shutdown();
}

int PlayerApp::Run(int argc, char* argv[]) {
    WatchOwnerProcess();
    if (!Init(argc, argv)) {
        return 1;
    }
    MainLoop();
    Shutdown();
    return 0;
}

}
