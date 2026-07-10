#include "Editor/Application/EditorApp.h"

#include "Engine/VN/PDS/Parser.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace px::editor {
namespace {
std::filesystem::path PlayerExecutableName() {
#ifdef _WIN32
    return "PrismatiXPlayer.exe";
#else
    return "PrismatiXPlayer";
#endif
}

#ifndef _WIN32
bool LaunchDetached(const std::filesystem::path& exe, const std::filesystem::path& workingDir) {
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        if (!workingDir.empty()) {
            const std::string cwd = workingDir.string();
            chdir(cwd.c_str());
        }
        const std::string exePath = exe.string();
        const std::string arg0 = exe.filename().string();
        execl(exePath.c_str(), arg0.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}
#endif
}

void EditorApp::OpenWorkspace() {
    OpenProject(std::filesystem::current_path());
    m_screen = Screen::Workspace;
}

void EditorApp::LoadRecentProjects() {
    m_recentProjects.clear();
    std::ifstream in(m_basePath + "PrismatiXRecent.json");
    if (!in) {
        return;
    }
    const Json j = Json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        return;
    }
    for (const Json& item : j) {
        if (item.is_string()) {
            m_recentProjects.push_back(item.get<std::string>());
        }
    }
}

void EditorApp::AddRecentProject(const std::filesystem::path& root) {
    const std::string entry = root.string();
    m_recentProjects.erase(
        std::remove(m_recentProjects.begin(), m_recentProjects.end(), entry),
        m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), entry);
    if (m_recentProjects.size() > 8) {
        m_recentProjects.resize(8);
    }
    std::ofstream out(m_basePath + "PrismatiXRecent.json");
    if (out) {
        out << Json(m_recentProjects).dump(2);
    }
}

