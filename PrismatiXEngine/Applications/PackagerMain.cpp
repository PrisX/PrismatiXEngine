#include "Engine/Support/Utf8Path.h"
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>

#include "Engine/SDK/Packager.h"
#include "Engine/IO/PackageVerifier.h"

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

}  // namespace

int RunMain(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--create-overlay") {
        const auto base = Utf8Path(argv[2]);
        const auto target = Utf8Path(argv[3]);
        const auto output = Utf8Path(argv[4]);
        if (!base.is_absolute() || !target.is_absolute() ||
            !output.is_absolute()) {
            Emit(FailedEvent({}, "PXPKG1516",
                             "Base, target, and output paths must be absolute"));
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
                });
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
    if (argc == 4 && std::string_view(argv[1]) == "--verify-overlay") {
        const std::filesystem::path distribution = Utf8Path(argv[2]);
        const std::filesystem::path overlay = Utf8Path(argv[3]);
        if (!distribution.is_absolute() || !overlay.is_absolute()) {
            Emit(FailedEvent({}, "PXPKG1513",
                             "Base distribution and overlay paths must be absolute"));
            return static_cast<int>(px::sdk::PackageExitCode::Failed);
        }
        try {
            const auto verified =
                px::io::VerifyPackageOverlay(distribution, overlay);
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
    const bool packageRequest = argc == 3 &&
        std::string_view(argv[1]) == "--request";
    const bool validateOnly = (argc == 3 || argc == 4) &&
        std::string_view(argv[1]) == "--validate-request" &&
        (argc == 3 || std::string_view(argv[3]) == "--verify-media");
    if (!packageRequest && !validateOnly) {
        Emit(FailedEvent({}, "PXPKG1001", "Usage: PrismatiXPackager --request <absolute-json-path> | --validate-request <absolute-json-path> [--verify-media] | --verify-package <absolute-distribution-path> | --create-overlay <absolute-base-distribution> <absolute-target-distribution> <absolute-output-pdx> | --verify-overlay <absolute-base-distribution> <absolute-overlay-pdx>"));
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
