#include "Editor/Workspace/RecoveryManager.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Support/Logger.h"

#include <zstd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace px::editor {

namespace {
constexpr char kMagic[] = "PXRC1";
void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff)); }
void WriteU64(std::vector<std::uint8_t>& out, std::uint64_t value) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff)); }
bool ReadU32(const std::vector<std::uint8_t>& in, std::size_t& offset, std::uint32_t& value) { if (offset + 4 > in.size()) return false; value = 0; for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(in[offset++]) << (i * 8); return true; }
bool ReadU64(const std::vector<std::uint8_t>& in, std::size_t& offset, std::uint64_t& value) { if (offset + 8 > in.size()) return false; value = 0; for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(in[offset++]) << (i * 8); return true; }
void WriteString(std::vector<std::uint8_t>& out, const std::string& value) { WriteU32(out, static_cast<std::uint32_t>(value.size())); out.insert(out.end(), value.begin(), value.end()); }
bool ReadString(const std::vector<std::uint8_t>& in, std::size_t& offset, std::string& value) { std::uint32_t size = 0; if (!ReadU32(in, offset, size) || offset + size > in.size()) return false; value.assign(reinterpret_cast<const char*>(in.data() + offset), size); offset += size; return true; }
diag::Diagnostic RecoveryError(const std::filesystem::path& path, std::string message, std::string details = {}) { diag::Diagnostic d{ diag::Severity::Error, "PXRECOVERY-E1001", "recovery", std::move(message), std::move(details) }; d.source.path = path.generic_string(); return d; }
std::uint64_t UnixNow() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
Result<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path& path) { std::ifstream in(path, std::ios::binary | std::ios::ate); if (!in) return Result<std::vector<std::uint8_t>>::Failure(RecoveryError(path, "Could not read recovery snapshot.")); const std::streamsize size = in.tellg(); in.seekg(0); std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size)); if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return Result<std::vector<std::uint8_t>>::Failure(RecoveryError(path, "Recovery snapshot is truncated.")); return Result<std::vector<std::uint8_t>>::Success(std::move(bytes)); }
Result<RecoverySnapshot> ReadHeader(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::size_t& payloadOffset, std::uint64_t& compressedSize) {
    if (bytes.size() < sizeof(kMagic) - 1 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic) - 1) != 0) return Result<RecoverySnapshot>::Failure(RecoveryError(path, "Invalid recovery snapshot header."));
    std::size_t offset = sizeof(kMagic) - 1; RecoverySnapshot snapshot; std::string id, source; std::uint64_t contentSize = 0;
    if (!ReadString(bytes, offset, id) || !ReadString(bytes, offset, source) || !ReadString(bytes, offset, snapshot.baseHash) || !ReadU64(bytes, offset, snapshot.timestamp) || !ReadU64(bytes, offset, contentSize) || !ReadU64(bytes, offset, compressedSize) || offset + compressedSize > bytes.size()) return Result<RecoverySnapshot>::Failure(RecoveryError(path, "Recovery snapshot metadata is corrupt."));
    auto parsed = Uuid::Parse(id); if (!parsed) return Result<RecoverySnapshot>::Failure(RecoveryError(path, "Recovery snapshot has an invalid document ID."));
    snapshot.documentId = *parsed; snapshot.file = path; snapshot.sourcePath = std::filesystem::u8path(source); snapshot.contentSize = static_cast<std::size_t>(contentSize); payloadOffset = offset; return Result<RecoverySnapshot>::Success(std::move(snapshot));
}
}