void EditorApp::OpenProject(const std::filesystem::path& root) {
    if (m_project.Open(root)) {
        AddRecentProject(root);
        const auto scaffolded = m_project.EnsureEssentials(m_basePath + "EditorAssets/UIFont.ttf");
        if (!scaffolded.empty()) {
            Log("Scaffolded " + std::to_string(scaffolded.size()) + " missing project file(s).");
        }
        m_assets.Scan(m_project.Context());
        if (m_preview) {
            m_preview->SetProjectRoot(root.string());
            m_preview->LoadUI(m_project.Context().manifest.startUI);
        }
        m_scriptDocs.clear();
        m_activeDoc = -1;
        OpenDocTab(m_project.Context().manifest.startScript);

        m_dbPath = (root / "Data" / "database.json").string();
        if (std::ifstream dbin(m_dbPath); dbin) {
            std::stringstream ss;
            ss << dbin.rdbuf();
            if (!m_database.Load(ss.str())) {
                m_database.SeedDefault();
            }
        }
        else {
            m_database.SeedDefault();
            // Don't seed a phantom chapter: point chapter 1 at the project's real
            // entry script instead of the hardcoded sample.
            if (!m_database.chapters.empty()) {
                m_database.chapters[0].script = m_project.Context().manifest.startScript;
            }
            std::ofstream out(m_dbPath);
            out << m_database.Serialize();
            Log("Created Data/database.json");
        }
        m_dbDirty = false;
        m_dbBaseline = m_database.Serialize();
        m_dbEditPending = false;
        m_undo.Clear();

        m_flow.SetOpenCallback([this](const std::string& script) { OpenDocTab(script); });
        m_flow.SetCreateChapterCallback([this](ImVec2 canvasPosition) { CreateFlowChapter(canvasPosition); });
        m_flow.SetEntryChangedCallback([this](const std::string& script) {
            m_project.Context().manifest.startScript = script;
            m_project.SaveManifest();
            Log("Entry point set to " + script);
        });
        m_flow.SetLayoutChangedCallback([this](const std::string& script, ImVec2 position) {
            if (script.empty()) {
                m_database.entryFlowX = position.x;
                m_database.entryFlowY = position.y;
            } else if (px::project::Chapter* ch = m_database.FindChapterByScript(script)) {
                ch->flowX = position.x;
                ch->flowY = position.y;
            }
            m_dbDirty = true;
            m_dbEditPending = true;  // node drags become one undo entry on release
        });
        m_flow.SetScriptChangedCallback([this](const std::string& chapterId, const std::string& script) {
            for (px::project::Chapter& ch : m_database.chapters) {
                if (ch.id == chapterId) {
                    const bool wasEntry = ch.script == m_project.Context().manifest.startScript;
                    ch.script = script;
                    if (wasEntry) {
                        m_project.Context().manifest.startScript = script;
                        m_project.SaveManifest();
                        m_flow.SetEntryScript(script);
                    }
                    break;
                }
            }
            m_dbDirty = true;
            m_flowStale = true;
            RefreshProblems();
        });
        m_flow.SetCreateScriptCallback([this](const std::string& script) { CreateScriptFile(script); });
        m_flow.SetLinkAddedCallback([this](const std::string& from, const std::string& to) {
            AddJumpToScript(from, to);
        });
        m_flow.SetLinkRemovedCallback([this](const std::string& from, const std::string& to) {
            RemoveJumpFromScript(from, to);
        });
        m_flow.SetChapterRemovedCallback([this](const std::string& chapterId) {
            const std::string before = m_database.Serialize();
            m_database.chapters.erase(
                std::remove_if(m_database.chapters.begin(), m_database.chapters.end(),
                               [&](const px::project::Chapter& ch) { return ch.id == chapterId; }),
                m_database.chapters.end());
            const std::string after = m_database.Serialize();
            RecordDatabaseUndo("Delete chapter " + chapterId, before, after);
            m_dbBaseline = after;
            m_dbDirty = true;
            RefreshProblems();
            Log("Flow: deleted chapter " + chapterId);
        });
        m_flow.SetTitleChangedCallback([this](const std::string& chapterId, const std::string& title) {
            for (px::project::Chapter& ch : m_database.chapters) {
                if (ch.id == chapterId) {
                    ch.title = title;
                    break;
                }
            }
            m_dbDirty = true;
        });
        m_flow.SetEntryScript(m_project.Context().manifest.startScript);
        m_flow.Rebuild(m_database, root);

        m_scripts.SetOnCommandsChanged([this](const std::vector<CustomCommandDef>& cmds) {
            m_customCommands = cmds;
            for (auto& doc : m_scriptDocs) {
                doc->SetCustomCommands(cmds);
            }
        });
        m_scripts.SetProject(&m_project.Context());

        RefreshProblems();
    }
}

NodeGraphEditor* EditorApp::ActiveDocPtr() {
    if (m_scriptDocs.empty()) {
        return nullptr;
    }
    m_activeDoc = std::clamp(m_activeDoc, 0, static_cast<int>(m_scriptDocs.size()) - 1);
    return m_scriptDocs[static_cast<std::size_t>(m_activeDoc)].get();
}

void EditorApp::ConfigureDoc(NodeGraphEditor& doc) {
    doc.SetHeaderTexture(m_nodeHeaderTex, m_nodeHeaderW, m_nodeHeaderH);
    doc.SetSelectedResourceCallback([this] { return m_selectedAsset; });
    doc.SetCustomCommands(m_customCommands);
    doc.SetBreakpointHooks(
        [this]() -> const std::set<int>* {
            return m_preview ? &m_preview->VMRef().Breakpoints() : nullptr;
        },
        [this](int line) {
            if (m_preview) m_preview->VMRef().ToggleBreakpoint(line);
        });
}

NodeGraphEditor* EditorApp::OpenDocTab(const std::string& runtimePath) {
    if (runtimePath.empty() || !m_project.Context().IsOpen()) {
        return nullptr;
    }
    const std::string wantedName = std::filesystem::path(runtimePath).filename().string();
    for (std::size_t i = 0; i < m_scriptDocs.size(); ++i) {
        const std::string current = m_scriptDocs[i]->CurrentRuntimePath();
        if (current == runtimePath ||
            std::filesystem::path(current).filename().string() == wantedName) {
            m_activeDoc = static_cast<int>(i);
            m_focusDocRequest = m_activeDoc;
            return m_scriptDocs[i].get();
        }
    }
    auto doc = std::make_unique<NodeGraphEditor>(NodeGraphEditor::GraphKind::PDSDialogue,
                                                 [this](const std::string& m) { Log(m); });
    ConfigureDoc(*doc);
    doc->SetProject(&m_project.Context());
    if (!doc->OpenDocument(runtimePath)) {
        Log("Could not open script: " + runtimePath);
        return nullptr;
    }
    m_scriptDocs.push_back(std::move(doc));
    m_activeDoc = static_cast<int>(m_scriptDocs.size()) - 1;
    m_focusDocRequest = m_activeDoc;
    return m_scriptDocs.back().get();
}

