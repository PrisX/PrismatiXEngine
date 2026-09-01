#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/BuiltinUiPackage.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/UISceneLoader.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace px::ui {
namespace {
Status EmitStatus(const Status& status) {
    for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
    return status;
}
Status GalgameFailure(std::string code,std::string message){diag::Diagnostic d{.severity=diag::Severity::Error,.code=std::move(code),.category="UI.Galgame",.message=std::move(message)};diag::Emit(d);return Status::Fail(std::move(d));}

class CardButton final : public Button {
public:
    using Button::Button;
    void SetImage(std::string value) { m_image = std::move(value); }
protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override {
        if (!m_image.empty()) renderer.DrawImage(m_image, LayoutRect(), Enabled() ? 210 : 70);
        Button::DrawSelf(renderer, theme);
    }
private:
    std::string m_image;
};

template<typename T>T* FindNamed(Control* root,std::string_view name){if(!root)return nullptr;if(root->Name()==name)return dynamic_cast<T*>(root);for(const auto& child:root->Children())if(auto* control=dynamic_cast<Control*>(child.get()))if(auto* found=FindNamed<T>(control,name))return found;return nullptr;}
void SetPresentationText(Label* label, std::string value) {
    if (auto* rich = dynamic_cast<RichTextLabel*>(label))
        rich->SetMarkup(std::move(value));
    else if (label)
        label->SetText(std::move(value));
}
std::string_view ScreenId(const GalgameUI::Screen screen) {
    switch (screen) {
        case GalgameUI::Screen::Title: return "title";
        case GalgameUI::Screen::HUD: return "hud";
        case GalgameUI::Screen::Backlog: return "backlog";
        case GalgameUI::Screen::Save: return "save";
        case GalgameUI::Screen::Load: return "load";
        case GalgameUI::Screen::Gallery: return "gallery";
        case GalgameUI::Screen::Settings: return "settings";
        case GalgameUI::Screen::Video: return "video";
    }
    return {};
}
std::optional<GalgameUI::Screen> ScreenFromId(const std::string_view id) {
    for (const auto screen : {GalgameUI::Screen::Title, GalgameUI::Screen::HUD,
                              GalgameUI::Screen::Backlog, GalgameUI::Screen::Save,
                              GalgameUI::Screen::Load, GalgameUI::Screen::Gallery,
                              GalgameUI::Screen::Settings, GalgameUI::Screen::Video})
        if (ScreenId(screen) == id) return screen;
    return std::nullopt;
}
}

class GalgameUI::ItemSource final : public IVirtualItemSource {
public:
    ItemSource(std::vector<GalgameItem> items, GalgameUI& owner) : m_items(std::move(items)), m_owner(owner) {}
    std::size_t Count() const override { return m_items.size(); }
    std::string StableKey(std::size_t index) const override { return m_items[index].key; }
    std::uint64_t Revision() const override { return m_revision; }
    Status Bind(std::size_t index, Control& item) override {
        if (index >= m_items.size()) {
            diag::Diagnostic d{.severity = diag::Severity::Error, .code = "PXUI2801", .category = "UI.Galgame",
                               .message = "Galgame list binding index is out of range"};
            diag::Emit(d); return Status::Fail(std::move(d));
        }
        auto* button = dynamic_cast<Button*>(&item);
        if (!button) {
            diag::Diagnostic d{.severity = diag::Severity::Error, .code = "PXUI2802", .category = "UI.Galgame",
                               .message = "Galgame virtual item factory must create Button controls"};
            diag::Emit(d); return Status::Fail(std::move(d));
        }
        const GalgameItem& value = m_items[index];
        button->SetText(value.title + (value.subtitle.empty() ? "" : "\n" + value.subtitle));
        button->SetEnabled(!value.disabled);
        if (auto* card = dynamic_cast<CardButton*>(button)) card->SetImage(value.image);
        button->SetOnActivated([this, index] {
            if (index < m_items.size()) m_owner.Emit(m_items[index].command, m_items[index].argument);
        });
        return Status::Ok();
    }
private:
    std::vector<GalgameItem> m_items;
    GalgameUI& m_owner;
    std::uint64_t m_revision = 1;
};

