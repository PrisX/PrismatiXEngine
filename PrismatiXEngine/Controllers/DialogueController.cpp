#include "DialogueController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <sol/sol.hpp>

#include "ArchiveManager.h"
#include "AudioManager.h"
#include "BacklogManager.h"
#include "TextManager.h"
#include "TextureManager.h"
#include "UIManager.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"
#include "VariableManager.h"

namespace {
constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;

std::vector<std::string> SplitUtf8Chars(const std::string& text) {
    std::vector<std::string> chars;
    size_t i = 0;
    // UTF-8 神奇拆解，反正就是要用記憶體位置 https://stackoverflow.com/questions/45716356/utf-text-in-sdl2
    while (i < text.length()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80) == 0)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;

        if (i + len > text.length()) {
            len = 1;
        }

        chars.push_back(text.substr(i, len));
        i += len;
    }

    return chars;
}

std::string ReadFirstArg(const std::map<std::string, std::string>& args, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = args.find(key);
        if (it != args.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return "";
}

std::string ReadTransitionStyleArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "transition", "trans", "style" }); }

std::string ReadTransitionSpeedArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "transitionSpeed", "transitionspeed", "trans_speed", "tspeed", "speed" }); }

std::string ReadTransitionEaseArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "ease", "easing" }); }

std::string ReadCharacterAnimationArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "anim", "animation", "motion" }); }

std::string ReadCharacterAnimationEaseArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "animease", "animationease", "ease", "easing" }); }

int ReadIntArg(const std::map<std::string, std::string>& args, std::initializer_list<const char*> keys, int fallback) {
    std::string value = ReadFirstArg(args, keys);
    if (value.empty()) return fallback;

    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

int ReadCharacterAnimationDurationArg(const std::map<std::string, std::string>& args) {
    int duration = ReadIntArg(args, { "animduration", "animframes", "duration", "frames", "speed" }, 18);
    return std::clamp(duration, 1, 120);
}

std::string ToLowerAlphaNumeric(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());

    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            lowered.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    return lowered;
}

std::string NormalizeCharacterAnimation(const std::string& value) {
    std::string lowered = ToLowerAlphaNumeric(value);

    if (lowered.empty() || lowered == "fade" || lowered == "default") return "fade";
    if (lowered == "none" || lowered == "instant") return "none";
    if (lowered == "slide" || lowered == "auto" || lowered == "slideauto") return "slide_auto";
    if (lowered == "slideleft" || lowered == "left") return "slide_left";
    if (lowered == "slideright" || lowered == "right") return "slide_right";
    if (lowered == "slideup" || lowered == "up") return "slide_up";
    if (lowered == "slidedown" || lowered == "down") return "slide_down";
    if (lowered == "pop" || lowered == "popin") return "pop";
    if (lowered == "bounce" || lowered == "jump") return "bounce";
    if (lowered == "zoom" || lowered == "zoomin") return "zoom";

    return "fade";
}

std::string ResolveCharacterAnimation(const ActiveCharacter& chara) {
    if (chara.animation == "slide_auto") {
        return (chara.targetX <= (kScreenWidth * 0.5f)) ? "slide_left" : "slide_right";
    }
    return chara.animation;
}

void StartCharacterAnimation(ActiveCharacter& chara, const std::map<std::string, std::string>& args) {
    chara.animation = NormalizeCharacterAnimation(ReadCharacterAnimationArg(args));
    chara.animationEase = ReadCharacterAnimationEaseArg(args);
    chara.animationDuration = ReadCharacterAnimationDurationArg(args);
    chara.animationFrame = 0;
    chara.animationActive = (chara.animation != "none");
    chara.renderOffsetX = 0.0f;
    chara.renderOffsetY = 0.0f;
    chara.renderScale = 1.0f;
}

void ApplyCharacterAnimationResult(ActiveCharacter& chara, const sol::table& result) {
    sol::optional<float> offsetX = result["offsetX"];
    if (offsetX) {
        chara.renderOffsetX = *offsetX;
    }

    sol::optional<float> offsetY = result["offsetY"];
    if (offsetY) {
        chara.renderOffsetY = *offsetY;
    }

    sol::optional<float> scale = result["scale"];
    if (scale) {
        chara.renderScale = *scale;
    }
}

