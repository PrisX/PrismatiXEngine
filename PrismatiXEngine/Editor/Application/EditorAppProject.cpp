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

struct ReferenceSnapshot {
    std::filesystem::path path;
    std::string before;
    std::string after;
};

struct ProjectUndoStorage {
    std::filesystem::path root;
    ~ProjectUndoStorage(){std::error_code error;if(!root.empty())std::filesystem::remove_all(root,error);}
};

diag::Diagnostic ProjectMutationError(std::string code, const std::filesystem::path& path,
                                      std::string message, std::string details = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),
        .category="Editor.ProjectMutation",.message=std::move(message),.details=std::move(details)};
    diagnostic.source.path=path.generic_string(); return diagnostic;
}

bool RewriteVariantReference(Variant& value, bool resourcePath,
                             const std::string& oldRel, const std::string& newRel,
                             const std::string& oldName, const std::string& newName) {
    bool changed=false;
    if(auto* text=value.TryGet<std::string>()){
        if(resourcePath&&(*text==oldRel||*text==oldName)){*text=*text==oldRel?newRel:newName;return true;}
    }else if(auto* reference=value.TryGet<ResourceRefValue>()){
        if(reference->lastKnownPath==oldRel||reference->lastKnownPath==oldName){reference->lastKnownPath=reference->lastKnownPath==oldRel?newRel:newName;return true;}
    }else if(auto* array=value.AsArray()){
        for(auto& item:*array)changed|=RewriteVariantReference(item,resourcePath,oldRel,newRel,oldName,newName);
    }else if(auto* object=value.AsObject()){
        for(auto& [_,item]:*object)changed|=RewriteVariantReference(item,resourcePath,oldRel,newRel,oldName,newName);
    }
    return changed;
}

Result<std::vector<ReferenceSnapshot>> PlanReferenceUpdates(
    const std::filesystem::path& root,const std::string& oldRel,const std::string& newRel){
    ui::RegisterBuiltinUITypes();
    std::vector<ReferenceSnapshot> snapshots;const std::string oldName=std::filesystem::path(oldRel).filename().string(),newName=std::filesystem::path(newRel).filename().string();
    std::vector<std::filesystem::path> candidates{root/"project.pxproject"};std::error_code ec;
    for(auto iterator=std::filesystem::recursive_directory_iterator(root/"Content",ec);!ec&&iterator!=std::filesystem::recursive_directory_iterator();iterator.increment(ec)){
        if(!iterator->is_regular_file(ec)||iterator->path().extension()==".pxmeta")continue;const std::string extension=iterator->path().extension().string();
        if(extension==".pxscene"||extension==".pxres"||extension==".pxtheme"||extension==".pxanim"||extension==".pxscenario")candidates.push_back(iterator->path());
    }
    for(const auto& path:candidates){std::ifstream input(path,std::ios::binary);if(!input)continue;std::ostringstream stream;stream<<input.rdbuf();const std::string before=stream.str();std::string after=before;bool changed=false;
        if(path.extension()==".pxscenario"){auto parsed=vn::scenario::ParseScenario(before,path.generic_string());if(!parsed)return Result<std::vector<ReferenceSnapshot>>::Failure(parsed.Diagnostics());auto document=parsed.TakeValue();for(auto& node:document.nodes)for(auto& [_,value]:node.parameters)changed|=RewriteVariantReference(value,true,oldRel,newRel,oldName,newName);if(changed)after=vn::scenario::WriteScenario(document);}
        else{auto parsed=resource::ParseTypedDocument(before,path.generic_string());if(!parsed)return Result<std::vector<ReferenceSnapshot>>::Failure(parsed.Diagnostics());auto document=parsed.TakeValue();
            static const std::unordered_set<std::string> projectResourceProperties{"routes","startScript","archive"};
            for(auto& [key,value]:document.properties){const auto* property=TypeRegistry::Global().FindProperty(document.type,key);const bool resourcePath=(property&&HasFlag(property->flags,PropertyFlags::ResourcePath))||(document.kind==resource::DocumentKind::Project&&projectResourceProperties.contains(key));changed|=RewriteVariantReference(value,resourcePath,oldRel,newRel,oldName,newName);}
            for(auto& node:document.nodes)for(auto& [key,value]:node.properties){const auto* property=TypeRegistry::Global().FindProperty(node.type,key);const bool resourcePath=property&&HasFlag(property->flags,PropertyFlags::ResourcePath);changed|=RewriteVariantReference(value,resourcePath,oldRel,newRel,oldName,newName);}
            if(changed)after=resource::WriteTypedDocument(document);
        }
        if(changed)snapshots.push_back({path,before,std::move(after)});
    }
    return Result<std::vector<ReferenceSnapshot>>::Success(std::move(snapshots));
}

Status MoveAssetPair(const std::filesystem::path& from,const std::filesystem::path& to){
    std::error_code ec;std::filesystem::create_directories(to.parent_path(),ec);if(ec)return Status::Fail(ProjectMutationError("PXASSETMOVE9202",to,"無法建立目的資料夾",ec.message()));
    if(std::filesystem::exists(to))return Status::Fail(ProjectMutationError("PXASSETMOVE9203",to,"目的地已存在"));
    std::filesystem::rename(from,to,ec);if(ec)return Status::Fail(ProjectMutationError("PXASSETMOVE9204",from,"無法移動素材",ec.message()));
    const auto fromMeta=resource::AssetRegistry::MetaPath(from),toMeta=resource::AssetRegistry::MetaPath(to);
    if(std::filesystem::exists(fromMeta)){std::filesystem::rename(fromMeta,toMeta,ec);if(ec){std::error_code rollback;std::filesystem::rename(to,from,rollback);return Status::Fail(ProjectMutationError("PXASSETMOVE9205",fromMeta,"無法移動素材 identity",ec.message()));}}
    return Status::Ok();
}

