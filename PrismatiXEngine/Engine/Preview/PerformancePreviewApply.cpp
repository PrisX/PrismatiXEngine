#include "Engine/Preview/PerformancePreview.h"

#include "Engine/Session/RuntimeSession.h"
#include "Engine/UI/GalgameUI.h"

#include <algorithm>
#include <cstdint>

namespace px::preview {

void ApplyPerformancePreviewPlan(RuntimeSession& session,
                                 const PerformancePreviewPlan& plan,
                                 const bool resetStage,
                                 const bool restoreAudioTracks) {
    if (resetStage) session.Stage().ClearAll();
    for (const auto& node : plan.nodes) {
        if (!node.visible || node.imagePath.empty()) continue;
        if (node.kind == "background") {
            if (resetStage)
                session.Stage().SetBackground(node.imagePath, false);
            continue;
        }
        session.Stage().SetLayerTransform(
            node.id, node.imagePath, node.x, node.y, node.scaleX, node.scaleY,
            node.rotation,
            static_cast<std::uint8_t>(std::clamp(node.opacity, 0.0f, 1.0f) *
                                      255.0f),
            node.zOrder);
    }
    for (const auto& character : plan.characters) {
        session.Stage().ClearLayer(character.targetId);
        if (character.visible) {
            session.Stage().SetCharacter(
                character.targetId, character.imagePath, character.slot,
                character.crossfade && !resetStage);
        } else {
            session.Stage().ClearCharacter(character.targetId,
                                           !resetStage && character.crossfade);
        }
    }
    for (const auto& property : plan.properties) {
        (void)session.Stage().ApplyAnimationProperty(
            property.targetId, property.property, Variant(property.value));
    }
    if (plan.background &&
        session.Stage().BackgroundPath() != plan.background->imagePath)
        session.Stage().SetBackground(plan.background->imagePath,
                                      plan.background->crossfade);

    auto& audioEngine = session.Audio();
    const auto applyChannel = [&](const PerformancePreviewAudioChannel& channel,
                                  const PerformancePreviewAudioBus bus) {
        if (!channel.managed) return;
        if (!restoreAudioTracks) {
            if (bus == PerformancePreviewAudioBus::Music)
                audioEngine.SetBGMVolume(channel.volume);
            else if (bus == PerformancePreviewAudioBus::Ambience)
                audioEngine.SetAmbienceVolume(channel.volume);
            else
                audioEngine.SetVoiceVolume(channel.volume);
            return;
        }
        audio::AudioEngine::TrackState state;
        state.path = channel.path;
        state.loop = channel.loop;
        state.playing = channel.playing;
        state.playbackFrame = static_cast<std::int64_t>(
            std::max(0.0, channel.offsetSeconds) *
            audioEngine.SampleRate());
        if (bus == PerformancePreviewAudioBus::Music)
            (void)audioEngine.RestoreMusicTrack(state, channel.volume);
        else if (bus == PerformancePreviewAudioBus::Ambience)
            (void)audioEngine.RestoreAmbienceTrack(state, channel.volume);
        else
            (void)audioEngine.RestoreVoiceTrack(state, channel.volume);
    };
    applyChannel(plan.audio.music, PerformancePreviewAudioBus::Music);
    applyChannel(plan.audio.ambience, PerformancePreviewAudioBus::Ambience);
    applyChannel(plan.audio.voice, PerformancePreviewAudioBus::Voice);
}

Status ApplyPerformancePreviewUiPlan(ui::GalgameUI& ui,
                                     const PerformancePreviewPlan& plan) {
    ui.ResetUiTimelineOverrides();
    for (const auto& control : plan.uiControls) {
        const Status status =
            ui.SetUiControlVisibility(control.targetId, control.visible);
        if (!status) return status;
    }
    if (plan.uiAnimation) {
        return ui.PreviewUiAnimation(
            plan.uiAnimation->animationClipId,
            static_cast<float>(plan.uiAnimation->offsetSeconds),
            plan.uiAnimation->playing);
    }
    if (plan.managesUiAnimation) return ui.StopUiAnimation(true);
    return Status::Ok();
}

}  // namespace px::preview