void UpdateCharacterAnimation(ActiveCharacter& chara, sol::state* luaState) {
    chara.renderOffsetX = 0.0f;
    chara.renderOffsetY = 0.0f;
    chara.renderScale = 1.0f;

    if (!chara.animationActive) return;

    float duration = static_cast<float>(std::max(1, chara.animationDuration));
    float progress = std::clamp(static_cast<float>(chara.animationFrame) / duration, 0.0f, 1.0f);
    std::string animation = ResolveCharacterAnimation(chara);

    if (luaState) {
        sol::object animationsObj = (*luaState)["PortraitAnimations"];
        if (animationsObj.valid() && animationsObj.get_type() == sol::type::table) {
            sol::table animations = animationsObj.as<sol::table>();
            sol::protected_function fx = animations[animation];

            if (fx.valid()) {
                sol::table ctx = luaState->create_table();
                ctx["name"] = chara.name;
                ctx["diff"] = chara.diff;
                ctx["animation"] = chara.animation;
                ctx["resolvedAnimation"] = animation;
                ctx["trigger"] = chara.animationTrigger;
                ctx["ease"] = chara.animationEase;
                ctx["progress"] = progress;
                ctx["frame"] = chara.animationFrame;
                ctx["duration"] = chara.animationDuration;
                ctx["pos"] = chara.pos;
                ctx["currentX"] = chara.currentX;
                ctx["targetX"] = chara.targetX;
                ctx["screenWidth"] = kScreenWidth;
                ctx["screenHeight"] = kScreenHeight;

                sol::protected_function_result result = fx(ctx);
                if (!result.valid()) {
                    sol::error err = result;
                    std::cerr << "Portrait animation runtime error (" << animation << "): " << err.what() << std::endl;
                    chara.animationActive = false;
                }
                else if (result.return_count() > 0) {
                    sol::optional<sol::table> animResult = result;
                    if (animResult) {
                        ApplyCharacterAnimationResult(chara, *animResult);
                    }
                }
            }
        }
    }

    if (progress >= 1.0f) {
        chara.animationActive = false;
        chara.renderOffsetX = 0.0f;
        chara.renderOffsetY = 0.0f;
        chara.renderScale = 1.0f;
        return;
    }

    chara.animationFrame++;
}
}  // namespace

DialogueController::DialogueController(TTF_Font* dialogueFont, const std::string& dialogueFontName, int dialogueFontSize, TTF_Font* nameFont, SDL_Renderer* ren, sol::state* lua) {
    (void)dialogueFontName;
    (void)dialogueFontSize;
    (void)nameFont;
    uiFont = dialogueFont;
    renderer = ren;
    luaState = lua;
    currentLine = 0;
    clickCooldown = 0;
    isFinished = false;
    hasStarted = false;
    InitializeCommandHandlers();
}

void DialogueController::InitializeCommandHandlers() {
    commandHandlers["charimg"] = [this](const VNCommand& cmd) { HandleCommandCharImg(cmd); };
    commandHandlers["clearcharimg"] = [this](const VNCommand& cmd) { HandleCommandClearCharImg(cmd); };
    commandHandlers["bg"] = [this](const VNCommand& cmd) { HandleCommandBg(cmd); };
    commandHandlers["text"] = [this](const VNCommand& cmd) { HandleCommandText(cmd); };
    commandHandlers["bgm"] = [this](const VNCommand& cmd) { HandleCommandBgm(cmd); };
    commandHandlers["stopbgm"] = [this](const VNCommand& cmd) { HandleCommandStopBgm(cmd); };
    commandHandlers["se"] = [this](const VNCommand& cmd) { HandleCommandSe(cmd); };
    commandHandlers["set"] = [this](const VNCommand& cmd) { HandleCommandSet(cmd); };
    commandHandlers["add"] = [this](const VNCommand& cmd) { HandleCommandAdd(cmd); };
    commandHandlers["del"] = [this](const VNCommand& cmd) { HandleCommandDel(cmd); };
    commandHandlers["bgminfo"] = [this](const VNCommand& cmd) { HandleCommandBgmInfo(cmd); };
    commandHandlers["chapter"] = [this](const VNCommand& cmd) { HandleCommandChapter(cmd); };
    commandHandlers["lua"] = [this](const VNCommand& cmd) { HandleCommandLua(cmd); };
    commandHandlers["luafx"] = [this](const VNCommand& cmd) { HandleCommandLua(cmd); };
    commandHandlers["transition"] = [this](const VNCommand& cmd) { HandleCommandTransition(cmd); };
    commandHandlers["label"] = [this](const VNCommand& cmd) { HandleCommandLabel(cmd); };
    commandHandlers["jump"] = [this](const VNCommand& cmd) { HandleCommandJump(cmd); };
    commandHandlers["if"] = [this](const VNCommand& cmd) { HandleCommandIf(cmd); };
    commandHandlers["else"] = [this](const VNCommand& cmd) { HandleCommandElse(cmd); };
    commandHandlers["endif"] = [this](const VNCommand& cmd) { HandleCommandEndIf(cmd); };
    commandHandlers["choice"] = [this](const VNCommand& cmd) { HandleCommandChoice(cmd); };
}

void DialogueController::ParseDialogueUTF8(const std::string& text) { dialogueParsedCharacters = SplitUtf8Chars(text); }