Status WriteReferenceSnapshots(const std::vector<ReferenceSnapshot>& snapshots,bool after,
                               const std::filesystem::path& movedFrom={},
                               const std::filesystem::path& movedTo={}){
    std::vector<std::pair<std::filesystem::path,std::string>> written;
    for(const auto& snapshot:snapshots){const auto path=!movedFrom.empty()&&snapshot.path==movedFrom?movedTo:snapshot.path;const Status status=io::AtomicFile::WriteText(path,after?snapshot.after:snapshot.before);if(!status){for(auto iterator=written.rbegin();iterator!=written.rend();++iterator)(void)io::AtomicFile::WriteText(iterator->first,iterator->second);return status;}written.emplace_back(path,after?snapshot.before:snapshot.after);}
    return Status::Ok();
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
    const Status written =
        io::AtomicFile::WriteText(m_basePath + "PrismatiXRecent.json",
                                  Json(m_recentProjects).dump(2));
    if (!written) {
        for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
    }
}

void EditorApp::OpenProject(const std::filesystem::path& root) {
    if (m_project.Context().IsOpen()) SaveEditorSession();
    if (m_project.Open(root)) {
        m_inactiveDesigners.clear();
        m_designer=UIDesigner{};ConfigureDesigner(m_designer);m_designerPath.clear();
        const px::Uuid projectId =
            px::Uuid::FromName(std::filesystem::weakly_canonical(root).generic_string());
        if (Status recovery = m_recovery.BeginSession(projectId, root); !recovery) {
            for (auto diagnostic : recovery.Diagnostics()) px::diag::Emit(std::move(diagnostic));
        }
        if (m_recovery.HadUncleanSession()) m_showRecoveryCenter = true;
        AddRecentProject(root);
        const auto scaffolded = m_project.EnsureEssentials(m_basePath + "EditorAssets/UIFont.ttf");
        (void)m_project.SaveManifest();
        if (!scaffolded.empty()) {
            Log("Scaffolded " + std::to_string(scaffolded.size()) + " missing project file(s).");
        }
        m_assets.Scan(m_project.Context());
        const Status identityStatus = m_assetRegistry.Scan(root);
        if (!identityStatus) m_showAssetIdentity = true;
        const Status storyLibraryStatus = m_storyLibrary.Open(
            &m_project.Context(), &m_assets, m_textures.get(),
            [this](const std::string& runtimePath) -> std::optional<ResourceRefValue> {
                const auto* asset = m_assetRegistry.FindPath(m_project.Context().root / runtimePath);
                if (!asset) asset = m_assetRegistry.FindPath(runtimePath);
                if (!asset) return std::nullopt;
                return ResourceRefValue{asset->id, std::filesystem::path(runtimePath).generic_string()};
            });
        if (!storyLibraryStatus)
            for (const auto& diagnostic : storyLibraryStatus.Diagnostics()) diag::Emit(diagnostic);
        if (m_preview) {
            m_preview->SetProjectRoot(root.string());
            m_preview->LoadUI(m_project.Context().StartScenePath());
        }
        m_scriptDocs.clear();
        m_docs.Clear();
        m_projectHistory.Clear();
        m_activeDoc = -1;


        m_flow.SetOpenCallback([this](const std::string& script) { OpenDocTab(script); });
        m_flow.SetReadOnly(false);
        m_flow.SetCreateChapterCallback([this](ImVec2 canvasPosition) { CreateFlowChapter(canvasPosition); });
        m_flow.SetEntryChangedCallback([this](const std::string& script) {
            m_project.Context().manifest.startScript = script;
            m_project.SaveManifest();
            Log("Entry point set to " + script);
        });
        m_flow.SetLayoutChangedCallback([](const std::string&, ImVec2) {});
        m_flow.SetScriptChangedCallback([this](const std::string&, const std::string&) { m_flowStale = true; RefreshProblems(); });
        m_flow.SetCreateScriptCallback([this](const std::string& script) { CreateScriptFile(script); });
        m_flow.SetLinkAddedCallback([this](const std::string& from, const std::string& to) {
            ConnectStoryScenarios(from, to);
        });
        m_flow.SetLinkRemovedCallback([this](const std::string& from, const std::string& to) {
            DisconnectStoryScenarios(from, to);
        });
        m_flow.SetChapterRemovedCallback([this](const std::string&) { m_flowStale = true; RefreshProblems(); });
        m_flow.SetTitleChangedCallback([](const std::string&, const std::string&) {});
        m_flow.SetEntryScript(m_project.Context().manifest.startScript);
        m_flow.Rebuild(ScriptFileNames(), root);

        m_scripts.SetOnCommandsChanged([this](const std::vector<CustomCommandDef>& cmds) {
            m_customCommands = cmds;
            for (const auto& command : cmds) if (!vn::CommandRegistry::Global().Find(command.name)) {
                vn::CommandDescriptor descriptor; descriptor.id=command.name;
                descriptor.displayName=command.name; descriptor.category=command.category;
                descriptor.allowAdditionalParameters=false; descriptor.waitPolicy=vn::CommandWaitPolicy::Async;
                descriptor.rollbackPolicy=vn::RollbackPolicy::Boundary;
                for(const auto& parameter:command.params){vn::CommandParameterDescriptor value;value.name=parameter.key;value.label=parameter.key;value.type=VariantType::String;value.defaultValue=parameter.defaultValue;descriptor.parameters.push_back(std::move(value));}
                const Status registered=vn::CommandRegistry::Global().Register(std::move(descriptor));
                if(!registered)for(const auto& diagnostic:registered.Diagnostics())diag::Emit(diagnostic);
            }
            for (auto& doc : m_scriptDocs) {
                doc->SetCustomCommands(cmds);
            }
        });
        m_scripts.SetProject(&m_project.Context());

        RestoreEditorSession();
        if (m_scriptDocs.empty()) OpenDocTab(m_project.Context().manifest.startScript);

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

void EditorApp::TrackDocument(const std::filesystem::path& absolutePath, DocumentType type,
                              bool dirty) {
    DocumentSession session;
    session.id.canonicalPath = absolutePath;
    if (const auto* asset = m_assetRegistry.FindPath(absolutePath)) session.id.assetGuid = asset->id;
    session.label = absolutePath.filename().string();
    session.type = type;
    session.workspace = DocumentManager::WorkspaceFor(type);
    session.dirty = dirty;
    (void)m_docs.Open(std::move(session));
}

void EditorApp::SyncDocumentStates() {
    if (m_designer.Document()) {
        const auto path = m_designer.Document()->Path();
        if (!m_docs.Find(path)) TrackDocument(path, DocumentType::UIScene, m_designer.Dirty());
        m_docs.SetDirty(path, m_designer.Dirty(), m_designer.Document()->History().Cursor());
        const auto& viewport = m_designer.ViewportState();
        m_docs.SetViewport(path, {viewport.zoom,viewport.scrollX,viewport.scrollY,viewport.fitToViewport,viewport.leftPanelVisible,viewport.rightPanelVisible,viewport.bottomPanelVisible,viewport.leftPanelWidth,viewport.rightPanelWidth,viewport.bottomPanelHeight});
    }
    for(const auto& [_,session]:m_inactiveDesigners){
        if(!session.editor||!session.editor->Document())continue;
        const auto path=session.editor->Document()->Path();
        if(!m_docs.Find(path))TrackDocument(path,DocumentType::UIScene,session.editor->Dirty());
        m_docs.SetDirty(path,session.editor->Dirty(),session.editor->Document()->History().Cursor());
        const auto& viewport=session.editor->ViewportState();m_docs.SetViewport(path,{viewport.zoom,viewport.scrollX,viewport.scrollY,viewport.fitToViewport,viewport.leftPanelVisible,viewport.rightPanelVisible,viewport.bottomPanelVisible,viewport.leftPanelWidth,viewport.rightPanelWidth,viewport.bottomPanelHeight});
    }
    for (const auto& document : m_scriptDocs) {
        if (document->DocumentPath().empty()) continue;
        if (!m_docs.Find(document->DocumentPath()))
            TrackDocument(document->DocumentPath(), DocumentType::Scenario, document->Dirty());
        m_docs.SetDirty(document->DocumentPath(), document->Dirty());
    }
    std::unordered_set<std::string> openLua;
    for(const auto& document:m_scripts.OpenDocuments()){
        const auto path=m_project.Context().root/document.runtimePath;
        openLua.insert(DocumentManager::Canonical(path).generic_string());
        if(!m_docs.Find(path))TrackDocument(path,DocumentType::Lua,document.dirty);
        m_docs.SetDirty(path,document.dirty);
    }
    std::vector<std::filesystem::path> closedLua;
    for(const auto& session:m_docs.Documents())if(session.type==DocumentType::Lua&&!openLua.contains(session.id.canonicalPath.generic_string()))closedLua.push_back(session.id.canonicalPath);
    for(const auto& path:closedLua)(void)m_docs.Close(path);
}

void EditorApp::SaveEditorSession() {
    if (m_editorSessionPath.empty()) return;
    SyncDocumentStates();
    const Status status = m_docs.SaveSession(m_editorSessionPath);
    if (!status) for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
    if (!m_editorSettingsPath.empty()) {
        m_folderViewSettings[m_assetDir] = {m_fileSystemView,m_assetSortColumn,
            m_assetSortAscending,m_assetRowHeight,m_assetThumbSize};
        resource::TypedDocument settings;
        settings.kind=resource::DocumentKind::Resource;settings.formatVersion=resource::TypedDocument::CurrentVersion;
        settings.id=Uuid::FromName("PrismatiXEditor.Settings.v4");settings.type="EditorSettings";
        settings.properties["workspace"]=Variant(static_cast<std::int64_t>(m_workspace));
        settings.properties["asset_directory"]=Variant(m_assetDir);
        VariantArray folders;
        for(const auto& [path,view]:m_folderViewSettings){VariantObject item;
            item["path"]=Variant(path);item["mode"]=Variant(static_cast<std::int64_t>(view.mode));
            item["sort"]=Variant(static_cast<std::int64_t>(view.sort));item["ascending"]=Variant(view.ascending);
            item["row_height"]=Variant(static_cast<double>(view.rowHeight));item["thumbnail_size"]=Variant(static_cast<double>(view.thumbnailSize));
            folders.emplace_back(std::move(item));}
        settings.properties["folder_views"]=Variant(std::move(folders));
        const Status written=io::AtomicFile::WriteText(m_editorSettingsPath,resource::WriteTypedDocument(settings));
        if(!written)for(const auto& diagnostic:written.Diagnostics())diag::Emit(diagnostic);
    }
}

void EditorApp::RestoreEditorSession() {
    if (m_editorSessionPath.empty()) return;
    if(!m_editorSettingsPath.empty()&&std::filesystem::exists(m_editorSettingsPath)){
        std::ifstream input(m_editorSettingsPath,std::ios::binary);std::ostringstream stream;stream<<input.rdbuf();
        auto parsed=resource::ParseTypedDocument(stream.str(),m_editorSettingsPath.generic_string());
        if(!parsed)for(const auto& diagnostic:parsed.Diagnostics())diag::Emit(diagnostic);
        else if(parsed.Value().type=="EditorSettings"&&parsed.Value().formatVersion==resource::TypedDocument::CurrentVersion){
            const auto& properties=parsed.Value().properties;
            if(const auto found=properties.find("workspace");found!=properties.end())if(const auto* value=found->second.TryGet<std::int64_t>())m_workspace=static_cast<EditorWorkspace>(std::clamp<std::int64_t>(*value,0,3));
            if(const auto found=properties.find("asset_directory");found!=properties.end())if(const auto* value=found->second.TryGet<std::string>())m_assetDir=*value;
            m_folderViewSettings.clear();
            if(const auto found=properties.find("folder_views");found!=properties.end())if(const auto* array=found->second.AsArray())for(const auto& value:*array){const auto* object=value.AsObject();if(!object)continue;
                const auto pathIt=object->find("path");if(pathIt==object->end())continue;const auto* path=pathIt->second.TryGet<std::string>();if(!path)continue;FolderViewSettings view;
                if(const auto it=object->find("mode");it!=object->end())if(const auto* number=it->second.TryGet<std::int64_t>())view.mode=static_cast<FileSystemViewMode>(std::clamp<std::int64_t>(*number,0,2));
                if(const auto it=object->find("sort");it!=object->end())if(const auto* number=it->second.TryGet<std::int64_t>())view.sort=static_cast<AssetSortColumn>(std::clamp<std::int64_t>(*number,0,3));
                if(const auto it=object->find("ascending");it!=object->end())if(const auto* boolean=it->second.TryGet<bool>())view.ascending=*boolean;
                if(const auto it=object->find("row_height");it!=object->end())if(const auto* number=it->second.TryGet<double>())view.rowHeight=static_cast<float>(*number);
                if(const auto it=object->find("thumbnail_size");it!=object->end())if(const auto* number=it->second.TryGet<double>())view.thumbnailSize=static_cast<float>(*number);
                m_folderViewSettings[*path]=view;}
            if(const auto view=m_folderViewSettings.find(m_assetDir);view!=m_folderViewSettings.end()){m_fileSystemView=view->second.mode;m_assetSortColumn=view->second.sort;m_assetSortAscending=view->second.ascending;m_assetRowHeight=view->second.rowHeight;m_assetThumbSize=view->second.thumbnailSize;}
        }
    }
    {
        std::error_code directoryError;
        const bool validDirectory = (m_assetDir == "Content" || m_assetDir.starts_with("Content/")) &&
            std::filesystem::is_directory(m_project.Context().root / m_assetDir, directoryError);
        if (!validDirectory) m_assetDir = "Content";
        m_assetPathInput = m_assetDir;
        m_assetDirectoryHistory.clear();
        m_assetDirectoryForward.clear();
    }
    const Status status = m_docs.RestoreSession(m_editorSessionPath);
    if (!status) {
        for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
        m_docs.Clear();
        return;
    }
    const auto restored = m_docs.Documents();
    m_docs.Clear();
    const auto root = DocumentManager::Canonical(m_project.Context().root).generic_string();
    for (const auto& session : restored) {
        const auto path = DocumentManager::Canonical(session.id.canonicalPath);
        if (!path.generic_string().starts_with(root)) continue;
        std::error_code error;
        const auto runtime = std::filesystem::relative(path, m_project.Context().root, error).generic_string();
        if (error) continue;
        if (session.type == DocumentType::Scenario) {
            if (OpenDocTab(runtime)) m_docs.SetPinned(path, session.pinned);
        } else if (session.type == DocumentType::UIScene && m_preview) {
            m_previewMode = 0;
            m_preview->LoadUI(runtime);
            SyncDesigner();
            m_docs.SetPinned(path, session.pinned);
        } else if(session.type==DocumentType::Lua){
            m_scripts.OpenFile(runtime);TrackDocument(path,DocumentType::Lua,false);m_docs.SetPinned(path,session.pinned);
        }
    }
}

void EditorApp::CheckExternalDocuments() {
    SyncDocumentStates();
    const auto sessions = m_docs.Documents();
    for (const auto& session : sessions) {
        const auto state = m_docs.CheckExternalState(session);
        if (state == ExternalDocumentState::Unchanged) continue;
        if (state == ExternalDocumentState::Missing) {
            diag::Diagnostic diagnostic{.severity=diag::Severity::Warning,.code="PXDOC2103",
                .category="Editor.DocumentManager",.message="開啟的文件已從磁碟移除"};
            diagnostic.source.path=session.id.canonicalPath.generic_string();diag::Emit(std::move(diagnostic));
            continue;
        }
        if (state == ExternalDocumentState::LocalConflict) {
            if (m_externalConflictPath.empty()) {
                m_externalConflictPath = session.id.canonicalPath;
                const auto stem = m_externalConflictPath.stem().string() + ".local-copy" +
                                  m_externalConflictPath.extension().string();
                const auto suggested = m_externalConflictPath.parent_path() / stem;
                std::snprintf(m_externalSaveAsPath, sizeof(m_externalSaveAsPath), "%s",
                              suggested.string().c_str());
            }
            continue;
        }
        std::error_code error;
        const auto runtime = fs::relative(session.id.canonicalPath,
                                          m_project.Context().root, error).generic_string();
        if (error) continue;
        bool reloaded = false;
        if (session.type == DocumentType::UIScene && m_designer.Document() &&
            DocumentManager::Canonical(m_designer.Document()->Path()) == session.id.canonicalPath) {
            reloaded = static_cast<bool>(m_designer.Open(session.id.canonicalPath));
            if (reloaded && m_preview) m_preview->LoadUIDocument(m_designer.Document()->Data(), runtime);
        } else if(session.type==DocumentType::UIScene) {
            if(auto found=m_inactiveDesigners.find(session.id.canonicalPath.generic_string());found!=m_inactiveDesigners.end())
                reloaded=static_cast<bool>(found->second.editor->Open(session.id.canonicalPath));
        } else if (session.type == DocumentType::Scenario) {
            for (auto& document : m_scriptDocs) {
                if (DocumentManager::Canonical(document->DocumentPath()) == session.id.canonicalPath) {
                    reloaded = document->OpenDocument(runtime); break;
                }
            }
        } else if(session.type==DocumentType::Lua) {
            reloaded=m_scripts.ReloadFile(runtime);
        }
        if (reloaded) {
            m_docs.AcknowledgeDiskVersion(session.id.canonicalPath);
            Log("已重新載入外部修改：" + runtime);
        }
    }
}

void EditorApp::ConfigureDoc(NodeGraphEditor& doc) {
    doc.SetHeaderTexture(m_nodeHeaderTex, m_nodeHeaderW, m_nodeHeaderH);
    doc.SetSelectedResourceCallback([this] { return m_selectedAsset; });
    doc.SetIdentityRegistrar([this](const std::vector<std::filesystem::path>& paths) {
        return EnsureAssetIdentities(paths);
    });
    doc.SetResourceResolver([this](const std::string& runtimePath)
        -> std::optional<ResourceRefValue> {
        if (!m_project.Context().IsOpen() || runtimePath.empty()) return std::nullopt;
        const auto absolute = m_project.Context().root / runtimePath;
        const auto* asset = m_assetRegistry.FindPath(absolute);
        if (!asset) asset = m_assetRegistry.FindPath(runtimePath);
        if (!asset) return std::nullopt;
        return ResourceRefValue{asset->id, std::filesystem::path(runtimePath).generic_string()};
    });
    doc.SetFieldOptionsCallback([this](std::string_view nodeType, std::string_view key) {
        std::vector<NodeGraphEditor::FieldOption> options;
        const std::string type(nodeType), field(key);
        const auto add=[&](std::string value,std::string label={},std::string detail={}){
            if(label.empty())label=value;options.push_back({std::move(value),std::move(label),std::move(detail)});
        };
        if (key == "target" && (type == "jump" || type == "call")) {
            for (const auto& asset : m_assets.Assets())
                if (asset.absolutePath.extension() == ".pxscenario")
                    add(asset.runtimePath, asset.absolutePath.filename().string(),
                        "切換至 Scenario：" + asset.runtimePath);
            return options;
        }
        if (key == "ui" || key == "route") {
            for (const auto& route : m_project.Context().manifest.routes) add(route.id,route.id,route.scene);
            return options;
        }
        if (key == "preset") {
            for (const auto& clip : animation::OfficialPresets()) add(clip.name);
            return options;
        }
        const auto& catalog=m_storyLibrary.Catalog();
        if (key == "var" || key == "lhs" || (type=="variable"&&key=="name")) {
            for (const auto& variable : catalog.Variables()) add(variable.name);
            return options;
        }
        const bool characterField=key=="character"||key=="speaker"||
            ((key=="id"||key=="target")&&(type=="character"||type=="char"||
             type=="char_clear"||type=="char_move"||type=="move"||type=="animate_actor"||
             type=="anim"||type=="tween"));
        if(characterField){
            for(const auto& character:catalog.Characters()){
                const std::string id=character.id.empty()?character.name:character.id;
                const std::string label=character.name.empty()?id:character.name;
                add(key=="speaker"?label:id,label,id);
            }
            return options;
        }
        if(field.starts_with("expression:")){
            const std::string characterId=field.substr(std::string("expression:").size());
            if(const auto* character=catalog.FindCharacter(characterId)){
                auto expressions=character->expressions;
                std::stable_sort(expressions.begin(),expressions.end(),[&](const auto& a,const auto& b){return a.id==character->defaultExpression&&b.id!=character->defaultExpression;});
                for(const auto& expression:expressions)
                    add(expression.id,expression.name.empty()?expression.id:expression.name,
                        expression.image.lastKnownPath);
            }
            return options;
        }
        if(key=="ease")for(const char* value:{"linear","inQuad","outQuad","inOutQuad","inCubic","outCubic","inOutCubic","inSine","outSine","inOutSine","inBack","outBack","inBounce","outBounce","smoothstep"})add(value);
        if(key=="effect")for(const char* value:{"none","shake","pulse"})add(value);
        if(key=="kind"&&type=="unlock")for(const char* value:{"gallery","cg","route"})add(value);
        if(key=="id"&&type=="unlock")
            for(const auto& item:catalog.Gallery())add(item.id,item.title.empty()?item.id:item.title,item.image);
        if(!options.empty())return options;
        const bool imageType=type=="background"||type=="bg"||type=="transition"||type=="character"||
            type=="char"||type=="layer"||type=="cg";
        const bool audioType=type=="bgm"||type=="se"||type=="voice"||type=="ambience"||key=="voice";
        const bool videoType=type=="video"||type=="movie";
        const bool animationType=type=="animation";
        const bool genericAsset=type=="asset"&&key=="path";
        if((imageType||audioType||videoType||animationType||genericAsset)&&
           (key=="file"||key=="image"||key=="rule"||key=="path"||key=="clip"||key=="voice")){
            for(const auto& asset:m_assets.Assets()){
                const std::string extension=asset.absolutePath.extension().string();
                if(imageType&&asset.type!="image")continue;
                if(audioType&&asset.type!="audio")continue;
                if(videoType&&extension!=".mp4"&&extension!=".webm"&&extension!=".mkv"&&extension!=".mov")continue;
                if(animationType&&extension!=".pxanim")continue;
                add(asset.runtimePath,asset.absolutePath.filename().string(),asset.runtimePath);
            }
        }
        std::sort(options.begin(), options.end(),[](const auto& a,const auto& b){return a.label==b.label?a.value<b.value:a.label<b.label;});
        options.erase(std::unique(options.begin(), options.end(),[](const auto& a,const auto& b){return a.value==b.value;}), options.end());
        return options;
    });
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
            (void)m_docs.Activate(m_scriptDocs[i]->DocumentPath());
            return m_scriptDocs[i].get();
        }
    }
    auto doc = std::make_unique<NodeGraphEditor>(NodeGraphEditor::GraphKind::Scenario,
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
    TrackDocument(m_scriptDocs.back()->DocumentPath(), DocumentType::Scenario,
                  m_scriptDocs.back()->Dirty());
    return m_scriptDocs.back().get();
}

void EditorApp::RefreshAfterProjectMutation() {
    m_assets.Scan(m_project.Context());
    const Status identities = m_assetRegistry.Scan(m_project.Context().root);
    if (!identities) m_showAssetIdentity = true;
    m_flowStale = true;
    RefreshProblems();
}

void EditorApp::OnAssetRelocated(const std::string& fromRuntimePath,
                                 const std::string& toRuntimePath) {
    const auto root=m_project.Context().root;
    const auto fromAbsolute=root/fromRuntimePath,toAbsolute=root/toRuntimePath;
    m_designer.RelocateDocument(fromAbsolute,toAbsolute);
    const auto inactiveKey=DocumentManager::Canonical(fromAbsolute).generic_string();
    if(auto found=m_inactiveDesigners.find(inactiveKey);found!=m_inactiveDesigners.end()){
        found->second.editor->RelocateDocument(fromAbsolute,toAbsolute);
        DesignerDocumentSession relocated=std::move(found->second);m_inactiveDesigners.erase(found);
        relocated.canonicalPath=DocumentManager::Canonical(toAbsolute);
        m_inactiveDesigners[relocated.canonicalPath.generic_string()]=std::move(relocated);
    }
    (void)m_docs.Relocate(fromAbsolute, toAbsolute);
    if(m_designerPath==fromAbsolute.string())m_designerPath=toAbsolute.string();
    if(m_preview&&m_preview->CurrentUIPath()==fromRuntimePath)m_preview->LoadUI(toRuntimePath);
    for(auto& document:m_scriptDocs)document->RelocateIfOpen(fromRuntimePath,toRuntimePath);
    auto& manifest=m_project.Context().manifest;
    const std::string fromName=fs::path(fromRuntimePath).filename().string(),toName=fs::path(toRuntimePath).filename().string();
    for(auto& route:manifest.routes)if(route.scene==fromRuntimePath)route.scene=toRuntimePath;
    if(manifest.startScript==fromRuntimePath||manifest.startScript==fromName)
        manifest.startScript=manifest.startScript==fromName?toName:toRuntimePath;
    if(m_selectedAsset==fromRuntimePath)m_selectedAsset=toRuntimePath;
    if(m_assetSelectionModel.selected.erase(fromRuntimePath))m_assetSelectionModel.selected.insert(toRuntimePath);
    RefreshAfterProjectMutation();
}

Status EditorApp::MoveAssetWithHistory(const std::string& oldRuntimePath,
                                       const std::string& newRuntimePath) {
    if(oldRuntimePath.empty()||newRuntimePath.empty()||oldRuntimePath==newRuntimePath)
        return Status::Ok();
    const auto root=m_project.Context().root;
    const auto oldAbsolute=root/oldRuntimePath,newAbsolute=root/newRuntimePath;
    if(m_designer.Document()&&m_designer.Document()->Path()==oldAbsolute&&m_designer.Dirty())
        return Status::Fail(ProjectMutationError("PXASSETMOVE9206",oldAbsolute,
            "請先儲存目前的 UI 文件再移動或重新命名"));
    if(const auto found=m_inactiveDesigners.find(DocumentManager::Canonical(oldAbsolute).generic_string());
       found!=m_inactiveDesigners.end()&&found->second.editor&&found->second.editor->Dirty())
        return Status::Fail(ProjectMutationError("PXASSETMOVE9206",oldAbsolute,
            "請先儲存開啟中的 UI 文件再移動或重新命名"));
    for(const auto& document:m_scriptDocs)if(document->DocumentPath()==oldAbsolute&&document->Dirty())
        return Status::Fail(ProjectMutationError("PXASSETMOVE9207",oldAbsolute,
            "請先儲存目前的劇情文件再移動或重新命名"));
    auto planned=PlanReferenceUpdates(root,oldRuntimePath,newRuntimePath);
    if(!planned)return Status::Fail(planned.Diagnostics());
    const auto snapshots=planned.TakeValue();
    auto apply=[this,oldRuntimePath,newRuntimePath,oldAbsolute,newAbsolute,snapshots]{
        Status moved=MoveAssetPair(oldAbsolute,newAbsolute);if(!moved)return moved;
        Status references=WriteReferenceSnapshots(snapshots,true,oldAbsolute,newAbsolute);
        if(!references){(void)MoveAssetPair(newAbsolute,oldAbsolute);return references;}
        OnAssetRelocated(oldRuntimePath,newRuntimePath);return Status::Ok();};
    auto revert=[this,oldRuntimePath,newRuntimePath,oldAbsolute,newAbsolute,snapshots]{
        Status moved=MoveAssetPair(newAbsolute,oldAbsolute);if(!moved)return moved;
        Status references=WriteReferenceSnapshots(snapshots,false);
        if(!references){(void)MoveAssetPair(oldAbsolute,newAbsolute);return references;}
        OnAssetRelocated(newRuntimePath,oldRuntimePath);return Status::Ok();};
    auto command=std::make_unique<FunctionalProjectCommand>(
        "移動 "+fs::path(oldRuntimePath).filename().string(),std::move(apply),std::move(revert));
    const Status status=m_projectHistory.Execute(std::move(command));
    if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);
    return status;
}

