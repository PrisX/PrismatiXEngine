#include "Engine/UI/GalgameUI.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/Core/TypeRegistry.h"

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
void ApplyDialogueStyle(Control& control){auto binding=control.StyleBinding();binding.baseStyle=BuiltinDialogueStyleId();control.SetStyleBinding(std::move(binding));}
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
    (void)m_context.Commands().Register("hud.toolbar.pin",[this](const Variant&){if(auto* toolbar=FindNamed<EdgeRevealContainer>(m_context.Root(),"EdgeToolbar"))toolbar->TogglePinned();return Status::Ok();});
}

Status GalgameUI::RegisterTemplate(Screen screen,std::string_view text,const std::string& sourcePath){
    auto parsed=resource::ParseTypedDocument(text,sourcePath);if(!parsed){for(const auto& d:parsed.Diagnostics())diag::Emit(d);return Status::Fail(parsed.Diagnostics());}
    if(parsed.Value().kind!=resource::DocumentKind::Scene||parsed.Value().type!="UIScene")return GalgameFailure("PXUI2803","Galgame template is not a typed UIScene: "+sourcePath);
    m_templates[static_cast<int>(screen)]=std::move(parsed.Value());return Status::Ok();
}

bool GalgameUI::HasTemplate(Screen screen)const{return m_templates.contains(static_cast<int>(screen));}

Status GalgameUI::InstallTemplate(Screen screen){
    const auto it=m_templates.find(static_cast<int>(screen));if(it==m_templates.end())return GalgameFailure("PXUI2804","Requested Galgame UI template is not registered");
    auto loaded=InstantiateUIScene(it->second,&m_viewModel,m_context.Formatters());if(!loaded)return Status::Fail(loaded.Diagnostics());
    auto bindings=std::move(loaded.Value().bindings);auto animations=std::move(loaded.Value().animations);auto theme=std::move(loaded.Value().theme);auto triggers=std::move(loaded.Value().triggers);auto interaction=std::move(loaded.Value().interactionGraph);const Status installed=Install(std::move(loaded.Value().root),screen);if(!installed)return installed;m_bindings=std::move(bindings);if(theme)m_context.SetTheme(std::move(*theme));if(animations){const Status status=m_context.SetAnimations(std::move(*animations),true);if(!status)return status;}const Status triggerStatus=m_context.ConfigureTriggers(std::move(triggers),std::move(interaction));if(!triggerStatus)return triggerStatus;
    if(screen==Screen::HUD){m_speaker=FindNamed<Label>(m_context.Root(),"Speaker");m_dialogue=FindNamed<Label>(m_context.Root(),"Dialogue");m_nvlText=FindNamed<Label>(m_context.Root(),"NVLText");m_choices=FindNamed<VBoxContainer>(m_context.Root(),"Choices");m_mode=FindNamed<Label>(m_context.Root(),"ModeState");m_noticePanel=FindNamed<EdgeRevealContainer>(m_context.Root(),"NoticePanel");m_chapterNotice=FindNamed<Label>(m_context.Root(),"ChapterNotice");m_musicNotice=FindNamed<Label>(m_context.Root(),"MusicNotice");m_lastChapterTitle.clear();m_lastMusicTitle.clear();if(m_dialogue){m_dialogueBaseOffsets=m_dialogue->Offsets();m_dialogueBaseFontSize=m_dialogue->FontSize()>0?m_dialogue->FontSize():30;}}
    return Status::Ok();
}
void GalgameUI::Emit(std::string command, std::string argument) { m_pendingActions.push_back({std::move(command),std::move(argument)}); }

Status GalgameUI::Add(Control& parent, std::unique_ptr<Control> child) {
    const Status status = parent.AddChild(std::move(child)); return status ? status : EmitStatus(status);
}

std::unique_ptr<Control> GalgameUI::MakeRoot(std::string name) {
    auto root = std::make_unique<StackContainer>(std::move(name));
    root->SetMouseFilter(MouseFilter::Pass);
    return root;
}