void DialogueController::SetDialogueText(const std::string& speaker, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor, const std::string& textEffect) {
    ParseDialogueUTF8(text);
    dialogueCurrentSpeakerName = speaker;
    dialogueCurrentDisplayText.clear();
    dialogueDisplayedText.clear();
    dialogueCurrentTextColor = textColor;
    dialogueCurrentOutlineColor = outlineColor;
    dialogueCurrentIndex = 0;
    dialogueTextSpeed = speed;
    dialogueLastTime = SDL_GetTicks();
    dialogueFadeAlpha = 255;
    dialogueActiveTextEffect = textEffect;
    dialogueEffectStartTime = SDL_GetTicks();

    if (dialogueTextSpeed <= 0) {
        dialogueCurrentDisplayText = text;
        dialogueDisplayedText = text;
        dialogueCurrentIndex = static_cast<int>(dialogueParsedCharacters.size());
    }
}

void DialogueController::UpdateDialogueText() {
    Uint32 currentTime = SDL_GetTicks();
    if (dialogueCurrentIndex < static_cast<int>(dialogueParsedCharacters.size())) {
        if (currentTime - dialogueLastTime >= static_cast<Uint32>(std::max(0, dialogueTextSpeed))) {
            dialogueDisplayedText = dialogueCurrentDisplayText;
            dialogueCurrentDisplayText += dialogueParsedCharacters[dialogueCurrentIndex];
            dialogueCurrentIndex++;
            dialogueLastTime = currentTime;
            dialogueFadeAlpha = 0;
            dialogueFadeStartTime = currentTime;
        }
    }

    if (dialogueFadeAlpha < 255) {
        Uint32 elapsed = SDL_GetTicks() - dialogueFadeStartTime;
        dialogueFadeAlpha = TransitionUtils::AlphaFromElapsed(elapsed, kDialogueFadeDuration);
    }
}

void DialogueController::ShowDialogueTextAll() {
    if (dialogueCurrentIndex < static_cast<int>(dialogueParsedCharacters.size())) {
        dialogueCurrentDisplayText.clear();
        for (const auto& ch : dialogueParsedCharacters) {
            dialogueCurrentDisplayText += ch;
        }
        dialogueCurrentIndex = static_cast<int>(dialogueParsedCharacters.size());
    }

    dialogueDisplayedText = dialogueCurrentDisplayText;
    dialogueFadeAlpha = 255;
}

bool DialogueController::IsDialogueTextFinished() const { return dialogueCurrentIndex >= static_cast<int>(dialogueParsedCharacters.size()); }

sol::table DialogueController::GetDialogueBoxContext(sol::this_state state, int screenW, int screenH) const {
    sol::state_view lua(state);
    sol::table ctx = lua.create_table();

    ctx["visible"] = !isFinished;
    ctx["speaker"] = dialogueCurrentSpeakerName;
    ctx["currentText"] = dialogueCurrentDisplayText;
    ctx["displayedText"] = dialogueDisplayedText;
    ctx["fadeAlpha"] = dialogueFadeAlpha;
    ctx["effect"] = dialogueActiveTextEffect;
    ctx["elapsedMs"] = static_cast<int>(SDL_GetTicks() - dialogueEffectStartTime);
    float progress = dialogueParsedCharacters.empty() ? 1.0f : std::clamp(static_cast<float>(dialogueCurrentIndex) / static_cast<float>(dialogueParsedCharacters.size()), 0.0f, 1.0f);
    ctx["progress"] = progress;
    ctx["screenW"] = screenW;
    ctx["screenH"] = screenH;

    sol::table textColor = lua.create_table();
    textColor["r"] = dialogueCurrentTextColor.r;
    textColor["g"] = dialogueCurrentTextColor.g;
    textColor["b"] = dialogueCurrentTextColor.b;
    textColor["a"] = dialogueCurrentTextColor.a;
    ctx["textColor"] = textColor;

    sol::table outlineColor = lua.create_table();
    outlineColor["r"] = dialogueCurrentOutlineColor.r;
    outlineColor["g"] = dialogueCurrentOutlineColor.g;
    outlineColor["b"] = dialogueCurrentOutlineColor.b;
    outlineColor["a"] = dialogueCurrentOutlineColor.a;
    ctx["outlineColor"] = outlineColor;

    return ctx;
}

// For S/L
std::string DialogueController::GetCurrentScriptName() const { return currentScriptName; }
int DialogueController::GetCurrentLine() const {
    if (UIManager::HasButtons()) {
        int line = currentLine - 1;

        while (line >= 0 && commands[line].type == "choice") {
            line--;
        }
        while (line >= 0 && commands[line].type != "text") {
            line--;
        }

        return (line >= 0) ? line : 0;
    }
    return currentLine;
}
std::string DialogueController::GetCurrentBgName() const { return currentBgName; }
std::string DialogueController::GetCurrentBgmName() const { return currentBgmName; }
void DialogueController::SetCurrentLine(int line) { currentLine = line; }
void DialogueController::SetSkipNextLog(bool skip) { skipNextLog = skip; }
bool DialogueController::PopScriptTransition(std::string& outTargetScript, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) {
    if (!hasPendingScriptTransition) return false;

    outTargetScript = pendingScriptTarget;
    outTransitionStyle = pendingTransitionStyle;
    outTransitionSpeed = pendingTransitionSpeed;
    outTransitionEase = pendingTransitionEase;
    pendingScriptTarget.clear();
    pendingTransitionStyle.clear();
    pendingTransitionSpeed.clear();
    pendingTransitionEase.clear();
    hasPendingScriptTransition = false;
    return true;
}