Status EditorApp::TrashAssetWithHistory(const std::string& runtimePath) {
    if(runtimePath.empty())return Status::Ok();const auto root=m_project.Context().root;
    const auto source=root/runtimePath;
    if(m_docs.Find(source))
        return Status::Fail(ProjectMutationError("PXASSETTRASH9301",source,
            "請先關閉開啟中的文件再移到垃圾桶"));
    auto references=PlanReferenceUpdates(root,runtimePath,runtimePath+".__trashed__");if(!references)return Status::Fail(references.Diagnostics());
    const auto transaction=root/".prismatix"/"Trash"/Uuid::Random().ToString();const auto trash=transaction/runtimePath;const auto manifest=transaction/"TrashRecord.pxres";
    VariantArray referencedBy;for(const auto& snapshot:references.Value())referencedBy.emplace_back(snapshot.path.generic_string());
    resource::TypedDocument record;record.kind=resource::DocumentKind::Resource;record.id=Uuid::Random();record.type="TrashRecord";record.properties["original_path"]=Variant(runtimePath);record.properties["referenced_by"]=Variant(std::move(referencedBy));const std::string recordText=resource::WriteTypedDocument(record);
    auto apply=[this,runtimePath,source,trash,manifest,recordText]{const Status moved=MoveAssetPair(source,trash);if(!moved)return moved;const Status written=io::AtomicFile::WriteText(manifest,recordText);if(!written){(void)MoveAssetPair(trash,source);return written;}if(m_selectedAsset==runtimePath)m_selectedAsset.clear();m_assetSelectionModel.selected.erase(runtimePath);RefreshAfterProjectMutation();return Status::Ok();};
    auto revert=[this,runtimePath,source,trash,manifest]{const Status moved=MoveAssetPair(trash,source);if(!moved)return moved;std::error_code error;std::filesystem::remove(manifest,error);m_selectedAsset=runtimePath;m_assetSelectionModel.selected={runtimePath};RefreshAfterProjectMutation();return Status::Ok();};
    auto command=std::make_unique<FunctionalProjectCommand>(
        "移到垃圾桶 "+fs::path(runtimePath).filename().string(),std::move(apply),std::move(revert));
    const Status status=m_projectHistory.Execute(std::move(command));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return status;
}

