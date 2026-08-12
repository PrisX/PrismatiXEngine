#include "Editor/Application/EditorApp.h"
#include "Engine/VN/GameCatalog.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/VN/Scenario/StoryMap.h"
#include "Engine/UI/UITypeRegistry.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace px::editor {
namespace {
namespace fs = std::filesystem;

std::filesystem::path PlayerExecutableName() {
#ifdef _WIN32
    return "PrismatiXPlayer.exe";
#else
    return "PrismatiXPlayer";
#endif
}

diag::Diagnostic ProjectMutationError(std::string code, const std::filesystem::path& path,
                                      std::string message, std::string details = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),
        .category="Editor.ProjectMutation",.message=std::move(message),.details=std::move(details)};
    diagnostic.source.path=path.generic_string(); return diagnostic;
}

}

std::string EditorApp::ClassifyAsset(const std::filesystem::path& path) {
    std::string extension=path.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),
        [](unsigned char value){return static_cast<char>(std::tolower(value));});
    if(extension==".png"||extension==".jpg"||extension==".jpeg"||extension==".webp"||extension==".bmp")return "image";
    if(extension==".mp3"||extension==".ogg"||extension==".wav"||extension==".flac"||extension==".opus")return "audio";
    if(extension==".pxscenario")return "script";
    if(extension==".pxscene"||extension==".pxcomponent")return "ui";
    if(extension==".pxres"||extension==".pxtheme"||extension==".pxanim")return "resource";
    if(extension==".ttf"||extension==".otf")return "font";
    if(extension==".lua")return "lua";
    return "other";
}

std::string EditorApp::AssetRuntimePath(const resource::AssetEntry& entry) const {
    std::error_code error;
    const auto relative=std::filesystem::relative(entry.sourcePath,m_project.Context().root,error);
    return error?entry.sourcePath.generic_string():relative.generic_string();
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
    const Status written =
        io::AtomicFile::WriteText(m_basePath + "PrismatiXRecent.json",
                                  Json(m_recentProjects).dump(2));
    if (!written) {
        for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
    }
}

void EditorApp::OpenProject(const std::filesystem::path& root) {
    if (m_project.Open(root)) {
        AddRecentProject(root);
        const auto scaffolded = m_project.EnsureEssentials(m_basePath + "EditorAssets/UIFont.ttf");
        (void)m_project.SaveManifest();
        if (!scaffolded.empty()) {
            Log("Scaffolded " + std::to_string(scaffolded.size()) + " missing project file(s).");
        }
        const Status identityStatus = m_assetRegistry.Scan(root);
        if (!identityStatus) m_showAssetIdentity = true;
        const Status storyLibraryStatus = m_storyLibrary.Open(
            &m_project.Context(), &m_assetRegistry,
            [this](const std::string& runtimePath) -> std::optional<ResourceRefValue> {
                const auto* asset = m_assetRegistry.FindPath(m_project.Context().root / runtimePath);
                if (!asset) asset = m_assetRegistry.FindPath(runtimePath);
                if (!asset) return std::nullopt;
                return ResourceRefValue{asset->id, std::filesystem::path(runtimePath).generic_string()};
            });
        if (!storyLibraryStatus)
            for (const auto& diagnostic : storyLibraryStatus.Diagnostics()) diag::Emit(diagnostic);

        RefreshProblems();
    }
}

void EditorApp::RefreshAfterProjectMutation() {
    const Status identities = m_assetRegistry.Scan(m_project.Context().root);
    if (!identities) m_showAssetIdentity = true;
    RefreshProblems();
}

