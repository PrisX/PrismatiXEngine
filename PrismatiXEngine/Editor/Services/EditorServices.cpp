#include "Editor/Services/EditorServices.h"

#include <fstream>

namespace px::editor {

namespace {
std::string ToForward(std::filesystem::path p) {
    std::string s = p.generic_string();
    return s;
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
            m.startUi = j.value("startUi", m.startUi);
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

namespace {
void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}
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
              "// " + name + " — starter script\n[name speaker=\"\"]\nWelcome to " + name +
                  "!\nThis is your first scene. Edit it in the Node Editor.\n");

    m_context.root = root;
    m_context.manifest = ProjectManifest{};
    m_context.manifest.name = name;
    m_context.manifest.startUi = "Data/UI/title.pxui";
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
    j["startUi"] = m.startUi;
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

std::string AssetDatabase::Classify(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(::tolower(c));
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp")
        return "image";
    if (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".opus")
        return "audio";
    if (ext == ".pds") return "script";
    if (ext == ".pxui") return "ui";
    if (ext == ".ttf" || ext == ".otf") return "font";
    if (ext == ".lua") return "lua";
    return "other";
}

void AssetDatabase::Scan(const ProjectContext& context) {
    m_assets.clear();
    const std::filesystem::path data = context.DataRoot();
    if (!std::filesystem::exists(data)) {
        Log("No Data/ folder to scan.");
        return;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(data, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path& abs = it->path();
        const std::string ext = abs.extension().string();
        if (ext == ".meta") continue;
        AssetRecord rec;
        rec.absolutePath = abs;
        rec.runtimePath = ToForward(std::filesystem::relative(abs, context.root, ec));
        rec.type = Classify(abs);
        rec.size = std::filesystem::file_size(abs, ec);
        m_assets.push_back(std::move(rec));
    }
    Log("Scanned " + std::to_string(m_assets.size()) + " assets.");
}

std::vector<AssetRecord> AssetDatabase::Filter(std::string_view text, std::string_view type) const {
    std::vector<AssetRecord> out;
    for (const AssetRecord& r : m_assets) {
        if (!type.empty() && type != "all" && r.type != type) continue;
        if (!text.empty() && r.runtimePath.find(text) == std::string::npos) continue;
        out.push_back(r);
    }
    return out;
}

void DocumentRegistry::Upsert(DocumentInfo info) {
    for (DocumentInfo& d : m_documents) {
        if (d.path == info.path) {
            d = std::move(info);
            return;
        }
    }
    m_documents.push_back(std::move(info));
}

void DocumentRegistry::SetDirty(const std::filesystem::path& path, bool dirty) {
    for (DocumentInfo& d : m_documents) {
        if (d.path == path) {
            d.dirty = dirty;
            return;
        }
    }
}

void UndoStack::Record(Command cmd) {
    m_undo.push_back(std::move(cmd));
    m_redo.clear();
}
bool UndoStack::Undo() {
    if (m_undo.empty()) return false;
    Command c = std::move(m_undo.back());
    m_undo.pop_back();
    if (c.undo) c.undo();
    m_redo.push_back(std::move(c));
    return true;
}
bool UndoStack::Redo() {
    if (m_redo.empty()) return false;
    Command c = std::move(m_redo.back());
    m_redo.pop_back();
    if (c.redo) c.redo();
    m_undo.push_back(std::move(c));
    return true;
}
void UndoStack::Clear() {
    m_undo.clear();
    m_redo.clear();
}

}
