#include <fstream>
#include <iostream>
#include <sstream>

#include "Engine/SDK/Packager.h"

namespace {

std::filesystem::path Utf8Path(const std::string_view value) { return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size())); }

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
    return event;
}

}  // namespace

int main(int argc, char** argv) {
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
        const auto& diagnostic = parsed.diagnostics.front();
        Emit(FailedEvent(parsed.request.requestId, diagnostic.code, diagnostic.message, diagnostic.retryable));
        return static_cast<int>(px::sdk::PackageExitCode::Failed);
    }

    const auto result = px::sdk::RunPackager(parsed.request, Emit);
    return static_cast<int>(result.exitCode);
}