Status EditorApp::CreateAssetWithHistory(const std::filesystem::path& absolutePath,int kind){
    const auto root=m_project.Context().root;
    std::error_code error;std::filesystem::create_directories(absolutePath.parent_path(),error);
    if(error)return Status::Fail(ProjectMutationError("PXASSETCREATE9401",absolutePath,"無法建立父資料夾",error.message()));
    if(kind==0){
        if(!std::filesystem::create_directory(absolutePath,error)||error)return Status::Fail(ProjectMutationError("PXASSETCREATE9403",absolutePath,"無法建立資料夾",error.message()));
    }else if(kind==1){
        const auto name=absolutePath.stem().string();vn::scenario::ScenarioDocument scenario;scenario.id=Uuid::Random();scenario.name=name;vn::scenario::ScenarioNode chapter{Uuid::Random(),"chapter",{{"title",name}}};vn::scenario::ScenarioNode dialogue{Uuid::Random(),"say",{{"textId",Uuid::Random().ToString()},{"speaker",std::string{}},{"value",std::string("New dialogue line")}}};scenario.entry=chapter.id;scenario.nodes={chapter,dialogue};scenario.edges.push_back({Uuid::Random(),chapter.id,"flow",dialogue.id,"in"});const Status written=io::AtomicFile::WriteText(absolutePath,vn::scenario::WriteScenario(scenario));if(!written)return written;
    }else{
        return Status::Fail(ProjectMutationError(
            "PXASSETCREATE9404", absolutePath,
            "Legacy UI Scene 建立已移至 PrismatiXStudio"));
    }
    if(kind!=0){auto registered=m_assetRegistry.RegisterAsset(root,absolutePath,ClassifyAsset(absolutePath));if(!registered){std::filesystem::remove(absolutePath,error);return Status::Fail(registered.Diagnostics());}}
    RefreshAfterProjectMutation();
    return Status::Ok();
}

void EditorApp::SaveAll() {
    if (m_storyLibrary.Dirty()) m_storyLibrary.Save();
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
    opt.startRoute = m.startRoute;
    opt.routes = m.routes;
    opt.splashes = m.splashes;
    opt.startScript = m.startScript;
    opt.key = m.encryptKey;
    opt.encrypt = m.encrypt;
    opt.gameWidth = m.gameWidth;
    opt.gameHeight = m.gameHeight;
    const auto profilePath=m_project.Context().root/".prismatix/ExportProfiles/windows-release.pxexport";
    if(std::ifstream profileStream(profilePath,std::ios::binary);profileStream){std::ostringstream profileText;profileText<<profileStream.rdbuf();auto parsed=ParseExportProfile(profileText.str(),profilePath.generic_string());if(!parsed){for(const auto& diagnostic:parsed.Diagnostics())diag::Emit(diagnostic);Log("Build failed: export profile is invalid.");return;}opt.profile=parsed.TakeValue();}
    if (!builder.Build(opt)) {
        Log("Build failed.");
    }
}

