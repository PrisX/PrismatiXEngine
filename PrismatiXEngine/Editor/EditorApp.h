#pragma once

#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <vector>
#include <string>
#include "Core/Models/VNCommand.h"

namespace ed = ax::NodeEditor;

namespace PrismatiX::App {
class Engine;
}

namespace PrismatiX::Editor {

struct VNPin {
    ed::PinId id;
    std::string label;
};

struct VNNode {
    ed::NodeId id;
    std::string name; // Label name
    std::vector<PrismatiX::Models::VNCommand> commands;
    
    VNPin input;
    std::vector<VNPin> outputs;

    VNNode(int id, const std::string& name);
};

struct VNLink {
    ed::LinkId id;
    ed::PinId start_pin;
    ed::PinId end_pin;
};

class EditorApp {
public:
    EditorApp(PrismatiX::App::Engine& engine);
    ~EditorApp();

    bool Initialize();
    void ProcessEvent(const SDL_Event& event);
    void NewFrame();
    void Render();
    void Clean();

private:
    PrismatiX::App::Engine& engine;
    ed::EditorContext* m_Context = nullptr;

    // Flowchart Data
    std::vector<VNNode> nodes;
    std::vector<VNLink> links;
    int next_id = 1;
    int selected_node_id = -1;

    void DrawMainMenuBar();
    void DrawFlowchartEditor();
    void DrawInspector();
    void DrawResourceBrowser();

    std::string ExportToPDS();

    // Helpers
    int GetNextId() { return next_id++; }
    VNNode* FindNode(int id);
    VNNode* FindNodeByInputPin(int pin_id);
};

} // namespace PrismatiX::Editor