bool DialogueController::PopInlineTransition(std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) {
    if (!hasPendingInlineTransition) return false;

    outTransitionStyle = pendingInlineTransitionStyle;
    outTransitionSpeed = pendingInlineTransitionSpeed;
    outTransitionEase = pendingInlineTransitionEase;
    pendingInlineTransitionStyle.clear();
    pendingInlineTransitionSpeed.clear();
    pendingInlineTransitionEase.clear();
    hasPendingInlineTransition = false;
    return true;
}

void DialogueController::QueueScriptTransition(const std::string& targetScript, const std::string& transitionStyle, const std::string& transitionSpeed, const std::string& transitionEase) {
    if (targetScript.empty()) return;

    BacklogManager::Clear();
    pendingScriptTarget = targetScript;
    pendingTransitionStyle = transitionStyle;
    pendingTransitionSpeed = transitionSpeed;
    pendingTransitionEase = transitionEase;
    hasPendingScriptTransition = true;
}

void DialogueController::QueueInlineTransition(const std::string& transitionStyle, const std::string& transitionSpeed, const std::string& transitionEase) {
    hasPendingInlineTransition = true;
    pendingInlineTransitionStyle = transitionStyle;
    pendingInlineTransitionSpeed = transitionSpeed;
    pendingInlineTransitionEase = transitionEase;
}

void DialogueController::ContinueScript() {
    if (isFinished) return;
    ExecuteNextCommands();
}

std::vector<SavedCharacter> DialogueController::GetSavedCharacters() const {
    std::vector<SavedCharacter> chars;
    for (const auto& [name, chara] : activeCharacters) {
        if (!chara.isExiting) {
            chars.push_back({ chara.name, chara.diff, chara.pos });
        }
    }
    return chars;
}

void DialogueController::RestoreSavedCharacters(const std::vector<SavedCharacter>& savedChars) {
    activeCharacters.clear();

    for (const auto& sc : savedChars) {
        ActiveCharacter ac;
        ac.name = sc.name;
        ac.diff = sc.diff;
        ac.pos = sc.pos;
        ac.alpha = 0.0f;
        ac.targetAlpha = 255.0f;
        ac.isExiting = false;

        ac.currentX = kScreenWidth / 2.0f;

        activeCharacters[ac.name] = ac;
    }

    RecalculateTargetPositions();

    // for (auto& [name, ac] : activeCharacters) {
    //     ac.currentX = ac.targetX;
    // }
}

void DialogueController::RestoreBackground(const std::string& bgName) {
    currentBgName = bgName;
    if (!bgName.empty()) {
        currentBgTexture = TextureManager::LoadTexture(bgName, renderer);
        bgFadeAlpha = 0.0f;
        previousBgTexture = nullptr;
    }
    else {
        currentBgTexture = nullptr;
    }
}

SDL_Texture* DialogueController::GetBackground() const { return currentBgTexture; }

void DialogueController::LoadScript(const std::string& scriptName, const std::vector<VNCommand>& newScript) {
    UIManager::Clear();
    commands = newScript;
    currentScriptName = scriptName;
    currentLine = 0;
    isFinished = false;
    hasStarted = false;
    pendingVoice.clear();
    currentSpeakingChar.clear();
    hasPendingScriptTransition = false;
    pendingScriptTarget.clear();
    pendingTransitionStyle.clear();
    pendingTransitionSpeed.clear();
    pendingTransitionEase.clear();
    hasPendingInlineTransition = false;
    pendingInlineTransitionStyle.clear();
    pendingInlineTransitionSpeed.clear();
    pendingInlineTransitionEase.clear();
    currentBgTexture = nullptr;
    activeCharacters.clear();
    dialogueParsedCharacters.clear();
    dialogueCurrentDisplayText.clear();
    dialogueDisplayedText.clear();
    dialogueCurrentSpeakerName.clear();
    dialogueCurrentTextColor = { 255, 255, 255, 255 };
    dialogueCurrentOutlineColor = { 0, 0, 0, 255 };
    dialogueCurrentIndex = 0;
    dialogueFadeAlpha = 255;
    dialogueActiveTextEffect.clear();
    dialogueEffectStartTime = SDL_GetTicks();
}

bool DialogueController::IsShowingBacklog() const { return isShowingBacklog; }

void DialogueController::ToggleBacklog() {
    if (backlogCooldown > 0) return;

    isShowingBacklog = !isShowingBacklog;
    if (isShowingBacklog) {
        backlogOffset = 0;
    }

    backlogCooldown = 15;
}