std::filesystem::path RecoveryManager::UserRecoveryRoot() {
#ifdef _WIN32
    if (const char* root = std::getenv("LOCALAPPDATA")) return std::filesystem::path(root) / "PrismatiX" / "Recovery";
#else
    if (const char* root = std::getenv("HOME")) return std::filesystem::path(root) / ".local" / "share" / "PrismatiX" / "Recovery";
#endif
    return std::filesystem::temp_directory_path() / "PrismatiX" / "Recovery";
}
Status RecoveryManager::BeginSession(const Uuid& projectId, const std::filesystem::path& projectPath) {
    if (!m_sessionMarker.empty()) EndSession();
    m_projectId = projectId; m_projectPath = projectPath; m_root = UserRecoveryRoot() / projectId.ToString(); m_sessionMarker = m_root / "session.lock";
    std::error_code ec; std::filesystem::create_directories(m_root, ec); if (ec) return Status::Fail(RecoveryError(m_root, "Could not create recovery storage.", ec.message()));
    m_hadUncleanSession = std::filesystem::exists(m_sessionMarker);
    Status status = io::AtomicFile::WriteText(m_sessionMarker, "project = " + projectPath.generic_string() + "\nstarted = " + std::to_string(UnixNow()) + "\n");
    if (m_hadUncleanSession) { diag::Diagnostic d{ diag::Severity::Warning, "PXRECOVERY-W1001", "recovery", "The previous editor session did not close cleanly.", "Open Recovery Center before editing recovered documents." }; d.source.path = projectPath.generic_string(); diag::Emit(std::move(d)); }
    return status;
}
Status RecoveryManager::EndSession() { if (m_sessionMarker.empty()) return Status::Ok(); std::error_code ec; std::filesystem::remove(m_sessionMarker, ec); m_sessionMarker.clear(); return ec ? Status::Fail(RecoveryError(m_root, "Could not clear the recovery session marker.", ec.message())) : Status::Ok(); }
bool RecoveryManager::ShouldSnapshot(const Uuid& documentId, std::chrono::steady_clock::time_point lastEdit) const { const auto now = std::chrono::steady_clock::now(); if (now - lastEdit < std::chrono::seconds(2)) return false; auto it = m_lastSnapshot.find(documentId); return it == m_lastSnapshot.end() || now - it->second >= std::chrono::seconds(30); }
Status RecoveryManager::SaveSnapshot(const Uuid& documentId, const std::filesystem::path& sourcePath, const std::string& baseHash, const std::string& content) {
    if (m_root.empty()) return Status::Fail(RecoveryError(sourcePath, "Recovery session is not active."));
    std::vector<std::uint8_t> compressed(ZSTD_compressBound(content.size())); const std::size_t size = ZSTD_compress(compressed.data(), compressed.size(), content.data(), content.size(), 3); if (ZSTD_isError(size)) return Status::Fail(RecoveryError(sourcePath, "Could not compress recovery snapshot.", ZSTD_getErrorName(size))); compressed.resize(size);
    const std::uint64_t timestamp = UnixNow(); std::vector<std::uint8_t> bytes(kMagic, kMagic + sizeof(kMagic) - 1); WriteString(bytes, documentId.ToString()); WriteString(bytes, sourcePath.generic_string()); WriteString(bytes, baseHash); WriteU64(bytes, timestamp); WriteU64(bytes, content.size()); WriteU64(bytes, compressed.size()); bytes.insert(bytes.end(), compressed.begin(), compressed.end());
    const auto path = m_root / (documentId.ToString() + "-" + std::to_string(timestamp) + ".pxrecovery"); Status status = io::AtomicFile::WriteBinary(path, bytes); if (status) { m_lastSnapshot[documentId] = std::chrono::steady_clock::now(); Prune(documentId); PX_LOG_DEBUG("recovery snapshot saved document={} path={} bytes={}", documentId.ToString(), path.generic_string(), content.size()); } return status;
}
Result<std::vector<RecoverySnapshot>> RecoveryManager::ListSnapshots() const {
    std::vector<RecoverySnapshot> snapshots; std::vector<diag::Diagnostic> diagnostics; std::error_code ec; if (!std::filesystem::exists(m_root)) return Result<std::vector<RecoverySnapshot>>::Success({});
    for (const auto& entry : std::filesystem::directory_iterator(m_root, ec)) { if (!entry.is_regular_file() || entry.path().extension() != ".pxrecovery") continue; auto bytes = ReadFile(entry.path()); if (!bytes) { diagnostics.insert(diagnostics.end(), bytes.Diagnostics().begin(), bytes.Diagnostics().end()); continue; } std::size_t offset = 0; std::uint64_t compressed = 0; auto header = ReadHeader(entry.path(), bytes.Value(), offset, compressed); if (!header) { diagnostics.insert(diagnostics.end(), header.Diagnostics().begin(), header.Diagnostics().end()); continue; } snapshots.push_back(header.TakeValue()); }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) { return a.timestamp > b.timestamp; }); return diagnostics.empty() ? Result<std::vector<RecoverySnapshot>>::Success(std::move(snapshots)) : Result<std::vector<RecoverySnapshot>>::Failure(std::move(diagnostics));
}
Result<std::string> RecoveryManager::LoadContent(const RecoverySnapshot& snapshot) const { auto bytes = ReadFile(snapshot.file); if (!bytes) return Result<std::string>::Failure(bytes.Diagnostics()); std::size_t offset = 0; std::uint64_t compressed = 0; auto header = ReadHeader(snapshot.file, bytes.Value(), offset, compressed); if (!header) return Result<std::string>::Failure(header.Diagnostics()); std::string content(header.Value().contentSize, '\0'); const std::size_t result = ZSTD_decompress(content.data(), content.size(), bytes.Value().data() + offset, compressed); if (ZSTD_isError(result) || result != content.size()) return Result<std::string>::Failure(RecoveryError(snapshot.file, "Could not decompress recovery snapshot.", ZSTD_getErrorName(result))); return Result<std::string>::Success(std::move(content)); }
Status RecoveryManager::Discard(const RecoverySnapshot& snapshot) { std::error_code ec; std::filesystem::remove(snapshot.file, ec); return ec ? Status::Fail(RecoveryError(snapshot.file, "Could not discard recovery snapshot.", ec.message())) : Status::Ok(); }
void RecoveryManager::Prune(const Uuid& documentId) { auto listed = ListSnapshots(); if (!listed) return; std::vector<RecoverySnapshot> own; for (const auto& item : listed.Value()) if (item.documentId == documentId) own.push_back(item); for (std::size_t i = 10; i < own.size(); ++i) { std::error_code ec; std::filesystem::remove(own[i].file, ec); } }

}  // namespace px::editor
