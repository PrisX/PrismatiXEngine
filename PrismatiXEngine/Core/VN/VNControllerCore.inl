VNController::VNController(PrismatiXEngine& eng, TTF_Font* dialogueFont, const std::string& dialogueFontName, int dialogueFontSize, TTF_Font* nameFont) : engine(eng) {
    (void)dialogueFontName;
    (void)dialogueFontSize;
    (void)nameFont;
    uiFont = dialogueFont;
    renderer = engine.GetRenderer();
    currentLine = 0;
    clickCooldown = 0;
    isFinished = false;
    hasStarted = false;
    pendingBgmInfo = "";
    pendingChapterInfo = "";
    InitializeCommandHandlers();
}

bool VNController::PopPendingBgmInfo(std::string& outMsg) {
    if (pendingBgmInfo.empty()) return false;
    outMsg = pendingBgmInfo;
    pendingBgmInfo = "";
    return true;
}

bool VNController::PopPendingChapterInfo(std::string& outMsg) {
    if (pendingChapterInfo.empty()) return false;
    outMsg = pendingChapterInfo;
    pendingChapterInfo = "";
    return true;
}

void VNController::InitializeCommandHandlers() {
    commandHandlers["char"] = [this](const VNCommand& cmd) { HandleCommandCharImg(cmd); };
    commandHandlers["char_clear"] = [this](const VNCommand& cmd) { HandleCommandClearCharImg(cmd); };
    commandHandlers["bg"] = [this](const VNCommand& cmd) { HandleCommandBg(cmd); };
    commandHandlers["text"] = [this](const VNCommand& cmd) { HandleCommandText(cmd); };
    commandHandlers["bgm"] = [this](const VNCommand& cmd) { HandleCommandBgm(cmd); };
    commandHandlers["stopbgm"] = [this](const VNCommand& cmd) { HandleCommandStopBgm(cmd); };
    commandHandlers["se"] = [this](const VNCommand& cmd) { HandleCommandSe(cmd); };
    commandHandlers["var"] = [this](const VNCommand& cmd) { HandleCommandVar(cmd); };
    commandHandlers["bgminfo"] = [this](const VNCommand& cmd) { HandleCommandBgmInfo(cmd); };
    commandHandlers["chapter"] = [this](const VNCommand& cmd) { HandleCommandChapter(cmd); };
    commandHandlers["lua"] = [this](const VNCommand& cmd) { HandleCommandLua(cmd); };
    commandHandlers["transition"] = [this](const VNCommand& cmd) { HandleCommandTransition(cmd); };
    commandHandlers["label"] = [this](const VNCommand& cmd) { HandleCommandLabel(cmd); };
    commandHandlers["jump"] = [this](const VNCommand& cmd) { HandleCommandJump(cmd); };
    commandHandlers["if"] = [this](const VNCommand& cmd) { HandleCommandIf(cmd); };
    commandHandlers["else"] = [this](const VNCommand& cmd) { HandleCommandElse(cmd); };
    commandHandlers["endif"] = [this](const VNCommand& cmd) { HandleCommandEndIf(cmd); };
    commandHandlers["choice"] = [this](const VNCommand& cmd) { HandleCommandChoice(cmd); };
}

void VNController::ParseDialogueUTF8(const std::string& text) { dialogueParsedCharacters = SplitUtf8Chars(text); }

