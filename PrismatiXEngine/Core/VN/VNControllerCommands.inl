void VNController::HandleCommandCharImg(const VNCommand& cmd) {
    std::string name = ReadCharacterIdArg(cmd.args);
    std::string expression = ReadCharacterExpressionArg(cmd.args);
    int targetPos = ReadCharacterSlotArg(cmd.args, 1);
    std::string speakerName = ReadSpeakerArg(cmd.args);
    bool focus = ReadBoolArg(cmd.args, { "focus", "speaking", "active" }, false);

    if (name.empty() || expression.empty()) {
        std::cerr << "Char command missing id/expression.\n";
        currentLine++;
        return;
    }

    if (activeCharacters.find(name) == activeCharacters.end()) {
        ActiveCharacter chara;
        chara.name = name;
        chara.speakerName = speakerName.empty() ? name : speakerName;
        chara.diff = expression;
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
        if (!speakerName.empty()) chara.speakerName = speakerName;
        chara.diff = expression;
        chara.pos = targetPos;
        chara.targetAlpha = 255.0f;
        chara.isExiting = false;
        RecalculateTargetPositions();
        chara.animationTrigger = (previousPos != targetPos) ? "move" : "refresh";
        StartCharacterAnimation(chara, cmd.args);
    }

    if (focus) currentSpeakingChar = name;
    characterSortDirty = true;
    currentLine++;
}

void VNController::HandleCommandClearCharImg(const VNCommand& cmd) {
    std::string targetName = ReadCharacterIdArg(cmd.args);
    if (!targetName.empty()) {
        auto it = activeCharacters.find(targetName);
        if (it != activeCharacters.end()) {
            it->second.targetAlpha = 0.0f;
            it->second.isExiting = true;
        }
    } else {
        int targetPos = ReadCharacterSlotArg(cmd.args, -9999);
        for (auto& [name, chara] : activeCharacters) {
            if (chara.pos == targetPos) { chara.targetAlpha = 0.0f; chara.isExiting = true; break; }
        }
    }
    RecalculateTargetPositions();
    characterSortDirty = true;
    currentLine++;
}

void VNController::HandleCommandBg(const VNCommand& cmd) {
    std::string fileName = cmd.args.at("file");
    currentBgName = fileName;
    previousBgTexture = currentBgTexture;
    currentBgTexture = engine.GetAssetManager().LoadTexture(fileName, renderer);
    bgFadeAlpha = 0.0f;
    currentLine++;
}

void VNController::HandleCommandText(const VNCommand& cmd) {
    std::string content = ReadTextArg(cmd.args);
    if (content.empty()) { currentLine++; return; }
    std::string speakingChar = ReadCharacterIdArg(cmd.args);
    if (!speakingChar.empty()) currentSpeakingChar = speakingChar;
    std::string speaker = ReadSpeakerArg(cmd.args);
    if (speaker.empty() && !currentSpeakingChar.empty()) {
        auto it = activeCharacters.find(currentSpeakingChar);
        if (it != activeCharacters.end()) speaker = it->second.speakerName.empty() ? it->second.name : it->second.speakerName;
    }
    size_t startPos = 0;
    while ((startPos = content.find('{', startPos)) != std::string::npos) {
        size_t endPos = content.find('}', startPos);
        if (endPos == std::string::npos) break;
        std::string varName = content.substr(startPos + 1, endPos - startPos - 1);
        std::string varValue = std::to_string(engine.GetVariableManager().Get(varName));
        content.replace(startPos, endPos - startPos + 1, varValue);
        startPos += varValue.length();
    }
    SDL_Color defaultText = { 255, 255, 255, 255 }, defaultOutline = { 0, 0, 0, 255 };
    SDL_Color textColor = cmd.args.count("color") ? engine.GetScriptManager().ParseColor(cmd.args.at("color"), defaultText) : defaultText;
    std::string outline = ReadFirstArg(cmd.args, { "olcolor" });
    SDL_Color outlineColor = !outline.empty() ? engine.GetScriptManager().ParseColor(outline, defaultOutline) : defaultOutline;
    std::string textEffect = cmd.args.count("fx") ? cmd.args.at("fx") : "";
    int textSpeed = 40;
    try {
        if (cmd.args.count("speed")) textSpeed = std::stoi(cmd.args.at("speed"));
        if (cmd.args.count("instant") && (cmd.args.at("instant") == "true" || cmd.args.at("instant") == "1")) textSpeed = 0;
    } catch (...) {}
    std::string lineVoice = cmd.args.count("voice") ? cmd.args.at("voice") : "";
    if (!skipNextLog) engine.GetBacklogManager().AddLog(speaker, content, lineVoice);
    skipNextLog = false;
    SetDialogueText(speaker, content, textSpeed, textColor, outlineColor, textEffect);
    pendingVoice = lineVoice;
}

