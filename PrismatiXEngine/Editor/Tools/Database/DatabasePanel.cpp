#include "Editor/Tools/Database/DatabasePanel.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <sstream>

namespace px::editor {

using namespace px::project;

namespace {
std::string Join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += v[i];
    }
    return out;
}
std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::size_t a = item.find_first_not_of(" \t");
        std::size_t b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}
}

bool DatabasePanel::Render(Database& db) {
    bool edited = false;
    if (!ImGui::BeginTabBar("dbtabs")) {
        return false;
    }

    if (ImGui::BeginTabItem("Cast")) {
        if (ImGui::Button("+ Character")) {
            db.characters.push_back(Character{ "new", "New", Color{ 245, 248, 255, 255 }, "", {} });
            edited = true;
        }
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(db.characters.size()); ++i) {
            Character& c = db.characters[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("id", &c.id)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputText("name", &c.name)) edited = true;
            ImGui::SameLine();
            float col[4] = { c.color.r / 255.0f, c.color.g / 255.0f, c.color.b / 255.0f, 1.0f };
            if (ImGui::ColorEdit3("color", col, ImGuiColorEditFlags_NoInputs)) {
                c.color = { (std::uint8_t)(col[0] * 255), (std::uint8_t)(col[1] * 255),
                            (std::uint8_t)(col[2] * 255), 255 };
                edited = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                db.characters.erase(db.characters.begin() + i);
                edited = true;
                ImGui::PopID();
                break;
            }
            std::string exprs = Join(c.expressions);
            ImGui::SetNextItemWidth(280);
            if (ImGui::InputText("expressions (csv)", &exprs)) {
                c.expressions = Split(exprs);
                edited = true;
            }
            if (ImGui::InputText("voiceDir", &c.voiceDir)) edited = true;
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Variables")) {
        if (ImGui::Button("+ Variable")) {
            db.variables.push_back(Variable{ "var", 0, false });
            edited = true;
        }
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(db.variables.size()); ++i) {
            Variable& v = db.variables[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText("name", &v.name)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragInt("default", &v.defaultValue)) edited = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("persistent", &v.persistent)) edited = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                db.variables.erase(db.variables.begin() + i);
                edited = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Gallery")) {
        if (ImGui::Button("+ CG")) {
            db.gallery.push_back(GalleryItem{ "cg_new", "New CG", "", "" });
            edited = true;
        }
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(db.gallery.size()); ++i) {
            GalleryItem& g = db.gallery[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("id", &g.id)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputText("title", &g.title)) edited = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                db.gallery.erase(db.gallery.begin() + i);
                edited = true;
                ImGui::PopID();
                break;
            }
            if (ImGui::InputText("thumbnail", &g.thumbnail)) edited = true;
            if (ImGui::InputText("image", &g.image)) edited = true;
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Chapters")) {
        if (ImGui::Button("+ Chapter")) {
            db.chapters.push_back(Chapter{ "ch", "Chapter", "", false });
            edited = true;
        }
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(db.chapters.size()); ++i) {
            Chapter& c = db.chapters[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputText("id", &c.id)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::InputText("title", &c.title)) edited = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("unlocked", &c.alwaysUnlocked)) edited = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                db.chapters.erase(db.chapters.begin() + i);
                edited = true;
                ImGui::PopID();
                break;
            }
            if (ImGui::InputText("script (.pds)", &c.script)) edited = true;
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Input")) {
        ImGui::TextDisabled("key -> action (screen.open/screen.close) -> target screen");
        if (ImGui::Button("+ Binding")) {
            db.inputMap.push_back(InputBinding{ "Escape", "screen.open", "Data/UI/settings.pxui" });
            edited = true;
        }
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(db.inputMap.size()); ++i) {
            InputBinding& b = db.inputMap[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputText("key", &b.key)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            if (ImGui::InputText("action", &b.action)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            if (ImGui::InputText("target", &b.target)) edited = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                db.inputMap.erase(db.inputMap.begin() + i);
                edited = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    return edited;
}

}