void DialogueController::ScrollBacklog(int direction) {
    if (!isShowingBacklog) return;

    backlogOffset += direction;

    if (backlogOffset < 0) backlogOffset = 0;

    int maxOffset = std::max(0, (int)BacklogManager::GetCount() - 1);
    if (backlogOffset > maxOffset) backlogOffset = maxOffset;
}

void DialogueController::RecalculateTargetPositions() {
    std::vector<ActiveCharacter*> nonExiting;
    for (auto& [name, chara] : activeCharacters) {
        if (!chara.isExiting) nonExiting.push_back(&chara);
    }
    std::sort(nonExiting.begin(), nonExiting.end(), [](const ActiveCharacter* a, const ActiveCharacter* b) { return a->pos < b->pos; });

    int total = (int)nonExiting.size();
    int sectionWidth = kScreenWidth / (total + 1);
    for (int i = 0; i < total; i++) {
        nonExiting[i]->targetX = (float)(sectionWidth * (i + 1));
    }
}

void DialogueController::HandleCommandCharImg(const VNCommand& cmd) {
    std::string name = cmd.args.at("name");
    int targetPos = std::stoi(cmd.args.at("pos"));

    if (activeCharacters.find(name) == activeCharacters.end()) {
        ActiveCharacter chara;
        chara.name = name;
        chara.diff = cmd.args.at("diff");
        chara.pos = targetPos;
        chara.alpha = 0.0f;
        chara.targetAlpha = 255.0f;
        activeCharacters[name] = chara;
        RecalculateTargetPositions();
        activeCharacters[name].currentX = activeCharacters[name].targetX;
        activeCharacters[name].animationTrigger = "enter";
        StartCharacterAnimation(activeCharacters[name], cmd.args);
    }
    else {
        ActiveCharacter& chara = activeCharacters[name];
        int previousPos = chara.pos;
        chara.diff = cmd.args.at("diff");
        chara.pos = targetPos;
        chara.targetAlpha = 255.0f;
        chara.isExiting = false;
        RecalculateTargetPositions();
        chara.animationTrigger = (previousPos != targetPos) ? "move" : "refresh";
        StartCharacterAnimation(chara, cmd.args);
    }
    currentLine++;
}

void DialogueController::HandleCommandClearCharImg(const VNCommand& cmd) {
    int targetPos = std::stoi(cmd.args.at("pos"));

    for (auto& [name, chara] : activeCharacters) {
        if (chara.pos == targetPos) {
            chara.targetAlpha = 0.0f;
            chara.isExiting = true;
            break;
        }
    }
    RecalculateTargetPositions();
    currentLine++;
}

void DialogueController::HandleCommandBg(const VNCommand& cmd) {
    std::string fileName = cmd.args.at("file");
    currentBgName = fileName;
    previousBgTexture = currentBgTexture;
    currentBgTexture = TextureManager::LoadTexture(fileName, renderer);
    bgFadeAlpha = 0.0f;
    currentLine++;
}

void DialogueController::HandleCommandText(const VNCommand& cmd) {
    std::string speaker = cmd.args.count("name") ? cmd.args.at("name") : "";
    std::string content = cmd.args.at("content");

    size_t startPos = 0;
    while ((startPos = content.find('{', startPos)) != std::string::npos) {
        size_t endPos = content.find('}', startPos);
        if (endPos == std::string::npos) break;

        std::string varName = content.substr(startPos + 1, endPos - startPos - 1);
        std::string varValue = std::to_string(VariableManager::Get(varName));
        content.replace(startPos, endPos - startPos + 1, varValue);
        startPos += varValue.length();
    }

    SDL_Color defaultText = { 255, 255, 255, 255 };
    SDL_Color defaultOutline = { 0, 0, 0, 255 };

    SDL_Color textColor = cmd.args.count("color") ? ScriptManager::ParseColor(cmd.args.at("color"), defaultText) : defaultText;

    SDL_Color outlineColor = cmd.args.count("olcolor") ? ScriptManager::ParseColor(cmd.args.at("olcolor"), defaultOutline) : defaultOutline;

    std::string textEffect;
    if (cmd.args.count("effect")) {
        textEffect = cmd.args.at("effect");
    }
    else if (cmd.args.count("texteffect")) {
        textEffect = cmd.args.at("texteffect");
    }
    else if (cmd.args.count("txfx")) {
        textEffect = cmd.args.at("txfx");
    }

    int textSpeed = 40;
    try {
        if (cmd.args.count("speed")) {
            textSpeed = std::stoi(cmd.args.at("speed"));
        }
        if (cmd.args.count("instant")) {
            const std::string& instantVal = cmd.args.at("instant");
            if (instantVal == "1" || instantVal == "true" || instantVal == "yes") {
                textSpeed = 0;
            }
        }
    } catch (...) {
        textSpeed = 40;
    }

    if (!skipNextLog) {
        BacklogManager::AddLog(speaker, content, pendingVoice);
    }
    skipNextLog = false;
    SetDialogueText(speaker, content, textSpeed, textColor, outlineColor, textEffect);

    currentSpeakingChar = cmd.args.count("char") ? cmd.args.at("char") : "";

    if (cmd.args.count("voice")) {
        pendingVoice = cmd.args.at("voice");
    }
    else {
        pendingVoice.clear();
    }
}