void VNController::HandleCommandBgm(const VNCommand& cmd) {
    engine.GetAudioSystem().PlayBGM(cmd.args.at("file"));
    currentBgmName = cmd.args.at("file");
    currentLine++;
}

void VNController::HandleCommandStopBgm(const VNCommand& cmd) {
    engine.GetAudioSystem().StopBGM();
    currentBgmName.clear();
    currentLine++;
}

void VNController::HandleCommandSe(const VNCommand& cmd) {
    engine.GetAudioSystem().PlaySFX(cmd.args.at("file"));
    currentLine++;
}

void VNController::HandleCommandVar(const VNCommand& cmd) {
    std::string varName = ReadFirstArg(cmd.args, { "var" });
    if (!varName.empty()) {
        std::string op = ToLowerAlphaNumeric(ReadVarOperatorArg(cmd.args));
        int val = cmd.args.count("val") ? std::stoi(cmd.args.at("val")) : 0;
        if (op == "set") engine.GetVariableManager().Set(varName, val);
        else if (op == "add") engine.GetVariableManager().Add(varName, val);
        else if (op == "sub") engine.GetVariableManager().Add(varName, -val);
    }
    currentLine++;
}

void VNController::HandleCommandBgmInfo(const VNCommand& cmd) {
    pendingBgmInfo = cmd.args.count("text") ? cmd.args.at("text") : "";
    currentLine++;
}

void VNController::HandleCommandChapter(const VNCommand& cmd) {
    pendingChapterInfo = cmd.args.count("text") ? cmd.args.at("text") : "";
    currentLine++;
}

void VNController::HandleCommandLua(const VNCommand& cmd) {
    std::string fnName = ReadFirstArg(cmd.args, {"fn", "func", "function"});
    if (!fnName.empty()) {
        sol::protected_function fn = engine.GetLuaState()[fnName];
        if (fn.valid()) {
            sol::table args = engine.GetLuaState().create_table();
            for (const auto& [k, v] : cmd.args) args[k] = v;
            fn(args);
        }
    }
    currentLine++;
}

void VNController::HandleCommandTransition(const VNCommand& cmd) {
    QueueInlineTransition(ReadTransitionStyleArg(cmd.args), ReadTransitionSpeedArg(cmd.args), ReadTransitionEaseArg(cmd.args));
    currentLine++;
}

void VNController::HandleCommandLabel(const VNCommand& cmd) { currentLine++; }

void VNController::HandleCommandJump(const VNCommand& cmd) {
    std::string target = cmd.args.at("target");
    if (target[0] == '*') {
        int line = FindLabelLine(commands, target.substr(1));
        if (line >= 0) currentLine = line;
        else currentLine++;
    } else {
        QueueScriptTransition(target, ReadTransitionStyleArg(cmd.args), ReadTransitionSpeedArg(cmd.args), ReadTransitionEaseArg(cmd.args));
    }
}

void VNController::HandleCommandIf(const VNCommand& cmd) {
    if (engine.GetVariableManager().Check(cmd.args.at("var"), cmd.args.at("op"), std::stoi(cmd.args.at("val")))) {
        currentLine++;
    } else {
        int depth = 0;
        while (++currentLine < (int)commands.size()) {
            if (commands[currentLine].type == "if") depth++;
            else if (commands[currentLine].type == "else" && depth == 0) { currentLine++; break; }
            else if (commands[currentLine].type == "endif") { if (depth == 0) { currentLine++; break; } else depth--; }
        }
    }
}

void VNController::HandleCommandElse(const VNCommand& cmd) {
    int depth = 0;
    while (++currentLine < (int)commands.size()) {
        if (commands[currentLine].type == "if") depth++;
        else if (commands[currentLine].type == "endif") { if (depth == 0) { currentLine++; break; } else depth--; }
    }
}

void VNController::HandleCommandEndIf(const VNCommand& cmd) { currentLine++; }

void VNController::HandleCommandChoice(const VNCommand& cmd) {
    PendingChoice pc;
    pc.text = cmd.args.at("text");
    pc.target = cmd.args.at("target");
    pc.transitionStyle = ReadTransitionStyleArg(cmd.args);
    pc.transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    pc.transitionEase = ReadTransitionEaseArg(cmd.args);
    pendingChoices.push_back(pc);
    currentLine++;
}

void VNController::ExecuteNextCommands() {
    while (currentLine < (int)commands.size()) {
        const VNCommand& cmd = commands[currentLine];
        auto it = commandHandlers.find(cmd.type);
        if (it != commandHandlers.end()) {
            it->second(cmd);
            if (cmd.type == "text") break;
            if (cmd.type == "transition") return;
            if (cmd.type == "jump" && cmd.args.count("target") && !cmd.args.at("target").empty() && cmd.args.at("target")[0] != '*') return;
            if (cmd.type == "choice") {
                if (currentLine < (int)commands.size() && commands[currentLine].type != "choice") break; 
            }
        } else {
            currentLine++;
        }
    }
    if (currentLine >= (int)commands.size()) isFinished = true;
}