std::unique_ptr<Control> GalgameUI::MakeMenuButton(std::string text, std::string command, std::string argument) {
    auto button = std::make_unique<Button>(std::move(text));
    button->SetOnActivated([this, command = std::move(command), argument = std::move(argument)] { Emit(command, argument); });
    return button;
}

Status GalgameUI::Install(std::unique_ptr<Control> root, Screen screen) {
    m_bindings.clear();
    if (screen != Screen::HUD) {
        m_speaker = m_dialogue = m_nvlText = m_mode = nullptr;
        m_choices = nullptr;
    }
    const Status status = m_context.SetRoot(std::move(root)); if (!status) return status;
    m_screen = screen; return Status::Ok();
}

Status GalgameUI::ApplyAnimationProperty(const animation::TrackBinding& binding,const Variant& value){Control* control=binding.target=="$root"?m_context.Root():FindNamed<Control>(m_context.Root(),binding.target);if(!control)return GalgameFailure("PXUI2810","Animation UI target was not found: "+binding.target);const auto number=[&]()->std::optional<double>{if(const auto* real=value.TryGet<double>())return *real;if(const auto* integer=value.TryGet<std::int64_t>())return static_cast<double>(*integer);return std::nullopt;}();if(binding.kind==animation::TargetKind::Text&&number){if(auto* label=dynamic_cast<Label*>(control)){if(binding.property=="typewriter"){auto [iterator,_]=m_animationTextBase.try_emplace(binding.target,label->Text());const auto& full=iterator->second;std::size_t bytes=static_cast<std::size_t>(std::clamp(*number,0.0,1.0)*full.size());while(bytes<full.size()&&(static_cast<unsigned char>(full[bytes])&0xC0)==0x80)++bytes;label->SetText(full.substr(0,bytes));if(*number>=1.0)m_animationTextBase.erase(binding.target);return Status::Ok();}if(binding.property=="shake"||binding.property=="wave"){Rect offsets=label->Offsets();offsets.x+=static_cast<float>(std::sin(*number*37.0)*4.0);offsets.y+=static_cast<float>(std::sin(*number*23.0)*2.0);label->SetOffsets(offsets);return Status::Ok();}if(binding.property=="fade"||binding.property=="slide"||binding.property=="pop"||binding.property=="rainbow"||binding.property=="glitch")return Status::Ok();}}std::string property=binding.property;if(property=="panel-fade"||property=="modal-open"||property=="modal-close"||property=="button-hover"||property=="button-press")property="opacity";const auto* descriptor=TypeRegistry::Global().FindProperty(std::string(control->TypeName()),property);if(!descriptor||!descriptor->set)return GalgameFailure("PXUI2811","Animation UI property was not found: "+property);return descriptor->set(*control,value);}

Status GalgameUI::ShowTitle() {
    if(HasTemplate(Screen::Title))return InstallTemplate(Screen::Title);
    auto root = MakeRoot("Title");
    auto shade = std::make_unique<Panel>("TitleShade"); shade->SetAnchors({0,0,1,1}); ApplyDialogueStyle(*shade);
    auto menu = std::make_unique<VBoxContainer>("TitleMenu"); menu->SetAnchors({0.34f,0.18f,0.66f,0.84f}); menu->SetSeparation(14);
    auto title = std::make_unique<Label>("PrismatiX", "GameTitle"); title->SetFontSize(52); title->SetSizeFlags(SizeFlag::Fill, SizeFlag::Expand);
    if (auto status = Add(*menu, std::move(title)); !status) return status;
    for (auto [text, command] : std::vector<std::pair<std::string,std::string>>{{"開始遊戲","game.start"},{"讀取遊戲","load.open"},{"CG 鑑賞","gallery.open"},{"設定","settings.open"},{"離開","app.quit"}})
        if (auto status = Add(*menu, MakeMenuButton(std::move(text), std::move(command))); !status) return status;
    if (auto status = Add(*shade, std::move(menu)); !status) return status;
    if (auto status = Add(*root, std::move(shade)); !status) return status;
    return Install(std::move(root), Screen::Title);
}