void DialogueController::HandleCommandBgm(const VNCommand& cmd) {
    std::string fileName = cmd.args.at("file");
    AudioManager::PlayBGM(fileName);
    currentBgmName = fileName;
    currentLine++;
}

void DialogueController::HandleCommandStopBgm(const VNCommand& cmd) {
    AudioManager::StopBGM();
    currentBgmName.clear();
    currentLine++;
}

void DialogueController::HandleCommandSe(const VNCommand& cmd) {
    AudioManager::PlaySFX(cmd.args.at("file"));
    currentLine++;
}

void DialogueController::HandleCommandSet(const VNCommand& cmd) {
    std::string varName = cmd.args.at("var");
    int value = 0;
    try {
        if (cmd.args.count("val")) value = std::stoi(cmd.args.at("val"));
    } catch (...) {
        std::cerr << "Failed to parse val parameters.\n";
    }

    VariableManager::Set(varName, value);
    currentLine++;
}

void DialogueController::HandleCommandAdd(const VNCommand& cmd) {
    std::string varName = cmd.args.at("var");
    int value = 0;
    try {
        if (cmd.args.count("val")) value = std::stoi(cmd.args.at("val"));
    } catch (...) {
        std::cerr << "Failed to parse val parameters.\n";
    }

    VariableManager::Add(varName, value);
    currentLine++;
}

void DialogueController::HandleCommandDel(const VNCommand& cmd) {
    if (cmd.args.count("var")) {
        VariableManager::Remove(cmd.args.at("var"));
    }
    currentLine++;
}

void DialogueController::HandleCommandBgmInfo(const VNCommand& cmd) {
    std::string text = cmd.args.count("text") ? cmd.args.at("text") : "";
    infoBanner.Show(text, true);
    currentLine++;
}

void DialogueController::HandleCommandChapter(const VNCommand& cmd) {
    std::string text = cmd.args.count("text") ? cmd.args.at("text") : "";
    chapterBanner.Show(text);
    currentLine++;
}

void DialogueController::HandleCommandLua(const VNCommand& cmd) {
    if (!luaState) {
        std::cerr << "Lua state unavailable for [" << cmd.type << "] command.\n";
        currentLine++;
        return;
    }

    std::string fnName;
    if (cmd.args.count("fn"))
        fnName = cmd.args.at("fn");
    else if (cmd.args.count("func"))
        fnName = cmd.args.at("func");
    else if (cmd.args.count("function"))
        fnName = cmd.args.at("function");

    if (fnName.empty()) {
        std::cerr << "Lua command missing fn/func/function parameter.\n";
        currentLine++;
        return;
    }

    sol::protected_function fn = (*luaState)[fnName];
    if (!fn.valid()) {
        std::cerr << "Lua callback not found: " << fnName << "\n";
        currentLine++;
        return;
    }

    sol::table args = luaState->create_table();
    for (const auto& [k, v] : cmd.args) {
        if (k == "fn" || k == "func" || k == "function") continue;
        args[k] = v;
    }

    sol::protected_function_result luaResult = fn(args);
    if (!luaResult.valid()) {
        sol::error err = luaResult;
        std::cerr << "Lua callback runtime error (" << fnName << "): " << err.what() << std::endl;
    }

    currentLine++;
}

void DialogueController::HandleCommandTransition(const VNCommand& cmd) {
    std::string transitionStyle = ReadTransitionStyleArg(cmd.args);
    std::string transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    std::string transitionEase = ReadTransitionEaseArg(cmd.args);
    currentLine++;
    QueueInlineTransition(transitionStyle, transitionSpeed, transitionEase);
}

void DialogueController::HandleCommandLabel(const VNCommand& cmd) { currentLine++; }

void DialogueController::HandleCommandJump(const VNCommand& cmd) {
    std::string target = cmd.args.at("target");
    std::string transitionStyle = ReadTransitionStyleArg(cmd.args);
    std::string transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    std::string transitionEase = ReadTransitionEaseArg(cmd.args);

    if (target.front() == '*') {
        std::string labelName = target.substr(1);
        for (size_t i = 0; i < commands.size(); ++i) {
            if (commands[i].type == "label" && commands[i].args["name"] == labelName) {
                currentLine = (int)i;
                break;
            }
        }
    }
    else {
        QueueScriptTransition(target, transitionStyle, transitionSpeed, transitionEase);
    }
}

