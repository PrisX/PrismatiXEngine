#pragma once

#include "Engine/SDK/Ui.h"
#include "Engine/UI/Startup/SplashTypes.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UiApplication.h"

#include <functional>
#include <optional>
#include <vector>

namespace px::ui::startup {

class SplashSequencePlayer {
public:
    enum class Phase { Idle, Loading, Entering, Holding, Exiting, Completed };
    using SceneLoader =
        std::function<Result<sdk::UiDocument>(const ResourceRefValue&)>;
    using AudioPlayer = std::function<Status(const ResourceRefValue&)>;
    using DiagnosticSink = std::function<void(const diag::Diagnostic&)>;
    using VisualPreloader = std::function<std::function<bool()>(
        const std::vector<std::string>&)>;

    struct Services {
        SceneLoader loadScene;
        UiAssetResolver resolveAsset;
        UiComponentLoader loadComponent;
        UiMotionLoader loadMotion;
        VisualPreloader preloadVisuals;
        std::function<bool()> completionVisualsReady;
        AudioPlayer playAudio;
        DiagnosticSink diagnostics;
    };

    explicit SplashSequencePlayer(Services services = {});

    Status Start(std::vector<SplashScreenEntry> entries, bool reducedMotion = false);
    void Update(float deltaSeconds, bool skipRequested = false);
    void RequestSkip() { m_skipRequested = true; }
    void SetCompletionCallback(std::function<void()> callback) {
        m_completion = std::move(callback);
    }

    [[nodiscard]] UIContext& Context() { return m_context; }
    [[nodiscard]] const UIContext& Context() const { return m_context; }
    [[nodiscard]] Phase CurrentPhase() const { return m_phase; }
    [[nodiscard]] std::size_t CurrentIndex() const { return m_index; }
    [[nodiscard]] std::uint64_t PresentationGeneration() const {
        return m_presentationGeneration;
    }
    [[nodiscard]] bool Completed() const { return m_phase == Phase::Completed; }
    [[nodiscard]] float Elapsed() const { return m_elapsed; }
    [[nodiscard]] const std::vector<SplashScreenEntry>& Entries() const { return m_entries; }

private:
    enum class LoadOutcome { Installed, Pending, Failed };
    LoadOutcome LoadCurrent();
    void LoadUntilReady();
    void PrepareNext();
    void BeginExit();
    void Advance();
    void Finish();
    void PlayCurrentAudio();
    void Emit(diag::Diagnostic diagnostic) const;

    Services m_services;
    UIContext m_context;
    std::vector<SplashScreenEntry> m_entries;
    std::function<void()> m_completion;
    Phase m_phase = Phase::Idle;
    std::size_t m_index = 0;
    float m_elapsed = 0.0f;
    float m_phaseElapsed = 0.0f;
    float m_enterDuration = 0.0f;
    float m_exitDuration = 0.0f;
    bool m_reducedMotion = false;
    bool m_skipRequested = false;
    bool m_audioPlayed = false;
    bool m_completionSent = false;
    std::optional<sdk::UiDocument> m_pendingDocument;
    std::function<bool()> m_visualReady;
    std::optional<sdk::UiDocument> m_nextDocument;
    std::function<bool()> m_nextReady;
    std::size_t m_nextIndex = 0;
    std::uint64_t m_presentationGeneration = 0;
};

}  // namespace px::ui::startup
