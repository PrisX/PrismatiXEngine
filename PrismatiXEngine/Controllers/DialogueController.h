#pragma once
#include <vector>
#include <string>
#include <iostream>
#include <map>
#include <algorithm>
#include "DialogueBox.h"
#include "ScriptManager.h"
#include "TextureManager.h"
#include "AudioManager.h"
#include "ArchiveManager.h"
#include "VariableManager.h"
#include "UIManager.h"
#include "BacklogManager.h"

struct ActiveCharacter {
    std::string name;
    std::string diff;
    int pos;

    float alpha = 0.0f; 
    float targetAlpha = 255.0f;
    bool isExiting = false;

    float currentX = 0.0f;
    float targetX = 0.0f;
};

class DialogueController {
private:
    DialogueBox* dialogueBox;
    SDL_Renderer* renderer;
    SDL_Texture* previousBgTexture = nullptr;
    SDL_Texture* currentBgTexture = nullptr;
    TTF_Font* uiFont;
    bool isShowingBacklog = false;
    int backlogOffset = 0;
    int backlogCooldown = 0;
    float bgFadeAlpha = 255.0f;
    std::vector<VNCommand> commands;
    int currentLine;
	int clickCooldown;
    bool isFinished;
    bool hasStarted;
    std::string pendingVoice;
    std::map<std::string, ActiveCharacter> activeCharacters;
public:
    DialogueController(DialogueBox* box, TTF_Font* font, SDL_Renderer* ren) {
        dialogueBox = box;
        uiFont = font;
        renderer = ren;
        currentLine = 0;
        clickCooldown = 0;
        isFinished = false;
        hasStarted = false;
    }

    SDL_Texture* GetBackground() const { return currentBgTexture; }

    void LoadScript(const std::vector<VNCommand>& newScript) {
        commands = newScript;
        currentLine = 0;
        isFinished = false;
        hasStarted = false;
        pendingVoice.clear();
		currentBgTexture = nullptr;
        activeCharacters.clear();
    }

    bool IsShowingBacklog() const { return isShowingBacklog; }

    void ToggleBacklog() {
        if (backlogCooldown > 0) return;

        isShowingBacklog = !isShowingBacklog;
        if (isShowingBacklog) {
            backlogOffset = 0;
        }

        backlogCooldown = 15;
    }

    void ScrollBacklog(int direction) {
        if (!isShowingBacklog) return;

        backlogOffset += direction;

        if (backlogOffset < 0) backlogOffset = 0;

        int maxOffset = std::max(0, (int)BacklogManager::GetCount() - 1);
        if (backlogOffset > maxOffset) backlogOffset = maxOffset;
    }

