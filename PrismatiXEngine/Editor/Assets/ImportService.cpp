#include "Editor/Assets/ImportService.h"

#include "Engine/Core/Uuid.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace px::editor {
namespace {

std::string Utf8Path(const std::filesystem::path& path) noexcept {
    try {
        const std::u8string value = path.generic_u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    } catch (...) {
        return "<unprintable path>";
    }
}

diag::Diagnostic ImportError(std::string code, const std::filesystem::path& path,
                             std::string message, std::string details = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "Editor.Import",
                                .message = std::move(message),
                                .details = std::move(details)};
    diagnostic.source.path = Utf8Path(path);
    diag::Emit(diagnostic);
    return diagnostic;
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code ec;
    const auto absoluteChild = std::filesystem::weakly_canonical(child, ec);
    if (ec) return false;
    const auto absoluteParent = std::filesystem::weakly_canonical(parent, ec);
    if (ec) return false;
    const auto relative = std::filesystem::relative(absoluteChild, absoluteParent, ec);
    if (ec || relative.empty()) return false;
    for (const auto& part : relative) if (part == "..") return false;
    return true;
}

std::filesystem::path TypeFolder(std::string_view type) {
    if (type == "image") return "Images";
    if (type == "audio") return "Audio";
    if (type == "video") return "Video";
    if (type == "font") return "Fonts";
    if (type == "script") return "Script";
    if (type == "ui") return "UI";
    if (type == "lua") return "Extensions";
    return "Imported";
}

}  // namespace

ImportService::~ImportService() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
}

std::string ImportService::DetectType(const std::filesystem::path& path) {
    std::string extension = Utf8Path(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".webp" || extension == ".bmp") return "image";
    if (extension == ".mp3" || extension == ".ogg" || extension == ".wav" ||
        extension == ".flac" || extension == ".opus") return "audio";
    if (extension == ".mp4" || extension == ".webm" || extension == ".mpeg") return "video";
    if (extension == ".ttf" || extension == ".otf") return "font";
    if (extension == ".pds") return "script";
    if (extension == ".pxscene") return "ui";
    if (extension == ".pxres" || extension == ".pxtheme" || extension == ".pxanim")
        return "resource";
    if (extension == ".lua") return "lua";
    return "other";
}

std::filesystem::path ImportService::UniqueTarget(const std::filesystem::path& requested) {
    if (!std::filesystem::exists(requested)) return requested;
    const auto parent = requested.parent_path();
    for (int index = 2; index < 10000; ++index) {
        auto filename = requested.stem();
        filename += std::filesystem::path("_" + std::to_string(index));
        filename += requested.extension();
        const auto candidate = parent / filename;
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    auto fallback = requested.stem();
    fallback += std::filesystem::path("_copy");
    fallback += requested.extension();
    return parent / fallback;
}

bool ImportService::SameContent(const std::filesystem::path& a,
                                const std::filesystem::path& b) {
    std::error_code ec;
    if (!std::filesystem::exists(a, ec) || !std::filesystem::exists(b, ec) ||
        std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec)) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right) return false;
    std::array<char, 64 * 1024> l{}, r{};
    while (left && right) {
        left.read(l.data(), static_cast<std::streamsize>(l.size()));
        right.read(r.data(), static_cast<std::streamsize>(r.size()));
        const auto count = left.gcount();
        if (count != right.gcount() || !std::equal(l.begin(), l.begin() + count, r.begin()))
            return false;
    }
    return true;
}