namespace {
// Matches `[jump target="X"]` lines whose target resolves to `script` (with or
// without the .pds extension).
bool IsJumpLineTo(const std::string& line, const std::string& script) {
    if (line.find("[jump") == std::string::npos) {
        return false;
    }
    const std::size_t key = line.find("target=\"");
    if (key == std::string::npos) {
        return false;
    }
    const std::size_t start = key + 8;
    const std::size_t end = line.find('"', start);
    if (end == std::string::npos) {
        return false;
    }
    const std::string target = line.substr(start, end - start);
    return target == script || target + ".pds" == script;
}
}

void EditorApp::AddJumpToScript(const std::string& fromScript, const std::string& toScript) {
    const std::filesystem::path path = m_project.Context().root / "Data" / "Script" / fromScript;
    std::ifstream in(path);
    if (!in) {
        Log("Flow: cannot open " + fromScript + " to add jump.");
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (IsJumpLineTo(line, toScript)) {
            return;  // Already linked in the script.
        }
    }
    in.close();

    std::ofstream out(path, std::ios::app);
    out << "\n[jump target=\"" << toScript << "\"]\n";
    Log("Flow: added [jump] " + fromScript + " -> " + toScript);
    for (auto& doc : m_scriptDocs) {
        doc->ReloadIfOpen(fromScript);
    }
}

void EditorApp::RemoveJumpFromScript(const std::string& fromScript, const std::string& toScript) {
    const std::filesystem::path path = m_project.Context().root / "Data" / "Script" / fromScript;
    std::ifstream in(path);
    if (!in) {
        return;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    in.close();

    std::vector<std::string> kept;
    kept.reserve(lines.size());
    bool removed = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (IsJumpLineTo(lines[i], toScript)) {
            // Drop the //@node meta line that belongs to this jump command.
            if (!kept.empty() && kept.back().rfind("//@node", 0) == 0) {
                kept.pop_back();
            }
            removed = true;
            continue;
        }
        kept.push_back(lines[i]);
    }
    if (!removed) {
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& l : kept) {
        out << l << "\n";
    }
    Log("Flow: removed [jump] " + fromScript + " -> " + toScript);
    for (auto& doc : m_scriptDocs) {
        doc->ReloadIfOpen(fromScript);
    }
}

void EditorApp::ApplyDatabaseSnapshot(const std::string& json) {
    m_database.Load(json);
    m_dbBaseline = json;
    m_dbDirty = true;
    m_flowStale = true;
    RefreshProblems();
}

void EditorApp::RecordDatabaseUndo(const std::string& label, std::string before,
                                   std::string after) {
    if (before == after) {
        return;
    }
    m_undo.Record(UndoStack::Command{
        label,
        [this, snapshot = std::move(before)] { ApplyDatabaseSnapshot(snapshot); },
        [this, snapshot = std::move(after)] { ApplyDatabaseSnapshot(snapshot); },
    });
}

namespace {
int ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty() || from == to) {
        return 0;
    }
    int count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        ++count;
    }
    return count;
}
}

