#include "Engine/IO/AtomicFile.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <atomic>
#include <chrono>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace px::io {

namespace {
std::atomic<std::uint64_t> g_tempId{ 0 };

diag::Diagnostic FileError(const std::filesystem::path& path, std::string message,
                           std::string details = {}) {
    diag::Diagnostic d;
    d.severity = diag::Severity::Error;
    d.code = "PXIO-E1001";
    d.category = "io";
    d.message = std::move(message);
    d.details = std::move(details);
    d.source.path = path.generic_string();
    return d;
}

std::filesystem::path TempPathFor(const std::filesystem::path& target) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return target.parent_path() /
           (target.filename().string() + ".tmp." + std::to_string(now) + "." +
            std::to_string(g_tempId.fetch_add(1)));
}

Status Commit(const std::filesystem::path& temp, const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return Status::Fail(
            FileError(target, "Could not replace the destination file atomically.",
                      "Win32 error " + std::to_string(code)));
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return Status::Fail(FileError(target, "Could not replace the destination file atomically.",
                                      ec.message()));
    }
#endif
    return Status::Ok();
}
}  // namespace

Status AtomicFile::WriteText(const std::filesystem::path& path, std::string_view text) {
    return WriteBinary(path, std::span<const std::uint8_t>(
                                 reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Status AtomicFile::WriteBinary(const std::filesystem::path& path,
                               std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return Status::Fail(
                FileError(path, "Could not create the destination directory.", ec.message()));
        }
    }

    const std::filesystem::path temp = TempPathFor(path);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Status::Fail(FileError(path, "Could not open a temporary file for writing."));
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) {
        out.close();
        std::filesystem::remove(temp, ec);
        return Status::Fail(FileError(path, "Writing the temporary file failed."));
    }
    out.close();
    return Commit(temp, path);
}

}  // namespace px::io