Status EditorApp::CreateAssetWithHistory(const std::filesystem::path& absolutePath,int kind){
    const auto root=m_project.Context().root;auto storage=std::make_shared<ProjectUndoStorage>();
    storage->root=root/".prismatix"/"Undo"/"create"/Uuid::Random().ToString();
    const auto backup=storage->root/absolutePath.filename();
    auto apply=[this,root,absolutePath,backup,kind,storage]{
        std::error_code error;std::filesystem::create_directories(absolutePath.parent_path(),error);
        if(error)return Status::Fail(ProjectMutationError("PXASSETCREATE9401",absolutePath,"無法建立父資料夾",error.message()));
        if(std::filesystem::exists(backup)){
            if(kind==0){std::filesystem::rename(backup,absolutePath,error);if(error)return Status::Fail(ProjectMutationError("PXASSETCREATE9402",absolutePath,"無法重做資料夾建立",error.message()));}
            else{const Status moved=MoveAssetPair(backup,absolutePath);if(!moved)return moved;}
        }else if(kind==0){
            if(!std::filesystem::create_directory(absolutePath,error)||error)return Status::Fail(ProjectMutationError("PXASSETCREATE9403",absolutePath,"無法建立資料夾",error.message()));
        }else if(kind==1){
            const auto name=absolutePath.stem().string();vn::scenario::ScenarioDocument scenario;scenario.id=Uuid::Random();scenario.name=name;vn::scenario::ScenarioNode chapter{Uuid::Random(),"chapter",{{"title",name}}};vn::scenario::ScenarioNode dialogue{Uuid::Random(),"say",{{"textId",Uuid::Random().ToString()},{"speaker",std::string{}},{"value",std::string("New dialogue line")}}};scenario.entry=chapter.id;scenario.nodes={chapter,dialogue};scenario.edges.push_back({Uuid::Random(),chapter.id,"flow",dialogue.id,"in"});const Status written=io::AtomicFile::WriteText(absolutePath,vn::scenario::WriteScenario(scenario));if(!written)return written;
        }else{
            UISceneDocument document;const Status created=document.New(absolutePath);if(!created)return created;if(!document.Save())return Status::Fail(ProjectMutationError("PXASSETCREATE9404",absolutePath,"無法儲存新 UI Scene"));
        }
        if(kind!=0){auto registered=m_assetRegistry.RegisterAsset(root,absolutePath,AssetDatabase::Classify(absolutePath));if(!registered){std::filesystem::remove(absolutePath,error);return Status::Fail(registered.Diagnostics());}}
        RefreshAfterProjectMutation();return Status::Ok();};
    auto revert=[this,absolutePath,backup,kind,storage]{
        std::error_code error;std::filesystem::create_directories(backup.parent_path(),error);if(error)return Status::Fail(ProjectMutationError("PXASSETCREATE9405",backup,"無法建立 Undo 儲存區",error.message()));
        if(kind==0){std::filesystem::rename(absolutePath,backup,error);if(error)return Status::Fail(ProjectMutationError("PXASSETCREATE9406",absolutePath,"無法復原資料夾建立",error.message()));}
        else{const Status moved=MoveAssetPair(absolutePath,backup);if(!moved)return moved;}
        RefreshAfterProjectMutation();return Status::Ok();};
    auto command=std::make_unique<FunctionalProjectCommand>(kind==0?"建立資料夾":"建立素材",std::move(apply),std::move(revert));
    const Status status=m_projectHistory.Execute(std::move(command));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return status;
}