Result<ImportPlan> ImportService::Prepare(const std::filesystem::path& projectRoot,
                                          const std::filesystem::path& destination,
                                          const std::vector<ImportSource>& sources,
                                          bool autoOrganize,
                                          bool preserveFolders) const {
    try {
        return PrepareImpl(projectRoot, destination, sources, autoOrganize, preserveFolders);
    } catch (const std::filesystem::filesystem_error& error) {
        return Result<ImportPlan>::Failure(ImportError("PXIMPORT9028", destination,
            "建立匯入計畫時發生檔案系統錯誤", error.what()));
    } catch (const std::exception& error) {
        return Result<ImportPlan>::Failure(ImportError("PXIMPORT9029", destination,
            "建立匯入計畫時發生未預期錯誤", error.what()));
    } catch (...) {
        return Result<ImportPlan>::Failure(ImportError("PXIMPORT9030", destination,
            "建立匯入計畫時發生未知錯誤"));
    }
}

Result<ImportPlan> ImportService::PrepareImpl(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& destination,
        const std::vector<ImportSource>& sources,
        bool autoOrganize,
        bool preserveFolders) const {
    if (projectRoot.empty() || destination.empty() || sources.empty()) {
        return Result<ImportPlan>::Failure(
            ImportError("PXIMPORT9001", destination, "匯入計畫缺少專案、目的地或來源檔案"));
    }
    std::error_code ec;
    if (!IsWithin(destination, projectRoot / "Content")) {
        return Result<ImportPlan>::Failure(
            ImportError("PXIMPORT9002", destination, "匯入目的地必須位於專案 Content 內",
                        "目的地不會在確認匯入前建立。"));
    }
    ImportPlan plan{.projectRoot = projectRoot, .destination = destination,
                    .autoOrganize = autoOrganize, .preserveFolders = preserveFolders};
    for (const auto& source : sources) {
        ec.clear();
        if (!std::filesystem::is_regular_file(source.path, ec) ||
            source.path.extension() == ".pxmeta") continue;
        ImportCandidate candidate;
        candidate.source = source;
        candidate.type = DetectType(source.path);
        candidate.size = std::filesystem::file_size(source.path, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        auto folder = destination;
        if (autoOrganize) folder = projectRoot / "Content" / TypeFolder(candidate.type);
        else if (preserveFolders && !source.relativePath.parent_path().empty())
            folder /= source.relativePath.parent_path();
        candidate.requestedTarget = folder / source.path.filename();
        candidate.target = candidate.requestedTarget;
        if (std::filesystem::exists(candidate.requestedTarget)) {
            candidate.identical = SameContent(source.path, candidate.requestedTarget);
            candidate.policy = candidate.identical ? ImportConflictPolicy::UseExisting
                                                    : ImportConflictPolicy::KeepBoth;
            if (!candidate.identical) candidate.target = UniqueTarget(candidate.target);
        }
        plan.items.push_back(std::move(candidate));
    }
    if (plan.items.empty()) {
        return Result<ImportPlan>::Failure(
            ImportError("PXIMPORT9003", destination, "沒有可匯入的檔案"));
    }
    return Result<ImportPlan>::Success(std::move(plan));
}

void ImportService::RecalculateTargets(ImportPlan& plan) const {
    for (auto& item : plan.items) {
        item.target = item.requestedTarget;
        if (item.policy == ImportConflictPolicy::KeepBoth &&
            std::filesystem::exists(item.target)) item.target = UniqueTarget(item.target);
    }
}

Status ImportService::Start(ImportPlan plan) {
    try {
    const auto state = Progress().state;
    if (state == ImportState::Staging || state == ImportState::Committing) {
        return Status::Fail(ImportError("PXIMPORT9004", plan.destination,
                                        "另一個匯入交易仍在執行"));
    }
    Reset();
    m_plan = std::move(plan);
    m_stagingRoot = m_plan.projectRoot / ".prismatix" / "import-staging" /
                    Uuid::Random().ToString();
    m_stagedFiles.resize(m_plan.items.size());
    {
        std::lock_guard lock(m_mutex);
        m_progress = {.state = ImportState::Staging,
                      .totalItems = m_plan.items.size()};
        for (const auto& item : m_plan.items)
            if (item.enabled && item.policy != ImportConflictPolicy::Skip &&
                item.policy != ImportConflictPolicy::UseExisting)
                m_progress.totalBytes += item.size;
    }
    m_cancel = false;
    m_worker = std::jthread([this](std::stop_token token) { Stage(token); });
    return Status::Ok();
    } catch (const std::exception& error) {
        SetFailure("無法啟動匯入工作：" + std::string(error.what()));
        return Status::Fail(ImportError("PXIMPORT9031", plan.destination,
            "無法啟動匯入工作", error.what()));
    } catch (...) {
        SetFailure("無法啟動匯入工作：未知錯誤");
        return Status::Fail(ImportError("PXIMPORT9032", plan.destination,
            "無法啟動匯入工作"));
    }
}

void ImportService::Stage(std::stop_token stopToken) noexcept {
    try {
    std::error_code ec;
    std::filesystem::create_directories(m_stagingRoot / "files", ec);
    if (ec) return SetFailure("無法建立匯入 staging：" + ec.message());
    std::array<char, 1024 * 1024> buffer{};
    for (std::size_t index = 0; index < m_plan.items.size(); ++index) {
        const auto& item = m_plan.items[index];
        {
            std::lock_guard lock(m_mutex);
            m_progress.currentItem = index + 1;
            m_progress.currentFile = Utf8Path(item.source.path.filename());
        }
        if (!item.enabled || item.policy == ImportConflictPolicy::Skip ||
            item.policy == ImportConflictPolicy::UseExisting) continue;
        if (m_cancel || stopToken.stop_requested()) {
            std::lock_guard lock(m_mutex); m_progress.state = ImportState::Cancelled; return;
        }
        auto stagedName = std::filesystem::path(std::to_string(index));
        stagedName += item.source.path.extension();
        const auto staged = m_stagingRoot / "files" / stagedName;
        std::ifstream input(item.source.path, std::ios::binary);
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        if (!input || !output) return SetFailure("無法讀取或建立 staging 檔案：" +
                                                 Utf8Path(item.source.path));
        while (input) {
            if (m_cancel || stopToken.stop_requested()) {
                std::lock_guard lock(m_mutex); m_progress.state = ImportState::Cancelled; return;
            }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) break;
            output.write(buffer.data(), count);
            if (!output) return SetFailure("寫入 staging 失敗：" + Utf8Path(staged));
            std::lock_guard lock(m_mutex);
            m_progress.copiedBytes += static_cast<std::uintmax_t>(count);
        }
        m_stagedFiles[index] = staged;
    }
    std::lock_guard lock(m_mutex);
    m_progress.state = ImportState::Ready;
    m_progress.message = "素材已準備完成";
    } catch (const std::filesystem::filesystem_error& error) {
        SetFailure("準備素材時發生檔案系統錯誤：" + std::string(error.what()));
    } catch (const std::exception& error) {
        SetFailure("準備素材時發生未預期錯誤：" + std::string(error.what()));
    } catch (...) {
        SetFailure("準備素材時發生未知錯誤。請查看 Editor log。 ");
    }
}

