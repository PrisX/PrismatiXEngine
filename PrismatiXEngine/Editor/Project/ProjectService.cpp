#include "Editor/Project/ProjectService.h"

#include <fstream>

namespace px::editor {

namespace {
void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}
}

bool ProjectService::Open(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        Log("Project root does not exist: " + root.string());
        return false;
    }
    m_context.root = root;
    m_context.manifest = ProjectManifest{};

    std::ifstream in(m_context.ManifestPath());
    if (in) {
        Json j = Json::parse(in, nullptr, false);
        if (!j.is_discarded()) {
            ProjectManifest& m = m_context.manifest;
            m.name = j.value("name", m.name);
            m.gameWidth = j.value("gameWidth", m.gameWidth);
            m.gameHeight = j.value("gameHeight", m.gameHeight);
            m.startUI = j.value("startUi", m.startUI);
            m.startScript = j.value("startScript", m.startScript);
            m.theme = j.value("theme", m.theme);
            m.encrypt = j.value("encrypt", m.encrypt);
            m.encryptKey = j.value("encryptKey", m.encryptKey);
            m.singleFile = j.value("singleFile", m.singleFile);
        }
    }
    Log("Opened project: " + m_context.manifest.name + " (" + root.string() + ")");
    return true;
}

bool ProjectService::Create(const std::filesystem::path& root, const std::string& name,
                            const std::filesystem::path& fontSource) {
    std::error_code ec;
    for (const char* sub : { "Data/UI", "Data/Script", "Data/Image/Background",
                             "Data/Image/Character", "Data/Image/CG", "Data/Audio/Music",
                             "Data/Audio/SFX", "Data/Audio/Voice", "Data/Font" }) {
        std::filesystem::create_directories(root / sub, ec);
    }

    const std::string font = "Data/Font/NotoSansTC-Bold.ttf";
    if (!fontSource.empty() && std::filesystem::exists(fontSource)) {
        std::filesystem::copy_file(fontSource, root / font,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    WriteText(root / "Data/UI/title.pxui",
              "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
              "  \"background\": { \"color\": [12,14,20,255] },\n  \"nodes\": [\n"
              "    { \"id\": \"title\", \"type\": \"text\", \"anchor\": \"top\", \"rect\": [0,120,1280,90], \"order\": 1, \"text\": \"" +
                  name + "\", \"fontSize\": 60, \"font\": \"" + font +
                  "\", \"align\": \"center\", \"textColor\": [245,248,255,255] },\n"
              "    { \"id\": \"start\", \"type\": \"button\", \"anchor\": \"center\", \"rect\": [-150,0,300,58], \"order\": 2, \"text\": \"Start\", \"font\": \"" +
                  font +
                  "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"scene.start\" },\n"
              "    { \"id\": \"quit\", \"type\": \"button\", \"anchor\": \"center\", \"rect\": [-150,75,300,58], \"order\": 3, \"text\": \"Quit\", \"font\": \"" +
                  font +
                  "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [87,40,40,240], \"actionType\": \"app.quit\" }\n  ]\n}\n");

    WriteText(root / "Data/UI/hud.pxui",
              "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n  \"background\": { \"color\": [0,0,0,0] },\n  \"nodes\": [\n"
              "    { \"id\": \"dbox\", \"type\": \"dialogue_box\", \"anchor\": \"bottomleft\", \"rect\": [60,-220,1160,180], \"order\": 1, \"bgColor\": [12,16,26,220], \"radius\": 16, \"borderTopHeight\": 3, \"font\": \"" +
                  font + "\", \"fontSize\": 30 },\n"
              "    { \"id\": \"choices\", \"type\": \"choice_list\", \"anchor\": \"center\", \"rect\": [-280,-40,560,420], \"order\": 2, \"bgColor\": [25,38,51,230], \"hoverColor\": [40,71,87,245], \"font\": \"" +
                  font + "\", \"fontSize\": 28 }\n  ]\n}\n");

    WriteText(root / "Data/Script/start.pds",
              "// " + name + " - starter script\n[name speaker=\"\"]\nWelcome to " + name +
                  "!\nThis is your first scene. Edit it in the Node Editor.\n");

    m_context.root = root;
    m_context.manifest = ProjectManifest{};
    m_context.manifest.name = name;
    m_context.manifest.startUI = "Data/UI/title.pxui";
    m_context.manifest.startScript = "start.pds";
    SaveManifest();
    Log("Created project '" + name + "' at " + root.string());
    return true;
}

bool ProjectService::SaveManifest() const {
    if (!m_context.IsOpen()) {
        return false;
    }
    const ProjectManifest& m = m_context.manifest;
    Json j;
    j["name"] = m.name;
    j["version"] = m.version;
    j["gameWidth"] = m.gameWidth;
    j["gameHeight"] = m.gameHeight;
    j["startUi"] = m.startUI;
    j["startScript"] = m.startScript;
    j["theme"] = m.theme;
    j["encrypt"] = m.encrypt;
    j["encryptKey"] = m.encryptKey;
    j["singleFile"] = m.singleFile;
    std::ofstream out(m_context.ManifestPath());
    if (!out) {
        return false;
    }
    out << j.dump(2);
    return true;
}

}
