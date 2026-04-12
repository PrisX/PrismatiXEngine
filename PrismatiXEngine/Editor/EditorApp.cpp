#include "EditorApp.h"
#include "Core/Engine.h"
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include "Utils/Logger.h"
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace PrismatiX::Editor {

VNNode::VNNode(int id, const std::string& name) : id(id), name(name) {
    input.id = id * 1000 + 1;
    input.label = "In";
    
    VNPin out;
    out.id = id * 1000 + 2;
    out.label = "Next";
    outputs.push_back(out);
}

EditorApp::EditorApp(PrismatiX::App::Engine& engine) : engine(engine) {}

EditorApp::~EditorApp() { Clean(); }

bool EditorApp::Initialize() {
    PX_LOG_INFO("Initializing Editor System (ImGui & ImGui-Node-Editor)...");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    // Initialize imgui-node-editor context
    ed::Config config;
    config.SettingsFile = "editor_layout.json";
    m_Context = ed::CreateEditor(&config);
    ed::SetCurrentEditor(m_Context);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load Chinese Font for better editing
    if (std::filesystem::exists("C:\\Windows\\Fonts\\msjh.ttc")) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msjh.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    } else if (std::filesystem::exists("C:\\Windows\\Fonts\\simhei.ttf")) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    } else {
        PX_LOG_WARN("Chinese fonts not found. Text might not render correctly.");
    }

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL2_InitForSDLRenderer(engine.GetWindow(), engine.GetRenderer())) {
        PX_LOG_ERROR("ImGui_ImplSDL2_Init failed");
        return false;
    }

    if (!ImGui_ImplSDLRenderer2_Init(engine.GetRenderer())) {
        PX_LOG_ERROR("ImGui_ImplSDLRenderer2_Init failed");
        return false;
    }

    nodes.emplace_back(GetNextId(), "Start");

    return true;
}

void EditorApp::ProcessEvent(const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void EditorApp::NewFrame() {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void EditorApp::Render() {
    DrawMainMenuBar();
    DrawFlowchartEditor();
    DrawInspector();
    DrawResourceBrowser();

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), engine.GetRenderer());
}