GalgameUI::GalgameUI() {
    m_context.SetDiagnosticOverlayEnabled(true);
    (void)m_viewModel.Define("dialogue.speaker",Variant(std::string{}),true);
    (void)m_viewModel.Define("dialogue.text",Variant(std::string{}),true);
    (void)m_viewModel.Define("chapter.title",Variant(std::string{}),true);
    (void)m_viewModel.Define("music.title",Variant(std::string{}),true);
    const std::vector<std::string> commands={"game.start","load.open","save.open","gallery.open","settings.open","app.quit","mode.auto","mode.skip","backlog.open","overlay.close"};
    for(const auto& command:commands)(void)m_context.Commands().Register(command,[this,command](const Variant&){Emit(command);return Status::Ok();});
    (void)m_context.Commands().Register("hud.toolbar.pin",[this](const Variant&){return ToggleHudToolbarPinned();});
    for (auto& entry : CreateBuiltinUiPackage()) {
        const auto screen = ScreenFromId(entry.route);
        if (!screen) continue;
        Template value;
        value.studioScene = std::move(entry.document);
        value.sourcePath = "BuiltinUI/" + std::string(entry.route) + ".pxui";
        m_templates.emplace(static_cast<int>(*screen), std::move(value));
    }
}

UIRuntimeState GalgameUI::CaptureRuntimeState() const {
    UIRuntimeState state = m_context.CaptureRuntimeState();
    state.surfaceId = std::string(ScreenId(m_screen));
    return state;
}

UIRuntimeState GalgameUI::CaptureGameplayRuntimeState() const {
    if (m_screen == Screen::HUD) return CaptureRuntimeState();
    if (m_lastGameplayState) return *m_lastGameplayState;
    UIRuntimeState empty;
    empty.surfaceId = "hud";
    return empty;
}

void GalgameUI::RememberGameplayCheckpoint(const Screen target) {
    if (target == m_screen || m_screen != Screen::HUD || !m_context.Root()) return;
    m_lastGameplayState = CaptureRuntimeState();
    m_lastGameplayState->surfaceId = "hud";
}

Status GalgameUI::ValidateRuntimeState(const UIRuntimeState& state) {
    const auto target = ScreenFromId(state.surfaceId);
    if (!target)
        return GalgameFailure("PXUI2817",
                              "UI checkpoint references an unknown surface: " +
                                  state.surfaceId);
    if (*target == m_screen) return m_context.ValidateRuntimeState(state);
    // Build the complete target document in an isolated UI runtime.  Action
    // providers are source-owned by the live ScriptHost, so validate those
    // handles against the live dispatcher and all topology/controller state
    // against the detached candidate.
    if (const Status actions =
            m_context.Actions().ValidateState(state.behavior.actions);
        !actions)
        return actions;
    GalgameUI candidate;
    candidate.m_templates = m_templates;
    candidate.m_studioAssetResolver = m_studioAssetResolver;
    candidate.m_studioComponentLoader = m_studioComponentLoader;
    if (const Status installed = candidate.InstallCheckpointSurface(*target);
        !installed)
        return installed;
    UIRuntimeState candidateState = state;
    candidateState.behavior.actions.clear();
    return candidate.m_context.ValidateRuntimeState(candidateState);
}

Status GalgameUI::RestoreRuntimeState(const UIRuntimeState& state) {
    if (const Status valid = ValidateRuntimeState(state); !valid) return valid;
    const Screen previousScreen = m_screen;
    const UIRuntimeState previousState = CaptureRuntimeState();
    const Screen target = *ScreenFromId(state.surfaceId);
    if (target != m_screen) {
        if (const Status installed = InstallCheckpointSurface(target); !installed)
            return installed;
    }
    if (const Status restored = m_context.RestoreRuntimeState(state); restored) {
        if (target == Screen::HUD) m_lastGameplayState = state;
        return restored;
    } else {
        Status rollback = Status::Ok();
        if (previousScreen != m_screen) {
            rollback = InstallCheckpointSurface(previousScreen);
        }
        if (rollback) rollback = m_context.RestoreRuntimeState(previousState);
        if (!rollback)
            diag::Emit(diag::Diagnostic{
                .severity=diag::Severity::Fatal,.code="PXUI2820",
                .category="UI.Galgame",
                .message="UI surface restore failed and the previous surface could not be recovered"});
        return restored;
    }
}