Status GalgameUI::ShowHUD(const DialoguePresentation& p) {
    if(HasTemplate(Screen::HUD)){const Status status=InstallTemplate(Screen::HUD);if(!status)return status;return RefreshHUD(p);}
    auto root = MakeRoot("HUD");
    auto nvl = std::make_unique<Panel>("NVLPanel"); nvl->SetAnchors({0.055f,0.06f,0.945f,0.88f}); ApplyDialogueStyle(*nvl);
    auto nvlLabel = std::make_unique<Label>("", "NVLText"); m_nvlText = nvlLabel.get(); nvlLabel->SetWrap(true); nvlLabel->SetAnchors({0.04f,0.04f,0.96f,0.96f});
    if (auto status = Add(*nvl, std::move(nvlLabel)); !status) return status;
    nvl->SetVisibility(p.nvlMode ? Visibility::Visible : Visibility::Collapsed);

    auto adv = std::make_unique<Panel>("ADVPanel"); adv->SetAnchors({0.045f,0.64f,0.955f,0.955f}); ApplyDialogueStyle(*adv);
    auto speaker = std::make_unique<Label>("", "Speaker"); m_speaker = speaker.get(); speaker->SetFontSize(25); speaker->SetColor({255,220,145,255}); speaker->SetAnchors({0.03f,0.08f,0.97f,0.25f});
    auto dialogue = std::make_unique<Label>("", "Dialogue"); m_dialogue = dialogue.get(); dialogue->SetFontSize(30); dialogue->SetWrap(true); dialogue->SetAnchors({0.03f,0.28f,0.97f,0.91f});
    if (auto status = Add(*adv, std::move(speaker)); !status) return status;
    if (auto status = Add(*adv, std::move(dialogue)); !status) return status;
    adv->SetVisibility(p.nvlMode ? Visibility::Collapsed : Visibility::Visible);

    auto choices = std::make_unique<VBoxContainer>("Choices"); m_choices = choices.get(); choices->SetAnchors({0.20f,0.18f,0.80f,0.60f}); choices->SetSeparation(12);
    auto quick = std::make_unique<HBoxContainer>("QuickMenu"); quick->SetAnchors({0.50f,0.025f,0.97f,0.105f}); quick->SetSeparation(8);
    for (auto [text, command] : std::vector<std::pair<std::string,std::string>>{{"AUTO","mode.auto"},{"SKIP","mode.skip"},{"LOG","backlog.open"},{"SAVE","save.open"},{"LOAD","load.open"},{"⚙","settings.open"}})
        if (auto status = Add(*quick, MakeMenuButton(std::move(text), std::move(command))); !status) return status;
    auto mode = std::make_unique<Label>("", "ModeState"); m_mode = mode.get(); mode->SetAnchors({0.02f,0.02f,0.45f,0.09f}); mode->SetFontSize(20);
    if (auto status = Add(*root, std::move(nvl)); !status) return status;
    if (auto status = Add(*root, std::move(adv)); !status) return status;
    if (auto status = Add(*root, std::move(choices)); !status) return status;
    if (auto status = Add(*root, std::move(quick)); !status) return status;
    if (auto status = Add(*root, std::move(mode)); !status) return status;
    const Status installed = Install(std::move(root), Screen::HUD); if (!installed) return installed;
    if (m_dialogue) { m_dialogueBaseOffsets = m_dialogue->Offsets(); m_dialogueBaseFontSize = m_dialogue->FontSize() > 0 ? m_dialogue->FontSize() : 30; }
    return RefreshHUD(p);
}