void EditorApp::Clean() {
    if (m_Context) {
        ed::DestroyEditor(m_Context);
        m_Context = nullptr;
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void EditorApp::DrawMainMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene")) {
                nodes.clear();
                links.clear();
                nodes.emplace_back(GetNextId(), "Start");
            }
            if (ImGui::MenuItem("Save Scenario (.pds)")) {
                std::string pds = ExportToPDS();
                PX_LOG_INFO("Exported PDS:\n{}", pds);
            }
            if (ImGui::MenuItem("Exit", "Alt+F4")) engine.Quit();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void EditorApp::DrawFlowchartEditor() {
    ImGui::Begin("VN Script Editor (Flowchart)");

    ed::SetCurrentEditor(m_Context);
    ed::Begin("My Editor", ImVec2(0.0, 0.0f));

    for (auto& node : nodes) {
        ed::BeginNode(node.id);
        
        ImGui::TextUnformatted(node.name.c_str());
        ImGui::Spacing();
        
        // Input Pin
        ed::BeginPin(node.input.id, ed::PinKind::Input);
        ImGui::Text("-> In");
        ed::EndPin();

        ImGui::Separator();
        
        ImGui::PushItemWidth(200.0f);
        for (size_t i = 0; i < node.commands.size(); ++i) {
            auto& cmd = node.commands[i];
            ImGui::PushID(static_cast<int>(i));
            
            if (cmd.type == "text") {
                ImGui::TextDisabled("[Dialogue]");
                char speaker_buf[64];
                strncpy(speaker_buf, cmd.args["name"].c_str(), sizeof(speaker_buf));
                if (ImGui::InputText("##speaker", speaker_buf, sizeof(speaker_buf))) {
                    cmd.args["name"] = speaker_buf;
                }
                char content_buf[256];
                strncpy(content_buf, cmd.args["content"].c_str(), sizeof(content_buf));
                if (ImGui::InputTextMultiline("##content", content_buf, sizeof(content_buf), ImVec2(0, 40))) {
                    cmd.args["content"] = content_buf;
                }
            } else if (cmd.type == "bg") {
                ImGui::TextDisabled("[BG]");
                char file_buf[128];
                strncpy(file_buf, cmd.args["file"].c_str(), sizeof(file_buf));
                if (ImGui::InputText("##bg", file_buf, sizeof(file_buf))) {
                    cmd.args["file"] = file_buf;
                }
            } else if (cmd.type == "char") {
                ImGui::TextDisabled("[Character]");
                char file_buf[128];
                strncpy(file_buf, cmd.args["file"].c_str(), sizeof(file_buf));
                if (ImGui::InputText("##char", file_buf, sizeof(file_buf))) {
                    cmd.args["file"] = file_buf;
                }
            } else {
                ImGui::Text("[%s]", cmd.type.c_str());
            }
            
            ImGui::PopID();
        }
        ImGui::PopItemWidth();

        ImGui::Separator();
        if (ImGui::Button("+ Dialogue")) {
            PrismatiX::Models::VNCommand cmd;
            cmd.type = "text";
            cmd.args["content"] = "New line...";
            cmd.args["name"] = "";
            node.commands.push_back(cmd);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ BG")) {
            PrismatiX::Models::VNCommand cmd;
            cmd.type = "bg";
            cmd.args["file"] = "bg.png";
            node.commands.push_back(cmd);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Char")) {
            PrismatiX::Models::VNCommand cmd;
            cmd.type = "char";
            cmd.args["file"] = "char.png";
            cmd.args["pos"] = "center";
            node.commands.push_back(cmd);
        }

        ImGui::Separator();

        // Output Pins
        for (const auto& out : node.outputs) {
            ed::BeginPin(out.id, ed::PinKind::Output);
            ImGui::Text("%s ->", out.label.c_str());
            ed::EndPin();
        }

        ed::EndNode();
    }

    for (const auto& link : links) {
        ed::Link(link.id, link.start_pin, link.end_pin);
    }

    // Handle creation
    if (ed::BeginCreate()) {
        ed::PinId startPinId, endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId)) {
            if (startPinId && endPinId) {
                if (ed::AcceptNewItem()) {
                    links.push_back({ ed::LinkId(GetNextId()), startPinId, endPinId });
                }
            }
        }
    }
    ed::EndCreate();

    // Handle deletion
    if (ed::BeginDelete()) {
        ed::NodeId nodeId;
        while (ed::QueryDeletedNode(&nodeId)) {
            if (ed::AcceptDeletedItem()) {
                auto node_it = std::find_if(nodes.begin(), nodes.end(), [nodeId](const VNNode& n) { return n.id == nodeId; });
                if (node_it != nodes.end()) {
                    std::vector<ed::PinId> pins_to_remove;
                    pins_to_remove.push_back(node_it->input.id);
                    for (auto& out : node_it->outputs) pins_to_remove.push_back(out.id);

                    links.erase(std::remove_if(links.begin(), links.end(), [&pins_to_remove](const VNLink& l) {
                        return std::find(pins_to_remove.begin(), pins_to_remove.end(), l.start_pin) != pins_to_remove.end() ||
                               std::find(pins_to_remove.begin(), pins_to_remove.end(), l.end_pin) != pins_to_remove.end();
                    }), links.end());

                    nodes.erase(node_it);
                }
            }
        }
        
        ed::LinkId linkId;
        while (ed::QueryDeletedLink(&linkId)) {
            if (ed::AcceptDeletedItem()) {
                links.erase(std::remove_if(links.begin(), links.end(), [linkId](const VNLink& l) { return l.id == linkId; }), links.end());
            }
        }
    }
    ed::EndDelete();

    // Context Menu
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) {
        ImGui::OpenPopup("Create Node Menu");
    }
    ed::Resume();

    ed::Suspend();
    if (ImGui::BeginPopup("Create Node Menu")) {
        if (ImGui::MenuItem("Add New Node")) {
            VNNode new_node(GetNextId(), "New Node");
            ed::SetNodePosition(new_node.id, ImGui::GetMousePos());
            nodes.push_back(new_node);
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    
    // Update selected node for Inspector
    if (ed::GetSelectedObjectCount() > 0) {
        ed::NodeId selected_nodes[1];
        if (ed::GetSelectedNodes(selected_nodes, 1) > 0) {
            selected_node_id = selected_nodes[0].Get();
        }
    } else {
        selected_node_id = -1;
    }

    ImGui::End();
}

void EditorApp::DrawInspector() {
    ImGui::Begin("Inspector");
    VNNode* selected = FindNode(selected_node_id);
    if (selected) {
        ImGui::Text("Node Settings");
        char name_buf[64];
        strncpy(name_buf, selected->name.c_str(), sizeof(name_buf));
        if (ImGui::InputText("Label ID", name_buf, sizeof(name_buf))) {
            selected->name = name_buf;
        }
        ImGui::Separator();
        
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Output Branches (Choices/Jumps)");
        for (size_t i = 0; i < selected->outputs.size(); ++i) {
            ImGui::PushID(static_cast<int>(selected->outputs[i].id.Get()));
            char branch_name[64];
            strncpy(branch_name, selected->outputs[i].label.c_str(), sizeof(branch_name));
            if (ImGui::InputText("##branch", branch_name, sizeof(branch_name))) {
                selected->outputs[i].label = branch_name;
            }
            ImGui::SameLine();
            if (ImGui::Button("X") && selected->outputs.size() > 1) {
                auto pin_id = selected->outputs[i].id;
                links.erase(std::remove_if(links.begin(), links.end(), [pin_id](const VNLink& l){ return l.start_pin == pin_id; }), links.end());
                selected->outputs.erase(selected->outputs.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add Branch")) {
            VNPin out;
            out.id = GetNextId();
            out.label = "Option " + std::to_string(selected->outputs.size() + 1);
            selected->outputs.push_back(out);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Commands Properties & Parameters");
        for (size_t i = 0; i < selected->commands.size(); ++i) {
            auto& cmd = selected->commands[i];
            ImGui::PushID(static_cast<int>(i));
            
            if (ImGui::Button("X")) {
                selected->commands.erase(selected->commands.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            
            if (ImGui::TreeNodeEx(("[" + cmd.type + "]").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                char type_buf[64];
                strncpy(type_buf, cmd.type.c_str(), sizeof(type_buf));
                if (ImGui::InputText("Command Type", type_buf, sizeof(type_buf))) {
                    cmd.type = type_buf;
                }

                std::vector<std::string> keys_to_remove;
                for (auto& [key, val] : cmd.args) {
                    ImGui::PushID(key.c_str());
                    char val_buf[256];
                    strncpy(val_buf, val.c_str(), sizeof(val_buf));
                    
                    ImGui::PushItemWidth(150.0f);
                    if (key == "content") {
                        if (ImGui::InputTextMultiline(("##" + key).c_str(), val_buf, sizeof(val_buf), ImVec2(0, 40))) val = val_buf;
                    } else {
                        if (ImGui::InputText(("##" + key).c_str(), val_buf, sizeof(val_buf))) val = val_buf;
                    }
                    ImGui::PopItemWidth();
                    
                    ImGui::SameLine();
                    ImGui::Text("%s", key.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("-")) keys_to_remove.push_back(key);
                    
                    ImGui::PopID();
                }

                for (const auto& k : keys_to_remove) cmd.args.erase(k);

                ImGui::Spacing();
                static char new_key[64] = "";
                static char new_val[128] = "";
                ImGui::PushItemWidth(80.0f);
                ImGui::InputText("##newkey", new_key, sizeof(new_key));
                ImGui::SameLine();
                ImGui::PushItemWidth(120.0f);
                ImGui::InputText("##newval", new_val, sizeof(new_val));
                ImGui::PopItemWidth();
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("Add Param") && strlen(new_key) > 0) {
                    cmd.args[new_key] = new_val;
                    new_key[0] = '\0';
                    new_val[0] = '\0';
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            ImGui::Separator();
        }

    } else {
        ImGui::Text("Select a node on the canvas to see its settings.");
    }
    ImGui::End();
}

void EditorApp::DrawResourceBrowser() {
    ImGui::Begin("Resources");
    ImGui::End();
}

VNNode* EditorApp::FindNode(int id) {
    for (auto& node : nodes) {
        if (node.id.Get() == id) return &node;
    }
    return nullptr;
}

VNNode* EditorApp::FindNodeByInputPin(int pin_id) {
    for (auto& node : nodes) {
        if (node.input.id.Get() == pin_id) return &node;
    }
    return nullptr;
}

std::string EditorApp::ExportToPDS() {
    std::stringstream ss;
    ss << "// Generated by PrismatiX Editor\n\n";

    for (const auto& node : nodes) {
        ss << "*" << node.name << "\n";
        
        for (const auto& cmd : node.commands) {
            if (cmd.type == "text") {
                if (cmd.args.count("name") > 0 && !cmd.args.at("name").empty()) {
                    ss << "[" << cmd.args.at("name") << "] " << cmd.args.at("content") << "\n";
                } else {
                    ss << cmd.args.at("content") << "\n";
                }
            } else {
                ss << "[" << cmd.type;
                for (const auto& [key, val] : cmd.args) {
                    ss << " " << key << "=\"" << val << "\"";
                }
                ss << "]\n";
            }
        }

        if (node.outputs.size() > 1) {
            for (const auto& out : node.outputs) {
                ss << "[choice text=\"" << out.label << "\" target=\"";
                
                std::string target_name = "null";
                for (const auto& link : links) {
                    if (link.start_pin == out.id) {
                        VNNode* target_node = FindNodeByInputPin(link.end_pin.Get());
                        if (target_node) target_name = "*" + target_node->name;
                    }
                }
                ss << target_name << "\"]\n";
            }
        } else if (node.outputs.size() == 1) {
            for (const auto& link : links) {
                if (link.start_pin == node.outputs[0].id) {
                    VNNode* target = FindNodeByInputPin(link.end_pin.Get());
                    if (target) {
                        ss << "[jump target=\"*" << target->name << "\"]\n";
                    }
                }
            }
        }
        
        ss << "\n";
    }

    return ss.str();
}

} // namespace PrismatiX::Editor