Status GalgameUI::RegisterTemplate(Screen screen,std::string_view text,const std::string& sourcePath){
    auto typed=resource::ParseTypedDocument(text,sourcePath);
    if(typed&&typed.Value().kind==resource::DocumentKind::Scene&&
       typed.Value().type=="UIScene"){
        Template value;
        value.typedScene=std::move(typed.Value());
        value.sourcePath=sourcePath;
        m_templates[static_cast<int>(screen)]=std::move(value);
        return Status::Ok();
    }
    auto studio=sdk::ParseUi(text);
    if(studio.Valid()){
        Template value;
        value.studioScene=std::move(studio.document);
        value.sourcePath=sourcePath;
        m_templates[static_cast<int>(screen)]=std::move(value);
        return Status::Ok();
    }
    if(typed&&
       (typed.Value().kind!=resource::DocumentKind::Scene||
        typed.Value().type!="UIScene"))
        return GalgameFailure("PXUI2803",
            "Galgame template is neither a typed UIScene nor UI document: "+sourcePath);
    for(const auto& diagnostic:typed.Diagnostics())diag::Emit(diagnostic);
    return Status::Fail(typed.Diagnostics());
}

bool GalgameUI::HasTemplate(Screen screen)const{return m_templates.contains(static_cast<int>(screen));}

Status GalgameUI::InstallTemplate(Screen screen){
    const auto it=m_templates.find(static_cast<int>(screen));if(it==m_templates.end())return GalgameFailure("PXUI2804","Requested Galgame UI template is not registered");
    RememberGameplayCheckpoint(screen);
    ResetUiTimelineOverrides();
    if(it->second.typedScene){
        auto loaded=InstantiateUIScene(*it->second.typedScene,&m_viewModel,m_context.Formatters());if(!loaded)return Status::Fail(loaded.Diagnostics());
        auto bindings=std::move(loaded.Value().bindings);auto animations=std::move(loaded.Value().animations);auto theme=std::move(loaded.Value().theme);auto triggers=std::move(loaded.Value().triggers);auto interaction=std::move(loaded.Value().interactionGraph);const Status installed=Install(std::move(loaded.Value().root),screen);if(!installed)return installed;m_bindings=std::move(bindings);if(theme)m_context.SetTheme(std::move(*theme));if(animations){const Status status=m_context.SetAnimations(std::move(*animations),true);if(!status)return status;}const Status triggerStatus=m_context.ConfigureTriggers(std::move(triggers),std::move(interaction));if(!triggerStatus)return triggerStatus;
    }else if(it->second.studioScene){
        m_bindings.clear();
        UiApplication application(m_context);
        auto applied=application.ApplyDocument(
            *it->second.studioScene,
            {.sourcePath=it->second.sourcePath,
             .resolveAsset=m_studioAssetResolver,
             .loadComponent=m_studioComponentLoader,
             .viewModel=&m_viewModel,
             .previewSafeMode=false,
             .diagnosticOverlay=false});
        if(!applied){
            for(const auto& diagnostic:applied.Diagnostics())diag::Emit(diagnostic);
            return Status::Fail(applied.Diagnostics());
        }
        if(screen!=Screen::HUD){
            m_speaker=m_dialogue=m_nvlText=m_mode=m_chapterNotice=
                m_musicNotice=nullptr;
            m_choices=nullptr;
            m_presentedChoices.clear();
            m_noticePanel=nullptr;
        }
        m_screen=screen;
    }else{
        return GalgameFailure("PXUI2805","Registered UI template has no document");
    }
    if(screen==Screen::HUD){m_speaker=FindNamed<Label>(m_context.Root(),"Speaker");m_dialogue=FindNamed<Label>(m_context.Root(),"Dialogue");m_nvlText=FindNamed<Label>(m_context.Root(),"NVLText");m_choices=FindNamed<VBoxContainer>(m_context.Root(),"Choices");m_presentedChoices.clear();m_mode=FindNamed<Label>(m_context.Root(),"ModeState");m_noticePanel=FindNamed<EdgeRevealContainer>(m_context.Root(),"NoticePanel");m_chapterNotice=FindNamed<Label>(m_context.Root(),"ChapterNotice");m_musicNotice=FindNamed<Label>(m_context.Root(),"MusicNotice");m_lastChapterTitle.clear();m_lastMusicTitle.clear();m_activeDialogueEffect.clear();m_dialogueEffectFinished=false;if(m_dialogue)m_dialogueBaseFontSize=m_dialogue->FontSize()>0?m_dialogue->FontSize():30;}
    return Status::Ok();
}