void EditorApp::RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir) {
    if (!std::filesystem::exists(exe)) {
        diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXRUN9701",.category="Editor.Run",.message="執行失敗：找不到 Player"};diagnostic.source.path=exe.generic_string();diag::Emit(std::move(diagnostic));
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
        diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXRUN9702",.category="Editor.Run",.message="執行失敗：無法啟動 Player process"};diagnostic.source.path=exe.generic_string();diag::Emit(std::move(diagnostic));
    }
#else
    if (LaunchDetached(exe, workingDir)) {
        Log("Launched player (cwd=" + workingDir.string() + ")");
    }
    else {
        diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXRUN9702",.category="Editor.Run",.message="執行失敗：無法啟動 Player process"};diagnostic.source.path=exe.generic_string();diag::Emit(std::move(diagnostic));
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
    constexpr std::size_t kMaxProblems = 200;

    const auto add = [&](diag::Severity severity, std::string code, std::string message,
                         std::string path = {}, std::string nodeId = {},
                         std::string property = {}) {
        if (m_problems.size() >= kMaxProblems) return;
        diag::Diagnostic diagnostic{.severity = severity,
                                    .code = std::move(code),
                                    .category = "Editor.ProjectValidation",
                                    .message = std::move(message)};
        diagnostic.source.path = std::move(path);
        diagnostic.source.nodeId = std::move(nodeId);
        diagnostic.source.property = std::move(property);
        m_problems.push_back(std::move(diagnostic));
    };

    const auto missing = [&](const std::filesystem::path& rel, const std::string& what) {
        if (!std::filesystem::exists(root / rel)) {
            add(diag::Severity::Error, "PXEDPROJECT5301",
                "Missing " + what + ": " + rel.generic_string(), rel.generic_string());
        }
    };
    if (m_project.Context().StartScenePath().empty())
        add(diag::Severity::Error, "PXEDPROJECT5302", "Missing start route: " + m.startRoute,
            "project.pxproject", {}, "startRoute");
    for(const auto& route:m.routes)missing(route.scene,"route '"+route.id+"'");
    if (!std::filesystem::exists(root / m.startScript)) {
        add(diag::Severity::Error, "PXEDPROJECT5303", "Missing start script: " + m.startScript,
            m.startScript, {}, "startScript");
    }

    const std::filesystem::path scriptDir = root / "Content" / "Scenario";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(scriptDir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (m_problems.size() >= kMaxProblems) break;
        if (!it->is_regular_file() || it->path().extension() != ".pxscenario") continue;
        const std::string runtimePath = it->path().lexically_relative(root).generic_string();
        std::ifstream in(it->path());
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        auto parsed = px::vn::scenario::ParseScenario(ss.str(), it->path().generic_string());
        if (!parsed) {
            for (auto diagnostic : parsed.Diagnostics()) {
                if (m_problems.size() >= kMaxProblems) break;
                diagnostic.source.path = runtimePath;
                m_problems.push_back(std::move(diagnostic));
            }
            continue;
        }
        const auto report = px::vn::scenario::ValidateScenario(parsed.Value(), px::vn::CommandRegistry::Builtins(), it->path().generic_string());
        for (auto diagnostic : report.diagnostics) {
            if (m_problems.size() >= kMaxProblems) break;
            diagnostic.source.path = runtimePath;
            m_problems.push_back(std::move(diagnostic));
        }
        const auto& catalog = m_storyLibrary.Catalog();
        const auto parameterText = [](const vn::scenario::ScenarioNode& node,
                                      const std::string& key) -> std::string {
            const auto found = node.parameters.find(key);
            if (found == node.parameters.end()) return {};
            const auto* text = found->second.TryGet<std::string>();
            return text ? *text : std::string{};
        };
        for (const auto& node : parsed.Value().nodes) {
            for (const auto& [name, value] : node.parameters)
                if (const auto* ref = value.TryGet<ResourceRefValue>();
                    ref && !ref->lastKnownPath.empty() &&
                    !std::filesystem::exists(root / ref->lastKnownPath) &&
                    m_problems.size() < kMaxProblems)
                    add(diag::Severity::Error, "PXEDSTORY5304",
                        "Missing resource " + name + ": " + ref->lastKnownPath, runtimePath,
                        node.id.ToString(), name);

            if (catalog.Characters().empty()) continue;  // Legacy name-based projects remain valid.
            const bool characterCommand = node.command == "char";
            const bool dialogueCommand = node.command == "say" || node.command == "text";
            if (!characterCommand && !dialogueCommand) continue;
            const std::string parameter = characterCommand ? "id" : "char";
            const std::string characterId = parameterText(node, parameter);
            if (characterId.empty()) continue;
            const auto* character = catalog.FindCharacter(characterId);
            if (!character) {
                add(diag::Severity::Error, "PXEDSTORY5305",
                    "Unknown character: " + characterId, runtimePath, node.id.ToString(),
                    parameter);
                continue;
            }
            if (!characterCommand || character->expressions.empty()) continue;
            const std::string expressionId = parameterText(node, "expression");
            const auto file = node.parameters.find("file");
            const auto* overrideRef = file == node.parameters.end()
                ? nullptr : file->second.TryGet<ResourceRefValue>();
            const bool hasOverride = overrideRef && !overrideRef->lastKnownPath.empty();
            const std::string selectedExpression = expressionId.empty()
                ? character->defaultExpression : expressionId;
            if (!hasOverride && !catalog.FindExpression(*character, selectedExpression))
                add(diag::Severity::Error, "PXEDSTORY5306",
                    "Unknown expression '" + selectedExpression + "' for character " + characterId,
                    runtimePath, node.id.ToString(), "expression");
        }
    }
}


}  // namespace px::editor