Status GalgameUI::RefreshHUD(const DialoguePresentation& p) {
    if (m_screen != Screen::HUD || !m_context.Root()) return ShowHUD(p);
    if (m_speaker) m_speaker->SetText(p.speaker);
    if (m_dialogue) {
        m_dialogue->SetText(p.text);
        Rect offsets = m_dialogueBaseOffsets;
        int fontSize = m_dialogueBaseFontSize;
        if (!p.reducedMotion && p.effect == "shake") {
            offsets.x += std::sin(p.effectProgress * 47.0f) * 3.0f;
            offsets.y += std::sin(p.effectProgress * 71.0f) * 2.0f;
        } else if (!p.reducedMotion && p.effect == "pulse") {
            fontSize += static_cast<int>(std::lround((std::sin(p.effectProgress * 7.0f) + 1.0f) * 1.5f));
        }
        m_dialogue->SetOffsets(offsets);
        m_dialogue->SetFontSize(static_cast<int>(std::lround(fontSize*std::clamp(p.textScale,.75f,2.0f))));
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
        std::string text; for (const auto& line : p.nvlLines) text += line + "\n\n"; m_nvlText->SetText(std::move(text));
        if (auto* panel = dynamic_cast<Control*>(m_nvlText->Parent())) panel->SetVisibility(p.nvlMode ? Visibility::Visible : Visibility::Collapsed);
    }
    if (m_dialogue) if (auto* panel = dynamic_cast<Control*>(m_dialogue->Parent())) panel->SetVisibility(p.nvlMode ? Visibility::Collapsed : Visibility::Visible);
    if (m_mode) m_mode->SetText(std::string(p.autoMode ? "● AUTO  " : "") + (p.skipMode ? "▶▶ SKIP" : ""));
    if (m_choices) {
        std::vector<Uuid> old; for (const auto& child : m_choices->Children()) old.push_back(child->Id());
        for (const auto& id : old) { auto removed = m_choices->RemoveChild(id); (void)removed; }
        for (std::size_t i = 0; i < p.choices.size(); ++i)
            if (auto status = Add(*m_choices, MakeMenuButton(p.choices[i], "choice.select", std::to_string(i))); !status) return status;
        m_choices->InvalidateLayout();
    }
    return Status::Ok();
}

Status GalgameUI::ShowBacklog(std::vector<GalgameItem> entries) {
    if(HasTemplate(Screen::Backlog)){const Status installed=InstallTemplate(Screen::Backlog);if(!installed)return installed;auto* list=FindNamed<ListView>(m_context.Root(),"Entries");if(!list)return GalgameFailure("PXUI2805","Backlog template requires a ListView named Entries");m_items=std::make_shared<ItemSource>(std::move(entries),*this);return list->SetSource(m_items,[]{return std::make_unique<Button>();});}
    auto root = MakeRoot("Backlog"); auto panel = std::make_unique<Panel>(); panel->SetAnchors({0.03f,0.03f,0.97f,0.97f}); ApplyDialogueStyle(*panel);
    auto list = std::make_unique<ListView>(); list->SetAnchors({0.03f,0.10f,0.97f,0.94f}); list->SetItemExtent({800,76});
    m_items = std::make_shared<ItemSource>(std::move(entries), *this);
    if (auto status = list->SetSource(m_items, [] { auto b = std::make_unique<Button>(); b->SetSizeFlags(SizeFlag::Expand | SizeFlag::Fill, SizeFlag::Fill); return b; }); !status) return status;
    auto close = MakeMenuButton("返回", "overlay.close"); close->SetAnchors({0.82f,0.02f,0.97f,0.09f});
    if (auto status = Add(*panel, std::move(list)); !status) return status;
    if (auto status = Add(*panel, std::move(close)); !status) return status;
    if (auto status = Add(*root, std::move(panel)); !status) return status;
    return Install(std::move(root), Screen::Backlog);
}

