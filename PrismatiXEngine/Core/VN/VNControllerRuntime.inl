void VNController::HandleClick(int mx, int my) {
    if (isFinished) return;
    if (clickCooldown > 0) return;
    clickCooldown = 1;

    if (engine.GetUIManager().HasButtons()) {
        std::string target;
        std::string transitionStyle;
        std::string transitionSpeed;
        std::string transitionEase;
        bool hasClick = engine.GetUIManager().CheckClick(mx, my, target, transitionStyle, transitionSpeed, transitionEase);

        if (hasClick && !target.empty()) {
            engine.GetBacklogManager().AddChoice(engine.GetUIManager().GetHoveredText());
            engine.GetUIManager().Clear();

            if (target[0] == '*') {
                std::string labelName = target.substr(1);
                int labelLine = FindLabelLine(commands, labelName);
                if (labelLine >= 0) {
                    currentLine = labelLine;
                    ExecuteNextCommands();
                    return;
                }

                std::cerr << "Choice target label not found: " << labelName << "\n";
                ExecuteNextCommands();
                return;
            }
            else {
                QueueScriptTransition(target, transitionStyle, transitionSpeed, transitionEase);
                return;
            }
        }

        if (hasClick && target.empty()) {
            std::cerr << "Choice target is empty.\n";
        }
        return;
    }

    engine.GetAudioManager().StopVoice();
    if (!IsDialogueTextFinished()) {
        ShowDialogueTextAll();
    }
    else {
        currentLine++;
        ExecuteNextCommands();
    }
}

void VNController::Update(int mx, int my) {
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
    {
        float target = isShowingBacklog ? 220.0f : 0.0f;
        const float BACKLOG_FADE_SPEED = 15.0f;
        TransitionUtils::MoveTowards(backlogFadeAlpha, target, BACKLOG_FADE_SPEED);
    }
    if (!isFinished) {
        UpdateDialogueText();
    }
    if (!pendingVoice.empty() && dialogueCurrentIndex > 0) {
        engine.GetAudioManager().PlayVoice(pendingVoice);
        pendingVoice.clear();
    }

    if (engine.GetUIManager().HasButtons()) {
        engine.GetUIManager().UpdateHover(mx, my);
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

        UpdateCharacterAnimation(chara, engine.GetLuaState());

        if (chara.alpha <= 0.0f && chara.isExiting) {
            i = activeCharacters.erase(i);
        }
        else {
            ++i;
        }
    }
}

void VNController::RenderBackground() {
    if (previousBgTexture) {
        engine.GetTextureManager().DrawAuto(previousBgTexture, renderer, TextureManager::DisplayMode::Fill, 255);
    }
    if (currentBgTexture) {
        engine.GetTextureManager().DrawAuto(currentBgTexture, renderer, TextureManager::DisplayMode::Fill, static_cast<Uint8>(bgFadeAlpha));
    }
}

void VNController::RenderBacklog(SDL_Renderer* renderer) {
    if (backlogFadeAlpha <= 0.0f || engine.GetBacklogManager().GetCount() == 0) return;

    Uint8 bgAlpha = (Uint8)backlogFadeAlpha;
    Uint8 textAlpha = (Uint8)(backlogFadeAlpha / 220.0f * 255.0f);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, bgAlpha);
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Rect bgRect = { 0, 0, w, h };
    SDL_RenderFillRect(renderer, &bgRect);

    SDL_Color outlineColor = { 0, 0, 0, textAlpha };
    engine.GetTextManager().DrawWithOutline(renderer, uiFont, "- Backlog -", { 255, 255, 255, textAlpha }, outlineColor, 2, 50, 30, 0, textAlpha, true);
    engine.GetTextManager().DrawWithOutline(renderer, uiFont, "(右鍵關閉)", { 150, 150, 150, textAlpha }, outlineColor, 1, 950, 40, 0, textAlpha, true);

    int startIdx = (int)engine.GetBacklogManager().GetCount() - 1 - backlogOffset;
    int drawY = 600;

    for (int i = startIdx; i >= 0 && drawY > 100; --i) {
        const auto& log = engine.GetBacklogManager().logs[i];

        if (log.isChoice) {
            engine.GetTextManager().DrawWithOutline(renderer, uiFont, log.text, { 255, 215, 0, textAlpha }, outlineColor, 1, 200, drawY, 800, textAlpha, true);
        }
        else {
            if (!log.speaker.empty()) {
                engine.GetTextManager().DrawWithOutline(renderer, uiFont, "【" + log.speaker + "】", { 255, 200, 100, textAlpha }, outlineColor, 1, 100, drawY, 0, textAlpha, true);
            }
            engine.GetTextManager().DrawWithOutline(renderer, uiFont, log.text, { 220, 220, 220, textAlpha }, outlineColor, 1, 300, drawY, 800, textAlpha, true);
        }

        drawY -= 80;
    }
}

void VNController::Render(SDL_Renderer* renderer) {
    if (!activeCharacters.empty()) {
        std::vector<ActiveCharacter> sortedChars;
        for (auto const& [name, chara] : activeCharacters) {
            sortedChars.push_back(chara);
        }

        std::sort(sortedChars.begin(), sortedChars.end(), [](const ActiveCharacter& a, const ActiveCharacter& b) { return a.pos < b.pos; });

        for (const auto& chara : sortedChars) {
            std::string fileName = chara.name + "_" + chara.diff + ".png";
            SDL_Texture* tex = engine.GetTextureManager().LoadTexture(fileName, renderer);

            if (tex) {
                int texW, texH;
                SDL_QueryTexture(tex, NULL, NULL, &texW, &texH);

                float targetHeight = 600.0f;
                float scale = (targetHeight / (float)texH) * chara.renderScale;
                int finalW = (int)(texW * scale);
                int finalH = (int)(texH * scale);

                int x = (int)(chara.currentX + chara.renderOffsetX) - (finalW / 2);
                int y = (kScreenHeight - finalH) + (int)chara.renderOffsetY;

                engine.GetTextureManager().Draw(tex, renderer, x, y, finalW, finalH, (Uint8)chara.alpha);
            }
        }
    }

    engine.GetUIManager().Render(renderer);

    RenderBacklog(renderer);
}

bool VNController::IsScriptFinished() const { return isFinished; }