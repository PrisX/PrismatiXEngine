#include "Editor/Tools/Lua/ScriptWorkspace.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/IO/AtomicFile.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/VN/Commands/CommandRegistry.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace px::editor {

void ScriptWorkspace::SetProject(const ProjectContext* context) {
    m_project = context;
    m_currentFile.clear();
    m_buffer.clear();
    m_dirty = false;
    m_inactiveDocuments.clear();
    Rescan();
}

void ScriptWorkspace::Rescan() {
    RefreshFiles();
    ScanCommands();
}

void ScriptWorkspace::RefreshFiles() {
    m_files.clear();
    if (!m_project || !m_project->IsOpen()) return;
    const fs::path root = m_project->root;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() == ".lua") {
            m_files.push_back(fs::relative(it->path(), root, ec).generic_string());
        }
    }
}

void ScriptWorkspace::ScanCommands() {
    m_commands.clear();
    if (!m_project || !m_project->IsOpen()) return;
    const fs::path root = m_project->root;

    std::error_code error;
    const auto extensions = root / "Content/Extensions";
    for (fs::recursive_directory_iterator iterator(extensions, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".pxextension") continue;
        std::ifstream stream(iterator->path(), std::ios::binary); std::ostringstream text; text << stream.rdbuf();
        const auto manifest = nlohmann::json::parse(text.str(), nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object() ||
            manifest.value("format", std::string{}) != "PrismatiXExtension" ||
            manifest.value("version", 0) != 4) continue;
        for (const auto& command : manifest.value("commands", nlohmann::json::array())) {
            if (!command.is_object()) continue;
            CustomCommandDef definition;definition.name=command.value("id",std::string{});
            definition.category=command.value("category",std::string("Extension"));
            definition.description=command.value("description",command.value("displayName",definition.name));
            definition.sourceFile=fs::relative(iterator->path(),root,error).generic_string();
            for(const auto& parameter:command.value("parameters",nlohmann::json::array())){
                if(!parameter.is_object())continue;CustomCommandParam field;field.key=parameter.value("name",std::string{});field.label=parameter.value("label",field.key);field.type=parameter.value("type",std::string("string"));
                if(parameter.contains("default")){if(parameter["default"].is_string())field.defaultValue=parameter["default"].get<std::string>();else field.defaultValue=parameter["default"].dump();}
                field.options=parameter.value("options",std::vector<std::string>{});field.required=parameter.value("required",false);if(!field.key.empty())definition.params.push_back(std::move(field));
            }
            if(!definition.name.empty()){
                if(!vn::CommandRegistry::Global().Find(definition.name)){
                    vn::CommandDescriptor descriptor;descriptor.id=definition.name;descriptor.displayName=command.value("displayName",definition.name);descriptor.category=definition.category;descriptor.allowAdditionalParameters=false;descriptor.waitPolicy=command.value("await",false)?vn::CommandWaitPolicy::Async:vn::CommandWaitPolicy::Immediate;descriptor.rollbackPolicy=command.value("rollback",std::string("boundary"))=="reversible"?vn::RollbackPolicy::Reversible:vn::RollbackPolicy::Boundary;
                    for(const auto& field:definition.params){vn::CommandParameterDescriptor parameter;parameter.name=field.key;parameter.label=field.label;parameter.required=field.required;parameter.options=field.options;if(field.type=="bool")parameter.type=VariantType::Bool;else if(field.type=="int")parameter.type=VariantType::Integer;else if(field.type=="number")parameter.type=VariantType::Number;else if(field.type=="resource")parameter.type=VariantType::ResourceRef;else if(field.type=="list")parameter.type=VariantType::Array;else if(field.type=="map"||field.type=="expression")parameter.type=VariantType::Object;else parameter.type=VariantType::String;descriptor.parameters.push_back(std::move(parameter));}
                    (void)vn::CommandRegistry::Global().Register(std::move(descriptor));
                }
                m_commands.push_back(std::move(definition));
            }
        }
    }

    Log("Scripting: discovered " + std::to_string(m_commands.size()) + " custom command(s).");
    if (m_onCommands) m_onCommands(m_commands);
}