Status GalgameUI::ShowSaveLoad(bool saveMode, std::vector<GalgameItem> slots) {
    const Screen target=saveMode?Screen::Save:Screen::Load;
    if(HasTemplate(target)){const Status installed=InstallTemplate(target);if(!installed)return installed;auto* grid=FindNamed<GridView>(m_context.Root(),"Slots");if(!grid)return GalgameFailure("PXUI2806","SaveLoad template requires a GridView named Slots");m_items=std::make_shared<ItemSource>(std::move(slots),*this);return grid->SetSource(m_items,[]{return std::make_unique<CardButton>();});}
    auto root = MakeRoot(saveMode ? "Save" : "Load"); auto panel = std::make_unique<Panel>(); panel->SetAnchors({0.03f,0.03f,0.97f,0.97f}); ApplyDialogueStyle(*panel);
    auto grid = std::make_unique<GridView>(3); grid->SetAnchors({0.03f,0.12f,0.97f,0.93f}); grid->SetItemExtent({320,170});
    m_items = std::make_shared<ItemSource>(std::move(slots), *this);
    if (auto status = grid->SetSource(m_items, [] { return std::make_unique<CardButton>(); }); !status) return status;
    auto close = MakeMenuButton("返回", "overlay.close"); close->SetAnchors({0.82f,0.02f,0.97f,0.10f});
    if (auto status = Add(*panel, std::move(grid)); !status) return status; if (auto status = Add(*panel, std::move(close)); !status) return status;
    if (auto status = Add(*root, std::move(panel)); !status) return status; return Install(std::move(root), saveMode ? Screen::Save : Screen::Load);
}

Status GalgameUI::ShowGallery(std::vector<GalgameItem> entries) {
    if(HasTemplate(Screen::Gallery)){const Status installed=InstallTemplate(Screen::Gallery);if(!installed)return installed;auto* grid=FindNamed<GridView>(m_context.Root(),"Items");if(!grid)return GalgameFailure("PXUI2807","Gallery template requires a GridView named Items");m_items=std::make_shared<ItemSource>(std::move(entries),*this);return grid->SetSource(m_items,[]{return std::make_unique<CardButton>();});}
    auto root = MakeRoot("Gallery"); auto panel = std::make_unique<Panel>(); panel->SetAnchors({0.03f,0.03f,0.97f,0.97f}); ApplyDialogueStyle(*panel);
    auto grid = std::make_unique<GridView>(4); grid->SetAnchors({0.03f,0.12f,0.97f,0.93f}); grid->SetItemExtent({260,180});
    m_items = std::make_shared<ItemSource>(std::move(entries), *this);
    if (auto status = grid->SetSource(m_items, [] { return std::make_unique<CardButton>(); }); !status) return status;
    auto close = MakeMenuButton("返回", "overlay.close"); close->SetAnchors({0.82f,0.02f,0.97f,0.10f});
    if (auto status = Add(*panel, std::move(grid)); !status) return status; if (auto status = Add(*panel, std::move(close)); !status) return status;
    if (auto status = Add(*root, std::move(panel)); !status) return status; return Install(std::move(root), Screen::Gallery);
}

