#include "DialogueController.h"
#include <iostream>
#include <algorithm>
#include "TextureManager.h"
#include "TextManager.h"
#include "AudioManager.h"
#include "ArchiveManager.h"
#include "VariableManager.h"
#include "UIManager.h"
#include "BacklogManager.h"
#include "PrismatiXEngine.h"
#include "Utils/TransitionUtils.h"
#include "Utils/EasingUtils.h"

DialogueController::DialogueController(DialogueBox* box, TTF_Font* font, SDL_Renderer* ren) {
    dialogueBox = box;
    uiFont = font;
    renderer = ren;
    currentLine = 0;
    clickCooldown = 0;
    isFinished = false;
    hasStarted = false;
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
bool DialogueController::ConsumePendingScriptTransition(std::string& outTargetScript) {
    if (!hasPendingScriptTransition) return false;
    outTargetScript = pendingScriptTarget;
    pendingScriptTarget.clear();
    hasPendingScriptTransition = false;
    return true;
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

        ac.currentX = 1280.0f / 2.0f;

        activeCharacters[ac.name] = ac;
    }

    RecalculateTargetPositions();


    //for (auto& [name, ac] : activeCharacters) {
    //    ac.currentX = ac.targetX;
    //}
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
    currentBgTexture = nullptr;
    activeCharacters.clear();
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
        if (!chara.isExiting)
            nonExiting.push_back(&chara);
    }
    std::sort(nonExiting.begin(), nonExiting.end(), [](const ActiveCharacter* a, const ActiveCharacter* b) {
        return a->pos < b->pos;
        });

    int total = (int)nonExiting.size();
    int screenWidth = 1280;
    int sectionWidth = screenWidth / (total + 1);
    for (int i = 0; i < total; i++) {
        nonExiting[i]->targetX = (float)(sectionWidth * (i + 1));
    }
}

