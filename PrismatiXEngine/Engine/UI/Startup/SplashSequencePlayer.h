#pragma once

#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/Startup/SplashTypes.h"
#include "Engine/UI/UIContext.h"

#include <functional>

namespace px::ui::startup {

class SplashSequencePlayer {
public:
    enum class Phase { Idle, Loading, Entering, Holding, Exiting, Completed };
    using SceneLoader =
        std::function<Result<resource::TypedDocument>(const ResourceRefValue&)>;
    using AudioPlayer = std::function<Status(const ResourceRefValue&)>;
    using DiagnosticSink = std::function<void(const diag::Diagnostic&)>;

    struct Services {
        SceneLoader loadScene;
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
    [[nodiscard]] bool Completed() const { return m_phase == Phase::Completed; }
    [[nodiscard]] float Elapsed() const { return m_elapsed; }
    [[nodiscard]] const std::vector<SplashScreenEntry>& Entries() const { return m_entries; }

private:
    bool LoadCurrent();
    void BeginExit();
    void Advance();
    void Finish();
    void Emit(diag::Diagnostic diagnostic) const;
    [[nodiscard]] static float AnimationDuration(const UIAnimationLibrary& library,
                                                 std::string_view state);

    Services m_services;
    UIContext m_context;
    std::vector<Binding> m_bindings;
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
    bool m_completionSent = false;
};

}  // namespace px::ui::startup
