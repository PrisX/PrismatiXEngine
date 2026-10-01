#include "Applications/Player/PlayerApp.h"
#include "Applications/Player/PlayerControls.h"
#include "Applications/Player/PlayerSupport.h"
#include "Applications/Player/QaHooks.h"

#include "Engine/Graphics/Screenshot.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Diagnostics/FrameProfiler.h"
#include "Engine/IO/PackageVerifier.h"
#include "Engine/Progression/AwaitedWork.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/ProfileRules.h"
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

// The `player.*` entries of the project's settings: UI data, title music,
// sounds, fonts, volumes, voice profiles, options, confirmations, choice
// layout, presentation transitions and splashes. Their shapes are checked by
// Contracts/project.schema.json at build time; this is the Player's reading
// of them. Returns false (after emitting a diagnostic) when the Player
// cannot start with them.
bool PlayerApp::ApplyPlayerSettings(const nlohmann::json& settingsObject) {
    const nlohmann::json* settings = &settingsObject;
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
    const auto badSetting = [](const char* key, std::string message) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Fatal,
                                    .code = "PXPLAYER5028",
                                    .category = "Player.Settings",
                                    .message = std::move(message),
                                    .details = key});
        return false;
    };
    const auto existingPath = [this](const nlohmann::json& value) -> std::optional<std::string> {
        if (!value.is_string()) return std::nullopt;
        auto safe = SafeVfsRelativePath(value.get<std::string>());
        if (!safe || !m_runtime.VFS().Exists(*safe)) return std::nullopt;
        return safe;
    };
    // player.titleBgm: a path, or rules [{"when"?: condition, "path"}].
    if (const auto titleBgm = settings->find("player.titleBgm"); titleBgm != settings->end()) {
        if (titleBgm->is_string()) {
            if (auto path = existingPath(*titleBgm))
                m_titleBgmRules.push_back({std::nullopt, std::move(*path)});
        } else if (titleBgm->is_array() && titleBgm->size() <= 64) {
            for (const auto& rule : *titleBgm) {
                const auto path = rule.is_object() ? rule.find("path") : rule.end();
                if (path == rule.end())
                    return badSetting("player.titleBgm", "player.titleBgm rules need a path");
                progress::ProfileChoice choice;
                if (const auto when = rule.find("when"); when != rule.end()) {
                    choice.when = progress::ParseProfileCondition(*when);
                    if (!choice.when)
                        return badSetting("player.titleBgm", "player.titleBgm has an invalid when condition");
                }
                if (auto existing = existingPath(*path)) {
                    choice.value = std::move(*existing);
                    m_titleBgmRules.push_back(std::move(choice));
                }
            }
        } else {
            return badSetting("player.titleBgm", "player.titleBgm must be a path or a list of rules");
        }
    }
    // Older single-flag form, checked before the other rules.
    if (const auto alternate = settings->find("player.titleBgmAfter");
        alternate != settings->end() && alternate->is_object()) {
        const auto flag = alternate->find("flag");
        const auto path = alternate->find("path");
        if (flag != alternate->end() && flag->is_string() && path != alternate->end())
            if (auto existing = existingPath(*path))
                m_titleBgmRules.insert(m_titleBgmRules.begin(),
                    {progress::ParseProfileCondition(*flag), std::move(*existing)});
    }
    if (const auto data = settings->find("player.profileUiData"); data != settings->end()) {
        if (!data->is_object() || data->size() > 256)
            return badSetting("player.profileUiData", "player.profileUiData must be an object");
        for (auto item = data->begin(); item != data->end(); ++item) {
            auto datum = progress::ParseProfileUiDatum(item.key(), item.value());
            if (!datum)
                return badSetting("player.profileUiData",
                                  "player.profileUiData." + item.key() + " is not a valid rule");
            std::vector<std::pair<std::string, Variant>> defaults;
            if (datum->condition) {
                defaults.emplace_back(datum->key, Variant(false));
            } else {
                defaults.emplace_back(datum->key, Variant(datum->fallback));
                for (const std::int64_t value : datum->Values())
                    defaults.emplace_back(datum->key + ".is" + std::to_string(value),
                                          Variant(value == datum->fallback));
            }
            for (auto& [key, value] : defaults)
                if (const Status defined = m_ui.DefineUiData(key, std::move(value)); !defined) {
                    for (const auto& diagnostic : defined.Diagnostics()) diag::Emit(diagnostic);
                    return false;
                }
            m_profileUiData.push_back(std::move(*datum));
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
        const auto invalidLayout = [] {
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Fatal,
                .code = "PXPLAYER5026",
                .category = "Player.Save",
                .message = "player.saveLayout needs integer pages 1-100, "
                           "slotsPerPage 1-32 and firstSlot >= 0 with at "
                           "most 1000 slots in total"});
            return false;
        };
        if (!layout->is_object()) return invalidLayout();
        for (const char* key : {"pages", "slotsPerPage", "firstSlot"}) {
            if (const auto field = layout->find(key);
                field != layout->end() && !field->is_number_integer())
                return invalidLayout();
        }
        const auto pages = layout->value("pages", 1);
        const auto perPage = layout->value("slotsPerPage", 12);
        const auto first = layout->value("firstSlot", 0);
        if (pages < 1 || pages > 100 || perPage < 1 || perPage > 32 ||
            first < 0 || first + pages * perPage > 1000) return invalidLayout();
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
    if (const auto touch = settings->find("player.touchProfile");
        touch != settings->end() && touch->is_string()) {
        if (const auto profile = ParseTouchProfile(touch->get<std::string>()))
            m_runtime.GetInput().SetTouchProfile(*profile);
        else
            PX_LOG_WARN("Ignoring player.touchProfile '{}'; use auto, on or off",
                        touch->get<std::string>());
    }
    // player.textureBudgetMiB: {"desktop"?: MiB, "mobile"?: MiB} replaces the
    // platform's default resident texture budget.
    if (const auto budget = settings->find("player.textureBudgetMiB");
        budget != settings->end() && budget->is_object()) {
        const auto platform = graphics::AssetCache::MobileTextureBudget() ? "mobile" : "desktop";
        if (const auto mebibytes = budget->find(platform);
            mebibytes != budget->end() && mebibytes->is_number_unsigned()) {
            const auto value = std::clamp<std::size_t>(mebibytes->get<std::size_t>(), 32, 16384);
            m_runtime.Assets().SetTextureBudget(value * 1024u * 1024u);
            PX_LOG_INFO("Player texture budget {} MiB ({})", value, platform);
        }
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
    return true;
}

}  // namespace px::player