int EditorApp::UpdateAssetReferences(const std::string& oldRel, const std::string& newRel) {
    if (oldRel.empty() || newRel.empty() || oldRel == newRel ||
        !m_project.Context().IsOpen()) {
        return 0;
    }
    const std::filesystem::path root = m_project.Context().root;
    const std::string oldName = std::filesystem::path(oldRel).filename().string();
    const std::string newName = std::filesystem::path(newRel).filename().string();
    const bool isScript = std::filesystem::path(oldRel).extension() == ".pds";

    int touched = 0;
    std::vector<std::string> changedScripts;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root / "Data", ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path& file = it->path();
        const std::string ext = file.extension().string();
        if (ext != ".pds" && ext != ".pxui" && ext != ".lua") continue;
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        std::string text = ss.str();

        int replaced = ReplaceAll(text, oldRel, newRel);
        // Bare-filename references (script jump targets, char/bgm shorthand).
        if (oldName != newName) {
            replaced += ReplaceAll(text, "\"" + oldName + "\"", "\"" + newName + "\"");
        }
        if (replaced == 0) continue;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        ++touched;
        if (ext == ".pds") {
            changedScripts.push_back(file.filename().string());
        }
    }

    // Database and manifest live in memory; update there instead of on disk.
    bool dbChanged = false;
    for (auto& ch : m_database.chapters) {
        if (isScript && (ch.script == oldName || ch.script == oldRel)) {
            ch.script = newName;
            dbChanged = true;
        }
    }
    for (auto& g : m_database.gallery) {
        if (g.image == oldRel) { g.image = newRel; dbChanged = true; }
        if (g.thumbnail == oldRel) { g.thumbnail = newRel; dbChanged = true; }
    }
    if (dbChanged) {
        m_dbDirty = true;
        m_flowStale = true;
    }
    ProjectManifest& manifest = m_project.Context().manifest;
    bool manifestChanged = false;
    if (isScript && manifest.startScript == oldName) {
        manifest.startScript = newName;
        manifestChanged = true;
    }
    if (manifest.startUI == oldRel) {
        manifest.startUI = newRel;
        manifestChanged = true;
    }
    if (manifestChanged) {
        m_project.SaveManifest();
        m_flow.SetEntryScript(manifest.startScript);
    }

    for (const std::string& script : changedScripts) {
        for (auto& doc : m_scriptDocs) {
            doc->ReloadIfOpen(script);
        }
    }
    return touched;
}

const std::vector<std::string>& EditorApp::ScriptFileNames() {
    if (m_scriptNameRevision == m_assets.Revision()) {
        return m_scriptNameCache;
    }
    m_scriptNameCache.clear();
    for (const AssetRecord& rec : m_assets.Assets()) {
        if (rec.type == "script") {
            m_scriptNameCache.push_back(
                std::filesystem::path(rec.runtimePath).filename().string());
        }
    }
    std::sort(m_scriptNameCache.begin(), m_scriptNameCache.end());
    m_scriptNameCache.erase(std::unique(m_scriptNameCache.begin(), m_scriptNameCache.end()),
                            m_scriptNameCache.end());
    m_scriptNameRevision = m_assets.Revision();
    return m_scriptNameCache;
}

void EditorApp::CreateScriptFile(const std::string& script) {
    if (!m_project.Context().IsOpen() || script.empty()) {
        return;
    }
    const std::filesystem::path path = m_project.Context().root / "Data" / "Script" / script;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (!std::filesystem::exists(path)) {
        std::ofstream out(path);
        out << "# " << script << "\n\n[text]\nNew line\n\n";
        Log("Created script " + script);
    }
    m_assets.Scan(m_project.Context());
    m_flowStale = true;
    RefreshProblems();
}