void VNController::SetDialogueText(const std::string& speaker, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor, const std::string& textEffect) {
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

void VNController::UpdateDialogueText() {
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

void VNController::ShowDialogueTextAll() {
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

bool VNController::IsDialogueTextFinished() const { return dialogueCurrentIndex >= static_cast<int>(dialogueParsedCharacters.size()); }

sol::table VNController::GetDialogueBoxContext(sol::this_state state, int screenW, int screenH) const {
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
std::string VNController::GetCurrentScriptName() const { return currentScriptName; }
int VNController::GetCurrentLine() const {
    if (engine.GetUIManager().HasButtons()) {
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
std::string VNController::GetCurrentBgName() const { return currentBgName; }
std::string VNController::GetCurrentBgmName() const { return currentBgmName; }
void VNController::SetCurrentLine(int line) { currentLine = line; }
void VNController::SetSkipNextLog(bool skip) { skipNextLog = skip; }
bool VNController::PopScriptTransition(std::string& outTargetScript, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) {
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

bool VNController::PopInlineTransition(std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) {
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

void VNController::QueueScriptTransition(const std::string& targetScript, const std::string& transitionStyle, const std::string& transitionSpeed, const std::string& transitionEase) {
    if (targetScript.empty()) return;

    engine.GetBacklogManager().Clear();
    pendingScriptTarget = targetScript;
    pendingTransitionStyle = transitionStyle;
    pendingTransitionSpeed = transitionSpeed;
    pendingTransitionEase = transitionEase;
    hasPendingScriptTransition = true;
}

void VNController::QueueInlineTransition(const std::string& transitionStyle, const std::string& transitionSpeed, const std::string& transitionEase) {
    hasPendingInlineTransition = true;
    pendingInlineTransitionStyle = transitionStyle;
    pendingInlineTransitionSpeed = transitionSpeed;
    pendingInlineTransitionEase = transitionEase;
}

void VNController::ContinueScript() {
    if (isFinished) return;
    ExecuteNextCommands();
}

std::vector<SavedCharacter> VNController::GetSavedCharacters() const {
    std::vector<SavedCharacter> chars;
    for (const auto& [name, chara] : activeCharacters) {
        if (!chara.isExiting) {
            chars.push_back({ chara.name, chara.diff, chara.pos });
        }
    }
    return chars;
}

void VNController::RestoreSavedCharacters(const std::vector<SavedCharacter>& savedChars) {
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

void VNController::RestoreBackground(const std::string& bgName) {
    currentBgName = bgName;
    if (!bgName.empty()) {
        currentBgTexture = engine.GetAssetManager().LoadTexture(bgName, renderer);
        bgFadeAlpha = 0.0f;
        previousBgTexture = nullptr;
    }
    else {
        currentBgTexture = nullptr;
    }
}

SDL_Texture* VNController::GetBackground() const { return currentBgTexture; }

void VNController::LoadScript(const std::string& scriptName, const std::vector<VNCommand>& newScript) {
    engine.GetUIManager().Clear();
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

bool VNController::IsShowingBacklog() const { return isShowingBacklog; }

void VNController::ToggleBacklog() {
    if (backlogCooldown > 0) return;

    isShowingBacklog = !isShowingBacklog;
    if (isShowingBacklog) {
        backlogOffset = 0;
    }

    backlogCooldown = 15;
}

void VNController::ScrollBacklog(int direction) {
    if (!isShowingBacklog) return;

    backlogOffset += direction;

    if (backlogOffset < 0) backlogOffset = 0;

    int maxOffset = std::max(0, (int)engine.GetBacklogManager().GetCount() - 1);
    if (backlogOffset > maxOffset) backlogOffset = maxOffset;
}

sol::table VNController::GetChoices(sol::this_state state) const {
    sol::state_view lua(state);
    sol::table list = lua.create_table();
    for (size_t i = 0; i < pendingChoices.size(); ++i) {
        sol::table item = lua.create_table();
        item["text"] = pendingChoices[i].text;
        item["index"] = (int)i + 1;
        list[i + 1] = item;
    }
    return list;
}

void VNController::SelectChoice(int index) {
    int idx = index - 1; // Lua 1-based to C++ 0-based
    if (idx < 0 || idx >= (int)pendingChoices.size()) return;

    auto choice = pendingChoices[idx];
    engine.GetBacklogManager().AddChoice(choice.text);
    pendingChoices.clear();

    if (!choice.target.empty()) {
        if (choice.target[0] == '*') {
            std::string labelName = choice.target.substr(1);
            int labelLine = FindLabelLine(commands, labelName);
            if (labelLine >= 0) {
                currentLine = labelLine;
                ExecuteNextCommands();
            }
        } else {
            QueueScriptTransition(choice.target, choice.transitionStyle, choice.transitionSpeed, choice.transitionEase);
        }
    }
}

void VNController::RecalculateTargetPositions() {
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
    characterSortDirty = true;
}