Status GalgameUI::InstallCheckpointSurface(const Screen screen) {
    if (HasTemplate(screen)) return InstallTemplate(screen);
    switch (screen) {
        case Screen::Title: return ShowTitle();
        case Screen::HUD: return ShowHUD({});
        case Screen::Backlog: return ShowBacklog({});
        case Screen::Save: return ShowSaveLoad(true, {});
        case Screen::Load: return ShowSaveLoad(false, {});
        case Screen::Gallery: return ShowGallery({});
        case Screen::Settings: return ShowSettings({});
        case Screen::Video: return ShowVideoOverlay(false);
    }
    return GalgameFailure("PXUI2818", "UI checkpoint surface is unsupported");
}
void GalgameUI::Emit(std::string command, std::string argument) { m_pendingActions.push_back({std::move(command),std::move(argument)}); }

Status GalgameUI::Add(Control& parent, std::unique_ptr<Control> child) {
    const Status status = parent.AddChild(std::move(child)); return status ? status : EmitStatus(status);
}

std::unique_ptr<Control> GalgameUI::MakeMenuButton(std::string text, std::string command, std::string argument) {
    auto button = std::make_unique<Button>(std::move(text));
    button->SetOnActivated([this, command = std::move(command), argument = std::move(argument)] { Emit(command, argument); });
    return button;
}

Status GalgameUI::Install(std::unique_ptr<Control> root, Screen screen) {
    RememberGameplayCheckpoint(screen);
    ResetUiTimelineOverrides();
    m_bindings.clear();
    if (screen != Screen::HUD) {
        m_speaker = m_dialogue = m_nvlText = m_mode = nullptr;
        m_choices = nullptr;
        m_presentedChoices.clear();
    }
    const Status status = m_context.SetRoot(std::move(root)); if (!status) return status;
    m_screen = screen; return Status::Ok();
}

Status GalgameUI::ApplyAnimationProperty(
    const animation::TrackBinding& binding, const Variant& value) {
    return m_context.ApplyAnimationProperty(binding, value);
}

Status GalgameUI::ValidateAnimationProperty(
    const animation::TrackBinding& binding, const Variant& value,
    const UIRuntimeState& state) {
    const auto target = ScreenFromId(state.surfaceId);
    if (!target)
        return GalgameFailure("PXUI2817",
                              "Animation checkpoint references an unknown UI surface");
    GalgameUI candidate;
    candidate.m_templates = m_templates;
    candidate.m_studioAssetResolver = m_studioAssetResolver;
    candidate.m_studioComponentLoader = m_studioComponentLoader;
    if (const Status installed = candidate.InstallCheckpointSurface(*target);
        !installed)
        return installed;
    return candidate.ApplyAnimationProperty(binding, value);
}

Status GalgameUI::ShowTitle() {
    return InstallTemplate(Screen::Title);
}

Status GalgameUI::ActivateUiControl(const std::string_view nodeId) {
    const auto id = Uuid::Parse(nodeId);
    if (!id)
        return GalgameFailure("PXUI2812",
                              "UI document activation requires a canonical node UUID");
    auto* button = m_context.Root()
                       ? dynamic_cast<Button*>(m_context.Root()->Find(*id))
                       : nullptr;
    if (!button)
        return GalgameFailure(
            "PXUI2813",
            "UI document activation target is not a Button-compatible control");
    button->Activate();
    return Status::Ok();
}