void EditorApp::CreateFlowChapter(ImVec2 canvasPosition) {
    if (!m_project.Context().IsOpen()) {
        Log("Flow: open a project before adding chapters.");
        return;
    }

    int index = static_cast<int>(m_database.chapters.size()) + 1;
    std::string id;
    std::string title;
    std::string script;
    const auto used = [&](const std::string& candidateId, const std::string& candidateScript) {
        for (const px::project::Chapter& chapter : m_database.chapters) {
            if (chapter.id == candidateId || chapter.script == candidateScript) {
                return true;
            }
        }
        return false;
    };
    do {
        id = "chapter" + std::to_string(index);
        title = "Chapter " + std::to_string(index);
        script = id + ".pds";
        ++index;
    } while (used(id, script));

    const std::string before = m_database.Serialize();
    px::project::Chapter chapter{ id, title, script, false };
    chapter.flowX = canvasPosition.x;
    chapter.flowY = canvasPosition.y;
    m_database.chapters.push_back(std::move(chapter));
    const std::string after = m_database.Serialize();
    RecordDatabaseUndo("Add chapter " + id, before, after);
    m_dbBaseline = after;
    m_dbDirty = true;

    const std::filesystem::path scriptDir = m_project.Context().root / "Data" / "Script";
    std::error_code ec;
    std::filesystem::create_directories(scriptDir, ec);
    const std::filesystem::path scriptPath = scriptDir / script;
    if (!std::filesystem::exists(scriptPath)) {
        std::ofstream out(scriptPath);
        out << "# " << title << "\n\n[text]\nNew line\n\n";
    }

    m_flow.Rebuild(m_database, m_project.Context().root);
    m_flow.SetNodePositionByScript(script, canvasPosition);
    RefreshProblems();
    Log("Flow: added " + title + " (" + script + ")");
}


void EditorApp::SaveAll() {
    if (m_designer.Dirty()) m_designer.Save();
    // Save the open script unconditionally: a freshly imported document is not
    // "dirty", but Save All should still normalize it to the compiled form.
    for (auto& doc : m_scriptDocs) {
        if (!doc->CurrentRuntimePath().empty()) doc->Save();
    }
    if (m_dbDirty && !m_dbPath.empty()) {
        std::ofstream out(m_dbPath);
        out << m_database.Serialize();
        m_dbDirty = false;
    }
    m_project.SaveManifest();
    RefreshProblems();
    Log("Saved all documents.");
}

void EditorApp::RunBuild() {
    if (!m_project.Context().IsOpen()) {
        Log("Build: no project open.");
        return;
    }
    ProjectManifest& m = m_project.Context().manifest;
    BuildService builder([this](const std::string& s) { Log(s); });
    BuildOptions opt;
    opt.projectRoot = m_project.Context().root;
    opt.outputDir = m_project.Context().ExportRoot();
    if (const char* base = SDL_GetBasePath()) {
        opt.playerExe = std::filesystem::path(base) / PlayerExecutableName();
    }
    opt.title = m.name;
    opt.startUI = m.startUI;
    opt.startScript = m.startScript;
    opt.key = m.encryptKey;
    opt.encrypt = m.encrypt;
    opt.gameWidth = m.gameWidth;
    opt.gameHeight = m.gameHeight;
    if (!builder.Build(opt)) {
        Log("Build failed.");
    }
}

