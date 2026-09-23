#include "Engine/UI/Startup/SplashSequencePlayer.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UiApplication.h"

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
    UiApplication application(m_context);
    auto applied = application.ApplyDocument(
        document.Value(),
        {.sourcePath=entry.scene.lastKnownPath,
         .resolveAsset=m_services.resolveAsset,
         .loadComponent=m_services.loadComponent,
         .previewSafeMode=false,
         .diagnosticOverlay=false});
    if (!applied) {
        for (const auto& diagnostic : applied.Diagnostics()) Emit(diagnostic);
        return false;
    }
    m_enterDuration = m_reducedMotion
                          ? 0.0f
                          : m_context.AnimationDuration(entry.enterAnimation);
    m_exitDuration = m_reducedMotion
                         ? 0.0f
                         : m_context.AnimationDuration(entry.exitAnimation);
    if (m_reducedMotion) (void)m_context.StopAnimation(true);

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
    m_audioPlayed = false;
    m_elapsed = 0.0f;
    m_phaseElapsed = 0.0f;
    m_skipRequested = false;
    m_phase = m_enterDuration > 0.0f ? Phase::Entering : Phase::Holding;
    if (entry.audioDelay <= 0.0f) PlayCurrentAudio();
    return true;
}

void SplashSequencePlayer::PlayCurrentAudio() {
    if (m_audioPlayed || m_index >= m_entries.size()) return;
    m_audioPlayed = true;
    const auto& entry = m_entries[m_index];
    if (!entry.audio || !m_services.playAudio) return;
    const Status audio = m_services.playAudio(*entry.audio);
    if (!audio)
        for (auto diagnostic : audio.Diagnostics()) {
            diagnostic.category = "Player.Splash";
            Emit(std::move(diagnostic));
        }
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
    if (m_elapsed >= entry.audioDelay) PlayCurrentAudio();

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