Status EditorApp::DuplicateAssetWithHistory(const std::string& runtimePath){
    const auto root=m_project.Context().root,source=root/runtimePath;std::filesystem::path target=source.parent_path()/(source.stem().string()+"_copy"+source.extension().string());
    for(int index=2;std::filesystem::exists(target)&&index<10000;++index)target=source.parent_path()/(source.stem().string()+"_copy_"+std::to_string(index)+source.extension().string());
    auto storage=std::make_shared<ProjectUndoStorage>();storage->root=root/".prismatix"/"Undo"/"duplicate"/Uuid::Random().ToString();const auto backup=storage->root/target.filename();
    auto apply=[this,root,source,target,backup,storage]{std::error_code error;
        if(std::filesystem::exists(backup)){const Status moved=MoveAssetPair(backup,target);if(!moved)return moved;}
        else{std::filesystem::copy_file(source,target,std::filesystem::copy_options::none,error);if(error)return Status::Fail(ProjectMutationError("PXASSETDUP9501",source,"無法複製素材",error.message()));auto registered=m_assetRegistry.RegisterAsset(root,target,AssetDatabase::Classify(target));if(!registered){std::filesystem::remove(target,error);return Status::Fail(registered.Diagnostics());}}
        RefreshAfterProjectMutation();return Status::Ok();};
    auto revert=[this,target,backup,storage]{const Status moved=MoveAssetPair(target,backup);if(!moved)return moved;RefreshAfterProjectMutation();return Status::Ok();};
    auto command=std::make_unique<FunctionalProjectCommand>("複製 "+source.filename().string(),std::move(apply),std::move(revert));
    const Status status=m_projectHistory.Execute(std::move(command));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return status;
}