void ScriptWorkspace::LoadFile(const std::string& runtimePath) {
    if (!m_project) return;
    if(runtimePath==m_currentFile)return;
    if(!m_currentFile.empty())m_inactiveDocuments[m_currentFile]={m_currentFile,std::move(m_buffer),m_dirty};
    if(auto found=m_inactiveDocuments.find(runtimePath);found!=m_inactiveDocuments.end()){
        m_currentFile=found->second.runtimePath;m_buffer=std::move(found->second.buffer);m_dirty=found->second.dirty;m_inactiveDocuments.erase(found);return;
    }
    std::ifstream in(m_project->root / runtimePath, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    m_buffer = ss.str();
    m_currentFile = runtimePath;
    m_dirty = false;
}

void ScriptWorkspace::SaveAll(){
    (void)SaveCurrent();
    if(!m_project)return;
    for(auto& [path,document]:m_inactiveDocuments){
        if(!document.dirty)continue;const Status written=io::AtomicFile::WriteText(m_project->root/path,document.buffer);
        if(!written)for(const auto& diagnostic:written.Diagnostics())diag::Emit(diagnostic);else document.dirty=false;
    }
    ScanCommands();
}

std::vector<ScriptWorkspace::DocumentSession> ScriptWorkspace::OpenDocuments()const{
    std::vector<DocumentSession> result;result.reserve(m_inactiveDocuments.size()+(!m_currentFile.empty()?1:0));
    if(!m_currentFile.empty())result.push_back({m_currentFile,m_buffer,m_dirty});
    for(const auto& [_,document]:m_inactiveDocuments)result.push_back(document);return result;
}

bool ScriptWorkspace::ReloadFile(const std::string& runtimePath){
    if(!m_project)return false;std::ifstream input(m_project->root/runtimePath,std::ios::binary);if(!input)return false;std::ostringstream stream;stream<<input.rdbuf();
    if(runtimePath==m_currentFile){m_buffer=stream.str();m_dirty=false;return true;}
    if(auto found=m_inactiveDocuments.find(runtimePath);found!=m_inactiveDocuments.end()){found->second.buffer=stream.str();found->second.dirty=false;return true;}
    return false;
}

bool ScriptWorkspace::SaveCurrent() {
    if (!m_project || m_currentFile.empty()) return false;
    const Status written = io::AtomicFile::WriteText(m_project->root / m_currentFile, m_buffer);
    if (!written) {
        for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    m_dirty = false;
    Log("Saved " + m_currentFile);
    ScanCommands();
    return true;
}

bool ScriptWorkspace::CloseFile(const std::string& runtimePath,bool save){
    if(runtimePath==m_currentFile){
        if(save&&m_dirty&&!SaveCurrent())return false;
        m_currentFile.clear();m_buffer.clear();m_dirty=false;
        if(!m_inactiveDocuments.empty()){auto found=m_inactiveDocuments.begin();m_currentFile=found->second.runtimePath;m_buffer=std::move(found->second.buffer);m_dirty=found->second.dirty;m_inactiveDocuments.erase(found);}
        return true;
    }
    const auto found=m_inactiveDocuments.find(runtimePath);if(found==m_inactiveDocuments.end())return true;
    if(save&&found->second.dirty){const Status written=io::AtomicFile::WriteText(m_project->root/runtimePath,found->second.buffer);if(!written){for(const auto& diagnostic:written.Diagnostics())diag::Emit(diagnostic);return false;}}
    m_inactiveDocuments.erase(found);return true;
}

void ScriptWorkspace::Render() {
    const float listW = 260.0f;
    ImGui::BeginChild("scriptsLeft", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    RenderFileList();
    ImGui::Separator();
    RenderCommandList();
    ImGui::Separator();
    RenderApiReference();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("scriptsRight", ImVec2(0, 0));
    RenderEditor();
    ImGui::EndChild();
}

void ScriptWorkspace::RenderFileList() {
    ImGui::TextDisabled("Lua Scripts");
    if (ImGui::SmallButton("Rescan")) Rescan();
    ImGui::SameLine();
    if (ImGui::SmallButton("New")) ImGui::OpenPopup("newLua");
    if (ImGui::BeginPopup("newLua")) {
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("name", m_newName, sizeof(m_newName));
        ImGui::SameLine();
        if (ImGui::Button("Create") && m_project && m_project->IsOpen()) {
            const fs::path rel = fs::path("Content") / "Extensions" /
                                 (std::string(m_newName) + ".lua");
            const fs::path abs = m_project->root / rel;
            std::error_code ec;
            fs::create_directories(abs.parent_path(), ec);
            if (!fs::exists(abs)) {
                const std::string extensionName = std::string(m_newName);
                const std::string commandName = extensionName + ".command";
                const std::string source =
                       "-- " + extensionName + ".lua (PrismatiX 3.0 extension)\n"
                       "Engine.RegisterCommand(\"" + commandName + "\", function(args)\n"
                       "    Engine.log(\"" + commandName + " value=\" .. tostring(args.value))\n"
                       "end)\n";
                const Status written = io::AtomicFile::WriteText(abs, source);
                if (!written) {
                    for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
                } else {
                    const fs::path manifestPath = abs.parent_path() / (extensionName + ".pxextension");
                    nlohmann::json manifest{{"format","PrismatiXExtension"},{"version",4},{"id",extensionName},{"order",0},{"entry",abs.filename().generic_string()},{"capabilities",nlohmann::json::array({"runtime"})}};
                    manifest["commands"] = nlohmann::json::array({{{"id",commandName},{"displayName",extensionName+" Command"},{"category","Extension"},{"description","Typed custom command"},{"await",false},{"rollback","reversible"},{"parameters",nlohmann::json::array({{{"name","value"},{"label","Value"},{"type","string"},{"required",false},{"default",""}}})}}});
                    const Status manifestWritten=io::AtomicFile::WriteText(manifestPath,manifest.dump(2)+"\n");
                    if(!manifestWritten)for(const auto& diagnostic:manifestWritten.Diagnostics())diag::Emit(diagnostic);
                    std::vector<std::string> manifests;for(fs::directory_iterator iterator(abs.parent_path(),ec),end;iterator!=end&&!ec;iterator.increment(ec))if(iterator->is_regular_file(ec)&&iterator->path().extension()==".pxextension")manifests.push_back("Content/Extensions/"+iterator->path().filename().generic_string());std::sort(manifests.begin(),manifests.end());
                    const fs::path indexPath=abs.parent_path()/"extensions.pxindex";const Status indexWritten=io::AtomicFile::WriteText(indexPath,nlohmann::json(manifests).dump(2)+"\n");if(!indexWritten)for(const auto& diagnostic:indexWritten.Diagnostics())diag::Emit(diagnostic);
                    resource::AssetRegistry registry;for(const auto& asset:{abs,manifestPath,indexPath})if(!fs::exists(resource::AssetRegistry::MetaPath(asset))){auto registered=registry.RegisterAsset(m_project->root,asset,"lua-extension");if(!registered)for(const auto& diagnostic:registered.Diagnostics())diag::Emit(diagnostic);}
                }
            }
            Rescan();
            LoadFile(rel.generic_string());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    if (m_files.empty()) {
        ImGui::TextDisabled("(no .lua files)");
    }
    for (const std::string& f : m_files) {
        if (ImGui::Selectable(f.c_str(), f == m_currentFile)) {
            LoadFile(f);
        }
    }
}

void ScriptWorkspace::RenderCommandList() {
    if (!ImGui::CollapsingHeader("Custom Commands", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (m_commands.empty()) {
        ImGui::TextDisabled("None. Add typed commands to a .pxextension manifest.");
        return;
    }
    for (const CustomCommandDef& c : m_commands) {
        ImGui::BulletText("%s", c.name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("[%s] category=%s\n%s\nfrom %s\nparams: %zu", c.name.c_str(),
                              c.category.c_str(), c.description.c_str(), c.sourceFile.c_str(),
                              c.params.size());
        }
    }
}

void ScriptWorkspace::RenderApiReference() {
    if (!ImGui::CollapsingHeader("API Reference")) return;
    struct Entry {
        const char* sig;
        const char* desc;
    };
    struct Group {
        const char* name;
        std::vector<Entry> entries;
    };
    static const std::vector<Group> kApi = {
        { "Core", {
            { "Engine.log(msg)", "Write to the engine log." },
            { "Engine.RegisterCommand(name, fn)", "Register a custom [name ...] VN command." },
            { "Engine.On(event, fn)", "Subscribe to an engine event." },
            { "Engine.Emit(event, payload?)", "Emit an event to subscribers." },
        } },
        { "Progress (read-only)", {
            { "Engine.HasSeen(key) / MarkSeen(key)", "Read/record a seen flag." },
            { "Engine.ClearCount()", "NG+ clear count." },
            { "Engine.CGUnlocked(id) / UnlockCG(id)", "Query/unlock a CG." },
            { "Engine.SceneUnlocked(id) / UnlockScene(id)", "Query/unlock a scene." },
            { "Engine.PersistentVar(name, value?)", "Get/set a persistent variable." },
        } },
        { "Audio", {
            { "Engine.PlaySE(path)", "Play a sound effect." },
            { "Engine.PlayBGM(path, loop?, fade?)", "Play background music." },
            { "Engine.StopBGM(fade?)", "Stop background music." },
            { "Engine.SetBGMVolume/SetSEVolume/SetVoiceVolume(v)", "Set a bus volume (0-100)." },
        } },
        { "Input", {
            { "Engine.GetMouseX() / GetMouseY()", "Cursor position." },
            { "Engine.GetLeftClick() / GetRightClick()", "Mouse button edge." },
        } },
        { "Draw", {
            { "Engine.GetLogicalSize()", "Returns w, h of the logical canvas." },
            { "Engine.DrawImage(path, x, y, w, h)", "Draw a texture." },
            { "Engine.DrawAuto(path, mode, alpha?)", "Draw using a DisplayMode." },
            { "Engine.DrawRect(x,y,w,h, r,g,b,a)", "Draw a filled rectangle." },
            { "Engine.DrawRoundedRect(x,y,w,h,radius, ...)", "Draw a rounded rectangle." },
            { "Engine.DrawText(text, x, y, ...)", "Draw text." },
            { "Engine.MeasureText(text, font, size)", "Returns {w, h}." },
            { "DisplayMode.{TopLeft,Center,Fit,Fill,Bottom,FitWidthBottom}", "Draw modes." },
        } },
    };
    for (const Group& g : kApi) {
        if (ImGui::TreeNode(g.name)) {
            for (const Entry& e : g.entries) {
                ImGui::TextWrapped("%s", e.sig);
                ImGui::TextDisabled("   %s", e.desc);
            }
            ImGui::TreePop();
        }
    }
}

void ScriptWorkspace::RenderEditor() {
    if (m_currentFile.empty()) {
        ImGui::TextDisabled("Select a .lua file on the left, or create one.");
        ImGui::TextWrapped(
            "Declare command schemas in a strict .pxextension manifest and implement them with "
            "Engine.RegisterCommand(\"name\", fn). The schema generates typed Story nodes.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "The player loads declared .pxextension manifests. Modules, commands and "
            "capabilities must be declared explicitly.");
        return;
    }

    std::vector<std::string> openFiles;if(!m_currentFile.empty())openFiles.push_back(m_currentFile);for(const auto& [path,_]:m_inactiveDocuments)openFiles.push_back(path);
    std::string activate,close;
    if(ImGui::BeginTabBar("##lua-document-tabs",ImGuiTabBarFlags_Reorderable|ImGuiTabBarFlags_FittingPolicyScroll)){
        for(const auto& path:openFiles){const bool current=path==m_currentFile;const bool dirty=current?m_dirty:m_inactiveDocuments.at(path).dirty;const std::string label=fs::path(path).filename().string()+(dirty?" ●":"")+"###lua-"+path;bool open=true;if(ImGui::BeginTabItem(label.c_str(),&open,dirty?ImGuiTabItemFlags_UnsavedDocument:0)){if(!current)activate=path;ImGui::EndTabItem();}if(!open)close=path;}
        ImGui::EndTabBar();
    }
    if(!activate.empty())LoadFile(activate);
    if(!close.empty()){const bool dirty=close==m_currentFile?m_dirty:m_inactiveDocuments.contains(close)&&m_inactiveDocuments.at(close).dirty;if(dirty){m_closeRequest=close;m_closePopup=true;}else(void)CloseFile(close,false);}
    if(m_closePopup)ImGui::OpenPopup("關閉未儲存 Lua 文件");
    if(ImGui::BeginPopupModal("關閉未儲存 Lua 文件",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::Text("「%s」有未儲存的變更。",fs::path(m_closeRequest).filename().string().c_str());
        if(ImGui::Button("儲存",ImVec2(110,0))){if(CloseFile(m_closeRequest,true)){m_closeRequest.clear();m_closePopup=false;ImGui::CloseCurrentPopup();}}
        ImGui::SameLine();if(ImGui::Button("捨棄",ImVec2(110,0))){(void)CloseFile(m_closeRequest,false);m_closeRequest.clear();m_closePopup=false;ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("取消",ImVec2(110,0))){m_closeRequest.clear();m_closePopup=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::Text("%s%s", m_currentFile.c_str(), m_dirty ? " *" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save")) (void)SaveCurrent();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) (void)ReloadFile(m_currentFile);
    ImGui::Separator();

    if (ImGui::InputTextMultiline("##luacode", &m_buffer, ImGui::GetContentRegionAvail(),
                                  ImGuiInputTextFlags_AllowTabInput)) {
        m_dirty = true;
    }
}

}
