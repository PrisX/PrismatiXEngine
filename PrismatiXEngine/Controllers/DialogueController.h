#pragma once
#include <vector>
#include <string>
#include <iostream>
#include <map>
#include <algorithm>
#include "DialogueBox.h"
#include "ScriptManager.h"
#include "TextureManager.h"

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

    std::vector<VNCommand> commands;
    int currentLine;
	int clickCooldown;
    bool isFinished;
    std::map<std::string, ActiveCharacter> activeCharacters;
public:
    DialogueController(DialogueBox* box, SDL_Renderer* ren) {
        dialogueBox = box;
        renderer = ren;
        currentLine = 0;
        clickCooldown = 0;
        isFinished = false;
    }

    void LoadScript(const std::vector<VNCommand>& newScript) {
        commands = newScript;
        currentLine = 0;
        isFinished = false;
        activeCharacters.clear();

        ExecuteNextCommands();
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
            else if (cmd.type == "text") {
                std::string speaker = cmd.args.count("name") ? cmd.args["name"] : "";
                std::string content = cmd.args["content"];

                dialogueBox->SetText(speaker, content, 40);

                break;
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
        if (!dialogueBox->IsFinished()) {
            dialogueBox->ShowAll();
        }
        else {
            currentLine++;
			ExecuteNextCommands();
        }
    }

    void Update() {
        if (clickCooldown > 0) {
            clickCooldown--;
        }
        if (!isFinished) {
            dialogueBox->Update();
        }

        float fadeSpeed = 10.0f;
        float factor = 0.15f; // 線性的 Factor

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
    }

    bool IsScriptFinished() const { return isFinished; }
};