const std::vector<std::string>& EditorApp::ScriptFileNames() {
    if (m_scriptNameRevision == m_assets.Revision()) {
        return m_scriptNameCache;
    }
    m_scriptNameCache.clear();
    for (const AssetRecord& rec : m_assets.Assets()) {
        if (rec.type == "script" && std::filesystem::path(rec.runtimePath).extension() == ".pxscenario")
            m_scriptNameCache.push_back(rec.runtimePath);
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
    std::filesystem::path runtimePath = script;
    if (runtimePath.extension() != ".pxscenario") runtimePath.replace_extension(".pxscenario");
    if (!runtimePath.generic_string().starts_with("Content/"))
        runtimePath = std::filesystem::path("Content/Scenario") / runtimePath.filename();
    const std::filesystem::path path = m_project.Context().root / runtimePath;
    if (!std::filesystem::exists(path)) {
        NodeGraphEditor document(NodeGraphEditor::GraphKind::Scenario,
                                 [this](const std::string& message) { Log(message); });
        document.SetProject(&m_project.Context());
        if (!document.NewDocument(runtimePath.generic_string())) return;
        auto registered = m_assetRegistry.RegisterAsset(m_project.Context().root, path, "script");
        if (!registered) for (const auto& diagnostic : registered.Diagnostics()) diag::Emit(diagnostic);
    }
    m_assets.Scan(m_project.Context());
    m_flowStale = true;
    RefreshProblems();
}

void EditorApp::ConnectStoryScenarios(const std::string& fromScenario,
                                      const std::string& toScenario) {
    const auto read = [this](const std::string& runtimePath)
        -> std::optional<vn::scenario::ScenarioDocument> {
        std::ifstream stream(m_project.Context().root / runtimePath, std::ios::binary);
        if (!stream) return std::nullopt;
        std::ostringstream text; text << stream.rdbuf();
        auto parsed = vn::scenario::ParseScenario(text.str(), runtimePath);
        if (!parsed) { for (const auto& diagnostic : parsed.Diagnostics()) diag::Emit(diagnostic); return std::nullopt; }
        return parsed.TakeValue();
    };
    auto source = read(fromScenario); const auto target = read(toScenario);
    if (!source || !target) return;
    vn::scenario::ScenarioNode* statement = nullptr;
    for (auto& node : source->nodes) {
        if ((node.command == "jump" || node.command == "call" || node.command == "choice") &&
            !vn::scenario::GetStoryTarget(node)) { statement = &node; break; }
    }
    if (!statement) {
        vn::scenario::ScenarioNode jump{Uuid::Random(), "jump", {}};
        const Uuid previous = source->nodes.empty() ? Uuid{} : source->nodes.back().id;
        source->nodes.push_back(std::move(jump)); statement = &source->nodes.back();
        if (!previous.Empty()) source->edges.push_back({Uuid::Random(), previous, "flow", statement->id, "in"});
        if (source->entry.Empty()) source->entry = statement->id;
    }
    const vn::scenario::StoryTarget storyTarget{target->id, target->entry, toScenario};
    const Status connected = vn::scenario::ConnectStoryTarget(*source, statement->id, "target", storyTarget);
    if (!connected) { for (const auto& diagnostic : connected.Diagnostics()) diag::Emit(diagnostic); return; }
    const Status written = io::AtomicFile::WriteText(m_project.Context().root/fromScenario,
                                                     vn::scenario::WriteScenario(*source));
    if (!written) { for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic); return; }
    if (auto* document = OpenDocTab(fromScenario); document && !document->Dirty()) document->Reload();
    m_flowStale = true; RefreshProblems();
    Log("Story Map: connected " + fromScenario + " -> " + toScenario);
}

