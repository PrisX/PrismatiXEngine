#include "Editor/Application/EditorApp.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <SDL3/SDL.h>

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

void EditorApp::OpenProject(const std::filesystem::path& root) {
    if (m_project.Open(root)) {
        m_assets.Scan(m_project.Context());
        if (m_preview) {
            m_preview->SetProjectRoot(root.string());
            m_preview->LoadUI(m_project.Context().manifest.startUI);
        }
        m_nodeEditor.SetProject(&m_project.Context());
        m_nodeEditor.OpenDocument(m_project.Context().manifest.startScript);

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
            std::ofstream out(m_dbPath);
            out << m_database.Serialize();
            Log("Created Data/database.json");
        }
        m_dbDirty = false;

        m_flow.SetOpenCallback([this](const std::string& script) { m_nodeEditor.OpenDocument(script); });
        m_flow.SetCreateChapterCallback([this](ImVec2 canvasPosition) { CreateFlowChapter(canvasPosition); });
        m_flow.Rebuild(m_database, root);

        m_scripts.SetOnCommandsChanged([this](const std::vector<CustomCommandDef>& cmds) { m_nodeEditor.SetCustomCommands(cmds); });
        m_scripts.SetProject(&m_project.Context());

        RefreshProblems();
    }
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

    m_database.chapters.push_back(px::project::Chapter{ id, title, script, false });
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
    if (m_nodeEditor.Dirty()) m_nodeEditor.Save();
    if (m_dbDirty && !m_dbPath.empty()) {
        std::ofstream out(m_dbPath);
        out << m_database.Serialize();
        m_dbDirty = false;
    }
    m_project.SaveManifest();
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
}


}  // namespace px::editor