void EditorApp::RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir) {
    if (!std::filesystem::exists(exe)) {
        Log("Run failed: player not found at " + exe.string());
        return;
    }
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    const std::wstring cwd = workingDir.wstring();
    if (CreateProcessW(exe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Log("Launched player (cwd=" + workingDir.string() + ")");
    }
    else {
        Log("Run failed: could not start the player process.");
    }
#else
    if (LaunchDetached(exe, workingDir)) {
        Log("Launched player (cwd=" + workingDir.string() + ")");
    }
    else {
        Log("Run failed: could not start the player process.");
    }
#endif
}

void EditorApp::RunDev() {
    if (!m_project.Context().IsOpen()) {
        Log("Run: no project open.");
        return;
    }
    SaveAll();
    RunPlayer(std::filesystem::path(m_basePath) / PlayerExecutableName(), m_project.Context().root);
}

void EditorApp::RunPackaged() {
    const std::filesystem::path exportRoot = m_project.Context().ExportRoot();
    RunPlayer(exportRoot / PlayerExecutableName(), exportRoot);
}

void EditorApp::OpenInExplorer(const std::filesystem::path& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    const pid_t pid = fork();
    if (pid == 0) {
        const std::string p = path.string();
        execlp("open", "open", p.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
#else
    const pid_t pid = fork();
    if (pid == 0) {
        const std::string p = path.string();
        execlp("xdg-open", "xdg-open", p.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
    Log("Open: " + path.string());
}


void EditorApp::RefreshProblems() {
    m_problems.clear();
    if (!m_project.Context().IsOpen()) {
        return;
    }
    const std::filesystem::path root = m_project.Context().root;
    const ProjectManifest& m = m_project.Context().manifest;

    const auto missing = [&](const std::filesystem::path& rel, const std::string& what) {
        if (!std::filesystem::exists(root / rel)) {
            m_problems.push_back("Missing " + what + ": " + rel.generic_string());
        }
    };
    missing(m.startUI, "start UI");
    if (!std::filesystem::exists(root / m.startScript) && !std::filesystem::exists(root / "Data" / "Script" / m.startScript)) {
        m_problems.push_back("Missing start script: " + m.startScript);
    }
    for (const auto& ch : m_database.chapters) {
        if (ch.script.empty()) continue;
        if (!std::filesystem::exists(root / "Data" / "Script" / ch.script) && !std::filesystem::exists(root / ch.script)) {
            m_problems.push_back("Chapter '" + ch.id + "' script not found: " + ch.script);
        }
    }
    for (const auto& g : m_database.gallery) {
        if (!g.image.empty() && !std::filesystem::exists(root / g.image)) {
            m_problems.push_back("Gallery '" + g.id + "' image not found: " + g.image);
        }
    }

    // Validate asset references inside every script (same dir conventions as VMConfig).
    const auto resolveRef = [&](const std::string& dir, const std::string& file) {
        if (file.empty()) return std::string{};
        if (file.find('/') != std::string::npos || file.find(':') != std::string::npos) {
            return file;
        }
        return dir + file;
    };
    const std::filesystem::path scriptDir = root / "Data" / "Script";
    std::error_code ec;
    constexpr std::size_t kMaxProblems = 200;
    for (std::filesystem::directory_iterator it(scriptDir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (m_problems.size() >= kMaxProblems) break;
        if (!it->is_regular_file() || it->path().extension() != ".pds") continue;
        const std::string scriptName = it->path().filename().string();
        std::ifstream in(it->path());
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        const px::vn::ParsedScript parsed = px::vn::ParsePDS(ss.str());

        const auto checkRef = [&](const px::vn::Command& cmd, const std::string& dir,
                                  const std::string& file, const char* what) {
            const std::string ref = resolveRef(dir, file);
            if (ref.empty() || std::filesystem::exists(root / ref)) return;
            if (m_problems.size() < kMaxProblems) {
                m_problems.push_back(scriptName + ":" + std::to_string(cmd.line) + "  missing " +
                                     what + ": " + ref);
            }
        };
        for (const px::vn::Command& cmd : parsed.commands) {
            const std::string& t = cmd.type;
            if (t == "bg") {
                checkRef(cmd, "Data/Image/Background/", cmd.Get("file", cmd.Get("value")), "bg");
            } else if (t == "char") {
                std::string file = cmd.Get("file");
                if (file.empty()) {
                    const std::string name = cmd.Get("name", cmd.Get("id"));
                    const std::string diff =
                        cmd.Get("diff", cmd.Get("expression", cmd.Get("exp", "d")));
                    file = name + "_" + diff + ".png";
                }
                checkRef(cmd, "Data/Image/Character/", file, "character image");
            } else if (t == "cg") {
                checkRef(cmd, "Data/Image/CG/", cmd.Get("image", cmd.Get("file")), "cg");
            } else if (t == "bgm") {
                checkRef(cmd, "Data/Audio/Music/", cmd.Get("file", cmd.Get("value")), "bgm");
            } else if (t == "se") {
                checkRef(cmd, "Data/Audio/SFX/", cmd.Get("file", cmd.Get("value")), "se");
            } else if (t == "voice") {
                checkRef(cmd, "Data/Audio/Voice/", cmd.Get("file", cmd.Get("value")), "voice");
            } else if (t == "video" || t == "movie") {
                checkRef(cmd, "Data/Video/", cmd.Get("file", cmd.Get("value")), "video");
            } else if ((t == "say" || t == "text") && cmd.Has("voice")) {
                checkRef(cmd, "Data/Audio/Voice/", cmd.Get("voice"), "voice");
            }
        }
    }
}


}  // namespace px::editor
