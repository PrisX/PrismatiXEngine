#include "Engine/Support/Utf8Path.h"
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>

#include "Engine/SDK/Packager.h"

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
    if (argc != 3 || std::string_view(argv[1]) != "--request") {
        Emit(FailedEvent({}, "PXPKG1001", "Usage: PrismatiXPackager --request <absolute-json-path>"));
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
        const auto result = px::sdk::RunPackager(parsed.request, Emit);
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