void ImportService::SetFailure(std::string message) {
    std::lock_guard lock(m_mutex);
    m_progress.state = ImportState::Failed;
    m_progress.message = std::move(message);
}

void ImportService::Cancel() {
    m_cancel = true;
    if (m_worker.joinable()) m_worker.request_stop();
}

ImportProgress ImportService::Progress() const {
    std::lock_guard lock(m_mutex);
    return m_progress;
}

Status ImportService::Commit(resource::AssetRegistry& registry) {
    try {
        return CommitImpl(registry);
    } catch (const std::filesystem::filesystem_error& error) {
        SetFailure("提交素材時發生檔案系統錯誤：" + std::string(error.what()));
        return Status::Fail(ImportError("PXIMPORT9014", m_plan.destination,
            "提交素材時發生檔案系統錯誤", error.what()));
    } catch (const std::exception& error) {
        SetFailure("提交素材時發生未預期錯誤：" + std::string(error.what()));
        return Status::Fail(ImportError("PXIMPORT9015", m_plan.destination,
            "提交素材時發生未預期錯誤", error.what()));
    } catch (...) {
        SetFailure("提交素材時發生未知錯誤。請查看 Editor log。");
        return Status::Fail(ImportError("PXIMPORT9016", m_plan.destination,
            "提交素材時發生未知錯誤"));
    }
}