Status GalgameUI::SetUiControlVisibility(const std::string_view nodeId,
                                             const bool visible) {
    const auto id = Uuid::Parse(nodeId);
    if (!id)
        return GalgameFailure(
            "PXUI2816",
            "UI document visibility requires a canonical node UUID");
    auto* control = m_context.Root()
                        ? dynamic_cast<Control*>(m_context.Root()->Find(*id))
                        : nullptr;
    if (!control)
        return GalgameFailure(
            "PXUI2817",
            "UI document visibility target is not present in the active scene");
    m_timelineVisibilityBase.try_emplace(std::string(nodeId),
                                         control->GetVisibility());
    control->SetVisibility(visible ? Visibility::Visible : Visibility::Hidden);
    return Status::Ok();
}

Status GalgameUI::PreviewUiAnimation(const std::string_view clipId,
                                         const float time,
                                         const bool playing) {
    const auto id = Uuid::Parse(clipId);
    if (!id)
        return GalgameFailure(
            "PXUI2818",
            "UI document animation requires a canonical clip UUID");
    return m_context.PreviewAnimation(*id, time, playing);
}

Status GalgameUI::StopUiAnimation(const bool restoreDesignState) {
    return m_context.StopAnimation(restoreDesignState);
}

void GalgameUI::ResetUiTimelineOverrides() {
    m_context.ResetAnimationPropertyOverrides();
    for (const auto& [nodeId, visibility] : m_timelineVisibilityBase) {
        const auto id = Uuid::Parse(nodeId);
        auto* control = id && m_context.Root()
                            ? dynamic_cast<Control*>(m_context.Root()->Find(*id))
                            : nullptr;
        if (control) control->SetVisibility(visibility);
    }
    m_timelineVisibilityBase.clear();
}

Status GalgameUI::ToggleHudToolbarPinned() {
    auto* toolbar = FindNamed<EdgeRevealContainer>(m_context.Root(), "EdgeToolbar");
    if (!toolbar)
        return GalgameFailure(
            "PXUI2814",
            "HUD toolbar pinning requires an EdgeToolbar control on the active screen");
    toolbar->TogglePinned();
    return Status::Ok();
}

Status GalgameUI::ActivateChoice(const std::size_t index) {
    if (!m_choices || index >= m_presentedChoices.size() ||
        index >= m_choices->Children().size())
        return GalgameFailure("PXUI2815",
                              "Choice activation index is outside the active HUD choices");
    auto* button = dynamic_cast<Button*>(m_choices->Children()[index].get());
    if (!button)
        return GalgameFailure("PXUI2816",
                              "Generated HUD choice is not Button-compatible");
    button->Activate();
    return Status::Ok();
}

Status GalgameUI::ShowHUD(const DialoguePresentation& p) {
    const Status status=InstallTemplate(Screen::HUD);if(!status)return status;
    return RefreshHUD(p);
}