void DialogueController::ExecuteNextCommands() {
    while (currentLine < (int)commands.size()) {
        VNCommand& cmd = commands[currentLine];

        if (cmd.type == "charimg") {
            std::string name = cmd.args["name"];

            if (activeCharacters.find(name) == activeCharacters.end()) {
                ActiveCharacter chara;
                chara.name = name;
                chara.diff = cmd.args["diff"];
                chara.pos = std::stoi(cmd.args["pos"]);
                chara.alpha = 0.0f;
                chara.targetAlpha = 255.0f;
                activeCharacters[name] = chara;
                RecalculateTargetPositions();
                activeCharacters[name].currentX = activeCharacters[name].targetX;
            }
            else {
                activeCharacters[name].diff = cmd.args["diff"];
                activeCharacters[name].pos = std::stoi(cmd.args["pos"]);
                activeCharacters[name].targetAlpha = 255.0f;
                activeCharacters[name].isExiting = false;
                RecalculateTargetPositions();
            }
            currentLine++;
        }
        else if (cmd.type == "clearcharimg") {
            int targetPos = std::stoi(cmd.args["pos"]);

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
        else if (cmd.type == "bg") {
            std::string fileName = cmd.args["file"];
            currentBgName = fileName;
            previousBgTexture = currentBgTexture;
            currentBgTexture = TextureManager::LoadTexture(fileName, renderer);
            bgFadeAlpha = 0.0f;
            currentLine++;
        }
        else if (cmd.type == "text") {
            std::string speaker = cmd.args.count("name") ? cmd.args["name"] : "";
            std::string content = cmd.args["content"];

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

            SDL_Color textColor = cmd.args.count("color") ?
                ScriptManager::ParseColor(cmd.args["color"], defaultText) : defaultText;

            SDL_Color outlineColor = cmd.args.count("olcolor") ?
                ScriptManager::ParseColor(cmd.args["olcolor"], defaultOutline) : defaultOutline;

            if (!skipNextLog) {
                BacklogManager::AddLog(speaker, content, pendingVoice);
            }
            skipNextLog = false;
            dialogueBox->SetText(speaker, content, 40, textColor, outlineColor);

            if (cmd.args.count("voice")) {
                pendingVoice = cmd.args["voice"];
            }
            else {
                pendingVoice.clear();
            }

            break;
        }
        else if (cmd.type == "bgm") {
            std::string fileName = cmd.args["file"];
            AudioManager::PlayBGM(fileName);
            currentBgmName = fileName;
            currentLine++;
        }
        else if (cmd.type == "stopbgm") {
            AudioManager::StopBGM();
			currentBgmName.clear();
            currentLine++;
        }
        else if (cmd.type == "se") {
            AudioManager::PlaySFX(cmd.args["file"]);
            currentLine++;
        }
        else if (cmd.type == "set") {
            std::string varName = cmd.args["var"];
            int value = 0;
            try { if (cmd.args.count("val")) value = std::stoi(cmd.args["val"]); }
            catch (...) { std::cerr << "Failed to parse val parameters.\n"; }

            VariableManager::Set(varName, value);
            currentLine++;
        }
        else if (cmd.type == "add") {
            std::string varName = cmd.args["var"];
            int value = 0;
            try { if (cmd.args.count("val")) value = std::stoi(cmd.args["val"]); }
            catch (...) { std::cerr << "Failed to parse val parameters.\n"; }

            VariableManager::Add(varName, value);
            currentLine++;
        }
        else if (cmd.type == "del") {
            if (cmd.args.count("var")) {
                VariableManager::Remove(cmd.args["var"]);
            }
            currentLine++;
        }
        else if (cmd.type == "bgminfo") {
            std::string text = cmd.args.count("text") ? cmd.args["text"] : "";
            infoBanner.Show(text, true);
            currentLine++;
        }
        else if (cmd.type == "label") {
            currentLine++;
        }
        else if (cmd.type == "jump") {
            std::string target = cmd.args["target"];

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
                BacklogManager::Clear();
                pendingScriptTarget = target;
                hasPendingScriptTransition = true;
                return;
            }
        }
        else if (cmd.type == "if") {
            std::string varName = cmd.args["var"];
            std::string op = cmd.args["op"];
            int val = 0;

            try { if (cmd.args.count("val")) val = std::stoi(cmd.args["val"]); }
            catch (...) { std::cerr << "Failed to parse val parameters.\n"; }

            if (VariableManager::Check(varName, op, val)) {
                currentLine++;
            }
            else {
                int depth = 0;
                bool found = false;
                while (++currentLine < (int)commands.size()) {
                    if (commands[currentLine].type == "if") depth++;
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
                        else depth--;
                    }
                }
                if (!found) {
                    std::cerr << "Can't find [else] or [endif].\n";
                }
            }
        }
        else if (cmd.type == "else") {
            int depth = 0;
            bool found = false;
            while (++currentLine < (int)commands.size()) {
                if (commands[currentLine].type == "if") depth++;
                else if (commands[currentLine].type == "endif") {
                    if (depth == 0) {
                        currentLine++;
                        found = true;
                        break;
                    }
                    else depth--;
                }
            }
            if (!found) {
                std::cerr << "Can't find [endif].\n";
            }
        }
        else if (cmd.type == "endif") {
            currentLine++;
        }
        else if (cmd.type == "choice") {
            std::string text = cmd.args["text"];
            std::string target = cmd.args["target"];

            SDL_Color idleCol = { 255, 255, 255, 255 };
            SDL_Color hoverCol = { 255, 215, 0, 255 };

            UIManager::AddTextButton(text, uiFont, idleCol, hoverCol, target);
            currentLine++;

            if (currentLine < (int)commands.size() && commands[currentLine].type != "choice") {
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
        std::string target = UIManager::CheckClick(mx, my);

        if (!target.empty()) {
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
                BacklogManager::Clear();
                pendingScriptTarget = target;
                hasPendingScriptTransition = true;
                return;
            }
        }
        return;
    }

    AudioManager::StopVoice();
    if (!dialogueBox->IsFinished()) {
        dialogueBox->ShowAll();
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
    {
        float target = isShowingBacklog ? 220.0f : 0.0f;
        const float BACKLOG_FADE_SPEED = 15.0f;
        TransitionUtils::MoveTowards(backlogFadeAlpha, target, BACKLOG_FADE_SPEED);
    }
    if (!isFinished) {
        dialogueBox->Update();
    }
    if (!pendingVoice.empty() && dialogueBox->GetCurrentIndex() > 0) {
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

    for (auto i = activeCharacters.begin(); i != activeCharacters.end(); ) {
        ActiveCharacter& chara = i->second;

        TransitionUtils::MoveTowards(chara.alpha, chara.targetAlpha, fadeSpeed);

        if (!chara.isExiting) {
            EasingUtils::ExpDecay(chara.currentX, chara.targetX, factor);
        }

        if (chara.alpha <= 0.0f && chara.isExiting) {
            i = activeCharacters.erase(i);
        }
        else {
            ++i;
        }
    }
}

void DialogueController::RenderBackground(PrismatiXEngine& engine) {
    if (previousBgTexture) {
        engine.DrawFullscreenBackground(previousBgTexture, 255);
    }
    if (currentBgTexture) {
        engine.DrawFullscreenBackground(currentBgTexture, (Uint8)bgFadeAlpha);
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
    TextManager::DrawWithOutline(renderer, uiFont, "- Backlog -", { 255, 255, 255, textAlpha }, outlineColor, 2, 50, 30, 0, textAlpha);
    TextManager::DrawWithOutline(renderer, uiFont, "(右鍵關閉)", { 150, 150, 150, textAlpha }, outlineColor, 1, 950, 40, 0, textAlpha);

    int startIdx = (int)BacklogManager::GetCount() - 1 - backlogOffset;
    int drawY = 600;

    for (int i = startIdx; i >= 0 && drawY > 100; --i) {
        const auto& log = BacklogManager::logs[i];

        if (log.isChoice) {
            TextManager::DrawWithOutline(renderer, uiFont, log.text, { 255, 215, 0, textAlpha }, outlineColor, 1, 200, drawY, 800, textAlpha);
        }
        else {
            if (!log.speaker.empty()) {
                TextManager::DrawWithOutline(renderer, uiFont, "【" + log.speaker + "】", { 255, 200, 100, textAlpha }, outlineColor, 1, 100, drawY, 0, textAlpha);
            }
            TextManager::DrawWithOutline(renderer, uiFont, log.text, { 220, 220, 220, textAlpha }, outlineColor, 1, 300, drawY, 800, textAlpha);
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

        std::sort(sortedChars.begin(), sortedChars.end(), [](const ActiveCharacter& a, const ActiveCharacter& b) {
            return a.pos < b.pos;
            });

        for (const auto& chara : sortedChars) {
            std::string fileName = chara.name + "_" + chara.diff + ".png";
            SDL_Texture* tex = TextureManager::LoadTexture(fileName, renderer);

            if (tex) {
                int texW, texH;
                SDL_QueryTexture(tex, NULL, NULL, &texW, &texH);

                float targetHeight = 600.0f;
                float scale = targetHeight / (float)texH;
                int finalW = (int)(texW * scale);
                int finalH = (int)(texH * scale);

                int x = (int)chara.currentX - (finalW / 2);
                int y = 720 - finalH;

                TextureManager::Draw(tex, renderer, x, y, finalW, finalH, (Uint8)chara.alpha);
            }
        }
    }

    if (!isFinished) {
        dialogueBox->Render(renderer);
    }
    UIManager::Render(renderer);

    infoBanner.Render(renderer, uiFont);

    RenderBacklog(renderer);
}

bool DialogueController::IsScriptFinished() const { return isFinished; }