Status ImportService::CommitImpl(resource::AssetRegistry& registry) {
    if (m_worker.joinable()) m_worker.join();
    if (Progress().state != ImportState::Ready)
        return Status::Fail(ImportError("PXIMPORT9005", m_plan.destination,
                                        "匯入 staging 尚未完成"));
    {
        std::lock_guard lock(m_mutex); m_progress.state = ImportState::Committing;
    }
    struct Applied { std::filesystem::path target, backup; bool replaced = false; };
    std::vector<Applied> applied;
    std::vector<std::filesystem::path> createdMeta;
    m_commitRecords.clear();
    m_undoRoot = m_plan.projectRoot / ".prismatix" / "Undo" / "import" /
                 Uuid::Random().ToString();
    std::error_code ec;
    std::string rollbackErrors;
    const auto rememberRollbackError = [&](const std::filesystem::path& path,
                                            const std::error_code& error) {
        if (!error) return;
        if (!rollbackErrors.empty()) rollbackErrors += "\n";
        rollbackErrors += Utf8Path(path) + ": " + error.message();
    };
    const auto rollback = [&] {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            std::filesystem::remove(it->target, ec);
            rememberRollbackError(it->target, ec); ec.clear();
            if (it->replaced && !it->backup.empty()) {
                std::filesystem::rename(it->backup, it->target, ec);
                rememberRollbackError(it->target, ec); ec.clear();
            }
        }
        for (const auto& meta : createdMeta) {
            std::filesystem::remove(meta, ec);
            rememberRollbackError(meta, ec); ec.clear();
        }
    };
    const auto abort=[&](Status failure){
        rollback();
        if (!rollbackErrors.empty()) {
            (void)ImportError("PXIMPORT9036", m_plan.destination,
                "匯入 rollback 未能完整復原，請勿繼續 Build", rollbackErrors);
        }
        (void)registry.Scan(m_plan.projectRoot);
        std::lock_guard lock(m_mutex);m_progress.state=ImportState::Failed;
        m_progress.message=failure.Diagnostics().empty()?"匯入 commit 失敗":failure.Diagnostics().front().message;
        return failure;
    };
    try {
    for (std::size_t index = 0; index < m_plan.items.size(); ++index) {
        auto& item = m_plan.items[index];
        if (!item.enabled || item.policy == ImportConflictPolicy::Skip ||
            item.policy == ImportConflictPolicy::UseExisting) continue;
        std::filesystem::create_directories(item.target.parent_path(), ec);
        if (ec) return abort(Status::Fail(ImportError("PXIMPORT9006", item.target,
            "無法建立素材目的地", ec.message())));
        Applied operation{.target = item.target};
        if (item.policy == ImportConflictPolicy::Replace &&
            std::filesystem::exists(item.target)) {
            operation.replaced = true;
            operation.backup = m_stagingRoot / "backup" / std::to_string(index);
            std::filesystem::create_directories(operation.backup.parent_path(), ec);
            std::filesystem::rename(item.target, operation.backup, ec);
            if (ec) return abort(Status::Fail(ImportError("PXIMPORT9007", item.target,
                "無法備份被取代的素材", ec.message())));
            applied.push_back(operation);
        }
        std::filesystem::rename(m_stagedFiles[index], item.target, ec);
        if (ec) return abort(Status::Fail(ImportError("PXIMPORT9008", item.target,
            "無法提交匯入素材", ec.message())));
        if (!operation.replaced) applied.push_back(operation);
        const auto meta = resource::AssetRegistry::MetaPath(item.target);
        if (m_plan.preserveIdentity && !operation.replaced) {
            const auto sourceMeta = resource::AssetRegistry::MetaPath(item.source.path);
            if (std::filesystem::exists(sourceMeta)) {
                std::filesystem::copy_file(sourceMeta, meta,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) return abort(Status::Fail(ImportError("PXIMPORT9010", meta,
                    "無法保留來源素材 GUID", ec.message())));
                createdMeta.push_back(meta);
            }
        }
        if (!std::filesystem::exists(meta)) {
            auto registered = registry.RegisterAsset(m_plan.projectRoot, item.target, item.type);
            if (!registered) return abort(Status::Fail(registered.Diagnostics()));
            createdMeta.push_back(meta);
        } else if (!registry.FindPath(item.target)) {
            auto registered = registry.RegisterAsset(m_plan.projectRoot, item.target, item.type);
            if (!registered) return abort(Status::Fail(registered.Diagnostics()));
        }
        if (const Status inclusion = registry.SetIncludeInBuild(item.target, item.includeInBuild);
            !inclusion) return abort(inclusion);
        m_committedPaths.push_back(item.target);
    }
    const Status identity = registry.Scan(m_plan.projectRoot);
    if (!identity) return abort(identity);
    std::filesystem::create_directories(m_undoRoot, ec);
    if (ec) return abort(Status::Fail(ImportError("PXIMPORT9011", m_undoRoot,
        "無法建立匯入 Undo 儲存區", ec.message())));
    for (std::size_t index = 0; index < applied.size(); ++index) {
        auto& operation = applied[index];
        ImportCommitRecord record{.target=operation.target,
            .meta=resource::AssetRegistry::MetaPath(operation.target),
            .replaced=operation.replaced};
        auto importedName = std::filesystem::path(std::to_string(index) + ".imported");
        importedName += operation.target.extension();
        record.importedBackup = m_undoRoot / importedName;
        record.metaBackup = m_undoRoot / (std::to_string(index) + ".pxmeta");
        if (operation.replaced) {
            auto originalName = std::filesystem::path(std::to_string(index) + ".original");
            originalName += operation.target.extension();
            record.originalBackup = m_undoRoot / originalName;
            std::filesystem::rename(operation.backup, record.originalBackup, ec);
            if (ec) return abort(Status::Fail(ImportError("PXIMPORT9012", operation.target,
                "無法保存被取代素材的 Undo 版本", ec.message())));
            operation.backup = record.originalBackup;
        }
        m_commitRecords.push_back(std::move(record));
    }
    std::filesystem::remove_all(m_stagingRoot, ec);
    {
        std::lock_guard lock(m_mutex);
        m_progress.state = ImportState::Completed;
        m_progress.message = "素材匯入完成";
    }
    return Status::Ok();
    } catch (const std::filesystem::filesystem_error& error) {
        return abort(Status::Fail(ImportError("PXIMPORT9033", error.path1(),
            "提交素材時已安全回復整筆交易", error.what())));
    } catch (const std::exception& error) {
        return abort(Status::Fail(ImportError("PXIMPORT9034", m_plan.destination,
            "提交素材時已安全回復整筆交易", error.what())));
    } catch (...) {
        return abort(Status::Fail(ImportError("PXIMPORT9035", m_plan.destination,
            "提交素材時已安全回復整筆交易", "未知錯誤")));
    }
}

void ImportService::Reset() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
    std::error_code ec;
    if (!m_stagingRoot.empty()) std::filesystem::remove_all(m_stagingRoot, ec);
    m_plan = {};
    m_stagingRoot.clear();
    m_stagedFiles.clear();
    m_committedPaths.clear();
    m_commitRecords.clear();
    m_undoRoot.clear();
    m_cancel = false;
    std::lock_guard lock(m_mutex);
    m_progress = {};
}

}  // namespace px::editor