Status GalgameUI::RefreshHUD(const DialoguePresentation& p) {
    if (m_screen != Screen::HUD || !m_context.Root()) return ShowHUD(p);
    if (m_speaker) m_speaker->SetText(p.speaker);
    if (m_dialogue) {
        SetPresentationText(m_dialogue, p.text);
        m_dialogue->SetFontSize(static_cast<int>(std::lround(
            m_dialogueBaseFontSize * std::clamp(p.textScale, .75f, 2.0f))));
        const std::string desiredEffect = p.reducedMotion ? std::string{} : p.effect;
        if (desiredEffect != m_activeDialogueEffect) {
            if (!m_activeDialogueEffect.empty())
                m_context.ResetAnimationPropertyOverrides("Dialogue");
            m_activeDialogueEffect = desiredEffect;
            m_dialogueEffectFinished = false;
        }
        const bool looping = desiredEffect == "wave" || desiredEffect == "rainbow";
        if (!desiredEffect.empty() &&
            (looping || !m_dialogueEffectFinished)) {
            const float elapsed = std::max(0.0f, p.effectProgress) / 0.6f;
            const float progress = looping ? std::fmod(elapsed, 1.0f)
                                           : std::min(elapsed, 1.0f);
            animation::TrackBinding binding{
                .kind = animation::TargetKind::Text,
                .target = "Dialogue",
                .property = desiredEffect};
            if (const Status applied =
                    m_context.ApplyAnimationProperty(binding, Variant(progress));
                !applied)
                return applied;
            m_dialogueEffectFinished = !looping && progress >= 1.0f;
        }
    }
    (void)m_viewModel.Write("dialogue.speaker",Variant(p.speaker));(void)m_viewModel.Write("dialogue.text",Variant(p.text));
    (void)m_viewModel.Write("chapter.title",Variant(p.chapterTitle));(void)m_viewModel.Write("music.title",Variant(p.musicTitle));
    const bool chapterChanged=p.chapterTitle!=m_lastChapterTitle;
    const bool musicChanged=p.musicTitle!=m_lastMusicTitle;
    if(m_noticePanel&&(chapterChanged||musicChanged)){
        const bool showChapter=chapterChanged&&!p.chapterTitle.empty();
        const bool showMusic=!showChapter&&musicChanged&&!p.musicTitle.empty();
        if(m_chapterNotice)m_chapterNotice->SetVisibility(showChapter?Visibility::Visible:Visibility::Collapsed);
        if(m_musicNotice)m_musicNotice->SetVisibility(showMusic?Visibility::Visible:Visibility::Collapsed);
        if(showChapter||showMusic)m_noticePanel->RevealFor(3.0f);
    }
    m_lastChapterTitle=p.chapterTitle;m_lastMusicTitle=p.musicTitle;
    if (m_nvlText) {
        std::string text; for (const auto& line : p.nvlLines) text += line + "[br][br]"; SetPresentationText(m_nvlText, std::move(text));
        if (auto* panel = dynamic_cast<Control*>(m_nvlText->Parent())) panel->SetVisibility(p.nvlMode ? Visibility::Visible : Visibility::Collapsed);
    }
    if (m_dialogue) if (auto* panel = dynamic_cast<Control*>(m_dialogue->Parent())) panel->SetVisibility(p.nvlMode ? Visibility::Collapsed : Visibility::Visible);
    if (m_mode) m_mode->SetText(std::string(p.autoMode ? "● AUTO  " : "") + (p.skipMode ? "▶▶ SKIP" : ""));
    if (auto* toolbar = FindNamed<EdgeRevealContainer>(m_context.Root(), "EdgeToolbar"))
        toolbar->SetReducedMotion(p.reducedMotion);
    if (m_choices && p.choices != m_presentedChoices) {
        for (std::size_t i = 0; i < p.choices.size(); ++i) {
            Button* button = i < m_choices->Children().size()
                                 ? dynamic_cast<Button*>(m_choices->Children()[i].get())
                                 : nullptr;
            if (!button) {
                if (auto status = Add(*m_choices, MakeMenuButton(
                        p.choices[i], "choice.select", std::to_string(i)));
                    !status)
                    return status;
                button = dynamic_cast<Button*>(m_choices->Children().back().get());
            } else {
                button->SetText(p.choices[i]);
                button->SetOnActivated([this, i] {
                    Emit("choice.select", std::to_string(i));
                });
            }
            if (button) button->SetVisibility(Visibility::Visible);
        }
        for (std::size_t i = p.choices.size(); i < m_choices->Children().size(); ++i)
            if (auto* button = dynamic_cast<Button*>(m_choices->Children()[i].get()))
                button->SetVisibility(Visibility::Collapsed);
        m_presentedChoices = p.choices;
        m_choices->InvalidateLayout();
    }
    return Status::Ok();
}

Status GalgameUI::ShowBacklog(std::vector<GalgameItem> entries) {
    const Status installed=InstallTemplate(Screen::Backlog);if(!installed)return installed;auto* list=FindNamed<ListView>(m_context.Root(),"Entries");if(!list)return GalgameFailure("PXUI2805","Backlog template requires a ListView named Entries");m_items=std::make_shared<ItemSource>(std::move(entries),*this);return list->SetSource(m_items,[]{return std::make_unique<Button>();});
}

Status GalgameUI::ShowSaveLoad(bool saveMode, std::vector<GalgameItem> slots) {
    const Screen target=saveMode?Screen::Save:Screen::Load;
    const Status installed=InstallTemplate(target);if(!installed)return installed;auto* grid=FindNamed<GridView>(m_context.Root(),"Slots");if(!grid)return GalgameFailure("PXUI2806","SaveLoad template requires a GridView named Slots");m_items=std::make_shared<ItemSource>(std::move(slots),*this);return grid->SetSource(m_items,[]{return std::make_unique<CardButton>();});
}