Status GalgameUI::ShowSettings(const SettingsPresentation& s) {
    if(HasTemplate(Screen::Settings)){
        const Status installed=InstallTemplate(Screen::Settings);if(!installed)return installed;
        auto slider=[this](const char* name,double value,double maximum,const char* command){if(auto* control=FindNamed<Slider>(m_context.Root(),name)){control->SetRange(0,maximum,maximum<=128?4:1);control->SetValue(value);control->SetOnChanged([this,command](double changed){Emit(command,std::to_string(static_cast<int>(changed)));});}};
        slider("BGM",s.bgm,128,"set.bgm.value");slider("SE",s.se,128,"set.se.value");slider("Voice",s.voice,128,"set.voice.value");slider("TextSpeed",s.textSpeedMs,120,"set.speed.value");
        if(auto* skip=FindNamed<CheckBox>(m_context.Root(),"SkipRead")){skip->SetChecked(s.skipReadOnly);skip->SetOnToggled([this](bool value){Emit("set.skipread.value",value?"true":"false");});}
        if(auto* full=FindNamed<CheckBox>(m_context.Root(),"Fullscreen")){full->SetChecked(s.fullscreen);full->SetOnToggled([this](bool value){Emit("set.fullscreen.value",value?"true":"false");});}
        slider("TextScale",s.textScale*100.0,200,"set.textscale.value");
        if(auto* value=FindNamed<CheckBox>(m_context.Root(),"HighContrast")){value->SetChecked(s.highContrast);value->SetOnToggled([this](bool changed){Emit("set.highcontrast.value",changed?"true":"false");});}
        if(auto* value=FindNamed<CheckBox>(m_context.Root(),"ReducedMotion")){value->SetChecked(s.reducedMotion);value->SetOnToggled([this](bool changed){Emit("set.reducedmotion.value",changed?"true":"false");});}
        if(auto* value=FindNamed<CheckBox>(m_context.Root(),"SelfVoicing")){value->SetChecked(s.selfVoicing);value->SetOnToggled([this](bool changed){Emit("set.selfvoicing.value",changed?"true":"false");});}
        return Status::Ok();
    }
    auto root = MakeRoot("Settings"); auto panel = std::make_unique<Panel>(); panel->SetAnchors({0.20f,0.08f,0.80f,0.92f}); ApplyDialogueStyle(*panel);
    auto rows = std::make_unique<VBoxContainer>(); rows->SetAnchors({0.06f,0.08f,0.94f,0.92f}); rows->SetSeparation(10);
    auto row = [this, &rows](std::string label, int value, std::string command) -> Status {
        auto line = std::make_unique<HBoxContainer>(); line->SetSizeFlags(SizeFlag::Fill, SizeFlag::Expand); auto text = std::make_unique<Label>(label + "  " + std::to_string(value)); text->SetSizeFlags(SizeFlag::Expand | SizeFlag::Fill, SizeFlag::Fill);
        if (auto status = Add(*line, std::move(text)); !status) return status; if (auto status = Add(*line, MakeMenuButton("−", command + ".down")); !status) return status;
        if (auto status = Add(*line, MakeMenuButton("＋", command + ".up")); !status) return status; return Add(*rows, std::move(line));
    };
    if (auto st = row("BGM",s.bgm,"set.bgm"); !st) return st; if (auto st = row("SE",s.se,"set.se"); !st) return st;
    if (auto st = row("VOICE",s.voice,"set.voice"); !st) return st; if (auto st = row("文字速度",s.textSpeedMs,"set.speed"); !st) return st;
    if (auto st = Add(*rows, MakeMenuButton(std::string("只快進已讀：") + (s.skipReadOnly ? "是" : "否"), "set.skipread.toggle")); !st) return st;
    if (auto st = Add(*rows, MakeMenuButton(std::string("全螢幕：") + (s.fullscreen ? "是" : "否"), "set.fullscreen.toggle")); !st) return st;
    if (auto st = Add(*rows, MakeMenuButton("完成", "overlay.close")); !st) return st;
    if (auto st = Add(*panel, std::move(rows)); !st) return st; if (auto st = Add(*root, std::move(panel)); !st) return st;
    return Install(std::move(root), Screen::Settings);
}

Status GalgameUI::ShowVideoOverlay(bool skippable) {
    if (HasTemplate(Screen::Video)) {
        const Status installed = InstallTemplate(Screen::Video);
        if (!installed) return installed;
        auto* hint = FindNamed<Label>(m_context.Root(), "SkipHint");
        if (!hint) return GalgameFailure("PXUI2808", "Video template requires a Label named SkipHint");
        hint->SetText(skippable ? "點擊跳過 ▶" : "");
        return Status::Ok();
    }
    auto root = MakeRoot("VideoOverlay"); auto label = std::make_unique<Label>(skippable ? "點擊跳過 ▶" : ""); label->SetAnchors({0.78f,0.03f,0.97f,0.10f});
    if (auto st = Add(*root, std::move(label)); !st) return st; return Install(std::move(root), Screen::Video);
}

bool GalgameUI::Update(const Input& input, int width, int height,float deltaSeconds) { const bool consumed=m_context.Update(input,width,height,deltaSeconds);auto actions=std::move(m_pendingActions);m_pendingActions.clear();for(const auto& action:actions)if(m_sink)m_sink(action);return consumed; }
void GalgameUI::Render(graphics::Renderer2D& renderer) { m_context.Render(renderer); }

}  // namespace px::ui