void DialogueController::HandleCommandIf(const VNCommand& cmd) {
    std::string varName = cmd.args.at("var");
    std::string op = cmd.args.at("op");
    int val = 0;

    try {
        if (cmd.args.count("val")) val = std::stoi(cmd.args.at("val"));
    } catch (...) {
        std::cerr << "Failed to parse val parameters.\n";
    }

    if (VariableManager::Check(varName, op, val)) {
        currentLine++;
    }
    else {
        int depth = 0;
        bool found = false;
        while (++currentLine < (int)commands.size()) {
            if (commands[currentLine].type == "if")
                depth++;
            else if (commands[currentLine].type == "else" && depth == 0) {
                currentLine++;
                found = true;
                break;
            }
            else if (commands[currentLine].type == "endif") {
                if (depth == 0) {
                    currentLine++;
                    found = true;
                    break;
                }
                else
                    depth--;
            }
        }
        if (!found) {
            std::cerr << "Can't find [else] or [endif].\n";
        }
    }
}

void DialogueController::HandleCommandElse(const VNCommand& cmd) {
    int depth = 0;
    bool found = false;
    while (++currentLine < (int)commands.size()) {
        if (commands[currentLine].type == "if")
            depth++;
        else if (commands[currentLine].type == "endif") {
            if (depth == 0) {
                currentLine++;
                found = true;
                break;
            }
            else
                depth--;
        }
    }
    if (!found) {
        std::cerr << "Can't find [endif].\n";
    }
}

void DialogueController::HandleCommandEndIf(const VNCommand& cmd) { currentLine++; }

void DialogueController::HandleCommandChoice(const VNCommand& cmd) {
    std::string text = cmd.args.at("text");
    std::string target = cmd.args.at("target");
    std::string transitionStyle = ReadTransitionStyleArg(cmd.args);
    std::string transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    std::string transitionEase = ReadTransitionEaseArg(cmd.args);

    SDL_Color idleCol = { 255, 255, 255, 255 };
    SDL_Color hoverCol = { 255, 215, 0, 255 };

    UIManager::AddTextButton(text, uiFont, idleCol, hoverCol, target, transitionStyle, transitionSpeed, transitionEase);
    currentLine++;
}

void DialogueController::ExecuteNextCommands() {
    while (currentLine < (int)commands.size()) {
        const VNCommand& cmd = commands[currentLine];
        auto it = commandHandlers.find(cmd.type);

        if (it != commandHandlers.end()) {
            it->second(cmd);

            if (cmd.type == "text") {
                break;
            }
            else if (cmd.type == "transition") {
                return;
            }
            else if (cmd.type == "jump" && cmd.args.count("target") && cmd.args.at("target").front() != '*') {
                return;
            }
            else if (cmd.type == "choice" && currentLine < (int)commands.size() && commands[currentLine].type != "choice") {
                break;
            }
        }
        else {
            currentLine++;
        }
    }

    if (UIManager::HasButtons()) {
        UIManager::RecalculateLayout(1280, 720);
    }

    if (currentLine >= (int)commands.size()) {
        isFinished = true;
    }
}

void DialogueController::HandleClick(int mx, int my) {
    if (isFinished) return;
    if (clickCooldown > 0) return;
    clickCooldown = 1;

    if (UIManager::HasButtons()) {
        std::string target;
        std::string transitionStyle;
        std::string transitionSpeed;
        std::string transitionEase;
        bool hasClick = UIManager::CheckClick(mx, my, target, transitionStyle, transitionSpeed, transitionEase);

        if (hasClick && !target.empty()) {
            BacklogManager::AddChoice(UIManager::GetHoveredText());
            UIManager::Clear();

            if (target.front() == '*') {
                std::string labelName = target.substr(1);
                for (size_t i = 0; i < commands.size(); ++i) {
                    if (commands[i].type == "label" && commands[i].args["name"] == labelName) {
                        currentLine = (int)i;
                        ExecuteNextCommands();
                        return;
                    }
                }
            }
            else {
                QueueScriptTransition(target, transitionStyle, transitionSpeed, transitionEase);
                return;
            }
        }
        return;
    }

    AudioManager::StopVoice();
    if (!IsDialogueTextFinished()) {
        ShowDialogueTextAll();
    }
    else {
        currentLine++;
        ExecuteNextCommands();
    }
}

