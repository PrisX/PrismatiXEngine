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

namespace px::editor {
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
        m_flow.Rebuild(m_database, root);

        m_scripts.SetOnCommandsChanged([this](const std::vector<CustomCommandDef>& cmds) { m_nodeEditor.SetCustomCommands(cmds); });
        m_scripts.SetProject(&m_project.Context());

        RefreshProblems();
    }
}


void EditorApp::SaveAll() {
    if (m_designer.Dirty()) m_designer.Save();
    if (m_nodeEditor.Dirty()) m_nodeEditor.Save();
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
        opt.playerExe = std::filesystem::path(base) / "PrismatiXPlayer.exe";
    }
    opt.title = m.name;
    opt.startUI = m.startUI;
    opt.startScript = m.startScript;
    opt.key = m.encryptKey;
    opt.encrypt = m.encrypt;
    opt.gameWidth = m.gameWidth;
    opt.gameHeight = m.gameHeight;
    builder.Build(opt);
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
    Log("Run player is only implemented on Windows in this build.");
#endif
}

void EditorApp::RunDev() {
    if (!m_project.Context().IsOpen()) {
        Log("Run: no project open.");
        return;
    }
    SaveAll();
    RunPlayer(std::filesystem::path(m_basePath) / "PrismatiXPlayer.exe", m_project.Context().root);
}

void EditorApp::RunPackaged() {
    const std::filesystem::path exportRoot = m_project.Context().ExportRoot();
    RunPlayer(exportRoot / "PrismatiXPlayer.exe", exportRoot);
}

void EditorApp::OpenInExplorer(const std::filesystem::path& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