void EditorApp::DisconnectStoryScenarios(const std::string& fromScenario,
                                         const std::string& toScenario) {
    std::ifstream sourceStream(m_project.Context().root/fromScenario, std::ios::binary);
    std::ifstream targetStream(m_project.Context().root/toScenario, std::ios::binary);
    if (!sourceStream || !targetStream) return;
    std::ostringstream sourceText, targetText; sourceText << sourceStream.rdbuf(); targetText << targetStream.rdbuf();
    auto source = vn::scenario::ParseScenario(sourceText.str(), fromScenario);
    auto target = vn::scenario::ParseScenario(targetText.str(), toScenario);
    if (!source || !target) return;
    bool removed = false;
    for (auto& node : source.Value().nodes) {
        const auto storyTarget = vn::scenario::GetStoryTarget(node);
        if (storyTarget && storyTarget->scenario == target.Value().id) {
            const Status disconnected = vn::scenario::DisconnectStoryTarget(source.Value(), node.id, "target");
            removed = disconnected.IsOk(); break;
        }
    }
    if (!removed) return;
    const Status written = io::AtomicFile::WriteText(m_project.Context().root/fromScenario,
                                                     vn::scenario::WriteScenario(source.Value()));
    if (!written) { for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic); return; }
    if (auto* document = OpenDocTab(fromScenario); document && !document->Dirty()) document->Reload();
    m_flowStale = true; RefreshProblems();
    Log("Story Map: disconnected " + fromScenario + " -> " + toScenario);
}

