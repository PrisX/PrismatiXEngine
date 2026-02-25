#pragma once
#include <vector>
#include <string>
#include <iostream>
#include "DialogueBox.h"

struct DialogLine {
    std::string name;
    std::string text;
};

class DialogueController {
private:
    DialogueBox* dialogueBox;
    std::vector<DialogLine> currentScriptData;
    int currentLine;
	int clickCooldown;
    bool isFinished;
public:
    DialogueController(DialogueBox* box) {
        dialogueBox = box;
        currentLine = 0;
        clickCooldown = 0;
        isFinished = false;
    }

    void LoadScript(const std::vector<DialogLine>& newScript) {
        currentScriptData = newScript;
        currentLine = 0;
        isFinished = false;
        if (!currentScriptData.empty()) {
            dialogueBox->SetText(currentScriptData[currentLine].name, currentScriptData[currentLine].text, 40);
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
            if (currentLine < currentScriptData.size()) {
                dialogueBox->SetText(currentScriptData[currentLine].name, currentScriptData[currentLine].text, 40);
            }
            else {
                isFinished = true;
            }
        }
    }

    void Update() {
        if (clickCooldown > 0) {
            clickCooldown--;
        }
        if (!isFinished) {
            dialogueBox->Update();
        }
    }

    void Render(SDL_Renderer* renderer) {
        if (!isFinished) {
            dialogueBox->Render(renderer);
        }
    }

    bool IsScriptFinished() const { return isFinished; }
};