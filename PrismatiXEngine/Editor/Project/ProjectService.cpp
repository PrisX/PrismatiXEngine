#include "Editor/Project/ProjectService.h"

#include <fstream>

namespace px::editor {

namespace {
void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

const char kDefaultFont[] = "Data/Font/NotoSansTC-Bold.ttf";

std::string TitleTemplate(const std::string& name, const std::string& font) {
    return "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
           "  \"background\": { \"color\": [12,14,20,255] },\n  \"nodes\": [\n"
           "    { \"id\": \"title\", \"type\": \"text\", \"anchor\": \"top\", \"rect\": [-640,120,1280,90], \"order\": 1, \"text\": \"" +
           name + "\", \"fontSize\": 60, \"font\": \"" + font +
           "\", \"align\": \"center\", \"textColor\": [245,248,255,255] },\n"
           "    { \"id\": \"start\", \"type\": \"button\", \"anchor\": \"center\", \"rect\": [-150,0,300,58], \"order\": 2, \"text\": \"Start\", \"font\": \"" +
           font +
           "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"scene.start\" },\n"
           "    { \"id\": \"load\", \"type\": \"button\", \"anchor\": \"center\", \"rect\": [-150,75,300,58], \"order\": 3, \"text\": \"Load\", \"font\": \"" +
           font +
           "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"load.open\" },\n"
           "    { \"id\": \"quit\", \"type\": \"button\", \"anchor\": \"center\", \"rect\": [-150,150,300,58], \"order\": 4, \"text\": \"Quit\", \"font\": \"" +
           font +
           "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [87,40,40,240], \"actionType\": \"app.quit\" }\n  ]\n}\n";
}

std::string HudTemplate(const std::string& font) {
    return "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n  \"background\": { \"color\": [0,0,0,0] },\n  \"nodes\": [\n"
           "    { \"id\": \"dbox\", \"type\": \"dialogue_box\", \"anchor\": \"bottomleft\", \"rect\": [60,-220,1160,180], \"order\": 1, \"bgColor\": [12,16,26,220], \"radius\": 16, \"borderTopHeight\": 3, \"font\": \"" +
           font + "\", \"fontSize\": 30 },\n"
           "    { \"id\": \"choices\", \"type\": \"choice_list\", \"anchor\": \"center\", \"rect\": [-280,-40,560,420], \"order\": 2, \"bgColor\": [25,38,51,230], \"hoverColor\": [40,71,87,245], \"font\": \"" +
           font + "\", \"fontSize\": 28 }\n  ]\n}\n";
}

std::string StartScriptTemplate(const std::string& name) {
    return "// " + name + " - starter script\n[name speaker=\"\"]\nWelcome to " + name +
           "!\nThis is your first scene. Edit it in the Node Editor.\n";
}

std::string SaveLoadTemplate(const std::string& font) {
    return "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
           "  \"background\": { \"color\": [10,12,18,235] },\n  \"nodes\": [\n"
           "    { \"id\": \"header\", \"type\": \"text\", \"anchor\": \"top\", \"rect\": [-640,30,1280,50], \"order\": 1, \"text\": \"Save / Load\", \"fontSize\": 38, \"font\": \"" +
           font + "\", \"align\": \"center\", \"textColor\": [245,248,255,255] },\n"
           "    { \"id\": \"saves\", \"type\": \"save_grid\", \"anchor\": \"topleft\", \"rect\": [140,110,1000,520], \"order\": 2, \"bind\": \"saves\", \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"font\": \"" +
           font + "\", \"fontSize\": 22 },\n"
           "    { \"id\": \"back\", \"type\": \"button\", \"anchor\": \"bottom\", \"rect\": [-150,-90,300,52], \"order\": 3, \"text\": \"Back\", \"font\": \"" +
           font +
           "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"screen.close\" }\n  ]\n}\n";
}

std::string GalleryTemplate(const std::string& font) {
    return "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
           "  \"background\": { \"color\": [10,12,18,235] },\n  \"nodes\": [\n"
           "    { \"id\": \"header\", \"type\": \"text\", \"anchor\": \"top\", \"rect\": [-640,30,1280,50], \"order\": 1, \"text\": \"Gallery\", \"fontSize\": 38, \"font\": \"" +
           font + "\", \"align\": \"center\", \"textColor\": [245,248,255,255] },\n"
           "    { \"id\": \"gallery\", \"type\": \"gallery_grid\", \"anchor\": \"topleft\", \"rect\": [80,110,1120,500], \"order\": 2, \"bind\": \"gallery\", \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"font\": \"" +
           font + "\", \"fontSize\": 20 },\n"
           "    { \"id\": \"back\", \"type\": \"button\", \"anchor\": \"bottom\", \"rect\": [-150,-80,300,52], \"order\": 3, \"text\": \"Back\", \"font\": \"" +
           font +
           "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"screen.close\" }\n  ]\n}\n";
}

std::string SettingsTemplate(const std::string& font) {
    auto row = [&](const char* id, const char* label, const char* downAct, const char* upAct,
                   int y) {
        return "    { \"id\": \"" + std::string(id) + "_label\", \"type\": \"text\", \"anchor\": \"topleft\", \"rect\": [340,"
               + std::to_string(y) + ",260,44], \"order\": 2, \"text\": \"" + label +
               "\", \"font\": \"" + font + "\", \"fontSize\": 26, \"align\": \"left\", \"textColor\": [225,232,245,255] },\n"
               "    { \"id\": \"" + id + "_down\", \"type\": \"button\", \"anchor\": \"topleft\", \"rect\": [620," + std::to_string(y) +
               ",52,44], \"order\": 2, \"text\": \"-\", \"font\": \"" + font +
               "\", \"radius\": 8, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"" + downAct + "\" },\n"
               "    { \"id\": \"" + id + "_val\", \"type\": \"text\", \"anchor\": \"topleft\", \"rect\": [680," + std::to_string(y) +
               ",140,44], \"order\": 2, \"text\": \"--\", \"font\": \"" + font +
               "\", \"fontSize\": 26, \"align\": \"center\", \"textColor\": [245,248,255,255] },\n"
               "    { \"id\": \"" + id + "_up\", \"type\": \"button\", \"anchor\": \"topleft\", \"rect\": [828," + std::to_string(y) +
               ",52,44], \"order\": 2, \"text\": \"+\", \"font\": \"" + font +
               "\", \"radius\": 8, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"" + upAct + "\" },\n";
    };
    std::string nodes;
    nodes += row("bgm", "BGM", "set.bgm.down", "set.bgm.up", 140);
    nodes += row("se", "SE", "set.se.down", "set.se.up", 210);
    nodes += row("voice", "Voice", "set.voice.down", "set.voice.up", 280);
    nodes += row("speed", "Text Speed", "set.speed.down", "set.speed.up", 350);
    nodes += "    { \"id\": \"skipread_label\", \"type\": \"text\", \"anchor\": \"topleft\", \"rect\": [340,420,260,44], \"order\": 2, \"text\": \"Skip Read Only\", \"font\": \"" +
             font + "\", \"fontSize\": 26, \"align\": \"left\", \"textColor\": [225,232,245,255] },\n"
             "    { \"id\": \"skipread_val\", \"type\": \"button\", \"anchor\": \"topleft\", \"rect\": [620,420,260,44], \"order\": 2, \"text\": \"ON\", \"font\": \"" +
             font +
             "\", \"radius\": 8, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"set.skipread.toggle\" },\n";
    return "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
           "  \"background\": { \"color\": [10,12,18,235] },\n  \"nodes\": [\n"
           "    { \"id\": \"header\", \"type\": \"text\", \"anchor\": \"top\", \"rect\": [-640,40,1280,50], \"order\": 1, \"text\": \"Settings\", \"fontSize\": 38, \"font\": \"" +
           font + "\", \"align\": \"center\", \"textColor\": [245,248,255,255] },\n" +
           nodes +
           "    { \"id\": \"back\", \"type\": \"button\", \"anchor\": \"bottom\", \"rect\": [-150,-90,300,52], \"order\": 3, \"text\": \"Back\", \"font\": \"" +
           font +
           "\", \"radius\": 10, \"bgColor\": [25,38,51,220], \"hoverColor\": [40,71,87,240], \"actionType\": \"screen.close\" }\n  ]\n}\n";
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
    else {
        // First time opening this folder as a project: persist defaults so the
        // player and editor agree on the entry point from now on.
        SaveManifest();
        Log("Created project.prismatix.json with defaults.");
    }
    Log("Opened project: " + m_context.manifest.name + " (" + root.string() + ")");
    return true;
}

bool ProjectService::Create(const std::filesystem::path& root, const std::string& name,
                            const std::filesystem::path& fontSource) {
    m_context.root = root;
    m_context.manifest = ProjectManifest{};
    m_context.manifest.name = name;
    m_context.manifest.startUI = "Data/UI/title.pxui";
    m_context.manifest.startScript = "start.pds";

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    EnsureEssentials(fontSource);
    SaveManifest();
    Log("Created project '" + name + "' at " + root.string());
    return true;
}

std::vector<std::string> ProjectService::EnsureEssentials(const std::filesystem::path& fontSource) {
    std::vector<std::string> created;
    if (!m_context.IsOpen()) {
        return created;
    }
    const std::filesystem::path& root = m_context.root;
    const ProjectManifest& m = m_context.manifest;

    std::error_code ec;
    for (const char* sub : { "Data/UI", "Data/Script", "Data/Image/Background",
                             "Data/Image/Character", "Data/Image/CG", "Data/Audio/Music",
                             "Data/Audio/SFX", "Data/Audio/Voice", "Data/Font" }) {
        std::filesystem::create_directories(root / sub, ec);
    }

    if (!std::filesystem::exists(root / kDefaultFont) && !fontSource.empty() &&
        std::filesystem::exists(fontSource)) {
        if (std::filesystem::copy_file(fontSource, root / kDefaultFont,
                                       std::filesystem::copy_options::skip_existing, ec)) {
            created.push_back(kDefaultFont);
        }
    }

    const std::filesystem::path titlePath = root / m.startUI;
    if (!std::filesystem::exists(titlePath)) {
        std::filesystem::create_directories(titlePath.parent_path(), ec);
        WriteText(titlePath, TitleTemplate(m.name, kDefaultFont));
        created.push_back(m.startUI);
    }
    if (!std::filesystem::exists(root / "Data/UI/hud.pxui")) {
        WriteText(root / "Data/UI/hud.pxui", HudTemplate(kDefaultFont));
        created.push_back("Data/UI/hud.pxui");
    }
    if (!std::filesystem::exists(root / "Data/UI/saveload.pxui")) {
        WriteText(root / "Data/UI/saveload.pxui", SaveLoadTemplate(kDefaultFont));
        created.push_back("Data/UI/saveload.pxui");
    }
    if (!std::filesystem::exists(root / "Data/UI/settings.pxui")) {
        WriteText(root / "Data/UI/settings.pxui", SettingsTemplate(kDefaultFont));
        created.push_back("Data/UI/settings.pxui");
    }
    if (!std::filesystem::exists(root / "Data/UI/gallery.pxui")) {
        WriteText(root / "Data/UI/gallery.pxui", GalleryTemplate(kDefaultFont));
        created.push_back("Data/UI/gallery.pxui");
    }

    const std::filesystem::path scriptPath =
        m.startScript.find('/') != std::string::npos ? root / m.startScript
                                                     : root / "Data/Script" / m.startScript;
    if (!std::filesystem::exists(scriptPath)) {
        std::filesystem::create_directories(scriptPath.parent_path(), ec);
        WriteText(scriptPath, StartScriptTemplate(m.name));
        created.push_back(m.startScript);
    }

    for (const std::string& file : created) {
        Log("Scaffolded missing file: " + file);
    }
    return created;
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
