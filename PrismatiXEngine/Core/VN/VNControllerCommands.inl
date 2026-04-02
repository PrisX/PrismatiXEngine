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
        if (!speakerName.empty()) {
            chara.speakerName = speakerName;
        }
        chara.diff = expression;
        chara.pos = targetPos;
        chara.targetAlpha = 255.0f;
        chara.isExiting = false;
        RecalculateTargetPositions();
        chara.animationTrigger = (previousPos != targetPos) ? "move" : "refresh";
        StartCharacterAnimation(chara, cmd.args);
    }

    if (focus) {
        currentSpeakingChar = name;
    }

    currentLine++;
}

void VNController::HandleCommandClearCharImg(const VNCommand& cmd) {
    std::string targetName = ReadCharacterIdArg(cmd.args);
    bool cleared = false;

    if (!targetName.empty()) {
        auto it = activeCharacters.find(targetName);
        if (it != activeCharacters.end()) {
            it->second.targetAlpha = 0.0f;
            it->second.isExiting = true;
            cleared = true;
        }
    }
    else {
        int targetPos = ReadCharacterSlotArg(cmd.args, -9999);
        for (auto& [name, chara] : activeCharacters) {
            if (chara.pos == targetPos) {
                chara.targetAlpha = 0.0f;
                chara.isExiting = true;
                cleared = true;
                break;
            }
        }
    }

    if (!cleared) {
        std::cerr << "Char clear target not found.\n";
    }

    RecalculateTargetPositions();
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
    bool hasText = (cmd.args.count("content") > 0);
    std::string content = ReadTextArg(cmd.args);
    if (!hasText) {
        std::cerr << "Text command missing content.\n";
        currentLine++;
        return;
    }

    std::string speakingChar = ReadCharacterIdArg(cmd.args);
    if (!speakingChar.empty()) {
        currentSpeakingChar = speakingChar;
    }

    std::string speaker = ReadSpeakerArg(cmd.args);
    if (speaker.empty() && !currentSpeakingChar.empty()) {
        auto it = activeCharacters.find(currentSpeakingChar);
        if (it != activeCharacters.end()) {
            speaker = it->second.speakerName.empty() ? it->second.name : it->second.speakerName;
        }
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

    SDL_Color defaultText = { 255, 255, 255, 255 };
    SDL_Color defaultOutline = { 0, 0, 0, 255 };

    SDL_Color textColor = cmd.args.count("color") ? engine.GetScriptManager().ParseColor(cmd.args.at("color"), defaultText) : defaultText;

    std::string outline = ReadFirstArg(cmd.args, { "olcolor" });
    SDL_Color outlineColor = !outline.empty() ? engine.GetScriptManager().ParseColor(outline, defaultOutline) : defaultOutline;

    std::string textEffect;
    if (cmd.args.count("fx")) {
        textEffect = cmd.args.at("fx");
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

    std::string lineVoice;
    if (cmd.args.count("voice")) {
        lineVoice = cmd.args.at("voice");
    }

    if (!skipNextLog) {
        engine.GetBacklogManager().AddLog(speaker, content, lineVoice);
    }
    skipNextLog = false;
    SetDialogueText(speaker, content, textSpeed, textColor, outlineColor, textEffect);

    if (!speakingChar.empty()) {
        currentSpeakingChar = speakingChar;
    }
    pendingVoice = lineVoice;
}

void VNController::HandleCommandBgm(const VNCommand& cmd) {
    std::string fileName = cmd.args.at("file");
    engine.GetAudioSystem().PlayBGM(fileName);
    currentBgmName = fileName;
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
    if (varName.empty()) {
        std::cerr << "Var command missing var/name.\n";
        currentLine++;
        return;
    }

    std::string op = ToLowerAlphaNumeric(ReadVarOperatorArg(cmd.args));
    int value = 0;
    try {
        if (cmd.args.count("val")) {
            value = std::stoi(cmd.args.at("val"));
        }
    } catch (...) {
        std::cerr << "Failed to parse var val.\n";
    }

    if (op.empty()) {
        if (cmd.args.count("val")) {
            op = "set";
        }
        else {
            std::cerr << "Var command missing op.\n";
            currentLine++;
            return;
        }
    }

    if (op == "set") {
        engine.GetVariableManager().Set(varName, value);
    }
    else if (op == "add") {
        engine.GetVariableManager().Add(varName, value);
    }
    else if (op == "sub") {
        engine.GetVariableManager().Add(varName, -value);
    }
    else if (op == "del" || op == "remove" || op == "clear") {
        engine.GetVariableManager().Remove(varName);
    }
    else {
        std::cerr << "Unknown var op: " << op << "\n";
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

    sol::protected_function fn = engine.GetLuaState()[fnName];
    if (!fn.valid()) {
        std::cerr << "Lua callback not found: " << fnName << "\n";
        currentLine++;
        return;
    }

    sol::table args = engine.GetLuaState().create_table();
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

void VNController::HandleCommandTransition(const VNCommand& cmd) {
    std::string transitionStyle = ReadTransitionStyleArg(cmd.args);
    std::string transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    std::string transitionEase = ReadTransitionEaseArg(cmd.args);
    currentLine++;
    QueueInlineTransition(transitionStyle, transitionSpeed, transitionEase);
}

void VNController::HandleCommandLabel(const VNCommand& cmd) { currentLine++; }

void VNController::HandleCommandJump(const VNCommand& cmd) {
    if (cmd.args.count("target") == 0) {
        std::cerr << "Jump command missing target.\n";
        currentLine++;
        return;
    }

    std::string target = cmd.args.at("target");
    if (target.empty()) {
        std::cerr << "Jump command has empty target.\n";
        currentLine++;
        return;
    }

    std::string transitionStyle = ReadTransitionStyleArg(cmd.args);
    std::string transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    std::string transitionEase = ReadTransitionEaseArg(cmd.args);

    if (target[0] == '*') {
        std::string labelName = target.substr(1);
        int labelLine = FindLabelLine(commands, labelName);
        if (labelLine >= 0) {
            currentLine = labelLine;
        }
        else {
            std::cerr << "Jump target label not found: " << labelName << "\n";
            currentLine++;
        }
    }
    else {
        QueueScriptTransition(target, transitionStyle, transitionSpeed, transitionEase);
    }
}

void VNController::HandleCommandIf(const VNCommand& cmd) {
    std::string varName = cmd.args.at("var");
    std::string op = cmd.args.at("op");
    int val = 0;

    try {
        if (cmd.args.count("val")) val = std::stoi(cmd.args.at("val"));
    } catch (...) {
        std::cerr << "Failed to parse val parameters.\n";
    }

    if (engine.GetVariableManager().Check(varName, op, val)) {
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

void VNController::HandleCommandElse(const VNCommand& cmd) {
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

void VNController::HandleCommandEndIf(const VNCommand& cmd) { currentLine++; }

void VNController::HandleCommandChoice(const VNCommand& cmd) {
    std::string text = cmd.args.at("text");
    std::string target = cmd.args.at("target");
    if (target.empty()) {
        std::cerr << "Choice command has empty target.\n";
        currentLine++;
        return;
    }

    std::string transitionStyle = ReadTransitionStyleArg(cmd.args);
    std::string transitionSpeed = ReadTransitionSpeedArg(cmd.args);
    std::string transitionEase = ReadTransitionEaseArg(cmd.args);

    SDL_Color idleCol = { 255, 255, 255, 255 };
    SDL_Color hoverCol = { 255, 215, 0, 255 };

    engine.GetUIManager().AddTextButton(text, uiFont, idleCol, hoverCol, target, transitionStyle, transitionSpeed, transitionEase);
    currentLine++;
}

void VNController::ExecuteNextCommands() {
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
            else if (cmd.type == "jump" && cmd.args.count("target") && !cmd.args.at("target").empty() && cmd.args.at("target")[0] != '*') {
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

    if (engine.GetUIManager().HasButtons()) {
        engine.GetUIManager().RecalculateLayout(1280, 720);
    }

    if (currentLine >= (int)commands.size()) {
        isFinished = true;
    }
}