void EditorApp::CreateFlowChapter(ImVec2 canvasPosition) {
    if (!m_project.Context().IsOpen()) {
        Log("Flow: open a project before adding chapters.");
        return;
    }

    int index = static_cast<int>(ScriptFileNames().size()) + 1;
    std::string id, title, script;
    do { id="chapter"+std::to_string(index);title="Chapter "+std::to_string(index);script="Content/Scenario/"+id+".pxscenario";++index; }
    while (std::filesystem::exists(m_project.Context().root/script));
    CreateScriptFile(script);
    m_assets.Scan(m_project.Context());
    m_flow.Rebuild(ScriptFileNames(), m_project.Context().root);
    m_flow.SetNodePositionByScript(script, canvasPosition);
    RefreshProblems();
    Log("Flow: added " + title + " (" + script + ")");
}


void EditorApp::SaveAll() {
    if (m_storyLibrary.Dirty()) m_storyLibrary.Save();
    if (m_designer.Dirty()) m_designer.Save();
    for(auto& [_,session]:m_inactiveDesigners)if(session.editor&&session.editor->Dirty())session.editor->Save();
    m_scripts.SaveAll();
    // Save the open script unconditionally: a freshly imported document is not
    // "dirty", but Save All should still normalize it to the compiled form.
    for (auto& doc : m_scriptDocs) {
        if (!doc->CurrentRuntimePath().empty() && doc->Save())
            (void)EnsureAssetIdentity(doc->DocumentPath());
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
    opt.startRoute = m.startRoute;
    opt.routes = m.routes;
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