void DialogueController::Update(int mx, int my) {
    if (!hasStarted && !commands.empty()) {
        hasStarted = true;
        ExecuteNextCommands();
    }
    if (clickCooldown > 0) {
        clickCooldown--;
    }
    if (backlogCooldown > 0) {
        backlogCooldown--;
    }
    infoBanner.Update();
    chapterBanner.Update();
    {
        float target = isShowingBacklog ? 220.0f : 0.0f;
        const float BACKLOG_FADE_SPEED = 15.0f;
        TransitionUtils::MoveTowards(backlogFadeAlpha, target, BACKLOG_FADE_SPEED);
    }
    if (!isFinished) {
        UpdateDialogueText();
    }
    if (!pendingVoice.empty() && dialogueCurrentIndex > 0) {
        AudioManager::PlayVoice(pendingVoice);
        pendingVoice.clear();
    }

    if (UIManager::HasButtons()) {
        UIManager::UpdateHover(mx, my);
    }

    float fadeSpeed = 10.0f;
    float factor = 0.15f;

    if (currentBgTexture) {
        if (TransitionUtils::FadeIn(bgFadeAlpha, fadeSpeed) && previousBgTexture) {
            previousBgTexture = nullptr;
        }
    }

    for (auto i = activeCharacters.begin(); i != activeCharacters.end();) {
        ActiveCharacter& chara = i->second;

        float targetAlpha = chara.isExiting ? 0.0f : (currentSpeakingChar.empty() || i->first == currentSpeakingChar) ? 255.0f : 180.0f;
        EasingUtils::ExpDecay(chara.alpha, targetAlpha, 0.08f);

        if (!chara.isExiting) {
            EasingUtils::ExpDecay(chara.currentX, chara.targetX, factor);
        }

        UpdateCharacterAnimation(chara, luaState);

        if (chara.alpha <= 0.0f && chara.isExiting) {
            i = activeCharacters.erase(i);
        }
        else {
            ++i;
        }
    }
}

void DialogueController::RenderBackground() {
    if (previousBgTexture) {
        TextureManager::DrawAuto(previousBgTexture, renderer, TextureManager::DisplayMode::Fill, 255);
    }
    if (currentBgTexture) {
        TextureManager::DrawAuto(currentBgTexture, renderer, TextureManager::DisplayMode::Fill, static_cast<Uint8>(bgFadeAlpha));
    }
}

void DialogueController::RenderBacklog(SDL_Renderer* renderer) {
    if (backlogFadeAlpha <= 0.0f || BacklogManager::GetCount() == 0) return;

    Uint8 bgAlpha = (Uint8)backlogFadeAlpha;
    Uint8 textAlpha = (Uint8)(backlogFadeAlpha / 220.0f * 255.0f);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, bgAlpha);
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Rect bgRect = { 0, 0, w, h };
    SDL_RenderFillRect(renderer, &bgRect);

    SDL_Color outlineColor = { 0, 0, 0, textAlpha };
    TextManager::DrawWithOutline(renderer, uiFont, "- Backlog -", { 255, 255, 255, textAlpha }, outlineColor, 2, 50, 30, 0, textAlpha, true);
    TextManager::DrawWithOutline(renderer, uiFont, "(右鍵關閉)", { 150, 150, 150, textAlpha }, outlineColor, 1, 950, 40, 0, textAlpha, true);

    int startIdx = (int)BacklogManager::GetCount() - 1 - backlogOffset;
    int drawY = 600;

    for (int i = startIdx; i >= 0 && drawY > 100; --i) {
        const auto& log = BacklogManager::logs[i];

        if (log.isChoice) {
            TextManager::DrawWithOutline(renderer, uiFont, log.text, { 255, 215, 0, textAlpha }, outlineColor, 1, 200, drawY, 800, textAlpha, true);
        }
        else {
            if (!log.speaker.empty()) {
                TextManager::DrawWithOutline(renderer, uiFont, "【" + log.speaker + "】", { 255, 200, 100, textAlpha }, outlineColor, 1, 100, drawY, 0, textAlpha, true);
            }
            TextManager::DrawWithOutline(renderer, uiFont, log.text, { 220, 220, 220, textAlpha }, outlineColor, 1, 300, drawY, 800, textAlpha, true);
        }

        drawY -= 80;
    }
}

void DialogueController::Render(SDL_Renderer* renderer) {
    if (!activeCharacters.empty()) {
        std::vector<ActiveCharacter> sortedChars;
        for (auto const& [name, chara] : activeCharacters) {
            sortedChars.push_back(chara);
        }

        std::sort(sortedChars.begin(), sortedChars.end(), [](const ActiveCharacter& a, const ActiveCharacter& b) { return a.pos < b.pos; });

        for (const auto& chara : sortedChars) {
            std::string fileName = chara.name + "_" + chara.diff + ".png";
            SDL_Texture* tex = TextureManager::LoadTexture(fileName, renderer);

            if (tex) {
                int texW, texH;
                SDL_QueryTexture(tex, NULL, NULL, &texW, &texH);

                float targetHeight = 600.0f;
                float scale = (targetHeight / (float)texH) * chara.renderScale;
                int finalW = (int)(texW * scale);
                int finalH = (int)(texH * scale);

                int x = (int)(chara.currentX + chara.renderOffsetX) - (finalW / 2);
                int y = (kScreenHeight - finalH) + (int)chara.renderOffsetY;

                TextureManager::Draw(tex, renderer, x, y, finalW, finalH, (Uint8)chara.alpha);
            }
        }
    }

    UIManager::Render(renderer);

    infoBanner.Render(renderer, uiFont);
    chapterBanner.Render(renderer, uiFont);

    RenderBacklog(renderer);
}

bool DialogueController::IsScriptFinished() const { return isFinished; }