    void RecalculateTargetPositions() {
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

    void ExecuteNextCommands() {
        while (currentLine < commands.size()) {
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

                std::string fx = cmd.args.count("fx") ? cmd.args["fx"] : "typewriter";

                SDL_Color defaultText = { 255, 255, 255, 255 };
                SDL_Color defaultOutline = { 0, 0, 0, 255 };

                SDL_Color textColor = cmd.args.count("color") ?
                    ScriptManager::ParseColor(cmd.args["color"], defaultText) : defaultText;

                SDL_Color outlineColor = cmd.args.count("olcolor") ?
                    ScriptManager::ParseColor(cmd.args["olcolor"], defaultOutline) : defaultOutline;
                BacklogManager::AddLog(speaker, content, pendingVoice);
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
                currentLine++;
			}
			else if (cmd.type == "stopbgm") {
                AudioManager::StopBGM();
                currentLine++;
			}
            else if (cmd.type == "se") {
                std::string fileName = cmd.args["file"];
                AudioManager::PlaySFX(fileName);
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
            else if (cmd.type == "label") {
                currentLine++;
            }
            else if (cmd.type == "jump") {
                std::string target = cmd.args["target"];

                if (target.front() == '*') {
                    std::string labelName = target.substr(1);
                    for (size_t i = 0; i < commands.size(); ++i) {
                        if (commands[i].type == "label" && commands[i].args["name"] == labelName) {
                            currentLine = i;
                            break;
                        }
                    }
                }
                else {
                    commands = ScriptManager::ParseFile(target);
                    currentLine = 0;
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
                    while (++currentLine < commands.size()) {
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
                 while (++currentLine < commands.size()) {
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
                int x = std::stoi(cmd.args["x"]);
                int y = std::stoi(cmd.args["y"]);

                SDL_Color idleCol = { 255, 255, 255, 255 };
                SDL_Color hoverCol = { 255, 215, 0, 255 };

                UIManager::AddTextButton(text, uiFont, x, y, idleCol, hoverCol, target);
                currentLine++;

                if (currentLine < commands.size() && commands[currentLine].type != "choice") {
                    break;
                }
            }
            else {
                currentLine++;
            }
        }

        if (currentLine >= commands.size()) {
            isFinished = true;
        }
    }


    void HandleClick() {
        if (isFinished) return;
        if (clickCooldown > 0) return;
        clickCooldown = 1;

        if (UIManager::HasButtons()) {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            std::string target = UIManager::CheckClick(mx, my);

            if (!target.empty()) {
                // AudioManager::PlaySFX("click.wav");
                UIManager::Clear();

                if (target.front() == '*') {
                    std::string labelName = target.substr(1);
                    for (size_t i = 0; i < commands.size(); ++i) {
                        if (commands[i].type == "label" && commands[i].args["name"] == labelName) {
                            currentLine = i;
                            ExecuteNextCommands();
                            return;
                        }
                    }
                }
                else {
                    commands = ScriptManager::ParseFile(target);
                    currentLine = 0;
                    ExecuteNextCommands();
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

    void Update() {

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
        if (!isFinished) {
            dialogueBox->Update();
        }
        if (!pendingVoice.empty() && dialogueBox->GetCurrentIndex() > 0) {
            AudioManager::PlayVoice(pendingVoice);
            pendingVoice.clear();
        }

        if (UIManager::HasButtons()) {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            UIManager::UpdateHover(mx, my);
        }
        float fadeSpeed = 10.0f;
        float factor = 0.15f; // 線性的 Factor

        if (bgFadeAlpha < 255.0f) {
            bgFadeAlpha += fadeSpeed;
            if (bgFadeAlpha >= 255.0f) {
                bgFadeAlpha = 255.0f;
                previousBgTexture = nullptr;
            }
        }

        for (auto i = activeCharacters.begin(); i != activeCharacters.end(); ) {
            ActiveCharacter& chara = i->second;

            if (chara.alpha < chara.targetAlpha) {
                chara.alpha += fadeSpeed;
                if (chara.alpha > chara.targetAlpha) chara.alpha = chara.targetAlpha;
            }
            else if (chara.alpha > chara.targetAlpha) {
                chara.alpha -= fadeSpeed;
                if (chara.alpha < chara.targetAlpha) chara.alpha = chara.targetAlpha;
            }

            if (!chara.isExiting) {
                float dx = chara.targetX - chara.currentX;
                chara.currentX += dx * factor;
                if (dx * dx < 0.25f) chara.currentX = chara.targetX;
            }

            if (chara.alpha <= 0.0f && chara.isExiting) {
                i = activeCharacters.erase(i);
            }
            else {
                ++i;
            }
        }

    }

    void RenderBackground(PrismatiXEngine& engine) {
        if (previousBgTexture) {
            engine.DrawFullscreenBackground(previousBgTexture, 255);
        }
        if (currentBgTexture) {
            engine.DrawFullscreenBackground(currentBgTexture, (Uint8)bgFadeAlpha);
        }
    }

    void RenderBacklog(SDL_Renderer* renderer) {
        if (!isShowingBacklog || BacklogManager::GetCount() == 0) return;

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
        int w, h;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_Rect bgRect = { 0, 0, w, h };
        SDL_RenderFillRect(renderer, &bgRect);

        SDL_Color titleColor = { 255, 255, 255, 255 };
        SDL_Color outlineColor = { 0, 0, 0, 255 };
        TextManager::DrawWithOutline(renderer, uiFont, "- Backlog -", titleColor, outlineColor, 2, 50, 30);
        TextManager::DrawWithOutline(renderer, uiFont, "(右鍵關閉)", { 150, 150, 150, 255 }, outlineColor, 1, 950, 40);


        int startIdx = (int)BacklogManager::GetCount() - 1 - backlogOffset;
        int drawY = 600;

        for (int i = startIdx; i >= 0 && drawY > 100; --i) {
            const auto& log = BacklogManager::logs[i];

            if (!log.speaker.empty()) {
                SDL_Color nameCol = { 255, 200, 100, 255 };
                TextManager::DrawWithOutline(renderer, uiFont, "【" + log.speaker + "】", nameCol, outlineColor, 1, 100, drawY);
            }

            SDL_Color textCol = { 220, 220, 220, 255 };
            TextManager::DrawWithOutline(renderer, uiFont, log.text, textCol, outlineColor, 1, 300, drawY, 800);

            drawY -= 80;
        }
    }

    void Render(SDL_Renderer* renderer) {
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
                    // 立繪縮放
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
        RenderBacklog(renderer);
    }

    bool IsScriptFinished() const { return isFinished; }
};