#include "Engine/UI/Startup/SplashSequencePlayer.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"

#include <algorithm>
#include <cmath>

namespace px::ui::startup {
namespace {

diag::Diagnostic RuntimeDiagnostic(diag::Severity severity, std::string code,
                                   std::string message, const std::size_t index,
                                   const std::string& path) {
    diag::Diagnostic diagnostic{.severity = severity,
                                .code = std::move(code),
                                .category = "Player.Splash",
                                .message = std::move(message),
                                .details = "Splash entry index: " + std::to_string(index)};
    diagnostic.source.path = path;
    return diagnostic;
}

}  // namespace

SplashSequencePlayer::SplashSequencePlayer(Services services)
    : m_services(std::move(services)) {}

Status SplashSequencePlayer::Start(std::vector<SplashScreenEntry> entries,
                                   const bool reducedMotion) {
    m_entries = std::move(entries);
    m_index = 0;
    m_elapsed = 0.0f;
    m_phaseElapsed = 0.0f;
    m_enterDuration = 0.0f;
    m_exitDuration = 0.0f;
    m_reducedMotion = reducedMotion;
    m_skipRequested = false;
    m_completionSent = false;
    m_bindings.clear();
    if (m_entries.empty()) {
        Finish();
        return Status::Ok();
    }
    m_phase = Phase::Loading;
    while (m_index < m_entries.size() && !LoadCurrent()) ++m_index;
    if (m_index >= m_entries.size()) Finish();
    return Status::Ok();
}

void SplashSequencePlayer::Emit(diag::Diagnostic diagnostic) const {
    if (m_services.diagnostics) m_services.diagnostics(diagnostic);
    else diag::Emit(std::move(diagnostic));
}

float SplashSequencePlayer::AnimationDuration(const UIAnimationLibrary& library,
                                              const std::string_view state) {
    if (state.empty()) return 0.0f;
    const auto* descriptor = library.machine.FindState(state);
    if (!descriptor) return 0.0f;
    const auto* clip = library.FindClip(descriptor->clip);
    return clip ? std::max(0.0f, clip->duration) : 0.0f;
}

bool SplashSequencePlayer::LoadCurrent() {
    m_phase = Phase::Loading;
    const auto& entry = m_entries[m_index];
    if (!m_services.loadScene) {
        Emit(RuntimeDiagnostic(diag::Severity::Error, "PXBOOT1101",
                               "Splash scene loader is unavailable", m_index,
                               entry.scene.lastKnownPath));
        return false;
    }
    auto document = m_services.loadScene(entry.scene);
    if (!document) {
        for (const auto& diagnostic : document.Diagnostics()) Emit(diagnostic);
        Emit(RuntimeDiagnostic(diag::Severity::Error, "PXBOOT1102",
                               "Splash scene could not be loaded; skipping entry", m_index,
                               entry.scene.lastKnownPath));
        return false;
    }
    if (document.Value().kind != resource::DocumentKind::Scene ||
        document.Value().type != "UIScene") {
        Emit(RuntimeDiagnostic(diag::Severity::Error, "PXBOOT1103",
                               "Splash resource is not a typed UIScene; skipping entry", m_index,
                               entry.scene.lastKnownPath));
        return false;
    }
    (void)RegisterBuiltinUITypes();
    auto loaded = InstantiateUIScene(document.Value(), nullptr, m_context.Formatters(),
                                     m_services.loadScene);
    if (!loaded) {
        for (const auto& diagnostic : loaded.Diagnostics()) Emit(diagnostic);
        return false;
    }
    m_bindings = std::move(loaded.Value().bindings);
    auto animations = std::move(loaded.Value().animations);
    m_context.SetTheme(loaded.Value().theme ? std::move(*loaded.Value().theme) : Theme{});
    const Status root = m_context.SetRoot(std::move(loaded.Value().root));
    if (!root) {
        for (const auto& diagnostic : root.Diagnostics()) Emit(diagnostic);
        return false;
    }
    m_enterDuration = 0.0f;
    m_exitDuration = 0.0f;
    if (animations) {
        m_enterDuration = m_reducedMotion
                              ? 0.0f
                              : AnimationDuration(*animations, entry.enterAnimation);
        m_exitDuration = m_reducedMotion
                             ? 0.0f
                             : AnimationDuration(*animations, entry.exitAnimation);
        const Status installed = m_context.SetAnimations(std::move(*animations), false);
        if (!installed) {
            for (const auto& diagnostic : installed.Diagnostics()) Emit(diagnostic);
            return false;
        }
    }
    const Status triggers = m_context.ConfigureTriggers(
        std::move(loaded.Value().triggers), std::move(loaded.Value().interactionGraph),
        entry.scene.lastKnownPath);
    if (!triggers) {
        for (const auto& diagnostic : triggers.Diagnostics()) Emit(diagnostic);
        return false;
    }

    if (!entry.enterAnimation.empty() && !m_reducedMotion && m_enterDuration <= 0.0f)
        Emit(RuntimeDiagnostic(diag::Severity::Warning, "PXBOOT1104",
                               "Splash enter animation is missing: " + entry.enterAnimation,
                               m_index, entry.scene.lastKnownPath));
    if (!entry.exitAnimation.empty() && !m_reducedMotion && m_exitDuration <= 0.0f)
        Emit(RuntimeDiagnostic(diag::Severity::Warning, "PXBOOT1105",
                               "Splash exit animation is missing: " + entry.exitAnimation,
                               m_index, entry.scene.lastKnownPath));
    if (m_enterDuration > 0.0f) {
        const Status animation = m_context.TravelAnimationState(entry.enterAnimation);
        if (!animation)
            for (const auto& diagnostic : animation.Diagnostics()) Emit(diagnostic);
    }
    if (entry.audio && m_services.playAudio) {
        const Status audio = m_services.playAudio(*entry.audio);
        if (!audio)
            for (auto diagnostic : audio.Diagnostics()) {
                diagnostic.category = "Player.Splash";
                Emit(std::move(diagnostic));
            }
    }
    m_elapsed = 0.0f;
    m_phaseElapsed = 0.0f;
    m_skipRequested = false;
    m_phase = m_enterDuration > 0.0f ? Phase::Entering : Phase::Holding;
    return true;
}

void SplashSequencePlayer::BeginExit() {
    if (m_phase == Phase::Exiting || m_phase == Phase::Completed) return;
    const auto& entry = m_entries[m_index];
    m_phase = Phase::Exiting;
    m_phaseElapsed = 0.0f;
    if (m_exitDuration > 0.0f) {
        const Status animation = m_context.TravelAnimationState(entry.exitAnimation);
        if (!animation) {
            for (const auto& diagnostic : animation.Diagnostics()) Emit(diagnostic);
            m_exitDuration = 0.0f;
        }
    }
    if (m_exitDuration <= 0.0f) Advance();
}

void SplashSequencePlayer::Advance() {
    ++m_index;
    m_phaseElapsed = 0.0f;
    m_skipRequested = false;
    while (m_index < m_entries.size() && !LoadCurrent()) ++m_index;
    if (m_index >= m_entries.size()) Finish();
}

void SplashSequencePlayer::Finish() {
    m_phase = Phase::Completed;
    if (m_completionSent) return;
    m_completionSent = true;
    if (m_completion) m_completion();
}

void SplashSequencePlayer::Update(const float deltaSeconds, const bool skipRequested) {
    if (m_phase == Phase::Idle || m_phase == Phase::Loading ||
        m_phase == Phase::Completed)
        return;
    const float delta = std::max(0.0f, std::isfinite(deltaSeconds) ? deltaSeconds : 0.0f);
    m_elapsed += delta;
    m_phaseElapsed += delta;
    m_skipRequested = m_skipRequested || skipRequested;
    const auto& entry = m_entries[m_index];

    if (m_skipRequested && entry.skippable && m_elapsed >= entry.skipAllowedAfter &&
        m_phase != Phase::Exiting) {
        BeginExit();
        return;
    }
    if (m_phase == Phase::Entering && m_phaseElapsed >= m_enterDuration) {
        m_phase = Phase::Holding;
        m_phaseElapsed = 0.0f;
    }
    if (m_phase == Phase::Holding) {
        const float exitStart = std::max(m_enterDuration,
                                         entry.minimumDuration - m_exitDuration);
        if (m_elapsed >= exitStart) BeginExit();
    } else if (m_phase == Phase::Exiting && m_phaseElapsed >= m_exitDuration) {
        Advance();
    }
}

}  // namespace px::ui::startup
