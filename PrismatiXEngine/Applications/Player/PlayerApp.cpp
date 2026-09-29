#include "Applications/Player/PlayerApp.h"

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
const std::string kSaveKey = "prismatix-demo-secret";
constexpr int kAutoSaveSlot = -1;
constexpr std::string_view kUIRouteE2ERoutes[] = {
    "backlog", "save", "load", "gallery", "settings",
    "settings-page1", "settings-page2", "settings-page3",
    "settings-page4", "settings-page5", "settings-page6",
    "settings-page7",
    "extra", "flowchart", "flowchart-common", "flowchart-yoshino",
    "flowchart-mako", "flowchart-murasame", "flowchart-lena",
    "flowchart-sub", "replay", "music",
};
constexpr std::string_view kUIActionsE2EReplayLabel =
    "sb.115-afterver1-01.yos-part-15";

// Player operations `player.confirmations` may ask before.
constexpr std::string_view kConfirmableOperations[] = {
    "quickSave", "quickLoad", "save", "overwrite", "load", "returnToTitle",
    "exit", "backlogJump", "previousChoice", "nextChoice", "resetSettings"};

bool IsConfirmableOperation(const std::string_view operation) {
    return std::ranges::find(kConfirmableOperations, operation) !=
           std::end(kConfirmableOperations);
}

template <typename T>
T* FindUiControlNamed(ui::Control* root, const std::string_view name) {
    if (!root) return nullptr;
    if (root->Name() == name) return dynamic_cast<T*>(root);
    for (const auto& child : root->Children()) {
        if (auto* control = dynamic_cast<ui::Control*>(child.get()))
            if (auto* found = FindUiControlNamed<T>(control, name))
                return found;
    }
    return nullptr;
}

std::optional<std::string> SafeVfsRelativePath(const std::string& value) {
    return io::VFS::NormalizeVirtualPath(value);
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
    m_settings.language = std::move(locale);
    m_session->VM().SetCharacterLocale(m_settings.language);
    m_script = runtimePath;
    if (!replacingRunningProgram && m_appState == AppState::Title &&
        m_runtimePreloadPath != m_script)
        BeginRuntimeIrPreload();
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
    if (!m_titleBgmAfterPath.empty() &&
        ProfileFlagEnabled(m_profile.Variable(m_titleBgmAfterFlag)))
        return m_titleBgmAfterPath;
    return m_titleBgm;
}

void PlayerApp::RefreshTitleProgressUi() {
    const auto flag = [this](const char* name) {
        return ProfileFlagEnabled(m_profile.Variable(name));
    };
    // titleflag.lua chooses the most advanced chapter milestone first.
    const int tier = flag("g_clear_5") ? 7 : flag("g_clear_4") ? 6 :
        flag("g_clear_ani") ? 5 : flag("g_clear_2") ? 4 :
        flag("g_clear_1") ? 3 : flag("g_clearop") ? 2 : 0;
    const std::array chapterKeys = {"op", "i", "ii", "iii", "iv", "v", "vi"};
    const std::array thresholds = {2, 2, 3, 4, 5, 6, 7};
    for (std::size_t index = 0; index < chapterKeys.size(); ++index)
        (void)m_ui.UpdateUiData(
            std::string("title.chapter.") + chapterKeys[index],
            Variant(tier >= thresholds[index]));

    const int variant = flag("g_clear_mar") && flag("g_clear_6") && flag("g_append") ? 9 :
        (flag("g_clear_mar") && flag("g_start_ani")) ||
        (flag("g_clear_ani") && flag("g_start_mar")) ? 8 :
        !flag("g_clear_mar") && flag("g_start_ani") ? 7 :
        (flag("g_clear_zyp") && flag("g_start_mar")) ||
        (flag("g_clear_mar") && flag("g_start_zyp")) ? 6 :
        !flag("g_clear_zyp") && flag("g_start_mar") ? 5 :
        !flag("g_clear_mar") && flag("g_start_zyp") ? 4 :
        (flag("g_clear_oly") && flag("g_start_pic")) ||
        (flag("g_clear_pic") && flag("g_start_oly")) ? 3 :
        !flag("g_clear_oly") && flag("g_start_pic") ? 2 :
        !flag("g_clear_pic") && flag("g_start_oly") ? 1 : 0;
    for (int index = 1; index <= 9; ++index)
        (void)m_ui.UpdateUiData("title.chapter3Case" + std::to_string(index),
                                Variant(index == variant));
}

