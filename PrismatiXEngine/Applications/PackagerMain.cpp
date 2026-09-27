#include "Engine/Support/Utf8Path.h"
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "Engine/SDK/Packager.h"
#include "Engine/IO/PackageVerifier.h"
#include "Engine/IO/OverlaySignature.h"

namespace {

std::filesystem::path Utf8Path(const std::string_view value) { return px::PathFromUtf8(value); }

void Emit(const px::sdk::PackageEvent& event) {
    std::cout << px::sdk::SerializePackageEvent(event) << '\n';
    std::cout.flush();
}

px::sdk::PackageEvent FailedEvent(std::string requestId, std::string code, std::string message, const bool retryable = false) {
    px::sdk::PackageEvent event;
    event.kind = px::sdk::PackageEventKind::Failed;
    event.requestId = std::move(requestId);
    event.code = std::move(code);
    event.message = std::move(message);
    event.retryable = retryable;
    event.diagnostics.push_back({event.code, event.message, event.retryable});
    return event;
}

px::sdk::PackageEvent FailedEvent(
    std::string requestId,
    std::vector<px::sdk::PackageDiagnostic> diagnostics) {
    px::sdk::PackageEvent event;
    event.kind = px::sdk::PackageEventKind::Failed;
    event.requestId = std::move(requestId);
    event.diagnostics = std::move(diagnostics);
    if (!event.diagnostics.empty()) {
        event.code = event.diagnostics.front().code;
        event.message = event.diagnostics.front().message;
        event.retryable = event.diagnostics.front().retryable;
    }
    return event;
}

bool WriteNewKeyFile(const std::filesystem::path& path,
                     const std::string_view text, const bool privateKey) {
#ifdef _WIN32
    (void)privateKey;
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(handle, text.data(),
                                   static_cast<DWORD>(text.size()), &written,
                                   nullptr) && written == text.size() &&
                         FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!success) DeleteFileW(path.c_str());
    return success;
#else
    const int mode = privateKey ? S_IRUSR | S_IWUSR
                                : S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = write(fd, text.data() + offset,
                                 text.size() - offset);
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
    }
    const bool flushed = offset == text.size() && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    const bool success = flushed && closed;
    if (!success) unlink(path.c_str());
    return success;
#endif
}

}  // namespace