Status GalgameUI::ShowGallery(std::vector<GalgameItem> entries) {
    const Status installed=InstallTemplate(Screen::Gallery);if(!installed)return installed;auto* grid=FindNamed<GridView>(m_context.Root(),"Items");if(!grid)return GalgameFailure("PXUI2807","Gallery template requires a GridView named Items");m_items=std::make_shared<ItemSource>(std::move(entries),*this);return grid->SetSource(m_items,[]{return std::make_unique<CardButton>();});
}

Status GalgameUI::ShowSettings(const SettingsPresentation& s) {
    const Status installed=InstallTemplate(Screen::Settings);if(!installed)return installed;
    auto slider=[this](const char* name,double value,double maximum,const char* command){if(auto* control=FindNamed<Slider>(m_context.Root(),name)){control->SetRange(0,maximum,maximum<=128?4:1);control->SetValue(value);control->SetOnChanged([this,command](double changed){Emit(command,std::to_string(static_cast<int>(changed)));});}};
    slider("BGM",s.bgm,128,"set.bgm.value");slider("SE",s.se,128,"set.se.value");slider("Voice",s.voice,128,"set.voice.value");slider("TextSpeed",s.textSpeedMs,120,"set.speed.value");
    if(auto* skip=FindNamed<CheckBox>(m_context.Root(),"SkipRead")){skip->SetChecked(s.skipReadOnly);skip->SetOnToggled([this](bool value){Emit("set.skipread.value",value?"true":"false");});}
    if(auto* full=FindNamed<CheckBox>(m_context.Root(),"Fullscreen")){full->SetChecked(s.fullscreen);full->SetOnToggled([this](bool value){Emit("set.fullscreen.value",value?"true":"false");});}
    slider("TextScale",s.textScale*100.0,200,"set.textscale.value");
    if(auto* value=FindNamed<CheckBox>(m_context.Root(),"HighContrast")){value->SetChecked(s.highContrast);value->SetOnToggled([this](bool changed){Emit("set.highcontrast.value",changed?"true":"false");});}
    if(auto* value=FindNamed<CheckBox>(m_context.Root(),"ReducedMotion")){value->SetChecked(s.reducedMotion);value->SetOnToggled([this](bool changed){Emit("set.reducedmotion.value",changed?"true":"false");});}
    if(auto* value=FindNamed<CheckBox>(m_context.Root(),"SelfVoicing")){value->SetChecked(s.selfVoicing);value->SetOnToggled([this](bool changed){Emit("set.selfvoicing.value",changed?"true":"false");});}
    if(auto* value=FindNamed<OptionButton>(m_context.Root(),"Language")){value->SetOptions(s.languages);const auto selected=std::find(s.languages.begin(),s.languages.end(),s.language);value->SetSelected(selected==s.languages.end()?0:static_cast<int>(std::distance(s.languages.begin(),selected)));value->SetOnActivated([this,value](){if(value->Selected()>=0&&static_cast<std::size_t>(value->Selected())<value->Options().size())Emit("set.language.value",value->Options()[static_cast<std::size_t>(value->Selected())]);});}
    return Status::Ok();
}

Status GalgameUI::ShowVideoOverlay(bool skippable) {
    const Status installed = InstallTemplate(Screen::Video);
    if (!installed) return installed;
    auto* hint = FindNamed<Label>(m_context.Root(), "SkipHint");
    if (!hint) return GalgameFailure("PXUI2808", "Video template requires a Label named SkipHint");
    hint->SetText(skippable ? "點擊跳過 ▶" : "");
    return Status::Ok();
}

bool GalgameUI::Update(const Input& input, int width, int height,float deltaSeconds) { const bool consumed=m_context.Update(input,width,height,deltaSeconds);auto actions=std::move(m_pendingActions);m_pendingActions.clear();for(const auto& action:actions)if(m_sink)m_sink(action);return consumed; }
void GalgameUI::Render(graphics::Renderer2D& renderer) { m_context.Render(renderer); }

}  // namespace px::ui