bool PlayerApp::Init(int argc, char* argv[]) {
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
    if (const char* isolatedSaveDir = SDL_getenv("PRISMATIX_E2E_SAVE_DIR");
        isolatedSaveDir && *isolatedSaveDir)
        m_saveDirectory = std::filesystem::path(isolatedSaveDir);
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
    if (SDL_getenv("PRISMATIX_E2E_EXPECT_REMOVED_RESOURCE")) {
        const bool hidden = !m_runtime.VFS().Exists(
            "Content/Overlay/player-second.txt");
        PX_LOG_INFO("Player E2E removed update resource hidden={}", hidden);
        if (!hidden) return false;
    }

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
    if (m_settings.fullscreen) {
        if (!SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), true) ||
            !SDL_SyncWindow(m_runtime.GetWindow().Handle()))
            PX_LOG_WARN("Player startup fullscreen request failed: {}", SDL_GetError());
    } else if (m_settings.windowWidth >= 320 && m_settings.windowHeight >= 180) {
        SDL_SetWindowSize(m_runtime.GetWindow().Handle(), m_settings.windowWidth,
                          m_settings.windowHeight);
        SDL_SetWindowPosition(m_runtime.GetWindow().Handle(), SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED);
        if (!SDL_SyncWindow(m_runtime.GetWindow().Handle()))
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
            if (settings != project.end() && settings->is_object()) {
                if (const auto data = settings->find("player.uiData"); data != settings->end()) {
                    if (!data->is_object() || data->size() > 256) {
                        diag::Emit(diag::Diagnostic{
                            .severity = diag::Severity::Fatal,
                            .code = "PXPLAYER5024",
                            .category = "Player.UI",
                            .message = "player.uiData must be a bounded property map"});
                        return false;
                    }
                    for (auto item = data->begin(); item != data->end(); ++item) {
                        auto value = JsonVariant(item.value());
                        if (!value) {
                            diag::Emit(diag::Diagnostic{
                                .severity = diag::Severity::Fatal,
                                .code = "PXPLAYER5024",
                                .category = "Player.UI",
                                .message = "player.uiData contains an unsupported value",
                                .details = item.key()});
                            return false;
                        }
                        if (const Status defined = m_ui.DefineUiData(
                                item.key(), std::move(*value)); !defined) {
                            for (const auto& diagnostic : defined.Diagnostics())
                                diag::Emit(diagnostic);
                            return false;
                        }
                    }
                }
                if (const auto contract = settings->find("authoring.uiContract");
                    contract != settings->end()) {
                    const Status verified = m_ui.ValidateUiContract(contract->dump());
                    if (!verified) {
                        for (const auto& diagnostic : verified.Diagnostics())
                            diag::Emit(diagnostic);
                        return false;
                    }
                }
                if (const auto titleBgm = settings->find("player.titleBgm");
                    titleBgm != settings->end() && titleBgm->is_string()) {
                    const auto safe = SafeVfsRelativePath(titleBgm->get<std::string>());
                    if (safe && m_runtime.VFS().Exists(*safe)) m_titleBgm = *safe;
                }
                if (const auto alternate = settings->find("player.titleBgmAfter");
                    alternate != settings->end() && alternate->is_object()) {
                    const auto flag = alternate->find("flag");
                    const auto path = alternate->find("path");
                    if (flag != alternate->end() && flag->is_string() &&
                        path != alternate->end() && path->is_string()) {
                        const auto safe = SafeVfsRelativePath(path->get<std::string>());
                        if (safe && m_runtime.VFS().Exists(*safe)) {
                            m_titleBgmAfterFlag = flag->get<std::string>();
                            m_titleBgmAfterPath = *safe;
                        }
                    }
                }
                if (const auto tracks = settings->find("player.musicTracks");
                    tracks != settings->end()) {
                    if (!tracks->is_array() || tracks->size() > 256) {
                        diag::Emit(diag::Diagnostic{
                            .severity = diag::Severity::Fatal,
                            .code = "PXPLAYER5025",
                            .category = "Player.Audio",
                            .message = "player.musicTracks must be an array of at most 256 paths"});
                        return false;
                    }
                    for (const auto& track : *tracks) {
                        const auto safe = track.is_string()
                            ? SafeVfsRelativePath(track.get<std::string>())
                            : std::nullopt;
                        if (!safe || !m_runtime.VFS().Exists(*safe)) {
                            diag::Emit(diag::Diagnostic{
                                .severity = diag::Severity::Fatal,
                                .code = "PXPLAYER5025",
                                .category = "Player.Audio",
                                .message = "player.musicTracks contains a missing or unsafe path"});
                            return false;
                        }
                        m_musicTracks.push_back(*safe);
                    }
                }
                if (const auto layout = settings->find("player.saveLayout");
                    layout != settings->end()) {
                    if (!layout->is_object()) return false;
                    for (const char* key : {"pages", "slotsPerPage", "firstSlot"}) {
                        if (const auto field = layout->find(key);
                            field != layout->end() && !field->is_number_integer())
                            return false;
                    }
                    const auto pages = layout->value("pages", 1);
                    const auto perPage = layout->value("slotsPerPage", 12);
                    const auto first = layout->value("firstSlot", 0);
                    if (pages < 1 || pages > 100 || perPage < 1 || perPage > 32 ||
                        first < 0 || first + pages * perPage > 1000) return false;
                    m_savePages = pages;
                    m_saveSlotsPerPage = perPage;
                    m_saveFirstSlot = first;
                }
                const auto readUiSound=[this,settings](const char* key,std::string& output){
                    const auto value=settings->find(key);
                    if(value==settings->end()||!value->is_string())return;
                    const auto safe=SafeVfsRelativePath(value->get<std::string>());
                    if(safe&&m_runtime.VFS().Exists(*safe))output=*safe;
                };
                readUiSound("player.uiHoverSound",m_defaultUiHoverSound);
                readUiSound("player.uiActivateSound",m_defaultUiActivateSound);
                readUiSound("player.uiCancelSound",m_defaultUiCancelSound);
                if (const auto uiFont = settings->find("player.uiFont");
                    uiFont != settings->end() && uiFont->is_string()) {
                    const auto safe = SafeVfsRelativePath(uiFont->get<std::string>());
                    if (safe && m_runtime.VFS().Exists(*safe)) {
                        m_runtime.Assets().PrefetchFontFile(*safe);
                        if (const Status applied = m_ui.SetDefaultFont(*safe); !applied) {
                            for (const auto& diagnostic : applied.Diagnostics())
                                diag::Emit(diagnostic);
                            return false;
                        }
                    }
                }
                if (const auto volumes = settings->find("player.defaultVolumes");
                    volumes != settings->end() && volumes->is_object()) {
                    // Percentages, as the project's Config shows them.
                    const auto level = [&](const char* key, int& target) {
                        const auto value = volumes->find(key);
                        if (value != volumes->end() && value->is_number())
                            target = static_cast<int>(std::lround(
                                std::clamp(value->get<double>(), 0.0, 100.0) * 128.0 / 100.0));
                    };
                    level("master", m_settingsDefaults.masterVolume);
                    level("bgm", m_settingsDefaults.bgmVolume);
                    level("se", m_settingsDefaults.seVolume);
                    level("voice", m_settingsDefaults.voiceVolume);
                    if (m_settingsMissing) {
                        m_settings.masterVolume = m_settingsDefaults.masterVolume;
                        m_settings.bgmVolume = m_settingsDefaults.bgmVolume;
                        m_settings.seVolume = m_settingsDefaults.seVolume;
                        m_settings.voiceVolume = m_settingsDefaults.voiceVolume;
                        m_runtime.Audio().SetMainVolume(m_settings.masterVolume);
                        m_runtime.Audio().SetBGMVolume(m_settings.bgmVolume);
                        m_runtime.Audio().SetSEVolume(m_settings.seVolume);
                        m_runtime.Audio().SetVoiceVolume(m_settings.voiceVolume);
                    }
                }
                if (const auto profiles = settings->find("player.voiceProfiles");
                    profiles != settings->end() && profiles->is_object()) {
                    const auto safeId = [](const std::string& id) {
                        return !id.empty() && id.size() <= 64 &&
                            std::ranges::all_of(id, [](const unsigned char c) {
                                return std::isalnum(c) || c == '_' || c == '-'; });
                    };
                    if (const auto ids = profiles->find("ids"); ids != profiles->end() && ids->is_array())
                        for (const auto& id : *ids)
                            if (id.is_string() && safeId(id.get<std::string>()) &&
                                m_voiceProfileIds.size() < 256)
                                m_voiceProfileIds.push_back(id.get<std::string>());
                    std::vector<std::pair<std::string, std::string>> fallbacks;
                    if (const auto prefixes = profiles->find("fallbackByFilePrefix");
                        prefixes != profiles->end() && prefixes->is_object())
                        for (auto item = prefixes->begin(); item != prefixes->end(); ++item)
                            if (!item.key().empty() && item.key().size() <= 64 &&
                                item.value().is_string() && safeId(item.value().get<std::string>()))
                                fallbacks.emplace_back(item.key(), item.value().get<std::string>());
                    m_runtime.Audio().SetVoiceProfileFallbacks(std::move(fallbacks));
                }
                if (const auto dock = settings->find("player.dockArea");
                    dock != settings->end() && dock->is_object()) {
                    m_dockArea = {dock->value("x", 0.0f), dock->value("y", 0.0f),
                                  dock->value("width", 0.0f), dock->value("height", 0.0f), true};
                }
                if (const auto layout = settings->find("player.choiceLayout");
                    layout != settings->end() && layout->is_object()) {
                    // Rows of offsets, one per choice count, in half-pitch units.
                    ui::GalgameUI::ChoiceLayout choices;
                    choices.x = layout->value("x", 0.0f);
                    choices.y = layout->value("y", 0.0f);
                    choices.width = layout->value("width", 0.0f);
                    choices.height = layout->value("height", 0.0f);
                    choices.halfPitch = layout->value("halfPitch", 0.0f);
                    if (const auto rows = layout->find("offsets");
                        rows != layout->end() && rows->is_object()) {
                        for (auto it = rows->begin(); it != rows->end(); ++it) {
                            if (!it.value().is_array()) continue;
                            const auto count = static_cast<std::size_t>(std::strtoul(it.key().c_str(), nullptr, 10));
                            std::vector<double> offsets;
                            for (const auto& offset : it.value())
                                if (offset.is_number()) offsets.push_back(offset.get<double>());
                            if (count > 0 && count <= 64 && offsets.size() == count)
                                choices.offsets[count] = std::move(offsets);
                        }
                    }
                    m_ui.SetChoiceLayout(std::move(choices));
                }
                if (const auto fonts = settings->find("player.fonts");
                    fonts != settings->end() && fonts->is_array()) {
                    for (const auto& font : *fonts) {
                        if (!font.is_string()) continue;
                        const auto safe = SafeVfsRelativePath(font.get<std::string>());
                        if (safe && m_runtime.VFS().Exists(*safe)) {
                            m_runtime.Assets().PrefetchFontFile(*safe);
                            m_fontChoices.push_back(*safe);
                        }
                    }
                }
                if (const auto options = settings->find("player.options");
                    options != settings->end() && options->is_object()) {
                    for (auto it = options->begin(); it != options->end(); ++it) {
                        const std::string& key = it.key();
                        if (key.empty() || key.size() > 48 || !it.value().is_object() ||
                            !std::ranges::all_of(key, [](const unsigned char c) {
                                return std::isalnum(c) || c == '_'; }))
                            continue;
                        OptionSpec spec;
                        spec.def = it.value().value("default", 0);
                        spec.min = it.value().value("min", 0);
                        spec.max = it.value().value("max", 100);
                        if (const auto choices = it.value().find("choices");
                            choices != it.value().end() && choices->is_array()) {
                            for (const auto& choice : *choices)
                                if (choice.is_number_integer())
                                    spec.choices.push_back(choice.get<int>());
                            if (!spec.choices.empty()) {
                                spec.min = *std::ranges::min_element(spec.choices);
                                spec.max = *std::ranges::max_element(spec.choices);
                            }
                        }
                        if (spec.min > spec.max) continue;
                        spec.def = std::clamp(spec.def, spec.min, spec.max);
                        if (const auto binds = it.value().find("binds");
                            binds != it.value().end()) {
                            const auto binding = binds->is_string()
                                ? ParseOptionBinding(binds->get<std::string>()) : std::nullopt;
                            if (!binding) {
                                PX_LOG_WARN("Player option {} binds an unknown behaviour; "
                                            "the option is stored but has no effect", key);
                            } else {
                                spec.binding = *binding;
                                const auto factor = [&](const char* field, const double fallback) {
                                    const auto found = it.value().find(field);
                                    return found != it.value().end() && found->is_number() &&
                                           std::isfinite(found->get<double>())
                                        ? found->get<double>() : fallback;
                                };
                                spec.scale = factor("scale", 1.0);
                                spec.offset = factor("offset", 0.0);
                            }
                        }
                        m_optionSpecs.insert_or_assign(key, std::move(spec));
                    }
                }
                if (const auto confirmations = settings->find("player.confirmations");
                    confirmations != settings->end() && confirmations->is_object()) {
                    const auto isName = [](const std::string& name) {
                        return !name.empty() && name.size() <= 48 &&
                               std::ranges::all_of(name, [](const unsigned char c) {
                                   return std::isalnum(c) || c == '_'; });
                    };
                    for (auto it = confirmations->begin(); it != confirmations->end(); ++it) {
                        if (!IsConfirmableOperation(it.key()) || !it.value().is_object()) {
                            PX_LOG_WARN("Player cannot ask before {}; see player.confirmations",
                                        it.key());
                            continue;
                        }
                        const auto text = [&](const char* field, const std::string& fallback) {
                            const auto found = it.value().find(field);
                            return found == it.value().end() ? fallback
                                : found->is_string() ? found->get<std::string>() : std::string();
                        };
                        ConfirmSpec confirm;
                        confirm.kind = text("kind", it.key());
                        confirm.dontAskOption = text("dontAskOption", "");
                        if (!isName(confirm.kind) ||
                            (it.value().contains("dontAskOption") && confirm.dontAskOption.empty()) ||
                            (!confirm.dontAskOption.empty() &&
                             !m_optionSpecs.contains(confirm.dontAskOption))) {
                            PX_LOG_WARN("Player confirmation {} has an invalid kind or an "
                                        "undeclared dontAskOption", it.key());
                            continue;
                        }
                        if (std::ranges::find(m_dialogKinds, confirm.kind) == m_dialogKinds.end())
                            m_dialogKinds.push_back(confirm.kind);
                        m_confirmations.insert_or_assign(it.key(), std::move(confirm));
                    }
                }
                const auto transitionDuration = [](const nlohmann::json& owner,
                                                    const char* key)
                    -> std::optional<float> {
                    const auto value = owner.find(key);
                    if (value == owner.end() || !value->is_number()) return std::nullopt;
                    const double seconds = value->get<double>();
                    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 10.0)
                        return std::nullopt;
                    return static_cast<float>(seconds);
                };
                const auto transitionPreset = [](const nlohmann::json& owner,
                                                  const char* key)
                    -> std::optional<std::string> {
                    const auto value = owner.find(key);
                    if (value == owner.end() || !value->is_string()) return std::nullopt;
                    const std::string preset = value->get<std::string>();
                    if (preset != "none" && preset != "fade" && preset != "crossfade" &&
                        preset != "slide-left" && preset != "slide-right")
                        return std::nullopt;
                    return preset;
                };
                if (const auto transitions = settings->find("player.presentationTransitions");
                    transitions != settings->end() && transitions->is_object()) {
                    if (const auto preset = transitionPreset(*transitions, "routePreset"))
                        m_routeTransition.preset = *preset;
                    if (const auto duration = transitionDuration(*transitions, "routeDuration"))
                        m_routeTransition.duration = *duration;
                    if (const auto preset = transitionPreset(*transitions, "splashPreset"))
                        m_splashTransition.preset = *preset;
                    if (const auto duration = transitionDuration(*transitions, "splashDuration")) {
                        m_splashTransition.duration = *duration;
                        m_splashTransitionConfigured = true;
                    }
                    if (const auto preset = transitionPreset(*transitions, "titlePreset"))
                        m_titleTransition.preset = *preset;
                    if (const auto duration = transitionDuration(*transitions, "titleDuration"))
                        m_titleTransition.duration = *duration;
                    if (const auto overrides = transitions->find("routeOverrides");
                        overrides != transitions->end() && overrides->is_array()) {
                        for (const auto& routeOverride : *overrides) {
                            if (!routeOverride.is_object()) continue;
                            const auto from = routeOverride.find("from");
                            const auto to = routeOverride.find("to");
                            const auto preset = transitionPreset(routeOverride, "preset");
                            const auto duration = transitionDuration(routeOverride, "duration");
                            if (from == routeOverride.end() || !from->is_string() ||
                                to == routeOverride.end() || !to->is_string() ||
                                !preset || !duration) continue;
                            m_routeTransitionOverrides.insert_or_assign(
                                from->get<std::string>() + "\n" + to->get<std::string>(),
                                PresentationTransition{*preset, *duration});
                        }
                    }
                }
                if (const auto titleVoices = settings->find("player.titleVoiceVariants");
                    titleVoices != settings->end() && titleVoices->is_array()) {
                    for (const auto& value : *titleVoices) {
                        if (!value.is_string()) continue;
                        const auto safe = SafeVfsRelativePath(value.get<std::string>());
                        if (safe && m_runtime.VFS().Exists(*safe))
                            m_titleVoices.push_back(*safe);
                    }
                }
                if (const auto splashes = settings->find("player.splashes");
                    splashes != settings->end() && splashes->is_array()) {
                    m_boot.splashes.clear();
                    m_splashTransitionOverrides.clear();
                    for (std::size_t index = 0; index < splashes->size(); ++index) {
                        const auto& value = (*splashes)[index];
                        if (!value.is_object()) continue;
                        const auto sceneValue = value.find("scene");
                        if (sceneValue == value.end() || !sceneValue->is_string()) continue;
                        const auto scene = SafeVfsRelativePath(sceneValue->get<std::string>());
                        if (!scene || !m_runtime.VFS().Exists(*scene)) continue;
                        ui::startup::SplashScreenEntry entry;
                        entry.scene = {Uuid::FromName("PrismatiX.Splash.Scene." + *scene), *scene};
                        entry.minimumDuration = value.value("minimumDuration", 2.0f);
                        entry.audioDelay = value.value("audioDelay", 0.0f);
                        entry.skippable = value.value("skippable", true);
                        entry.skipAllowedAfter = value.value("skipAllowedAfter", 0.5f);
                        entry.enterAnimation = value.value("enterAnimation", std::string("enter"));
                        entry.exitAnimation = value.value("exitAnimation", std::string("exit"));
                        if (const auto audioValue = value.find("audio");
                            audioValue != value.end() && audioValue->is_string()) {
                            const auto audio = SafeVfsRelativePath(audioValue->get<std::string>());
                            if (audio && m_runtime.VFS().Exists(*audio))
                                entry.audio = ResourceRefValue{
                                    Uuid::FromName("PrismatiX.Splash.Audio." + *audio), *audio};
                        }
                        if (const auto variants = value.find("audioVariants");
                            variants != value.end() && variants->is_array()) {
                            std::vector<std::string> available;
                            for (const auto& variant : *variants) {
                                if (!variant.is_string()) continue;
                                const auto safe = SafeVfsRelativePath(
                                    variant.get<std::string>());
                                if (safe && m_runtime.VFS().Exists(*safe))
                                    available.push_back(*safe);
                            }
                            if (!available.empty()) {
                                const auto tick = std::chrono::steady_clock::now()
                                    .time_since_epoch().count();
                                const std::string& chosen = available[
                                    static_cast<std::size_t>(tick) % available.size()];
                                entry.audio = ResourceRefValue{
                                    Uuid::FromName("PrismatiX.Splash.Audio." + chosen),
                                    chosen};
                            }
                        }
                        if (const Status valid = ui::startup::ValidateSplashEntry(
                                entry, index, "project.pxproject"); valid) {
                            m_splashTransitionOverrides.push_back({
                                transitionPreset(value, "transitionPreset"),
                                transitionDuration(value, "transitionDuration")});
                            m_boot.splashes.push_back(std::move(entry));
                        } else
                            for (const auto& diagnostic : valid.Diagnostics())
                                diag::Emit(diagnostic);
                    }
                }
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
        m_autoMode = false;
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
    const std::vector<std::string> authoredUiCommands = {
        "set.fullscreen.on", "set.fullscreen.off", "set.fullscreen.toggle",
        "set.mute.master", "set.mute.bgm", "set.mute.se", "set.mute.voice",
        "set.duck.on", "set.duck.off", "set.voiceCarry.on", "set.voiceCarry.off",
        "set.voiceProfile.value",
        "set.reducedmotion.on", "set.reducedmotion.off",
        "settings.reset", "settings.noop",
        "title.confirm.accept", "title.confirm.cancel",
        "system.confirm.accept", "system.confirm.cancel",
        "music.play", "music.previous", "music.stop", "music.resume",
        "music.next", "music.about", "music.random",
        "music.scroll.home", "music.scroll.up", "music.scroll.down",
        "music.scroll.end", "replay.launch", "replay.page", "title.chapter3", "flowchart.select", "flowchart.jump",
        "flowchart.filter", "flowchart.scroll.home", "flowchart.scroll.up",
        "flowchart.scroll.down", "flowchart.scroll.end",
    };
    for (const auto& command : authoredUiCommands) {
        if (const Status registered = m_ui.RegisterActionCommand(command);
            !registered) {
            for (const auto& diagnostic : registered.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
    }
    for (const char* command : {"dialog.accept", "dialog.cancel", "dialog.toggleSkip"}) {
        if (const Status registered = m_ui.RegisterActionCommand(command); !registered) {
            for (const auto& diagnostic : registered.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    for (const char* command : {"set.voiceProfile.mute", "voiceProfile.test",
                                "audio.test.se", "option.set"}) {
        if (const Status registered = m_ui.RegisterActionCommand(command, "argument");
            !registered) {
            for (const auto& diagnostic : registered.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    for (int page = 0; page < 8; ++page) {
        if (const Status registered = m_ui.RegisterActionCommand(
                "settings.page" + std::to_string(page));
            !registered) {
            for (const auto& diagnostic : registered.Diagnostics())
                diag::Emit(diagnostic);
            return false;
        }
    }
    constexpr std::string_view musicNames[] = {
        "SongOp", "BGM01", "BGM01i", "BGM34", "BGM04", "BGM02",
        "BGM05", "BGM37", "BGM11", "BGM38", "BGM35", "BGM03",
        "BGM40", "BGM41", "BGM51", "BGM42", "BGM52", "BGM43",
        "BGM53", "BGM44", "BGM54", "BGM45", "BGM46", "BGM56",
        "BGM25", "BGM21", "BGM22", "BGM24", "BGM23", "BGM31",
        "BGM26", "BGM39", "BGM08", "BGM28", "BGM12", "BGM29",
        "BGM30", "BGM13", "BGM27", "BGM33", "BGM32", "BGM36",
        "BGM09", "BGM15", "BGM06", "BGM07", "BGM14", "BGM10",
        "SongED1", "SongED2", "SongED3", "SongED4", "SongED5",
        "SongED6", "BGM61",
    };
    if (m_musicTracks.empty()) for (const auto name : musicNames) {
        const std::string path = "Assets/Audio/BGM/" + std::string(name) + ".wav";
        if (m_runtime.VFS().Exists(path)) m_musicTracks.push_back(path);
    }
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
        const std::string id = std::filesystem::path(path).stem().string();
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
    for (const char* chapter : {"op", "i", "ii", "iii", "iv", "v", "vi"}) {
        if (const Status defined = m_ui.DefineUiData(
                std::string("title.chapter.") + chapter, Variant(false)); !defined) {
            for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    for (int variant = 1; variant <= 9; ++variant) {
        if (const Status defined = m_ui.DefineUiData(
                "title.chapter3Case" + std::to_string(variant), Variant(false)); !defined) {
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
    ConfigureE2EJourney();
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
    }
    if (!m_profileDirty || !m_profileWritable) return;
    const std::uint64_t age = SDL_GetTicks() - m_profileDirtySince;
    if (age < (m_profileUrgent ? 2'000u : 30'000u)) return;
    if (!m_profileTrusted) {
        (void)SaveProfileIfWritable();
        return;
    }
    nlohmann::json encoded;
    if (!m_profile.TryToJson(encoded)) return;
    m_profileDirty = false;
    m_profileUrgent = false;
    m_profileWrite = std::async(
        std::launch::async,
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
    m_settingsWrite = std::async(
        std::launch::async,
        [snapshot = std::move(snapshot),
         path = PathToUtf8(m_saveDirectory / "config.dat"), key = m_saveKey,
         fallback = m_saveFallbackKeys] {
            return snapshot.SavePreservingExisting(path, &key, fallback);
        });
}

bool PlayerApp::ApplyFullscreenSetting(const bool fullscreen) {
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
        SDL_SetWindowSize(window, std::max(320, m_settings.windowWidth),
                          std::max(180, m_settings.windowHeight));
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED);
        if (!SDL_SyncWindow(window)) {
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
        m_runtimePreload = std::async(
            std::launch::async,
            [session = m_session.get(), path = m_script] {
                const auto started = std::chrono::steady_clock::now();
                auto program = session->PrepareRuntimeIr(path);
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
                PX_LOG_INFO(
                    "Player Runtime IR preload finished script={} ms={} operations={}",
                    path, elapsed.count(), program ? program->code.size() : 0);
                return program;
            });
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

void PlayerApp::BeginPresentationTransition(
    const PresentationTransition& transition) {
    if (m_uiTransitionStartedThisFrame || !m_uiEffects) return;
    const auto handle = m_runtime.Renderer().PlayScreenEffect(
        transition.preset, transition.duration);
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
        if(m_presentationE2E&&
           m_splash->CurrentPhase()==ui::startup::SplashSequencePlayer::Phase::Holding){
            const std::size_t index=m_splash->CurrentIndex();
            if(index<m_presentationE2ESplashCaptured.size()&&
               !m_presentationE2ESplashCaptured[index]){
                (void)CapturePresentationE2EFrame(
                    "splash-"+std::to_string(index+1));
                m_presentationE2ESplashCaptured[index]=true;
            }
        }
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
    // QA hook: begin at a Story label without playing through the game.
    if (m_pendingStoryEntry.empty())
        if (const char* label = SDL_getenv("PRISMATIX_E2E_START_LABEL"); label && *label)
            m_pendingStoryEntry = label;
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
    constexpr float duration=1.0f;
    m_gameStartTransitionElapsed+=std::max(0.0f,dt);
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
        m_runtime.Renderer().DrawRect(
            Rect{0,0,static_cast<float>(width),static_cast<float>(height)},
            Color{0,0,0,static_cast<std::uint8_t>(std::lround(alpha*255.0f))});
    }
    if(m_presentationE2E&&m_presentationE2EStage==2&&
       !m_gameStartTransitionCommitted&&
       m_gameStartTransitionElapsed>=0.55f){
        (void)CapturePresentationE2EFrame("new-game-fade-out");
        m_presentationE2EStage=3;
    }
}

bool PlayerApp::LoadSlot(int slot) {
    HarvestBackgroundSave(true);
    auto snap = m_saves.Load(slot);
    if (!snap) {
        return false;
    }
    m_hudFadeSnap = true;
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
        snap->packageFingerprint != m_boot.packageFingerprint &&
        std::ranges::find(m_boot.compatibleSaveFingerprints,
                          snap->packageFingerprint) ==
            m_boot.compatibleSaveFingerprints.end()) {
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
            (void)m_scriptHost->RestoreCheckpoint(previousPending, previousActions);
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
            snap->scriptPending, snap->scriptActions);
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
    m_saveWrite = std::async(
        std::launch::async,
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
    progress::SaveSnapshot snap = std::move(*captured);
    snap.thumbnailPng = std::move(thumbnail);
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
            (void)m_scriptHost->RestoreCheckpoint(previousPending, previousActions);
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
            s.scriptPending, s.scriptActions);
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
        items.push_back({"slot-" + std::to_string(slot), label, subtitle, image,
                         (saveMode && m_appState != AppState::Game) ||
                             (!saveMode && !info.exists),
                         action, std::to_string(slot)});
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
        const std::string key="bgm."+std::filesystem::path(currentBgm).stem().string();
        if(const auto translated=m_langTable.find(key);translated!=m_langTable.end())
            view.musicTitle=translated->second;
        else
            view.musicTitle=std::filesystem::path(currentBgm).stem().string();
    }
    view.effect = m_session->Dialogue().State().effect;
    view.effectProgress = m_session->Dialogue().State().effectProgress;
    view.choices = m_choiceTexts;
    view.nvlMode = m_nvlMode;
    view.autoMode = m_autoMode;
    view.skipMode = m_skipMode;
    view.textScale=m_settings.textScale;view.reducedMotion=m_settings.reducedMotion;
    view.windowOpacity = m_messageWindowOpacity;
    view.dimReadText = m_dimReadText && m_session->VM().CurrentLineSeen();
    for (const auto& line : m_nvlLines) view.nvlLines.push_back(line.speaker.empty() ? line.text : "【" + line.speaker + "】" + line.text);
    if (!view.nvlLines.empty() && m_session->Dialogue().State().fullText == m_nvlLines.back().text)
        view.nvlLines.back() = m_session->Dialogue().State().speaker.empty() ? m_session->Dialogue().State().displayText : "【" + m_session->Dialogue().State().speaker + "】" + m_session->Dialogue().State().displayText;
    return view;
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
    const bool inside = m_dockArea.valid &&
        input.MouseX() >= m_dockArea.x && input.MouseX() < m_dockArea.x + m_dockArea.width &&
        input.MouseY() >= m_dockArea.y && input.MouseY() < m_dockArea.y + m_dockArea.height;
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

void PlayerApp::HandleUIAction(const ui::GalgameAction& action) {
    const std::string& t = action.command;
    auto& audio = m_runtime.Audio();
    // Settings actions keep the MUTE checkboxes' data in step with the model.
    struct MuteSync {
        PlayerApp* app;
        bool active;
        ~MuteSync() { if (active) app->PublishMuteData(); }
    } muteSync{this, t.starts_with("set.") || t.starts_with("settings.") ||
                      t == "system.reset.apply"};
    const auto musicUnlocked = [this](const std::size_t index) {
        return index < m_musicTracks.size() && m_profile.MusicUnlocked(
            std::filesystem::path(m_musicTracks[index]).stem().string());
    };
    const auto playMusicAt = [this, &audio](const std::size_t index) {
        if (m_musicTracks.empty()) return;
        const std::size_t selected = index % m_musicTracks.size();
        if (!m_profile.MusicUnlocked(
                std::filesystem::path(m_musicTracks[selected]).stem().string())) return;
        m_musicTrackIndex = selected;
        const auto handle = audio.PlayBGM(m_musicTracks[m_musicTrackIndex], true, 250);
        if (audio.Status(handle) == px::audio::PlaybackStatus::Failed) {
            PX_LOG_ERROR("Player music gallery failed path={}",
                         m_musicTracks[m_musicTrackIndex]);
            return;
        }
        m_musicTrackSelected = true;
    };
    const auto scrollRoute = [this](const std::string_view controlName,
                                    const std::string_view command) {
        auto* scroll = FindUiControlNamed<ui::ScrollContainer>(
            m_ui.Root(), controlName);
        if (!scroll) return;
        auto offset = scroll->ScrollOffset();
        if (command.ends_with(".home")) offset.y = 0.0f;
        else if (command.ends_with(".end")) offset.y = 1'000'000.0f;
        else if (command.ends_with(".up")) offset.y = std::max(0.0f, offset.y - 360.0f);
        else if (command.ends_with(".down")) offset.y += 360.0f;
        scroll->SetScrollOffset(offset);
    };
    if (t == "saveLoad.page") {
        char* end = nullptr;
        const long page = std::strtol(action.argument.c_str(), &end, 10);
        if (end == action.argument.c_str() || *end != '\0' ||
            page < 1 || page > m_savePages) return;
        m_saveLoadPage = static_cast<int>(page);
        for (int index = 1; index <= m_savePages; ++index)
            (void)m_ui.UpdateUiData("saveLoad.page" + std::to_string(index),
                                    Variant(index == m_saveLoadPage));
        const bool saveMode = m_ui.CurrentScreen() == ui::GalgameUI::Screen::Save;
        (void)m_ui.ShowSaveLoad(saveMode, SaveItems(saveMode));
    } else if (t == "load.slot" || t == "save.slot") {
        const bool saveMode = t == "save.slot";
        if (saveMode ? AsksBefore("save") || AsksBefore("overwrite") : AsksBefore("load")) {
            char* end = nullptr;
            const long slot = std::strtol(action.argument.c_str(), &end, 10);
            if (end == action.argument.c_str() || *end != '\0' ||
                slot < m_saveFirstSlot ||
                slot >= m_saveFirstSlot + m_savePages * m_saveSlotsPerPage ||
                (!saveMode && !m_saves.Peek(static_cast<int>(slot)).exists))
                return;
            const int number = static_cast<int>(slot);
            const std::string_view operation = !saveMode ? "load"
                : m_saves.Peek(number).exists ? "overwrite" : "save";
            RequestConfirm(operation, [this, number, saveMode] {
                if (saveMode) {
                    if (SaveSlot(number, m_menuThumb))
                        (void)m_ui.ShowSaveLoad(true, SaveItems(true));
                } else if (LoadSlot(number)) {
                    BeginPresentationTransition(m_routeTransition);
                } else {
                    (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
                }
            });
        } else if (m_ui.SupportsSaveLoadConfirmation()) {
            char* end = nullptr;
            const long slot = std::strtol(action.argument.c_str(), &end, 10);
            if (end == action.argument.c_str() || *end != '\0' ||
                slot < m_saveFirstSlot ||
                slot >= m_saveFirstSlot + m_savePages * m_saveSlotsPerPage ||
                (!saveMode && !m_saves.Peek(static_cast<int>(slot)).exists))
                return;
            m_pendingSaveLoadSlot = static_cast<int>(slot);
            m_pendingSaveLoadSaveMode = saveMode;
            const auto prompt = !saveMode
                ? ui::GalgameUI::SaveLoadPrompt::Load
                : m_saves.Peek(static_cast<int>(slot)).exists
                    ? ui::GalgameUI::SaveLoadPrompt::Overwrite
                    : ui::GalgameUI::SaveLoadPrompt::Save;
            (void)m_ui.SetSaveLoadConfirmation(prompt);
        } else if (saveMode) {
            SaveSlot(std::atoi(action.argument.c_str()), m_menuThumb);
            m_ui.ShowSaveLoad(true, SaveItems(true));
        } else {
            LoadSlot(std::atoi(action.argument.c_str()));
        }
    } else if (t == "saveLoad.confirm.accept") {
        const auto pending = std::exchange(m_pendingSaveLoadSlot, std::nullopt);
        (void)m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
        if (!pending) return;
        if (m_pendingSaveLoadSaveMode) {
            if (SaveSlot(*pending, m_menuThumb))
                (void)m_ui.ShowSaveLoad(true, SaveItems(true));
        } else {
            if (LoadSlot(*pending))
                BeginPresentationTransition(m_routeTransition);
            else
                (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
        }
    } else if (t == "saveLoad.confirm.cancel") {
        m_pendingSaveLoadSlot.reset();
        (void)m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
    } else if (t == "saveLoad.error.dismiss") {
        (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(false));
    } else if (t == "set.master.value") {
        m_settings.masterVolume = std::clamp(std::atoi(action.argument.c_str()), 0, 128);
        m_settings.masterMuted = false;
        audio.SetMainVolume(m_settings.masterVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteMasterLabel"))
            label->SetText("□MUTE");
    } else if (t == "set.bgm.value") {
        m_settings.bgmVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);m_settings.bgmMuted=false;audio.SetBGMVolume(m_settings.bgmVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteBGMLabel")) label->SetText("□MUTE");
    } else if (t == "set.se.value") {
        m_settings.seVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);m_settings.seMuted=false;audio.SetSEVolume(m_settings.seVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteSELabel")) label->SetText("□MUTE");
    } else if (t == "set.voice.value") {
        m_settings.voiceVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);m_settings.voiceMuted=false;audio.SetVoiceVolume(m_settings.voiceVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteVoiceLabel")) label->SetText("□MUTE");
    } else if (t.starts_with("set.mute.")) {
        const std::string bus = t.substr(std::string("set.mute.").size());
        bool* muted = nullptr;
        int volume = 0;
        std::string label;
        if (bus == "master") { muted = &m_settings.masterMuted; volume = m_settings.masterVolume; label = "MuteMasterLabel"; }
        else if (bus == "bgm") { muted = &m_settings.bgmMuted; volume = m_settings.bgmVolume; label = "MuteBGMLabel"; }
        else if (bus == "se") { muted = &m_settings.seMuted; volume = m_settings.seVolume; label = "MuteSELabel"; }
        else if (bus == "voice") { muted = &m_settings.voiceMuted; volume = m_settings.voiceVolume; label = "MuteVoiceLabel"; }
        if (muted) {
            *muted = !*muted;
            const int effective = *muted ? 0 : volume;
            if (bus == "master") audio.SetMainVolume(effective);
            else if (bus == "bgm") audio.SetBGMVolume(effective);
            else if (bus == "se") audio.SetSEVolume(effective);
            else audio.SetVoiceVolume(effective);
            if (auto* control = FindUiControlNamed<ui::Label>(m_ui.Root(), label))
                control->SetText(*muted ? "☑MUTE" : "□MUTE");
            // The slider keeps the level; only the number reads "mute" (conf_mutetext).
            (void)m_ui.UpdateUiData("settings." + bus + "Value", Variant(*muted ? "mute" :
                std::to_string(static_cast<int>(std::round(volume * 100.0 / 128.0)))));
        }
    } else if (t == "set.duck.on" || t == "set.duck.off") {
        m_settings.duckBgmOnVoice = t.ends_with(".on");
        audio.SetVoiceDuckingEnabled(m_settings.duckBgmOnVoice);
        (void)m_ui.UpdateUiData("settings.duckOn", Variant(m_settings.duckBgmOnVoice));
        (void)m_ui.UpdateUiData("settings.duckOff", Variant(!m_settings.duckBgmOnVoice));
    } else if (t == "set.voiceCarry.on" || t == "set.voiceCarry.off") {
        m_settings.continueVoiceOnAdvance = t.ends_with(".on");
        (void)m_ui.UpdateUiData("settings.voiceCarryOn", Variant(m_settings.continueVoiceOnAdvance));
        (void)m_ui.UpdateUiData("settings.voiceCarryOff", Variant(!m_settings.continueVoiceOnAdvance));
    } else if (t == "set.voiceProfile.value") {
        const auto separator = action.argument.find(':');
        if (separator != std::string::npos && separator > 0 && separator <= 64) {
            const std::string id = action.argument.substr(0, separator);
            const int volume = std::clamp(std::atoi(action.argument.c_str() + separator + 1), 0, 128);
            m_settings.voiceProfileVolumes[id] = volume;
            m_settings.voiceProfileMuted[id] = false;
            audio.SetVoiceProfileVolume(id, volume);
            if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(),
                    "VoiceProfileMute_" + id + "Label")) label->SetText("□MUTE");
        }
    } else if (t == "set.voiceProfile.mute") {
        const std::string& id = action.argument;
        if (!id.empty() && id.size() <= 64) {
            const bool muted = !(m_settings.voiceProfileMuted[id]);
            m_settings.voiceProfileMuted[id] = muted;
            const auto volume = m_settings.voiceProfileVolumes.find(id);
            audio.SetVoiceProfileVolume(id, muted ? 0 :
                volume == m_settings.voiceProfileVolumes.end() ? 128 : volume->second);
            if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(),
                    "VoiceProfileMute_" + id + "Label"))
                label->SetText(muted ? "☑MUTE" : "□MUTE");
            if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(),
                    "VoiceProfileValue_" + id))
                label->SetText(muted ? "mute" : std::to_string(static_cast<int>(std::round(
                    (volume == m_settings.voiceProfileVolumes.end() ? 128 : volume->second)
                    * 100.0 / 128.0))));
        }
    } else if (t == "voiceProfile.test") {
        audio.PlayVoice(action.argument);
    } else if (t == "audio.test.se") {
        audio.PlaySE(action.argument);
    } else if (t == "set.speed.value") {
        m_settings.textSpeedMs=std::clamp(std::atoi(action.argument.c_str()),0,120);m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.skipread.value") {
        m_settings.skipReadOnly=action.argument=="true";
    } else if (t == "set.fullscreen.value") {
        (void)ApplyFullscreenSetting(action.argument == "true");
    } else if (t == "set.fullscreen.on" || t == "set.fullscreen.off") {
        (void)ApplyFullscreenSetting(t.ends_with(".on"));
    } else if(t=="set.textscale.value") { m_settings.textScale=std::clamp(std::atoi(action.argument.c_str())/100.0f,.75f,2.0f);
    } else if(t=="set.highcontrast.value") { m_settings.highContrast=action.argument=="true";
    } else if(t=="set.reducedmotion.value") { m_settings.reducedMotion=action.argument=="true";
    } else if(t=="set.reducedmotion.on"||t=="set.reducedmotion.off") {
        m_settings.reducedMotion=t.ends_with(".on");
        (void)m_ui.ShowSettings(SettingsUI());
    } else if(t=="set.selfvoicing.value") { m_settings.selfVoicing=action.argument=="true";if(!m_settings.selfVoicing)m_speech.Stop();
    } else if(t=="set.language.value") { if(LoadLocale(action.argument,true))(void)SaveSettingsIfWritable();
    } else if (t == "set.master.up") {
        m_settings.masterVolume = std::min(128, m_settings.masterVolume + 8);
        audio.SetMainVolume(m_settings.masterVolume);
    } else if (t == "set.master.down") {
        m_settings.masterVolume = std::max(0, m_settings.masterVolume - 8);
        audio.SetMainVolume(m_settings.masterVolume);
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
        if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::Load ||
            m_appState != AppState::Game)
            OpenScreen("save");
        else
            m_pendingSaveScreen = true;
    } else if (t == "settings.open") {
        OpenScreen("settings");
    } else if (t == "settings.reset" && AsksBefore("resetSettings")) {
        RequestConfirm("resetSettings", [this] { HandleUIAction({"system.reset.apply", {}}); });
    } else if (t == "settings.reset" &&
               m_ui.SupportsSystemConfirmation()) {
        m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::Reset;
        (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
    } else if (t == "settings.reset" || t == "system.reset.apply") {
        const std::string locale=m_settings.language;
        const bool fullscreen=m_settings.fullscreen;
        m_settings=m_settingsDefaults;
        m_runtime.GetInput().ResetKeyboardBindings();
        m_runtime.GetInput().ResetWheelBindings();
        m_settings.language=locale;
        m_settings.fullscreen=fullscreen;
        audio.SetMainVolume(m_settings.masterVolume);
        audio.SetBGMVolume(m_settings.bgmVolume);
        audio.SetSEVolume(m_settings.seVolume);
        audio.SetVoiceVolume(m_settings.voiceVolume);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
        ApplyAllOptions();
        (void)m_ui.ShowSettings(SettingsUI());
    } else if (t.starts_with("settings.page")) {
        const std::string page = t.substr(std::string("settings.page").size());
        if (m_boot.routeScenes.contains("settings") && page.size() == 1 &&
            page[0] >= '1' && page[0] <= '4') {
            m_settingsPage = page[0] - '0';
            for (int index = 1; index <= 4; ++index)
                (void)m_ui.UpdateUiData("settings.page" + std::to_string(index),
                                        Variant(index == m_settingsPage));
            // Pages are bound groups of one installed screen; rebuilding it
            // here would restart every transition on the screen.
            return;
        }
        while (!m_session->Routes().CurrentModalRoute().empty())
            (void)m_session->Routes().CloseModal();
        OpenScreen(page == "0" ? "settings" : "settings-page" + page);
    } else if (t == "settings.noop") {
        // The original setting exists visually but has no matching portable
        // host capability yet. Keep the authored control responsive without
        // mutating an unrelated setting.
    } else if (t == "music.play") {
        const auto found = std::ranges::find(m_musicTracks, action.argument);
        if (found != m_musicTracks.end()) {
            playMusicAt(static_cast<std::size_t>(
                std::distance(m_musicTracks.begin(), found)));
            PX_LOG_INFO("Player music gallery selected path={}", action.argument);
        } else {
            PX_LOG_ERROR("Player music gallery received unknown track path={}",
                         action.argument);
        }
    } else if (t == "music.previous") {
        if (!m_musicTracks.empty())
            for (std::size_t step = 1; step <= m_musicTracks.size(); ++step) {
                const auto index = (m_musicTrackIndex + m_musicTracks.size() -
                                    step % m_musicTracks.size()) % m_musicTracks.size();
                if (musicUnlocked(index)) { playMusicAt(index); break; }
            }
    } else if (t == "music.next") {
        if (!m_musicTracks.empty())
            for (std::size_t step = 1; step <= m_musicTracks.size(); ++step) {
                const auto index = (m_musicTrackIndex + step) % m_musicTracks.size();
                if (musicUnlocked(index)) { playMusicAt(index); break; }
            }
    } else if (t == "music.random") {
        std::vector<std::size_t> available;
        for (std::size_t index = 0; index < m_musicTracks.size(); ++index)
            if (musicUnlocked(index)) available.push_back(index);
        if (!available.empty())
            playMusicAt(available[static_cast<std::size_t>(SDL_GetTicks()) %
                                  available.size()]);
    } else if (t == "music.stop") {
        audio.StopBGM(200);
        m_musicTrackSelected = false;
    } else if (t == "music.resume") {
        if (m_musicTrackSelected) audio.SetMusicPaused(false);
        else if (musicUnlocked(m_musicTrackIndex)) playMusicAt(m_musicTrackIndex);
        else {
            for (std::size_t index = 0; index < m_musicTracks.size(); ++index)
                if (musicUnlocked(index)) { playMusicAt(index); break; }
        }
    } else if (t == "music.about") {
        audio.SetMusicPaused(true);
    } else if (t.starts_with("music.scroll.")) {
        scrollRoute("MusicTracksScroll", t);
    } else if (t == "flowchart.select") {
        const auto selected = nlohmann::json::parse(
            action.argument, nullptr, false);
        if (selected.is_object() && selected.value("entry", "") != "" &&
            selected.value("heading", "").size() < 512 &&
            selected.value("details", "").size() < 2048) {
            m_selectedFlowEntry = selected.value("entry", "");
            if (auto* item = FindUiControlNamed<ui::Label>(
                    m_ui.Root(), "FlowSelectedItem"))
                item->SetText(selected.value("heading", ""));
            if (auto* heading = FindUiControlNamed<ui::Label>(
                    m_ui.Root(), "FlowSelectedHeading"))
                heading->SetText(selected.value("heading", ""));
            if (auto* details = FindUiControlNamed<ui::Label>(
                    m_ui.Root(), "FlowSelectedDetails"))
                details->SetText(selected.value("details", ""));
            if (auto* jump = FindUiControlNamed<ui::Button>(
                    m_ui.Root(), "scenejumpButton"))
                jump->SetEnabled(true);
            PX_LOG_INFO("Player flowchart selected entry={}", m_selectedFlowEntry);
        }
    } else if (t == "flowchart.jump") {
        if (!m_selectedFlowEntry.empty()) {
            m_pendingStoryEntry = std::exchange(m_selectedFlowEntry, {});
            (void)m_session->Routes().CloseModal();
            StartGame();
        }
    } else if (t == "replay.launch") {
        if (!action.argument.empty()) {
            m_pendingStoryEntry = action.argument;
            while (!m_session->Routes().CurrentModalRoute().empty())
                (void)m_session->Routes().CloseModal();
            StartGame();
        }
    } else if (t == "replay.page") {
        const int page = std::atoi(action.argument.c_str());
        if (page >= 1 && page <= 4) {
            m_replayPage = page;
            for (int index = 1; index <= 4; ++index) {
                if (const Status updated = m_ui.UpdateUiData(
                        "replay.page" + std::to_string(index),
                        Variant(index == page)); !updated)
                    for (const auto& diagnostic : updated.Diagnostics())
                        diag::Emit(diagnostic);
            }
        }
    } else if (t == "title.chapter3") {
        OpenScreen("chapter3");
    } else if (t.starts_with("flowchart.scroll.")) {
        scrollRoute("FlowGraphScroll", t);
    } else if (t == "flowchart.filter") {
        if (!action.argument.empty() &&
            (action.argument == "flowchart" ||
             action.argument.starts_with("flowchart-"))) {
            m_selectedFlowEntry.clear();
            while (!m_session->Routes().CurrentModalRoute().empty())
                (void)m_session->Routes().CloseModal();
            OpenScreen(action.argument);
        }
    } else if (t == "cg.view") {
        const auto& entries = m_catalog.Gallery();
        const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
            return entry.id == action.argument;
        });
        if (found != entries.end()) {
            if (m_profile.CGUnlocked(found->id)) m_viewingCG = found->image;
        } else {
            m_viewingCG = action.argument;
        }
    } else if (t == "set.skipread.toggle") {
        m_settings.skipReadOnly = !m_settings.skipReadOnly;
        m_ui.ShowSettings(SettingsUI());
    } else if (t == "set.fullscreen.toggle") {
        (void)ApplyFullscreenSetting(!m_settings.fullscreen);
        if (m_ui.CurrentRoute() == "settings")
            (void)m_ui.ShowSettings(SettingsUI());
    } else if (t == "game.start") {
        StartGame();
    } else if (t == "game.continue") {
        const auto saves = m_saves.List(1000);
        std::optional<int> latest;
        std::uint64_t latestTimestamp = 0;
        for (std::size_t index = 0; index < saves.size(); ++index) {
            if (saves[index].exists &&
                (!latest || saves[index].timestamp >= latestTimestamp)) {
                latest = static_cast<int>(index);
                latestTimestamp = saves[index].timestamp;
            }
        }
        if (latest) (void)LoadSlot(*latest);
        else OpenScreen("load");
    } else if (t == "extra.open") {
        OpenScreen("extra");
    } else if (t == "flowchart.open") {
        OpenScreen("flowchart");
    } else if (t == "replay.open") {
        OpenScreen("replay");
    } else if (t == "music.open") {
        OpenScreen("music");
    } else if (t == "title.confirm.cancel") {
        (void)m_ui.SetTitleConfirmation(false);
    } else if (t == "title.open" && m_appState == AppState::Game &&
               AsksBefore("returnToTitle")) {
        RequestConfirm("returnToTitle", [this] { HandleUIAction({"title.confirm.accept", {}}); });
    } else if (t == "title.open" && m_appState == AppState::Game &&
               m_ui.SupportsTitleConfirmation()) {
        (void)m_ui.SetTitleConfirmation(true);
    } else if (t == "title.open" || t == "title.confirm.accept") {
        (void)m_ui.SetTitleConfirmation(false);
        while (!m_session->Routes().CurrentModalRoute().empty())
            (void)m_session->Routes().CloseModal();
        (void)m_session->Routes().Replace("title");
    } else if (t == "hud.hide") {
        m_hudHidden = true;
    } else if (t == "choice.select") {
        m_session->SelectChoice(std::atoi(action.argument.c_str()));
    } else if (t == "mode.auto") {
        m_autoMode = !m_autoMode; if (m_autoMode) m_skipMode = false;
    } else if (t == "mode.skip") {
        m_skipMode = !m_skipMode; if (m_skipMode) m_autoMode = false;
    } else if (t == "backlog.open") {
        OpenScreen("backlog");
    } else if (t == "save.quick") {
        // Written after the Stage renders, so the thumbnail has no HUD.
        RequestConfirm("quickSave", [this] { m_saveRequested = true; });
    } else if (t == "load.quick") {
        RequestConfirm("quickLoad", [this] {
            if (m_saves.Peek(0).exists) (void)LoadSlot(0);
        });
    } else if (t == "voice.replay") {
        const auto& lines = m_session->Backlog().Entries();
        const auto voiced = std::find_if(lines.rbegin(), lines.rend(), [](const auto& entry) {
            return !entry.voice.empty();
        });
        if (voiced != lines.rend() && voiced == lines.rbegin())
            m_runtime.Audio().PlayVoice(voiced->voice.find('/') != std::string::npos
                ? voiced->voice : m_session->VM().Config().voiceDir + voiced->voice);
    } else if (t == "choice.previous") {
        // The choice autosave is the state at the most recent choice prompt.
        RequestConfirm("previousChoice", [this] {
            if (m_saves.Peek(kAutoSaveSlot).exists) (void)LoadSlot(kAutoSaveSlot);
        });
    } else if (t == "choice.next") {
        // Skip stops at the next choice, and at unread text unless the
        // player allowed skipping it.
        RequestConfirm("nextChoice", [this] {
            m_skipMode = true;
            m_autoMode = false;
        });
    } else if (t == "backlog.voice" || t == "backlog.rollback") {
        std::uint64_t sequence = 0;
        try {
            sequence = std::stoull(action.argument);
        } catch (const std::exception&) {
            sequence = 0;
        }
        if (const auto* entry = m_session->Backlog().Find(sequence)) {
            if (t == "backlog.voice" && !entry->voice.empty())
                m_runtime.Audio().PlayVoice(entry->voice.find('/') != std::string::npos ? entry->voice : m_session->VM().Config().voiceDir + entry->voice);
            else RequestConfirm("backlogJump", [this, sequence = entry->sequence] {
                RollbackToBacklogSequence(sequence);
            });
        }
    } else if (t == "overlay.close") {
        if (m_dialog) {
            HandleUIAction({"dialog.cancel", {}});
            return;
        }
        if (m_pendingSystemPrompt != ui::GalgameUI::SystemPrompt::None) {
            m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
            (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
            return;
        }
        if (m_pendingSaveLoadSlot) {
            m_pendingSaveLoadSlot.reset();
            (void)m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
            return;
        }
        const bool restoreTitleMusic = m_musicTrackSelected &&
            (m_ui.CurrentRoute() == "music" || m_ui.CurrentRoute() == "extra");
        if (!m_session->Routes().CloseModal()) return;
        if (restoreTitleMusic && !TitleBgmPath().empty()) {
            audio.PlayBGM(TitleBgmPath(), true, 300);
            m_musicTrackSelected = false;
        }
        // Closing a menu must not wait on disk; both files are written by
        // background workers and flushed synchronously at shutdown.
        SaveSettingsInBackground();
        MarkProfileDirty(true);
        PX_LOG_INFO("Player overlay closed modal={} route={}",
                    m_session->Routes().CurrentModalRoute(),
                    m_session->Routes().CurrentRoute());
    } else if (t == "system.confirm.cancel") {
        m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
        (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
    } else if (t == "system.confirm.accept") {
        const auto prompt = std::exchange(
            m_pendingSystemPrompt, ui::GalgameUI::SystemPrompt::None);
        (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
        if (prompt == ui::GalgameUI::SystemPrompt::Reset)
            HandleUIAction({"system.reset.apply", {}});
        else if (prompt == ui::GalgameUI::SystemPrompt::Exit)
            m_quitRequested = true;
    } else if (t == "dialog.accept") {
        if (!m_dialog) return;
        auto dialog = std::move(*m_dialog);
        m_dialog.reset();
        if (m_dialogSkipNext && !dialog.dontAskOption.empty())
            (void)SetOption(dialog.dontAskOption, 1);
        m_dialogSkipNext = false;
        PublishDialog();
        dialog.accept();
    } else if (t == "dialog.cancel") {
        m_dialog.reset();
        m_dialogSkipNext = false;
        PublishDialog();
    } else if (t == "dialog.toggleSkip") {
        if (!m_dialog) return;
        m_dialogSkipNext = !m_dialogSkipNext;
        PublishDialog();
    } else if (t == "option.set") {
        const auto split = action.argument.find('=');
        if (split != std::string::npos) {
            char* end = nullptr;
            const long value = std::strtol(action.argument.c_str() + split + 1, &end, 10);
            if (end != action.argument.c_str() + split + 1 && *end == '\0')
                (void)SetOption(std::string_view(action.argument).substr(0, split),
                                static_cast<int>(value));
        }
    } else if (t == "app.quit" && AsksBefore("exit")) {
        RequestConfirm("exit", [this] { m_quitRequested = true; });
    } else if (t == "app.quit") {
        if (m_ui.SupportsSystemConfirmation()) {
            m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::Exit;
            (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
        } else m_quitRequested = true;
    } else {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                    .code = "PXPLAYER5023",
                                    .category = "Player.Input",
                                    .message = "UI command is not handled by this Player",
                                    .details = t});
        return;
    }
    if (t.ends_with(".up") || t.ends_with(".down")) m_ui.ShowSettings(SettingsUI());
}

void PlayerApp::RenderStageWithScripts() {
    const diag::ScopedFramePhase profiled(diag::FramePhase::StageRender);
    if (m_scriptHost)
        m_scriptHost->RenderRetained(script::RenderPhase::BeforeStage);
    m_session->Stage().Render();
    if (m_scriptHost)
        m_scriptHost->RenderRetained(script::RenderPhase::AfterStage);
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
                         pose.y != 0.0f || pose.scale != 1.0f || pose.rotation != 0.0f;
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
        m_runtime.Renderer().PushAffineTransform(
            {a, b, -b, a, pivotX - (a * pivotX - b * pivotY) + pose.x + shakeX,
             pivotY - (b * pivotX + a * pivotY) + pose.y + shakeY});
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
    if (m_settingsSoundE2E && m_settingsSoundE2EStage == 3 &&
        !m_routePreloadBatch &&
        !m_runtime.Renderer().PresentationHoldActive()) {
        if (const auto& transition = m_session->Routes().ActiveTransition();
            transition && m_runtime.Renderer().ScreenEffectPlaying(transition->handle))
            return;
        const auto summary = graphics::CaptureFrameSummary(m_runtime.Renderer().Handle());
        // The project's first and last voice profiles; the journey needs some.
        const std::string firstProfile = m_voiceProfileIds.empty() ? "" : m_voiceProfileIds.front();
        const std::string lastProfile = m_voiceProfileIds.empty() ? "" : m_voiceProfileIds.back();
        const bool hasFirst = !firstProfile.empty() && FindUiControlNamed<ui::Slider>(
            m_ui.Root(), "VoiceProfile_" + firstProfile) != nullptr;
        const bool hasLast = !lastProfile.empty() && FindUiControlNamed<ui::Slider>(
            m_ui.Root(), "VoiceProfile_" + lastProfile) != nullptr;
        const bool captured = CapturePresentationE2EFrame("settings-sound");
        const int originalMaster = m_runtime.Audio().MainVolume();
        HandleUIAction({"set.mute.master", {}});
        const bool muted = m_runtime.Audio().MainVolume() == 0;
        HandleUIAction({"set.mute.master", {}});
        const bool restored = m_runtime.Audio().MainVolume() == originalMaster;
        HandleUIAction({"set.voiceProfile.value", firstProfile + ":64"});
        const bool profileChanged = m_settings.voiceProfileVolumes[firstProfile] == 64;
        HandleUIAction({"set.voiceProfile.mute", firstProfile});
        const bool profileMuted = m_settings.voiceProfileMuted[firstProfile];
        HandleUIAction({"set.voiceProfile.mute", firstProfile});
        // PRISMATIX_E2E_TEST_VOICE names a voice file the TEST button must play;
        // without it the journey does not check playback.
        const char* testVoiceEnv = SDL_getenv("PRISMATIX_E2E_TEST_VOICE");
        const std::string testVoice = testVoiceEnv ? testVoiceEnv : "";
        bool testPlayed = true;
        if (!testVoice.empty()) {
            HandleUIAction({"voiceProfile.test", testVoice});
            testPlayed = m_runtime.Audio().CaptureState().voice.path == testVoice;
            m_runtime.Audio().StopVoice();
        }
        const bool passed = hasFirst && hasLast && captured &&
            summary.Valid() && summary.sampledColors >= 8 && muted &&
            restored && profileChanged && profileMuted && testPlayed;
        PX_LOG_INFO("Player settings sound E2E {} colors={} first={} last={} mute={} test={}",
                    passed ? "passed" : "failed", summary.sampledColors,
                    hasFirst, hasLast, muted && restored,
                    testVoice.empty() ? "unchecked" : testPlayed ? "true" : "false");
        m_e2eFailed = !passed;
        m_settingsSoundE2E = false;
        m_quitRequested = true;
    }
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
    CaptureE2EGalleryFrame();
    CaptureUIRoutesE2EFrame();
}

void PlayerApp::ConfigureE2EJourney() {
    const char* journey=SDL_getenv("PRISMATIX_E2E_JOURNEY");
    if(!journey)return;
    if(std::string_view(journey)=="save-load"){
        m_saveLoadE2E=true;
        m_saveLoadE2EStartedAt=SDL_GetTicks();
        PX_LOG_INFO("Player save-load E2E armed slot=2");
        return;
    }
    if(std::string_view(journey)=="presentation"){
        m_presentationE2E=true;
        m_presentationE2EStage=1;
        m_presentationE2EStartedAt=SDL_GetTicks();
        const char* captureDir=SDL_getenv("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir=captureDir?captureDir:"e2e-captures";
        const char* targetOperation=SDL_getenv("PRISMATIX_E2E_TARGET_OPERATION_ID");
        m_presentationE2ETargetOperationId=targetOperation?targetOperation:"";
        m_presentationE2ESplashCaptured.assign(
            m_splash?m_splash->Entries().size():0,false);
        PX_LOG_INFO("Player presentation E2E armed captureDir={}",
                    m_presentationE2ECaptureDir);
        return;
    }
    if(std::string_view(journey)=="ui-routes"){
        m_uiRoutesE2E=true;
        m_uiRoutesE2EStartedAt=SDL_GetTicks();
        const char* captureDir=SDL_getenv("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir=captureDir?captureDir:"e2e-ui-routes";
        PX_LOG_INFO("Player UI routes E2E armed captureDir={} routes={}",
                    m_presentationE2ECaptureDir,
                    std::size(kUIRouteE2ERoutes));
        return;
    }
    if(std::string_view(journey)=="ui-actions"){
        m_uiActionsE2E=true;
        m_uiActionsE2EStartedAt=SDL_GetTicks();
        PX_LOG_INFO("Player UI actions E2E armed");
        return;
    }
    if(std::string_view(journey)=="settings-fullscreen"){
        m_settingsFullscreenE2E=true;
        m_settingsFullscreenE2EStartedAt=SDL_GetTicks();
        PX_LOG_INFO("Player settings fullscreen E2E armed");
        return;
    }
    if(std::string_view(journey)=="save-load-confirm"){
        m_saveLoadConfirmE2E = true;
        m_saveLoadConfirmE2EStartedAt = SDL_GetTicks();
        PX_LOG_INFO("Player save/load confirmation E2E armed");
        return;
    }
    if(std::string_view(journey)=="settings-sound"){
        m_settingsSoundE2E = true;
        m_settingsSoundE2EStartedAt = SDL_GetTicks();
        const char* captureDir = SDL_getenv("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir = captureDir ? captureDir : "e2e-settings-sound";
        PX_LOG_INFO("Player settings sound E2E armed captureDir={}",
                    m_presentationE2ECaptureDir);
        return;
    }
    if(std::string_view(journey)=="save-load-incompatible"){
        m_incompatibleSaveE2E = true;
        m_incompatibleSaveE2EStartedAt = SDL_GetTicks();
        PX_LOG_INFO("Player incompatible save UI E2E armed");
        return;
    }
    if(std::string_view(journey)=="bench"){
        m_benchE2E = true;
        m_benchStageAt = SDL_GetTicks();
        PX_LOG_INFO("Player bench armed");
        return;
    }
    if(std::string_view(journey)!="catalog")return;
    m_e2eStage=E2EStage::AwaitTitle;
    m_e2eStartedAt=SDL_GetTicks();
    PX_LOG_INFO("Player E2E journey armed name=catalog");
}

bool PlayerApp::CapturePresentationE2EFrame(const std::string_view name){
    int width=0,height=0;
    m_runtime.Renderer().GetLogicalSize(width,height);
    auto png=graphics::CaptureThumbnailPng(
        m_runtime.Renderer().Handle(),width,height);
    if(png.empty())return false;
    std::error_code error;
    const std::filesystem::path directory=m_presentationE2ECaptureDir;
    std::filesystem::create_directories(directory,error);
    if(error)return false;
    const auto path=directory/(std::string(name)+".png");
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
    const bool saved=stream.good();
    PX_LOG_INFO("Player presentation E2E capture name={} path={} saved={}",
                name,path.string(),saved);
    return saved;
}

void PlayerApp::BenchCapture(std::string name) {
    if (!SDL_getenv("PRISMATIX_E2E_CAPTURE_DIR")) return;
    m_presentationE2ECaptureDir = SDL_getenv("PRISMATIX_E2E_CAPTURE_DIR");
    m_runtime.OnNextComposedFrame([this, name = std::move(name)] {
        (void)CapturePresentationE2EFrame(name);
    });
}

void PlayerApp::DriveBenchJourney() {
    // Reading (auto), skipping, then opening each menu route. Timings come
    // from the frame profiler (PRISMATIX_PERF) plus the summaries logged here.
    static constexpr std::array<std::string_view, 5> kRoutes{
        "save", "load", "settings", "backlog", "gallery"};
    const std::uint64_t now = SDL_GetTicks();
    const auto lines = [this] {
        return m_session->Backlog().LastSequence() - m_benchSequence;
    };
    switch (m_benchStage) {
        case 0:
            if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title" &&
                !m_routePreloadBatch && ++m_benchSettledFrames >= 60) {
                m_benchSettledFrames = 0;
                BenchCapture("title");
                StartGame();
                m_benchStage = 1;
            }
            break;
        case 1:
            if (m_appState == AppState::Game && !m_gameStartTransitionActive) {
                m_settings.skipReadOnly = false;
                m_benchSequence = m_session->Backlog().LastSequence();
                m_benchStageAt = now;
                m_benchStage = 2;
            }
            break;
        case 2:
            if (m_session->VM().State() == vn::VMState::WaitingChoice) {
                if (!m_benchChoiceCaptured) {
                    // Hold the prompt a few frames so the capture shows it.
                    if (++m_benchSettledFrames < 20) break;
                    m_benchSettledFrames = 0;
                    m_benchChoiceCaptured = true;
                    BenchCapture("choice");
                    break;
                }
                m_session->SelectChoice(0);
            }
            if (!m_benchDialogueCaptured &&
                m_session->VM().State() == vn::VMState::WaitingClick) {
                m_benchDialogueCaptured = true;
                BenchCapture("dialogue");
            }
            m_skipMode = true;
            if (now - m_benchStageAt >= 15'000) {
                PX_LOG_INFO("Player bench skip lines={} seconds=15 linesPerSecond={:.1f}",
                            lines(), static_cast<double>(lines()) / 15.0);
                m_skipMode = false;
                m_autoMode = true;
                m_settings.autoWaitMs = 300;
                m_benchSequence = m_session->Backlog().LastSequence();
                m_benchStageAt = now;
                m_benchStage = 3;
            }
            break;
        case 3:
            if (m_session->VM().State() == vn::VMState::WaitingChoice)
                m_session->SelectChoice(0);
            m_autoMode = true;
            // Screenshots of ordinary reading and of any screen transition.
            if (const auto effect = m_runtime.Renderer().ActiveScreenEffect();
                effect && effect->elapsedSeconds >= effect->durationSeconds * 0.5f &&
                m_benchCapturedEffect != effect->handle) {
                m_benchCapturedEffect = effect->handle;
                BenchCapture("transition-" + effect->effect + "-" + std::to_string(effect->handle));
            }
            if ((now - m_benchStageAt) / 4000 != m_benchReadingShot) {
                m_benchReadingShot = (now - m_benchStageAt) / 4000;
                BenchCapture("reading-" + std::to_string(m_benchReadingShot));
            }
            if (now - m_benchStageAt >= 20'000) {
                PX_LOG_INFO("Player bench reading lines={} seconds=20", lines());
                m_autoMode = false;
                m_benchStage = 4;
            }
            break;
        case 4:
            if (m_benchRouteIndex >= kRoutes.size()) {
                PX_LOG_INFO("Player bench complete");
                m_benchE2E = false;
                m_quitRequested = true;
                break;
            }
            OpenScreen(std::string(kRoutes[m_benchRouteIndex]));
            m_benchStageAt = now;
            m_benchSettledFrames = 0;
            m_benchStage = 5;
            break;
        case 5:
            if (m_routePreloadBatch ||
                m_runtime.Renderer().PresentationHoldActive()) break;
            if (++m_benchSettledFrames == 1)
                PX_LOG_INFO("Player bench route={} readyMs={}",
                            kRoutes[m_benchRouteIndex], now - m_benchStageAt);
            if (m_benchSettledFrames == 30 || m_benchSettledFrames == 85)
                BenchCapture("route-" + std::string(kRoutes[m_benchRouteIndex]) + "-" +
                             std::to_string(m_benchSettledFrames));
            if (m_benchSettledFrames >= 90) {
                HandleUIAction({"overlay.close", {}});
                ++m_benchRouteIndex;
                m_benchStage = 4;
            }
            break;
        default:
            break;
    }
}

void PlayerApp::DriveE2EJourney() {
    if (m_benchE2E) {
        DriveBenchJourney();
        return;
    }
    if (m_incompatibleSaveE2E) {
        if (SDL_GetTicks() - m_incompatibleSaveE2EStartedAt > 30'000) {
            PX_LOG_ERROR("Player incompatible save UI E2E timed out stage={}",
                         m_incompatibleSaveE2EStage);
            m_e2eFailed = true;
            m_incompatibleSaveE2E = false;
            m_quitRequested = true;
            return;
        }
        switch (m_incompatibleSaveE2EStage) {
            case 0:
                if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title") {
                    OpenScreen("load");
                    m_incompatibleSaveE2EStage = 1;
                }
                break;
            case 1:
                if (m_ui.CurrentRoute() == "load") {
                    HandleUIAction({"load.slot", "1"});
                    m_incompatibleSaveE2EStage = 2;
                }
                break;
            case 2: {
                HandleUIAction({"saveLoad.confirm.accept", {}});
                auto* error = FindUiControlNamed<ui::Control>(m_ui.Root(), "SaveLoadError");
                const bool passed = error && error->IsVisibleInTree() &&
                    m_ui.CurrentRoute() == "load" && m_appState == AppState::Title;
                PX_LOG_INFO("Player incompatible save UI E2E {}", passed ? "passed" : "failed");
                m_e2eFailed = !passed;
                m_incompatibleSaveE2E = false;
                m_quitRequested = true;
                break;
            }
            default: break;
        }
        return;
    }
    if (m_settingsSoundE2E) {
        if (SDL_GetTicks() - m_settingsSoundE2EStartedAt > 30'000) {
            PX_LOG_ERROR("Player settings sound E2E timed out stage={}",
                         m_settingsSoundE2EStage);
            m_e2eFailed = true;
            m_settingsSoundE2E = false;
            m_quitRequested = true;
            return;
        }
        if (m_settingsSoundE2EStage == 0 && m_appState == AppState::Title &&
            m_ui.CurrentRoute() == "title") {
            OpenScreen("settings");
            m_settingsSoundE2EStage = 1;
        } else if (m_settingsSoundE2EStage == 1 &&
                   m_ui.CurrentRoute() == "settings") {
            m_settingsPage = 3;
            for (int page = 1; page <= 4; ++page)
                (void)m_ui.UpdateUiData("settings.page" + std::to_string(page),
                                        Variant(page == 3));
            m_settingsSoundE2EStage = 2;
        } else if (m_settingsSoundE2EStage == 2 &&
                   !m_routePreloadBatch &&
                   SDL_GetTicks() - m_settingsSoundE2EStartedAt > 2'000) {
            m_settingsSoundE2EStage = 3;
        }
        return;
    }
    if (m_saveLoadConfirmE2E) {
        const auto finish = [this](bool passed) {
            m_e2eFailed = !passed;
            PX_LOG_INFO("Player save/load confirmation E2E {}", passed ? "passed" : "failed");
            m_saveLoadConfirmE2E = false;
            m_quitRequested = true;
        };
        if (SDL_GetTicks() - m_saveLoadConfirmE2EStartedAt > 90'000) {
            PX_LOG_ERROR("Player save/load confirmation E2E timed out stage={}",
                         m_saveLoadConfirmE2EStage);
            finish(false);
            return;
        }
        switch (m_saveLoadConfirmE2EStage) {
            case 0:
                if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title") {
                    StartGame();
                    m_saveLoadConfirmE2EStage = 1;
                }
                break;
            case 1:
                if (m_appState == AppState::Game && !m_gameStartTransitionActive &&
                    m_session->Dialogue().Active()) {
                    if (!SaveSlot(1, {})) { finish(false); break; }
                    OpenScreen("load");
                    m_saveLoadConfirmE2EStage = 2;
                }
                break;
            case 2:
                if (m_ui.CurrentRoute() == "load") {
                    HandleUIAction({"load.slot", "1"});
                    m_saveLoadConfirmE2EStage = 3;
                }
                break;
            case 3:
                HandleUIAction({"saveLoad.confirm.accept", {}});
                if (m_appState == AppState::Game && m_ui.CurrentRoute() != "load")
                    finish(true);
                else
                    finish(false);
                break;
            default: break;
        }
        return;
    }
    if(m_settingsFullscreenE2E){
        if(SDL_GetTicks()-m_settingsFullscreenE2EStartedAt>30'000){
            PX_LOG_ERROR("Player settings fullscreen E2E timed out stage={}",
                         m_settingsFullscreenE2EStage);
            m_e2eFailed=true;m_settingsFullscreenE2E=false;m_quitRequested=true;
            return;
        }
        const auto activate=[this](const char* name){
            auto* button=FindUiControlNamed<ui::Button>(m_ui.Root(),name);
            if(button)button->Activate();
            return button!=nullptr;
        };
        switch(m_settingsFullscreenE2EStage){
            case 0:
                if(m_appState==AppState::Title&&m_ui.CurrentRoute()=="title"){
                    OpenScreen("settings");
                    m_settingsFullscreenE2EStage=1;
                }
                break;
            case 1:
                if(m_ui.CurrentRoute()=="settings"){
                    if(!activate("FullscreenModeLabelHit")){
                        PX_LOG_ERROR("Player settings fullscreen label button missing");
                        m_e2eFailed=true;m_settingsFullscreenE2E=false;m_quitRequested=true;
                    }else m_settingsFullscreenE2EStage=2;
                }
                break;
            case 2:
                if(m_settings.fullscreen&&
                   (SDL_GetWindowFlags(m_runtime.GetWindow().Handle())&
                    SDL_WINDOW_FULLSCREEN)){
                    if(!activate("WindowModeLabelHit")){
                        PX_LOG_ERROR("Player settings windowed label button missing");
                        m_e2eFailed=true;m_settingsFullscreenE2E=false;m_quitRequested=true;
                    }else m_settingsFullscreenE2EStage=3;
                }
                break;
            case 3:
                if(!m_settings.fullscreen&&
                   !(SDL_GetWindowFlags(m_runtime.GetWindow().Handle())&
                     SDL_WINDOW_FULLSCREEN)){
                    int width = 0, height = 0;
                    SDL_GetWindowSize(m_runtime.GetWindow().Handle(), &width, &height);
                    if (width != m_settings.windowWidth ||
                        height != m_settings.windowHeight) {
                        PX_LOG_ERROR("Player windowed E2E size mismatch actual={}x{} expected={}x{}",
                                     width, height, m_settings.windowWidth, m_settings.windowHeight);
                        m_e2eFailed = true;
                    } else {
                        PX_LOG_INFO("Player settings fullscreen E2E complete windowed={}x{}",
                                    width, height);
                    }
                    m_settingsFullscreenE2E=false;m_quitRequested=true;
                }
                break;
            default:break;
        }
        return;
    }
    if(m_saveLoadE2E){
        if(SDL_GetTicks()-m_saveLoadE2EStartedAt>120'000){
            PX_LOG_ERROR("Player save-load E2E timed out before title");
            m_e2eFailed=true;
            m_saveLoadE2E=false;
            m_quitRequested=true;
            return;
        }
        if(m_appState!=AppState::Title)return;
        const bool loaded=LoadSlot(2);
        m_e2eFailed=!loaded||m_appState!=AppState::Game;
        if(m_e2eFailed)
            PX_LOG_ERROR("Player save-load E2E failed slot=2");
        else
            PX_LOG_INFO("Player save-load E2E passed slot=2");
        m_saveLoadE2E=false;
        m_quitRequested=true;
        return;
    }
    if(m_presentationE2E){
        if(SDL_GetTicks()-m_presentationE2EStartedAt>90'000){
            PX_LOG_ERROR("Player presentation E2E timed out stage={}",
                         m_presentationE2EStage);
            m_quitRequested=true;
            m_presentationE2E=false;
            return;
        }
        if(m_presentationE2EStage==3&&m_appState==AppState::Game&&
           !m_gameStartTransitionActive){
            if(!m_presentationE2ETargetOperationId.empty()&&
               m_session->VM().CurrentOperationId()==m_presentationE2ETargetOperationId){
                m_session->Dialogue().ShowAll();
                return;
            }
            const auto actors=m_session->Stage().Snapshot();
            const auto& dialogue=m_session->Dialogue().State();
            const bool waitingForPresentation=!actors.empty()&&
                !dialogue.fullText.empty()&&
                std::ranges::any_of(actors,[](const auto& actor){
                    return !actor.exiting&&actor.targetAlpha>=245.0f&&
                           actor.alpha<245.0f;
                });
            if(waitingForPresentation)return;
            if(m_session->VM().State()==vn::VMState::WaitingChoice)
                m_session->SelectChoice(0);
            else if(m_session->VM().State()==vn::VMState::WaitingClick||
                    m_session->VM().State()==vn::VMState::WaitingTimer){
                m_session->Dialogue().ShowAll();
                m_session->Advance();
            }
        }
        return;
    }
    if(m_uiRoutesE2E){
        const std::uint64_t now=SDL_GetTicks();
        if(now-m_uiRoutesE2EStartedAt>120'000){
            PX_LOG_ERROR("Player UI routes E2E timed out index={} route={}",
                         m_uiRoutesE2EIndex,
                         m_uiRoutesE2EIndex<std::size(kUIRouteE2ERoutes)
                             ? kUIRouteE2ERoutes[m_uiRoutesE2EIndex]
                             : std::string_view("complete"));
            m_e2eFailed=true;
            m_uiRoutesE2E=false;
            m_quitRequested=true;
            return;
        }
        if(m_uiRoutesE2ESaveConfirmStage==1){
            auto* dialog=FindUiControlNamed<ui::Control>(
                m_ui.Root(),"SaveLoadConfirm");
            if(m_pendingSaveLoadSlot==0&&dialog&&dialog->IsVisibleInTree()){
                m_uiRoutesE2ESaveConfirmStage=2;
                m_uiRoutesE2EAwaitingCapture=true;
                m_uiRoutesE2ECaptureAt=now+350;
            }
            return;
        }
        if(m_uiRoutesE2ESaveConfirmStage==3){
            auto* dialog=FindUiControlNamed<ui::Control>(
                m_ui.Root(),"SaveLoadConfirm");
            if(m_pendingSaveLoadSlot||(dialog&&dialog->IsVisibleInTree()))return;
            const Status closed=m_session->Routes().CloseModal();
            if(!closed){
                for(const auto& diagnostic:closed.Diagnostics())diag::Emit(diagnostic);
                m_e2eFailed=true;m_uiRoutesE2E=false;m_quitRequested=true;
                return;
            }
            m_uiRoutesE2ESaveConfirmStage=0;
            ++m_uiRoutesE2EIndex;
            return;
        }
        if(m_uiRoutesE2EAwaitingCapture)return;
        if(m_uiRoutesE2EIndex>=std::size(kUIRouteE2ERoutes)){
            PX_LOG_INFO("Player UI routes E2E complete routes={}",
                        std::size(kUIRouteE2ERoutes));
            m_uiRoutesE2E=false;
            m_quitRequested=true;
            return;
        }
        if(!m_uiRoutesE2EReadinessLogged&&
           now-m_uiRoutesE2EStartedAt>15'000){
            PX_LOG_INFO(
                "Player UI routes E2E readiness appState={} base={} modal={} ui={}",
                static_cast<int>(m_appState),
                m_session->Routes().CurrentRoute(),
                m_session->Routes().CurrentModalRoute(),m_ui.CurrentRoute());
            m_uiRoutesE2EReadinessLogged=true;
        }
        if(m_appState!=AppState::Title||
           !m_session->Routes().CurrentModalRoute().empty())
            return;
        const std::string target(kUIRouteE2ERoutes[m_uiRoutesE2EIndex]);
        const Status shown=m_session->Routes().ShowModal(target);
        if(!shown){
            for(const auto& diagnostic:shown.Diagnostics())diag::Emit(diagnostic);
            PX_LOG_ERROR("Player UI routes E2E could not open route={}",target);
            m_e2eFailed=true;
            m_uiRoutesE2E=false;
            m_quitRequested=true;
            return;
        }
        m_uiRoutesE2EAwaitingCapture=true;
        m_uiRoutesE2ECaptureAt=now+350;
        PX_LOG_INFO("Player UI routes E2E opened index={} route={}",
                    m_uiRoutesE2EIndex,target);
        return;
    }
    if(m_uiActionsE2E){
        if(SDL_GetTicks()-m_uiActionsE2EStartedAt>90'000){
            PX_LOG_ERROR("Player UI actions E2E timed out stage={}",
                         m_uiActionsE2EStage);
            m_e2eFailed=true;
            m_uiActionsE2E=false;
            m_quitRequested=true;
            return;
        }
        switch(m_uiActionsE2EStage){
            case 0:
                if(m_appState==AppState::Title&&m_ui.CurrentRoute()=="title"){
                    OpenScreen("flowchart");
                    m_uiActionsE2EStage=1;
                }
                break;
            case 1:
                if(m_session->Routes().CurrentModalRoute()=="flowchart"&&
                   m_ui.CurrentRoute()=="flowchart"){
                    auto* filter=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"btyosButton");
                    if(!filter){
                        PX_LOG_ERROR("Player UI actions E2E flowchart filter missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    filter->Activate();
                    m_uiActionsE2EStage=2;
                }
                break;
            case 2:
                if(m_session->Routes().CurrentModalRoute()=="flowchart-yoshino"&&
                   m_ui.CurrentRoute()=="flowchart-yoshino"){
                    PX_LOG_INFO("Player UI actions E2E flowchart filter dispatched route={}",
                                m_ui.CurrentRoute());
                    auto* item=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"flow_yoshino_1Button");
                    if(!item){
                        PX_LOG_ERROR("Player UI actions E2E flowchart item missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    item->Activate();
                    m_uiActionsE2EStage=7;
                }
                break;
            case 7:
                if(m_selectedFlowEntry=="sb.100-ver1-01.yos-part-0"&&
                   m_appState==AppState::Title&&
                   m_ui.CurrentRoute()=="flowchart-yoshino"){
                    auto* heading=FindUiControlNamed<ui::Label>(
                        m_ui.Root(),"FlowSelectedHeading");
                    auto* jump=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"scenejumpButton");
                    if(!heading||heading->Text().empty()||!jump||!jump->Enabled()){
                        PX_LOG_ERROR("Player UI actions E2E flowchart selection detail missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    PX_LOG_INFO("Player UI actions E2E flowchart selection visible heading={}",
                                heading->Text());
                    HandleUIAction({"overlay.close",{}});
                    OpenScreen("music");
                    m_uiActionsE2EStage=3;
                }
                break;
            case 3:
                if(m_session->Routes().CurrentModalRoute()=="music"&&
                   m_ui.CurrentRoute()=="music"){
                    auto* track=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"music_SongOpButton");
                    if(!track){
                        PX_LOG_ERROR("Player UI actions E2E music control missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    track->Activate();
                    m_uiActionsE2EStage=4;
                }
                break;
            case 4:
                if(m_musicTrackSelected&&!m_musicTracks.empty()){
                    const std::string activePath =
                        m_runtime.Audio().CaptureState().music.path;
                    if(activePath!=m_musicTracks.front()){
                        PX_LOG_ERROR(
                            "Player UI actions E2E music path mismatch expected={} actual={}",
                            m_musicTracks.front(),activePath);
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    PX_LOG_INFO("Player UI actions E2E music argument dispatched path={}",
                                m_musicTracks.front());
                    HandleUIAction({"overlay.close",{}});
                    OpenScreen("replay");
                    m_uiActionsE2EStage=5;
                }
                break;
            case 5:
                if(m_session->Routes().CurrentModalRoute()=="replay"&&
                   m_ui.CurrentRoute()=="replay"){
                    auto* replay=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"yoshinoButton");
                    if(!replay){
                        PX_LOG_ERROR("Player UI actions E2E replay control missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    replay->Activate();
                    m_uiActionsE2EStage=6;
                }
                break;
            case 6:
                if(m_appState==AppState::Game&&!m_gameStartTransitionActive){
                    const int target=m_session->VM().CurrentProgram().LabelIndex(
                        std::string(kUIActionsE2EReplayLabel));
                    if(target<0||m_session->VM().ProgramCounter()<=target){
                        PX_LOG_ERROR(
                            "Player UI actions E2E replay did not restart at label target={} pc={}",
                            target,m_session->VM().ProgramCounter());
                        m_e2eFailed=true;
                    }else{
                        PX_LOG_INFO(
                            "Player UI actions E2E complete replayLabel={} pc={}",
                            kUIActionsE2EReplayLabel,m_session->VM().ProgramCounter());
                    }
                    m_uiActionsE2E=false;
                    m_quitRequested=true;
                }
                break;
            default:break;
        }
        return;
    }
    if(m_e2eStage==E2EStage::Disabled||m_e2eStage==E2EStage::Complete)
        return;
    if(SDL_GetTicks()-m_e2eStartedAt>30'000){
        PX_LOG_ERROR("Player E2E journey timed out stage={} modal={} route={} ui={}",
                     static_cast<int>(m_e2eStage),
                     m_session->Routes().CurrentModalRoute(),
                     m_session->Routes().CurrentRoute(), m_ui.CurrentRoute());
        m_e2eFailed=true;
        m_e2eStage=E2EStage::Complete;
        m_quitRequested=true;
        return;
    }
    auto& input=m_runtime.GetInput();
    switch(m_e2eStage){
        case E2EStage::AwaitTitle: {
            if(m_appState==AppState::Title){
                const auto titleState=m_ui.CaptureRuntimeState();
                if(!titleState.animation||titleState.animation->position<=0.0f)
                    break;
                PX_LOG_INFO("Player Phase 2 authored title animation advanced");
                ui::ActionInvocation invocation;
                invocation.action="acceptance.phase2-title";
                invocation.context.sourceScene=std::string(m_ui.CurrentRoute());
                const auto started=m_ui.Actions().Start(std::move(invocation));
                if(!started){
                    PX_LOG_ERROR("Player Phase 2 title Action failed to start");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eStage=E2EStage::AwaitTitleAsync;
            }
            break;
        }
        case E2EStage::AwaitTitleAsync:
            if(m_session->Routes().CurrentRoute()=="phone"){
                const bool actualScene=
                    m_ui.CurrentRoute()=="phone"&&
                    m_ui.CurrentScreen()==ui::GalgameUI::Screen::Custom&&
                    m_ui.Root()&&m_ui.Root()->Name()=="PhoneRoot";
                if(!actualScene){
                    PX_LOG_ERROR("Player arbitrary phone route did not install its authored scene");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eCustomTitleRouteSeen=true;
            }
            if(m_e2eCustomTitleRouteSeen&&m_scriptHost&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Routes().CurrentRoute()=="title"){
                PX_LOG_INFO("Player Phase 2 title async and custom phone route complete");
                input.InjectAction(InputAction::FocusNext);
                input.InjectAction(InputAction::Accept);
                m_e2eStage=E2EStage::AwaitGameReady;
            }
            break;
        case E2EStage::AwaitGameReady:
            if(m_appState==AppState::Game&&!m_gameStartTransitionActive&&
               m_profile.CGUnlocked("ending-rin")&&
               m_scriptHost&&!m_scriptHost->HasPendingCommand()&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Dialogue().Active()){
                ui::ActionInvocation invocation;
                invocation.action="acceptance.phase2-menu";
                invocation.context.sourceScene=std::string(m_ui.CurrentRoute());
                const auto started=m_ui.Actions().Start(std::move(invocation));
                if(!started){
                    PX_LOG_ERROR("Player Phase 2 menu Action failed to start");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eStage=E2EStage::AwaitMenuAsync;
            }
            break;
        case E2EStage::AwaitMenuAsync:
            if(m_session->Routes().CurrentModalRoute()=="replay"){
                const bool actualScene=
                    m_ui.CurrentRoute()=="replay"&&
                    m_ui.CurrentScreen()==ui::GalgameUI::Screen::Custom&&
                    m_ui.Root()&&m_ui.Root()->Name()=="ReplayRoot";
                if(!actualScene){
                    PX_LOG_ERROR("Player arbitrary replay route did not install its authored scene");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eCustomMenuRouteSeen=true;
            }
            if(m_e2eCustomMenuRouteSeen&&m_scriptHost&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Routes().CurrentModalRoute().empty()){
                if (SDL_getenv("PRISMATIX_E2E_LOAD_PREVIOUS_SAVE")) {
                    if (!LoadSlot(2)) {
                        PX_LOG_ERROR("Player E2E previous-version save could not load");
                        m_e2eFailed = true;
                        m_e2eStage = E2EStage::Complete;
                        m_quitRequested = true;
                        break;
                    }
                    PX_LOG_INFO("Player E2E previous-version save loaded");
                }
                bool consistent =
                    m_session->Routes().ShowModal("replay").IsOk()&&
                    SaveSlot(1,{})&&
                    m_session->Routes().CloseModal().IsOk()&&
                    m_session->Routes().Push("phone").IsOk()&&LoadSlot(1);
                const auto restored=m_session->Routes().CaptureState();
                consistent=consistent&&
                    restored.stack==std::vector<std::string>{"hud"}&&
                    restored.modals==std::vector<std::string>{"replay"}&&
                    m_ui.CurrentRoute()=="replay"&&m_ui.Root()&&
                    m_ui.Root()->Name()=="ReplayRoot";
                if(!consistent){
                    PX_LOG_ERROR("Player save/load did not restore the route and UI stack");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                (void)m_session->Routes().CloseModal();
                PX_LOG_INFO("Player Phase 2 menu async and save/load stack complete");
                HandleUIAction({"save.open", {}});
                m_e2eStage=E2EStage::AwaitSaveScreen;
            }
            break;
        case E2EStage::AwaitSaveScreen:
            if (m_pendingSaveScreen) break;
            if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Save) {
                PX_LOG_ERROR("Player save menu was replaced before the next frame");
                m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;break;
            }
            PX_LOG_INFO("Player save menu survived a rendered frame");
            if (!SaveSlot(2, {}) || !LoadSlot(2) || m_ui.CurrentRoute() != "hud" ||
                !m_session->Routes().CurrentModalRoute().empty()) {
                PX_LOG_ERROR("Player save picker leaked into the loaded gameplay checkpoint");
                m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;break;
            }
            PX_LOG_INFO("Player save picker checkpoint restored gameplay HUD");
            m_e2eStage=E2EStage::AwaitPostMenuGameReady;
            break;
        case E2EStage::AwaitPostMenuGameReady:
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
                    const auto localizedAudio = m_session->Audio().CaptureState();
                    std::string localizedVoice = localizedAudio.voice.path;
                    if (localizedVoice.empty()) {
                        const auto& backlog = m_session->Backlog().Entries();
                        const auto voice = std::ranges::find_if(
                            backlog.rbegin(), backlog.rend(),
                            [](const vn::BacklogEntry& entry) {
                                return !entry.voice.empty();
                            });
                        if (voice != backlog.rend())
                            localizedVoice = voice->voice;
                    }
                    PX_LOG_INFO(
                        "Player E2E localized media background={} voice={}",
                        m_session->Stage().BackgroundPath(),
                        localizedVoice);
                    break;
                }
                m_e2eClearCountBeforeF2=m_profile.ClearCount();
                if(const auto* value=m_session->Variables().GetValue("routeName"))
                    m_e2eSessionValueBeforeF2=value->Clone();
                input.InjectKeyPress(SDL_SCANCODE_F2);
                m_e2eStage=E2EStage::VerifyReleaseF2;
            }
            break;
        case E2EStage::VerifyReleaseF2: {
            const auto* value=m_session->Variables().GetValue("routeName");
            const bool sessionUnchanged=
                (!m_e2eSessionValueBeforeF2&&value==nullptr)||
                (m_e2eSessionValueBeforeF2&&value&&
                 *m_e2eSessionValueBeforeF2==*value);
            if(m_profile.ClearCount()!=m_e2eClearCountBeforeF2||
               !sessionUnchanged){
                PX_LOG_ERROR("Player release F2 policy changed persistent or session state");
                m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;break;
            }
            PX_LOG_INFO("Player release F2 policy ignored demo reset shortcut");
            if (input.KeyBindings("screen.open.gallery") !=
                std::vector<int>{SDL_SCANCODE_F8}) {
                PX_LOG_ERROR("Player E2E saved Gallery shortcut was not restored");
                m_e2eFailed = true;
                m_e2eStage = E2EStage::Complete;
                m_quitRequested = true;
                break;
            }
            input.InjectKeyPress(SDL_SCANCODE_F8);
            m_e2eStage=E2EStage::AwaitGallery;
            break;
        }
        case E2EStage::AwaitGallery:
            if(m_session->Routes().CurrentModalRoute()=="gallery"&&
               m_ui.CurrentRoute()=="gallery")
                m_e2eStage=E2EStage::CaptureGallery;
            break;
        case E2EStage::CloseGallery:
            if(m_routePreloadBatch)break;
            if (input.HasAction("acceptance.close-gallery"))
                input.InjectKeyPress(SDL_SCANCODE_F6);
            else {
                input.InjectAction(InputAction::Cancel);
                input.InjectKeyPress(SDL_SCANCODE_ESCAPE);
            }
            m_e2eStage=E2EStage::AwaitOverlayClosed;
            break;
        case E2EStage::AwaitOverlayClosed:
            if(m_session->Routes().CurrentModalRoute().empty()){
                PX_LOG_INFO("Player E2E journey complete name=catalog");
                m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;
            }
            break;
        default:break;
    }
}

void PlayerApp::CaptureUIRoutesE2EFrame() {
    if(!m_uiRoutesE2E||!m_uiRoutesE2EAwaitingCapture||
       m_uiRoutesE2EIndex>=std::size(kUIRouteE2ERoutes)||
       SDL_GetTicks()<m_uiRoutesE2ECaptureAt)
        return;
    // The capture must show the incoming route, not the held outgoing frame
    // or a mid-transition blend after a slow first-time asset preload.
    if (m_routePreloadBatch) return;
    if (const auto& transition = m_session->Routes().ActiveTransition();
        transition &&
        m_runtime.Renderer().ScreenEffectPlaying(transition->handle))
        return;
    const std::string_view route=kUIRouteE2ERoutes[m_uiRoutesE2EIndex];
    if(m_session->Routes().CurrentModalRoute()!=route||
       m_ui.CurrentRoute()!=route)
        return;
    const auto summary=graphics::CaptureFrameSummary(
        m_runtime.Renderer().Handle());
    const bool confirmCapture=route=="save"&&m_uiRoutesE2ESaveConfirmStage==2;
    const std::string captureName=
        "route-"+std::to_string(m_uiRoutesE2EIndex+1)+"-"+
        std::string(route)+(confirmCapture?"-confirm":"");
    const bool saved=CapturePresentationE2EFrame(captureName);
    PX_LOG_INFO(
        "Player UI routes E2E frame index={} route={} width={} height={} colors={} hash={} saved={}",
        m_uiRoutesE2EIndex,route,summary.width,summary.height,
        summary.sampledColors,summary.hash,saved);
    if(!saved||!summary.Valid()||summary.width<640||summary.height<360||
       summary.sampledColors<8){
        PX_LOG_ERROR("Player UI routes E2E frame is blank or undersized route={}",
                     route);
        m_e2eFailed=true;
        m_uiRoutesE2E=false;
        m_quitRequested=true;
        return;
    }
    if(route=="save"&&m_uiRoutesE2ESaveConfirmStage==0){
        auto* slot=FindUiControlNamed<ui::Button>(m_ui.Root(),"Slot0Button");
        // TSX-authored slot grids reuse one keyed template rather than
        // materializing a separately named button for every save slot.
        if(!slot)slot=FindUiControlNamed<ui::Button>(m_ui.Root(),"SlotButton");
        if(!slot){
            PX_LOG_ERROR("Player UI routes E2E authored save slot missing");
            m_e2eFailed=true;m_uiRoutesE2E=false;m_quitRequested=true;
            return;
        }
        slot->Activate();
        m_uiRoutesE2ESaveConfirmStage=1;
        m_uiRoutesE2EAwaitingCapture=false;
        return;
    }
    if(confirmCapture){
        auto* cancel=FindUiControlNamed<ui::Button>(
            m_ui.Root(),"ConfirmNoButton");
        if(!cancel){
            PX_LOG_ERROR("Player UI routes E2E authored save confirmation missing");
            m_e2eFailed=true;m_uiRoutesE2E=false;m_quitRequested=true;
            return;
        }
        cancel->Activate();
        m_uiRoutesE2ESaveConfirmStage=3;
        m_uiRoutesE2EAwaitingCapture=false;
        return;
    }
    const Status closed=m_session->Routes().CloseModal();
    if(!closed){
        for(const auto& diagnostic:closed.Diagnostics())diag::Emit(diagnostic);
        m_e2eFailed=true;
        m_uiRoutesE2E=false;
        m_quitRequested=true;
        return;
    }
    ++m_uiRoutesE2EIndex;
    m_uiRoutesE2EAwaitingCapture=false;
}

void PlayerApp::CaptureE2EGalleryFrame() {
    if(m_e2eStage!=E2EStage::CaptureGallery)return;
    if(m_routePreloadBatch)return;
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
    RenderUIWithScripts();
    if(m_presentationE2E&&m_presentationE2EStage==1){
        const auto state=m_ui.CaptureRuntimeState();
        // Titles without an entrance animation start after a short settle.
        if(m_presentationE2ETitleSeenAt==0)m_presentationE2ETitleSeenAt=SDL_GetTicks();
        if(state.animation?state.animation->position>=2.15f
                          :SDL_GetTicks()-m_presentationE2ETitleSeenAt>=1500){
            (void)CapturePresentationE2EFrame("title");
            StartGame();
            m_presentationE2EStage=2;
        }
    }
}

bool PlayerApp::VideoFrame(float dt) {
    const bool vmWaiting =
        m_session->VM().State() == vn::VMState::WaitingVideo;
    if (!vmWaiting && m_scriptVideoHandle == 0) return false;
    px::Input& input = m_runtime.GetInput();

    const bool skipping =
        m_skipMode || input.KeyDown(SDL_SCANCODE_LCTRL) ||
        input.KeyDown(SDL_SCANCODE_RCTRL);
    if (!m_pendingVideo.empty() && vmWaiting && m_videoSkippable && skipping) {
        // Skip passes over a skippable movie without decoding a frame.
        m_pendingVideo.clear();
        m_session->VM().NotifyVideoDone();
        return false;
    }
    if (!m_pendingVideo.empty()) {
        diag::FrameProfiler::Instance().Note("video-open");
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

    m_video->Update(dt);
    const bool skipRequested =
        m_videoSkippable &&
        (skipping || input.ActionPressed(InputAction::Advance) ||
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

    if (m_dialog == std::nullopt) {
        if (input.ActionPressed(InputAction::QuickLoad))
            RequestConfirm("quickLoad", [this] { LoadSlot(0); });
        else if (input.ActionPressed(InputAction::QuickSave))
            RequestConfirm("quickSave", [this] { m_saveRequested = true; });
    }
    if (input.ActionPressed(InputAction::OpenSave)) m_pendingSaveScreen = true;

    // Mode toggles (KAG conventions: A=auto, S=skip toggle, Ctrl=skip held).
    if (input.ActionPressed(InputAction::ToggleAuto)) {
        m_autoMode = !m_autoMode;
        if (m_autoMode) m_skipMode = false;
    }
    if (input.ActionPressed(InputAction::ToggleSkip)) {
        m_skipMode = !m_skipMode;
        if (m_skipMode) m_autoMode = false;
    }
    if (input.ActionPressed(InputAction::Backlog)) {
        OpenScreen("backlog");
        return;
    }
    if (input.ActionPressed(InputAction::Rollback)) {
        RollbackOneLine();
    }
    if (m_storyHudVisible &&
        (input.RightClick() || input.ActionPressed(InputAction::ToggleUi))) {
        m_hudHidden = !m_hudHidden;
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
    if (!m_storyHudVisible && m_session->Routes().CurrentModalRoute().empty()) {
        if ((input.LeftClick() || advanceKey || nonKeyboardAdvance) &&
            m_session->VM().State() != vn::VMState::WaitingChoice) {
            advanceDialogue();
            advancedByClick = true;
            m_autoMode = false;
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
        if (!uiConsumed && (input.LeftClick() || advanceKey || nonKeyboardAdvance) &&
               m_session->VM().State() != vn::VMState::WaitingChoice) {
            advanceDialogue();
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
    m_session->VM().SetFastForward(skipping);
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
        } else if (now - m_autoTimerStart >=
                   static_cast<std::uint64_t>(std::max(0, m_settings.autoWaitMs))) {
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
            const std::string id = std::filesystem::path(storyBgm).stem().string();
            if (m_profile.UnlockMusic(id)) {
                (void)m_ui.UpdateUiData("musicUnlocked." + id, Variant(true));
                MarkProfileDirty(true);
            }
        }
    }
    syncChoiceTexts();

    // New dialogue lines feed the NVL page and the rollback ring.
    const std::uint64_t backlogSequence = m_session->Backlog().LastSequence();
    if (backlogSequence > m_lastBacklogSequence) {
        m_storyHudVisible = true;
        const vn::BacklogEntry* latestEntry = nullptr;
        for (const vn::BacklogEntry& e :
             m_session->Backlog().EntriesAfter(m_lastBacklogSequence)) {
            latestEntry = &e;
            if (m_nvlMode && !e.isChoice) m_nvlLines.push_back(e);
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
        if (m_hudFadeSnap || !m_uiEffects) {
            m_hudFade = target;
            m_hudFadeSnap = false;
        } else {
            const float step = dt / 0.2f;
            m_hudFade += std::clamp(target - m_hudFade, -step, step);
        }
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
    if(m_presentationE2E&&m_presentationE2EStage==3&&
       !m_gameStartTransitionActive){
        const auto stage=m_session->Stage().CaptureState();
        const std::string background=m_session->Stage().BackgroundPath();
        const std::string bgm=m_session->VM().CurrentBgm();
        const auto& dialogue=m_session->Dialogue().State();
        const bool visibleBackground=!background.empty()&&
            background.find("画面_黒")==std::string::npos&&
            background.find("black")==std::string::npos;
        const bool settledActor=std::ranges::any_of(
            stage.actors,[](const auto& actor){
                return !actor.exiting&&actor.targetAlpha>=245.0f&&
                       actor.alpha>=245.0f;
            });
        const bool targetReached=m_presentationE2ETargetOperationId.empty()||
            m_session->VM().CurrentOperationId()==m_presentationE2ETargetOperationId;
        if(targetReached&&visibleBackground&&settledActor&&!bgm.empty()&&
           !dialogue.displayText.empty()){
            (void)CapturePresentationE2EFrame("game");
            PX_LOG_INFO(
                "Player presentation E2E complete background={} actors={} actorAsset={} bgm={}",
                background,stage.actors.size(),stage.actors.front().imagePath,bgm);
            m_presentationE2E=false;
            m_quitRequested=true;
        }
    }
}

void PlayerApp::MainLoop() {
    while (!m_quitRequested && m_runtime.BeginFrame()) {
        m_uiTransitionStartedThisFrame = false;
        UpdateRouteVisualPreload();
        px::Input& input = m_runtime.GetInput();
        const float dt = m_runtime.GetClock().DeltaSeconds();
        const std::uint64_t now = m_runtime.GetClock().NowMs();
        UpdateCursorVisibility(now);
        const std::string_view inputRoute = m_session->Routes().CurrentRoute();
        const bool gameInputContext = m_appState == AppState::Game &&
            !m_gameStartTransitionActive && m_viewingCG.empty() &&
            m_session->Routes().CurrentModalRoute().empty() &&
            (inputRoute == "hud" || inputRoute == "game");
        (void)input.SetBaseContext(gameInputContext ? "game" : "ui");
        DriveE2EJourney();

        if (input.KeyPressed(SDL_SCANCODE_RETURN) &&
            (input.KeyDown(SDL_SCANCODE_LALT) || input.KeyDown(SDL_SCANCODE_RALT))) {
            (void)ApplyFullscreenSetting(!m_settings.fullscreen);
        }

        // Script jobs and accepted save/load requests have one application
        // clock and safe point, independent of the currently presented route.
        UpdateScriptScheduler(dt);
        FlushProfileIfDue();
        HarvestBackgroundSave(false);

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

        if (!handledUiShortcut && m_appState == AppState::Game &&
            !modalOpen && gameplayRoute) {
            for (const auto& [action, route] : m_screenTriggers)
                if (input.ActionPressed(action)) {
                    OpenScreen(route);
                    break;
                }
            // An input binding may have opened a modal during this frame. Do
            // not let the gameplay HUD refresh replace its authored scene.
            modalOpen = !m_session->Routes().CurrentModalRoute().empty();
        }

        if (modalOpen &&
            m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) {
            if (!handledUiShortcut &&
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
    if (!Init(argc, argv)) {
        return 1;
    }
    MainLoop();
    Shutdown();
    return m_e2eFailed?2:0;
}

}