int RunMain(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--generate-overlay-key") {
        const auto privatePath = Utf8Path(argv[2]);
        const auto publicPath = Utf8Path(argv[3]);
        if (!privatePath.is_absolute() || !publicPath.is_absolute() ||
            privatePath == publicPath) {
            Emit(FailedEvent({}, "PXPKG1520",
                             "Private and public key outputs must be distinct absolute paths"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        auto keys = px::io::GenerateOverlaySigningKeyPair();
        if (!keys) {
            Emit(FailedEvent({}, "PXPKG1521", "Overlay signing key generation failed"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        if (!WriteNewKeyFile(publicPath, keys->publicKeyHex + "\n", false)) {
            Emit(FailedEvent({}, "PXPKG1522", "Public key output already exists or cannot be written"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        const bool privateWritten = WriteNewKeyFile(
            privatePath, keys->privateKeyHex + "\n", true);
        std::fill(keys->privateKeyHex.begin(), keys->privateKeyHex.end(), '\0');
        if (!privateWritten) {
            Emit(FailedEvent({}, "PXPKG1523", "Private key output already exists or cannot be written; the new public key file was retained"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        std::cout << "{\"event\":\"overlay-key-generated\"}\n";
        return static_cast<int>(px::sdk::PackageExitCode::Completed);
    }
    if ((argc == 5 ||
         (argc == 7 && std::string_view(argv[5]) == "--signing-key") ||
         (argc == 9 && std::string_view(argv[5]) == "--signing-key" &&
          std::string_view(argv[7]) == "--optional-id")) &&
        std::string_view(argv[1]) == "--create-overlay") {
        const auto base = Utf8Path(argv[2]);
        const auto target = Utf8Path(argv[3]);
        const auto output = Utf8Path(argv[4]);
        const auto signingKey = argc >= 7 ? Utf8Path(argv[6])
                                          : std::filesystem::path{};
        const std::string_view optionalId = argc == 9 ? argv[8] : "";
        if (!base.is_absolute() || !target.is_absolute() ||
            !output.is_absolute() ||
            (argc >= 7 && !signingKey.is_absolute())) {
            Emit(FailedEvent({}, "PXPKG1516",
                             "Base, target, output, and optional signing key paths must be absolute"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        try {
            const auto created = px::io::CreatePackageOverlay(
                base, target, output,
                [](const std::string_view phase, const std::uint64_t current,
                   const std::uint64_t total, const std::uint64_t rawBytes) {
                    px::sdk::PackageEvent progress;
                    progress.kind = px::sdk::PackageEventKind::Progress;
                    progress.phase = std::string(phase);
                    progress.current = current;
                    progress.total = total;
                    progress.message = std::to_string(rawBytes) +
                                       " raw bytes processed";
                    Emit(progress);
                }, signingKey, optionalId);
            if (!created.created) {
                Emit(FailedEvent({}, "PXPKG1517", created.error));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            std::cout << "{\"event\":\"overlay-created\",\"changedResources\":"
                      << created.changedResources << ",\"removedResources\":"
                      << created.removedResources << ",\"changedRawBytes\":"
                      << created.changedRawBytes << "}\n";
            return static_cast<int>(px::sdk::PackageExitCode::Completed);
        } catch (const std::exception& error) {
            Emit(FailedEvent({}, "PXPKG1518",
                             std::string("Overlay creation failed unexpectedly: ") +
                                 error.what()));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        } catch (...) {
            Emit(FailedEvent({}, "PXPKG1518",
                             "Overlay creation failed unexpectedly"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
    }
    if ((argc == 4 ||
         (argc == 5 && std::string_view(argv[4]) == "--require-signature")) &&
        std::string_view(argv[1]) == "--verify-overlay") {
        const std::filesystem::path distribution = Utf8Path(argv[2]);
        const std::filesystem::path overlay = Utf8Path(argv[3]);
        if (!distribution.is_absolute() || !overlay.is_absolute()) {
            Emit(FailedEvent({}, "PXPKG1513",
                             "Base distribution and overlay paths must be absolute"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        try {
            const auto verified = argc == 5
                ? px::io::VerifySignedPackageOverlay(distribution, overlay)
                : px::io::VerifyPackageOverlay(distribution, overlay);
            if (!verified.valid) {
                Emit(FailedEvent({}, "PXPKG1514", verified.error));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            std::cout << "{\"event\":\"overlay-verified\",\"changedResources\":"
                      << verified.changedResources << ",\"removedResources\":"
                      << verified.tombstones.size() << ",\"changedRawBytes\":"
                      << verified.changedRawBytes << "}\n";
            return static_cast<int>(px::sdk::PackageExitCode::Completed);
        } catch (const std::exception& error) {
            Emit(FailedEvent({}, "PXPKG1515",
                             std::string("Overlay verification failed unexpectedly: ") +
                                 error.what()));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        } catch (...) {
            Emit(FailedEvent({}, "PXPKG1515",
                             "Overlay verification failed unexpectedly"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
    }
    if (argc == 5 &&
        std::string_view(argv[1]) == "--verify-overlay-after-chain") {
        const auto base = Utf8Path(argv[2]);
        const auto updates = Utf8Path(argv[3]);
        const auto patch = Utf8Path(argv[4]);
        if (!base.is_absolute() || !updates.is_absolute() ||
            !patch.is_absolute()) {
            Emit(FailedEvent({}, "PXPKG1513",
                             "Base, updates, and overlay paths must be absolute"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        try {
            const auto chain = px::io::ResolveSignedPackageOverlayChain(
                base, updates, {}, patch);
            if (!chain.valid || chain.mounts.empty() ||
                !chain.ignoredFiles.empty()) {
                Emit(FailedEvent({}, "PXPKG1514",
                                 "A complete signed main update chain is required: " +
                                     (chain.error.empty()
                                          ? "unverified or unrelated update files"
                                          : chain.error)));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            const auto verified =
                px::io::VerifySignedPackageOverlayFromDocuments(
                    chain.targetManifestJson, chain.targetContentIndexJson,
                    patch);
            if (!verified.valid) {
                Emit(FailedEvent({}, "PXPKG1514", verified.error));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            std::cout << "{\"event\":\"overlay-verified\",\"changedResources\":"
                      << verified.changedResources << ",\"removedResources\":"
                      << verified.tombstones.size() << ",\"changedRawBytes\":"
                      << verified.changedRawBytes << "}\n";
            return static_cast<int>(px::sdk::PackageExitCode::Completed);
        } catch (const std::exception& error) {
            Emit(FailedEvent({}, "PXPKG1515",
                             std::string("Overlay verification failed unexpectedly: ") +
                                 error.what()));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        } catch (...) {
            Emit(FailedEvent({}, "PXPKG1515",
                             "Overlay verification failed unexpectedly"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "--verify-package") {
        const std::filesystem::path distribution = Utf8Path(argv[2]);
        if (!distribution.is_absolute()) {
            Emit(FailedEvent({}, "PXPKG1510", "Distribution path must be absolute"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        try {
            const auto verified = px::io::VerifyPackageContents(
                distribution, [](const std::uint64_t current,
                                 const std::uint64_t total,
                                 const std::uint64_t rawBytes) {
                    px::sdk::PackageEvent event;
                    event.kind = px::sdk::PackageEventKind::Progress;
                    event.phase = "contents";
                    event.current = current;
                    event.total = total;
                    event.message = std::to_string(rawBytes) + " raw bytes checked";
                    Emit(event);
                });
            if (!verified.valid) {
                Emit(FailedEvent({}, "PXPKG1511", verified.error));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            std::cout << "{\"event\":\"verified\",\"archives\":"
                      << verified.archives << ",\"resources\":" << verified.resources
                      << ",\"rawBytes\":" << verified.rawBytes << "}\n";
            return static_cast<int>(px::sdk::PackageExitCode::Completed);
        } catch (const std::exception& error) {
            Emit(FailedEvent({}, "PXPKG1512",
                             std::string("Package verification failed unexpectedly: ") + error.what()));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        } catch (...) {
            Emit(FailedEvent({}, "PXPKG1512",
                             "Package verification failed unexpectedly"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
    }
    const std::string_view mode = argv[1];
    const bool planOverlay = mode == "--plan-overlay-from-request" ||
                             mode == "--plan-overlay-from-chain-request";
    const bool overlayFromRequest =
        (argc == 5 ||
         (argc == 7 && std::string_view(argv[5]) == "--signing-key") ||
         (argc == 9 && std::string_view(argv[5]) == "--signing-key" &&
          std::string_view(argv[7]) == "--optional-id")) &&
        (mode == "--create-overlay-from-request" ||
         mode == "--plan-overlay-from-request");
    const bool overlayFromChainRequest =
        ((argc == 8 && std::string_view(argv[6]) == "--signing-key") ||
         (argc == 10 && std::string_view(argv[6]) == "--signing-key" &&
          std::string_view(argv[8]) == "--optional-id")) &&
        (mode == "--create-overlay-from-chain-request" ||
         mode == "--plan-overlay-from-chain-request");
    const bool packageRequest = argc == 3 &&
        std::string_view(argv[1]) == "--request";
    const bool validateOnly = (argc == 3 || argc == 4) &&
        std::string_view(argv[1]) == "--validate-request" &&
        (argc == 3 || std::string_view(argv[3]) == "--verify-media");
    if (!packageRequest && !validateOnly && !overlayFromRequest &&
        !overlayFromChainRequest) {
        Emit(FailedEvent({}, "PXPKG1001",
                         "Usage: PrismatiXPackager --request <absolute-json-path> | --validate-request <absolute-json-path> [--verify-media] | --verify-package <absolute-distribution-path> | --generate-overlay-key <absolute-private-key-file> <absolute-public-key-file> | --create-overlay <absolute-base-distribution> <absolute-target-distribution> <absolute-output-pdx> [--signing-key <absolute-private-key-file> [--optional-id <id>]] | --create-overlay-from-request <absolute-json-path> <absolute-base-distribution> <absolute-output-pdx> [--signing-key <absolute-private-key-file> [--optional-id <id>]] | --plan-overlay-from-request <absolute-json-path> <absolute-base-distribution> <absolute-unused-output-pdx> [--signing-key <absolute-private-key-file> [--optional-id <id>]] | --create-overlay-from-chain-request <absolute-json-path> <absolute-base-distribution> <absolute-updates-directory> <absolute-output-pdx> --signing-key <absolute-private-key-file> [--optional-id <id>] | --plan-overlay-from-chain-request <absolute-json-path> <absolute-base-distribution> <absolute-updates-directory> <absolute-unused-output-pdx> --signing-key <absolute-private-key-file> [--optional-id <id>] | --verify-overlay <absolute-base-distribution> <absolute-overlay-pdx> [--require-signature] | --verify-overlay-after-chain <absolute-base-distribution> <absolute-updates-directory> <absolute-overlay-pdx>"));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }

    const std::filesystem::path requestPath = Utf8Path(argv[2]);
    if (!requestPath.is_absolute()) {
        Emit(FailedEvent({}, "PXPKG1002", "The request path must be absolute"));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }

    std::ifstream input(requestPath, std::ios::binary);
    if (!input) {
        Emit(FailedEvent({}, "PXPKG1003", "The request file could not be opened", true));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }
    std::ostringstream text;
    text << input.rdbuf();
    if (!input.eof() && input.fail()) {
        Emit(FailedEvent({}, "PXPKG1004", "The request file could not be read", true));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }

    auto parsed = px::sdk::ParsePackageRequest(text.str());
    if (!parsed.Valid()) {
        Emit(FailedEvent(parsed.request.requestId, std::move(parsed.diagnostics)));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }

    try {
        if (overlayFromRequest || overlayFromChainRequest) {
            const auto base = Utf8Path(argv[3]);
            const auto updates = overlayFromChainRequest
                ? Utf8Path(argv[4]) : std::filesystem::path{};
            const auto output = Utf8Path(argv[overlayFromChainRequest ? 5 : 4]);
            const auto signingKey = argc >= (overlayFromChainRequest ? 8 : 7)
                ? Utf8Path(argv[overlayFromChainRequest ? 7 : 6])
                : std::filesystem::path{};
            const std::string_view optionalId =
                argc == (overlayFromChainRequest ? 10 : 9)
                    ? argv[overlayFromChainRequest ? 9 : 8] : "";
            if (!base.is_absolute() || !output.is_absolute() ||
                (overlayFromChainRequest && !updates.is_absolute()) ||
                (!signingKey.empty() && !signingKey.is_absolute())) {
                Emit(FailedEvent(parsed.request.requestId, "PXPKG1516",
                                 "Base, output, and optional signing key paths must be absolute"));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            std::string previousManifest;
            if (overlayFromChainRequest) {
                const auto chain = px::io::ResolveSignedPackageOverlayChain(
                    base, updates);
                if (!chain.valid || chain.mounts.empty() ||
                    !chain.ignoredFiles.empty()) {
                    Emit(FailedEvent(parsed.request.requestId, "PXPKG1517",
                                     "A complete signed main update chain is required: " +
                                         (chain.error.empty()
                                              ? "unverified or unrelated update files"
                                              : chain.error)));
                    return static_cast<int>(px::sdk::PackageExitCode::Failed);
                }
                previousManifest = chain.targetManifestJson;
            }
            auto created = px::io::CreatePackageOverlayFromRequest(
                base, parsed.request, output, Emit,
                [](const std::string_view phase, const std::uint64_t current,
                   const std::uint64_t total, const std::uint64_t rawBytes) {
                    px::sdk::PackageEvent progress;
                    progress.kind = px::sdk::PackageEventKind::Progress;
                    progress.phase = std::string(phase);
                    progress.current = current;
                    progress.total = total;
                    progress.message = std::to_string(rawBytes) +
                                       " raw bytes processed";
                    Emit(progress);
                }, signingKey, optionalId, updates, previousManifest,
                planOverlay);
            if (planOverlay ? !created.planned : !created.created) {
                if (!created.diagnostics.empty())
                    Emit(FailedEvent(parsed.request.requestId,
                                     std::move(created.diagnostics)));
                else
                    Emit(FailedEvent(parsed.request.requestId, "PXPKG1517",
                                     created.error));
                return static_cast<int>(px::sdk::PackageExitCode::Failed);
            }
            std::cout << "{\"event\":\""
                      << (planOverlay ? "overlay-planned" : "overlay-created")
                      << "\",\"changedResources\":"
                      << created.changedResources << ",\"removedResources\":"
                      << created.removedResources << ",\"changedRawBytes\":"
                      << created.changedRawBytes << "}\n";
            return static_cast<int>(px::sdk::PackageExitCode::Completed);
        }
        const auto result = validateOnly
            ? px::sdk::ValidatePackageRequest(
                  parsed.request, Emit,
                  argc == 4 ? px::sdk::PackageValidationDepth::AllInputs
                            : px::sdk::PackageValidationDepth::Structural)
            : px::sdk::RunPackager(parsed.request, Emit);
        return static_cast<int>(result.exitCode);
    } catch (const std::exception& error) {
        Emit(FailedEvent(parsed.request.requestId, "PXPKG1005",
                         std::string("Packager failed unexpectedly: ") + error.what()));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    } catch (...) {
        Emit(FailedEvent(parsed.request.requestId, "PXPKG1005",
                         "Packager failed unexpectedly with a non-standard exception"));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> encoded;
    for (int i = 0; i < argc; ++i) encoded.push_back(px::PathToUtf8(std::filesystem::path(argv[i])));
    std::vector<char*> arguments;
    for (auto& value : encoded) arguments.push_back(value.data());
    return RunMain(argc, arguments.data());
}
#else
int main(int argc, char** argv) { return RunMain(argc, argv); }
#endif
