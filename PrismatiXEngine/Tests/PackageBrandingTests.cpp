// Distribution branding and platform layouts (Engine/SDK/PackageBranding.h):
// icons, the Windows version resource and engine folder, the macOS .app
// bundle and the Linux lib/ + .desktop layout. The byte and layout builders
// are platform neutral, so every host checks every platform's output; the
// Windows resource round trip runs on Windows against the built Players.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "Engine/SDK/PackageBranding.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Support/Utf8Path.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

using px::sdk::branding::Bytes;
using px::sdk::branding::RgbaImage;
namespace branding = px::sdk::branding;

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

void WriteBytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

Bytes ReadBytes(const std::filesystem::path& path) {
    const std::string text = Read(path);
    return {text.begin(), text.end()};
}

std::uint16_t U16(const Bytes& bytes, const std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}
std::uint32_t U32(const Bytes& bytes, const std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16)) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::uint32_t BigEndian(const Bytes& bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
}

// A recognisable non-square test image: opaque gradient with a transparent
// border column.
RgbaImage Gradient(const int width, const int height) {
    RgbaImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto* pixel = &image.pixels[(static_cast<std::size_t>(y) * width + x) * 4];
            pixel[0] = static_cast<std::uint8_t>(x * 255 / std::max(1, width - 1));
            pixel[1] = static_cast<std::uint8_t>(y * 255 / std::max(1, height - 1));
            pixel[2] = 128;
            pixel[3] = x == 0 ? 0 : 255;
        }
    return image;
}

std::string Lines(const std::vector<std::string>& values) {
    std::string text;
    for (const auto& value : values) text += value + "\n";
    return text;
}

void Put16(Bytes& bytes, const std::size_t at, const std::uint32_t value) {
    bytes[at] = static_cast<std::uint8_t>(value & 0xff);
    bytes[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}
void Put32(Bytes& bytes, const std::size_t at, const std::uint32_t value) {
    Put16(bytes, at, value & 0xffff);
    Put16(bytes, at + 2, value >> 16);
}

// A minimal PE32+ image the way MSVC lays one out: .text, an empty .rsrc
// and a .reloc block after it, then `overlay` bytes. No host can run it, but
// every host can rewrite its resources.
constexpr std::size_t kSyntheticOptional = 0x98;
constexpr std::size_t kSyntheticDirectories = kSyntheticOptional + 112;
constexpr std::size_t kSyntheticSections = kSyntheticOptional + 240;
Bytes SyntheticPe(const std::string_view overlay = {}) {
    Bytes image(0xA00, 0);
    image[0] = 'M';
    image[1] = 'Z';
    Put32(image, 0x3C, 0x80);
    std::memcpy(&image[0x80], "PE\0\0", 4);
    Put16(image, 0x84, 0x8664);
    Put16(image, 0x86, 3);
    Put16(image, 0x94, 240);
    Put16(image, 0x96, 0x22);
    Put16(image, kSyntheticOptional, 0x20b);
    Put32(image, kSyntheticOptional + 8, 0x400);       // SizeOfInitializedData
    Put32(image, kSyntheticOptional + 16, 0x1000);     // AddressOfEntryPoint
    Put32(image, kSyntheticOptional + 28, 0x1);        // ImageBase 0x140000000 (high half)
    Put32(image, kSyntheticOptional + 24, 0x40000000);
    Put32(image, kSyntheticOptional + 32, 0x1000);     // SectionAlignment
    Put32(image, kSyntheticOptional + 36, 0x200);      // FileAlignment
    Put32(image, kSyntheticOptional + 56, 0x4000);     // SizeOfImage
    Put32(image, kSyntheticOptional + 60, 0x400);      // SizeOfHeaders
    Put16(image, kSyntheticOptional + 68, 2);          // GUI subsystem
    Put32(image, kSyntheticOptional + 108, 16);
    Put32(image, kSyntheticDirectories + 8 * 5, 0x3000);  // base relocations
    Put32(image, kSyntheticDirectories + 8 * 5 + 4, 12);
    const auto section = [&](const std::size_t index, const char* name, const std::uint32_t address,
                             const std::uint32_t pointer, const std::uint32_t characteristics) {
        const std::size_t at = kSyntheticSections + 40 * index;
        std::memcpy(&image[at], name, std::strlen(name));
        Put32(image, at + 8, 0x200);
        Put32(image, at + 12, address);
        Put32(image, at + 16, 0x200);
        Put32(image, at + 20, pointer);
        Put32(image, at + 36, characteristics);
    };
    section(0, ".text", 0x1000, 0x400, 0x60000020);
    section(1, ".rsrc", 0x2000, 0x600, 0x40000040);
    section(2, ".reloc", 0x3000, 0x800, 0x42000040);
    image[0x400] = 0xC3;  // ret
    // One relocation block for the .text page, then a recognisable tail.
    Put32(image, 0x800, 0x1000);
    Put32(image, 0x804, 12);
    Put16(image, 0x808, 0xA010);
    std::memcpy(&image[0x9F0], "RELOC-TAIL", 10);
    image.insert(image.end(), overlay.begin(), overlay.end());
    return image;
}

struct SyntheticSection {
    std::uint32_t virtualSize, address, rawSize, pointer;
};
SyntheticSection SectionOf(const Bytes& image, const std::size_t index) {
    const std::size_t at = kSyntheticSections + 40 * index;
    return {U32(image, at + 8), U32(image, at + 12), U32(image, at + 16), U32(image, at + 20)};
}

std::string Text(const std::optional<Bytes>& bytes) {
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string{};
}

}  // namespace

int main(int argc, char** argv) {
    px::test::Suite suite("PackageBranding");
    px::test::TempDirectory fixture("package-branding");

    suite.Run("PngRoundTripAndSquareResize", [&] {
        std::string error;
        const auto png = branding::EncodePng(Gradient(300, 200), error);
        suite.Require(!png.empty(), "a PNG encodes", error);
        const auto decoded = branding::DecodePng(png, error);
        suite.Require(decoded.has_value(), "the PNG decodes again", error);
        suite.Expect(decoded->width == 300 && decoded->height == 200 && decoded->pixels == Gradient(300, 200).pixels,
                     "PNG round trip keeps every pixel");
        const auto square = branding::ResizeSquare(*decoded, 64);
        suite.Expect(square.width == 64 && square.height == 64, "resized to the requested square");
        // A 300x200 image is centered on a 300x300 canvas: the top rows are padding.
        suite.Expect(square.pixels[3] == 0 && square.pixels[(32 * 64 + 32) * 4 + 3] == 255,
                     "non-square icons are padded with transparency, not stretched");
        const auto large = branding::ResizeSquare(*decoded, 512);
        suite.Expect(large.width == 512, "small sources are enlarged");
        suite.Expect(!branding::DecodePng(Bytes{'n', 'o', 'p', 'e'}, error).has_value(), "non-PNG input is rejected");
    });

    suite.Run("IcoHasEverySizeWithPngAt256", [&] {
        std::string error;
        const auto ico = branding::BuildIco(Gradient(512, 512), error);
        suite.Require(ico.size() > 6 && U16(ico, 0) == 0 && U16(ico, 2) == 1, "an .ico header", error);
        suite.Require(U16(ico, 4) == 7, "seven icon frames");
        std::vector<int> sizes;
        for (std::size_t index = 0; index < 7; ++index) {
            const std::size_t at = 6 + 16 * index;
            const int size = ico[at] == 0 ? 256 : ico[at];
            sizes.push_back(size);
            const std::size_t offset = U32(ico, at + 12), length = U32(ico, at + 8);
            suite.Require(offset + length <= ico.size(), "frame lies inside the file");
            const bool png = ico[offset] == 0x89 && ico[offset + 1] == 'P' && ico[offset + 2] == 'N' &&
                             ico[offset + 3] == 'G';
            if (size <= 48) {
                suite.Expect(!png && U32(ico, offset) == 40 && U32(ico, offset + 4) == static_cast<std::uint32_t>(size) &&
                                 U32(ico, offset + 8) == static_cast<std::uint32_t>(size * 2),
                             "small frames are 32-bit DIBs with an AND mask");
            } else {
                suite.Expect(png, "frames from 64 px are PNG");
            }
        }
        suite.Expect(sizes == std::vector<int>{16, 24, 32, 48, 64, 128, 256}, "16 to 256 px frames");
        const auto largest = branding::DecodeIcoLargest(ico, error);
        suite.Expect(largest && largest->width == 256, "the largest .ico frame decodes", error);
    });

    suite.Run("IcnsHasRetinaMembers", [&] {
        std::string error;
        const auto icns = branding::BuildIcns(Gradient(1024, 1024), error);
        suite.Require(icns.size() > 8 && std::string(icns.begin(), icns.begin() + 4) == "icns", "an .icns header", error);
        const std::uint32_t total = BigEndian(icns, 4);
        suite.Expect(total == icns.size(), "the .icns length covers the file");
        std::vector<std::string> types;
        for (std::size_t at = 8; at + 8 <= icns.size();) {
            types.emplace_back(icns.begin() + static_cast<std::ptrdiff_t>(at), icns.begin() + static_cast<std::ptrdiff_t>(at + 4));
            const std::uint32_t length = BigEndian(icns, at + 4);
            suite.Require(length > 8, "member length");
            at += length;
        }
        suite.Equal("icp4\nicp5\nic11\nic12\nic07\nic13\nic08\nic14\nic09\nic10\n", Lines(types), ".icns members");
        const auto from256 = branding::BuildIcns(Gradient(256, 256), error);
        suite.Expect(std::string(from256.begin(), from256.end()).find("ic10") == std::string::npos,
                     "no 1024 px member is invented from a 256 px source");
    });

    suite.Run("VersionNumbers", [&] {
        suite.Expect(branding::NumericVersion("1.2.3-beta.4") == std::array<std::uint16_t, 4>{1, 2, 3, 0},
                     "pre-release suffixes are dropped");
        suite.Expect(branding::NumericVersion("v2.0") == std::array<std::uint16_t, 4>{2, 0, 0, 0}, "a leading v");
        suite.Expect(branding::NumericVersion("70000.1.2.3.4") == std::array<std::uint16_t, 4>{65535, 1, 2, 3},
                     "parts are clamped to 16 bits and limited to four");
        suite.Expect(branding::NumericVersion("release") == std::array<std::uint16_t, 4>{}, "no digits is 0.0.0.0");
        suite.Equal("1.4.2", branding::ShortVersion("1.4.2.7"), "macOS short version");
        suite.Equal("3.0", branding::ShortVersion("3"), "short version has two parts");
    });

    suite.Run("VersionResourceRoundTrip", [&] {
        const auto resource = branding::BuildVersionResource({
            .productName = "光之軌跡",
            .fileDescription = "光之軌跡",
            .companyName = "Hikari Works",
            .legalCopyright = "© 2026 Hikari Works",
            .productVersion = "1.2.0-rc.1",
            .originalFilename = "Hikari.exe",
            .internalName = "Hikari"});
        suite.Expect(resource.size() % 2 == 0 && U16(resource, 0) == resource.size(), "wLength covers the resource");
        const auto parsed = branding::ParseVersionResource(resource);
        suite.Require(parsed.has_value(), "the version resource parses");
        suite.Expect(parsed->fileVersion == std::array<std::uint16_t, 4>{1, 2, 0, 0}, "fixed file version");
        std::string strings;
        for (const auto& [key, value] : parsed->strings) strings += key + "=" + value + "\n";
        suite.Equal("CompanyName=Hikari Works\nFileDescription=光之軌跡\nFileVersion=1.2.0.0\n"
                    "InternalName=Hikari\nLegalCopyright=© 2026 Hikari Works\nOriginalFilename=Hikari.exe\n"
                    "ProductName=光之軌跡\nProductVersion=1.2.0-rc.1\n",
                    strings, "every string, UTF-8 through UTF-16 and back");
        const auto minimal = branding::ParseVersionResource(branding::BuildVersionResource(
            {.productName = "Game", .fileDescription = "Game", .originalFilename = "Game.exe", .internalName = "Game"}));
        suite.Expect(minimal && std::ranges::none_of(minimal->strings, [](const auto& entry) {
                         return entry.first == "CompanyName" || entry.first == "LegalCopyright";
                     }),
                     "unset company and copyright are omitted");
    });

    suite.Run("InfoPlistAndBundleIdentifier", [&] {
        const std::string plist = branding::InfoPlist({
            .bundleName = "Tom & Jerry's <Game>",
            .executable = "Hikari",
            .identifier = "com.hikari.game",
            .version = "1.2.3-beta",
            .iconFile = "Hikari.icns",
            .minimumSystemVersion = "13.0",
            .copyright = "© 2026"});
        for (const std::string_view expected : {
                 "<key>CFBundleName</key>\n\t<string>Tom &amp; Jerry&apos;s &lt;Game&gt;</string>",
                 "<key>CFBundleExecutable</key>\n\t<string>Hikari</string>",
                 "<key>CFBundleIdentifier</key>\n\t<string>com.hikari.game</string>",
                 "<key>CFBundleShortVersionString</key>\n\t<string>1.2.3</string>",
                 "<key>CFBundleVersion</key>\n\t<string>1.2.3</string>",
                 "<key>CFBundleIconFile</key>\n\t<string>Hikari.icns</string>",
                 "<key>LSMinimumSystemVersion</key>\n\t<string>13.0</string>",
                 "<key>NSHighResolutionCapable</key>\n\t<true/>",
                 "<key>CFBundlePackageType</key>\n\t<string>APPL</string>",
                 "<key>SDL_FILESYSTEM_BASE_DIR_TYPE</key>\n\t<string>resource</string>"})
            suite.Expect(plist.find(expected) != std::string::npos, std::string("Info.plist has ") + std::string(expected));
        suite.Equal("com.example.first-light", branding::DefaultBundleIdentifier("first-light"), "default identifier");
        suite.Equal("jp.hikari.game", branding::DefaultBundleIdentifier("jp.hikari.game"), "reverse-DNS ids are kept");
        suite.Equal("com.example.my-game", branding::DefaultBundleIdentifier("my_game"), "underscores become hyphens");
        suite.Expect(branding::ValidBundleIdentifier("com.example.game") &&
                         !branding::ValidBundleIdentifier("game") && !branding::ValidBundleIdentifier("com..game") &&
                         !branding::ValidBundleIdentifier("com.game_x"),
                     "bundle identifier validation");
    });

    suite.Run("MachODeploymentTarget", [&] {
        Bytes thin(32, 0);
        const auto put = [](Bytes& bytes, const std::size_t at, const std::uint32_t value) {
            for (int index = 0; index < 4; ++index) bytes[at + index] = static_cast<std::uint8_t>(value >> (8 * index));
        };
        put(thin, 0, 0xFEEDFACF);
        put(thin, 16, 2);
        // A segment-like command to skip, then LC_BUILD_VERSION (platform 1 = macOS).
        thin.resize(32 + 16 + 24, 0);
        put(thin, 32, 0x19);
        put(thin, 36, 16);
        put(thin, 48, 0x32);
        put(thin, 52, 24);
        put(thin, 56, 1);
        put(thin, 60, (13u << 16) | (5u << 8) | 1u);
        suite.Equal("13.5.1", branding::MachOMinimumSystemVersion(thin).value_or("none"), "thin Mach-O");
        Bytes fat{0xCA, 0xFE, 0xBA, 0xBE, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0, 3, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0};
        fat.resize(64, 0);
        fat.insert(fat.end(), thin.begin(), thin.end());
        suite.Equal("13.5.1", branding::MachOMinimumSystemVersion(fat).value_or("none"), "universal binary");
        suite.Expect(!branding::MachOMinimumSystemVersion(Bytes(64, 0)), "not a Mach-O");
    });

    suite.Run("DesktopEntryAndAssemblies", [&] {
        const std::string entry = branding::DesktopEntry({.name = "Hikari: Prologue", .executable = "hikari game",
                                                          .icon = "hikari game"});
        suite.Equal("[Desktop Entry]\nType=Application\nVersion=1.0\nName=Hikari: Prologue\n"
                    "Exec=\"hikari game\"\nIcon=hikari game\nTerminal=false\nCategories=Game;\n",
                    entry, ".desktop entry");
        suite.Expect(branding::ValidEngineFolder("Engine") && branding::ValidEngineFolder("game_data-1") &&
                         !branding::ValidEngineFolder("Package") && !branding::ValidEngineFolder("../x") &&
                         !branding::ValidEngineFolder("My Engine") && !branding::ValidEngineFolder("com1") &&
                         !branding::ValidEngineFolder(".hidden") && !branding::ValidEngineFolder(""),
                     "engine folder names are portable assembly names");
        const std::string assembly = branding::AssemblyManifest("Engine", "amd64", {"SDL3.dll", "FLAC++.dll"});
        suite.Expect(assembly.find("name=\"Engine\" version=\"1.0.0.0\" processorArchitecture=\"amd64\"") != std::string::npos &&
                         assembly.find("<file name=\"SDL3.dll\"/>") != std::string::npos &&
                         assembly.find("<file name=\"FLAC++.dll\"/>") != std::string::npos,
                     "assembly manifest lists the DLLs");
        const std::string msvc =
            "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\r\n"
            "<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>\r\n"
            "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges>"
            "<requestedExecutionLevel level='asInvoker' uiAccess='false' /></requestedPrivileges></security></trustInfo>\r\n"
            "</assembly>\r\n";
        const auto merged = branding::AddAssemblyDependency(msvc, "Engine", "amd64");
        suite.Require(merged.has_value(), "MSVC's manifest takes the dependency");
        suite.Expect(merged->find("<trustInfo") < merged->find("<dependency>") &&
                         merged->find("<dependency>") < merged->rfind("</assembly>"),
                     "the dependency goes inside the root element, after existing content");
        const auto prefixed = branding::AddAssemblyDependency("<asmv1:assembly></asmv1:assembly>", "Engine", "x86");
        suite.Expect(prefixed && prefixed->ends_with("</asmv1:assembly>"), "a prefixed root element");
        suite.Expect(!branding::AddAssemblyDependency("<xml/>", "Engine", "amd64"), "no assembly element");
        suite.Expect(branding::AddAssemblyDependency("", "Engine", "amd64").value_or("").find("<dependency>") !=
                         std::string::npos,
                     "a missing manifest is created");
        suite.Equal("Engine", branding::LayoutDataDirectory(branding::LayoutResource("Engine")).value_or(""),
                    "layout resource round trip");
        suite.Expect(!branding::LayoutDataDirectory(branding::LayoutResource("..")), "layout resource rejects ..");
    });

    std::string pngError;
    const auto iconPath = fixture.path / "icon.png";
    WriteBytes(iconPath, branding::EncodePng(Gradient(600, 600), pngError));

    suite.Run("MacAppBundleLayout", [&] {
        const auto staging = fixture.path / "mac";
        Write(staging / "Hikari", "player");
        Write(staging / "libavcodec.62.dylib", "lib");
        Write(staging / "Content.pdx", "pdx");
        Write(staging / "Package/manifest.json", "{}");
        Write(staging / "Notices/LICENSE", "license");
        branding::LayoutOutcome outcome;
        std::string error;
        const branding::LayoutRequest request{
            .platform = branding::LayoutPlatform::MacOS, .playerFile = "Hikari", .displayName = "光之軌跡",
            .version = "1.2.0", .copyright = "© 2026", .platformLayout = true, .icon = iconPath,
            .bundleIdentifier = "com.hikari.game", .appName = "Hikari"};
        suite.Require(branding::ApplyLayout(staging, request, outcome, error), "the .app layout applies", error);
        const auto contents = staging / "Hikari.app/Contents";
        for (const char* path : {"MacOS/Hikari", "Frameworks/libavcodec.62.dylib", "Resources/Content.pdx",
                                 "Resources/Package/manifest.json", "Resources/Notices/LICENSE",
                                 "Resources/Hikari.icns", "Info.plist", "PkgInfo"})
            suite.Expect(std::filesystem::is_regular_file(contents / path), std::string("bundle has Contents/") + path);
        std::vector<std::string> root;
        for (const auto& entry : std::filesystem::directory_iterator(staging))
            root.push_back(px::PathToUtf8(entry.path().filename()));
        suite.Equal("Hikari.app\n", Lines(root), "the distribution root holds only the .app");
        suite.Expect(outcome.player == contents / "MacOS/Hikari" &&
                         branding::LaidOutPlayer(request) == std::filesystem::path("Hikari.app/Contents/MacOS/Hikari"),
                     "the Player path inside the bundle");
        const std::string plist = Read(contents / "Info.plist");
        suite.Expect(plist.find("<string>光之軌跡</string>") != std::string::npos &&
                         plist.find("<string>Hikari.icns</string>") != std::string::npos &&
                         plist.find("<key>LSMinimumSystemVersion</key>\n\t<string>11.0</string>") != std::string::npos,
                     "Info.plist names the product and icon, with the fallback minimum system");
        suite.Expect(px::sdk::DistributionDataRoot(staging) == contents / "Resources",
                     "tools find the game data inside the .app");
    });

    suite.Run("LinuxLibAndDesktopLayout", [&] {
        const auto staging = fixture.path / "linux";
        Write(staging / "hikari", "player");
        Write(staging / "libSDL3.so.0", "lib");
        Write(staging / "libfoo.so", "lib");
        Write(staging / "Content.pdx", "pdx");
        Write(staging / "Package/manifest.json", "{}");
        branding::LayoutOutcome outcome;
        std::string error;
        suite.Require(branding::ApplyLayout(staging, {.platform = branding::LayoutPlatform::Linux,
                                                      .playerFile = "hikari", .displayName = "Hikari",
                                                      .platformLayout = true, .icon = iconPath},
                                            outcome, error),
                      "the Linux layout applies", error);
        for (const char* path : {"hikari", "lib/libSDL3.so.0", "lib/libfoo.so", "Content.pdx", "Package/manifest.json",
                                 "hikari.desktop", "icons/hicolor/16x16/apps/hikari.png",
                                 "icons/hicolor/256x256/apps/hikari.png"})
            suite.Expect(std::filesystem::is_regular_file(staging / path), std::string("Linux layout has ") + path);
        suite.Expect(Read(staging / "hikari.desktop").find("Icon=hikari\n") != std::string::npos, ".desktop icon name");
        std::string decodeError;
        const auto icon48 = branding::DecodePng(ReadBytes(staging / "icons/hicolor/48x48/apps/hikari.png"), decodeError);
        suite.Expect(icon48 && icon48->width == 48, "hicolor sizes are resampled", decodeError);
        suite.Expect(px::sdk::DistributionDataRoot(staging) == staging, "Linux data stays at the root");
    });

    suite.Run("PeResourceEditorOnAnyHost", [&] {
        const branding::PeResourceKey manifestType{24, {}}, rcData{10, {}}, one{1, {}};
        std::string error;
        const std::string manifestText =
            "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\r\n"
            "<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>\r\n</assembly>\r\n";
        // The authored overlay survives; a signature over the old bytes does not.
        Bytes base = SyntheticPe("OVERLAY!SIGNATURE");
        Put32(base, kSyntheticDirectories + 8 * 4, static_cast<std::uint32_t>(base.size() - 9));
        Put32(base, kSyntheticDirectories + 8 * 4 + 4, 9);
        const auto none = branding::ReadPeResources(base, error);
        suite.Require(none && none->empty(), "an image without a resource directory has no resources", error);
        const auto withManifest = branding::ReplacePeResources(
            base, {{manifestType, one, 0x0409, Bytes(manifestText.begin(), manifestText.end())},
                   {rcData, {0, u"Zeta"}, 0, Bytes{'z'}}, {rcData, {0, u"alpha"}, 0, Bytes{'a'}}},
            error);
        suite.Require(withManifest.has_value(), "resources are written into the empty .rsrc", error);
        const auto read = branding::ReadPeResources(*withManifest, error);
        suite.Require(read && read->size() == 3, "three resources read back", error);
        suite.Equal(manifestText, Text(branding::FindPeResource(*read, manifestType, one)), "the manifest round trips");
        suite.Equal("a", Text(branding::FindPeResource(*read, rcData, {0, u"alpha"})), "named resources round trip");
        suite.Expect(std::string(withManifest->end() - 8, withManifest->end()) == "OVERLAY!" &&
                         U32(*withManifest, kSyntheticDirectories + 8 * 4) == 0,
                     "the overlay is kept and the stale signature dropped");

        const auto staging = fixture.path / "windows-any-host";
        std::filesystem::create_directories(staging);
        WriteBytes(staging / "Hikari.exe", *withManifest);
        Write(staging / "SDL3.dll", "dll");
        Write(staging / "Content.pdx", "pdx");
        Write(staging / "Package/manifest.json", "{}");
        branding::LayoutOutcome outcome;
        const branding::LayoutRequest request{
            .platform = branding::LayoutPlatform::Windows, .playerFile = "Hikari.exe", .displayName = "光之軌跡",
            .version = "1.2.0", .companyName = "Hikari Works", .engineFolder = "Engine", .icon = iconPath};
        suite.Require(branding::ApplyLayout(staging, request, outcome, error), "the bundled layout applies on any host",
                      error);
        // Twice: replaced icons, version and manifest stay single.
        for (const char* name : {"SDL3.dll", "Content.pdx", "Package"})
            std::filesystem::rename(staging / "Engine" / name, staging / name);
        std::filesystem::remove_all(staging / "Engine");
        suite.Require(branding::ApplyLayout(staging, request, outcome, error), "the layout applies again", error);
        const Bytes branded = ReadBytes(staging / "Hikari.exe");
        const auto resources = branding::ReadPeResources(branded, error);
        suite.Require(resources.has_value(), "the branded executable's resources read back", error);
        const auto count = [&](const std::uint16_t type) {
            return std::ranges::count_if(*resources, [&](const auto& item) { return item.type.id == type; });
        };
        suite.Expect(count(3) == 7 && count(14) == 1 && count(16) == 1 && count(24) == 1,
                     "seven icon images, one icon group, one version and one manifest");
        suite.Expect(Text(branding::FindPeResource(*resources, {14, {}}, one)).size() == 6 + 14 * 7,
                     "the icon group lists seven sizes");
        const auto version =
            branding::ParseVersionResource(branding::FindPeResource(*resources, {16, {}}, one).value_or(Bytes{}));
        const std::pair<std::string, std::string> product{"ProductName", "光之軌跡"};
        suite.Expect(version && std::ranges::find(version->strings, product) != version->strings.end(),
                     "the version resource names the product");
        const std::string merged = Text(branding::FindPeResource(*resources, manifestType, one));
        suite.Expect(merged.find("name=\"Engine\"") != std::string::npos &&
                         merged.find("name=\"Engine\"") == merged.rfind("name=\"Engine\""),
                     "the manifest depends on the engine folder once");
        suite.Equal("Engine",
                    branding::LayoutDataDirectory(
                        Text(branding::FindPeResource(*resources, rcData, {0, u"PRISMATIX_LAYOUT"})))
                        .value_or(""),
                    "PRISMATIX_LAYOUT names the engine folder");

        const auto rsrc = SectionOf(branded, 1);
        const auto reloc = SectionOf(branded, 2);
        suite.Expect(rsrc.address == 0x2000 && rsrc.pointer == 0x600 && rsrc.virtualSize > 0x200 &&
                         rsrc.rawSize % 0x200 == 0 && rsrc.rawSize >= rsrc.virtualSize,
                     ".rsrc keeps its address and grows");
        suite.Expect(reloc.address % 0x1000 == 0 && reloc.address >= rsrc.address + rsrc.virtualSize &&
                         reloc.pointer == rsrc.pointer + rsrc.rawSize &&
                         U32(branded, kSyntheticDirectories + 8 * 5) == reloc.address,
                     ".reloc moves up behind it and its directory follows");
        suite.Expect(U32(branded, reloc.pointer) == 0x1000 && U16(branded, reloc.pointer + 8) == 0xA010 &&
                         std::memcmp(&branded[reloc.pointer + 0x1F0], "RELOC-TAIL", 10) == 0,
                     "the relocation table is carried over byte for byte");
        suite.Expect(U32(branded, kSyntheticOptional + 56) == reloc.address + 0x1000, "SizeOfImage covers .reloc");
        suite.Expect(branding::PeChecksum(branded) == U32(branded, kSyntheticOptional + 64), "the checksum is valid");
        suite.Expect(branding::PeArchitecture(branded) == "amd64" && branding::PeSubsystem(branded) == std::uint16_t{2},
                     "the machine and subsystem are untouched");

        Bytes trailing = SyntheticPe();
        std::memcpy(&trailing[kSyntheticSections + 40 * 2], ".tls\0\0\0\0", 8);
        Put32(trailing, kSyntheticDirectories + 8 * 5, 0);
        suite.Expect(!branding::ReplacePeResources(trailing, {}, error) && error.find(".tls") != std::string::npos,
                     "a section that cannot move behind .rsrc is refused", error);
        suite.Expect(!branding::ReadPeResources(Bytes{'M', 'Z'}, error), "truncated images are rejected");
    });

    suite.Run("TargetPlatforms", [&] {
        using branding::LayoutPlatform;
        suite.Expect(branding::TargetLayoutPlatform("win32-x64") == LayoutPlatform::Windows &&
                         branding::TargetLayoutPlatform("linux-x64") == LayoutPlatform::Linux &&
                         branding::TargetLayoutPlatform("darwin-arm64") == LayoutPlatform::MacOS &&
                         branding::TargetLayoutPlatform("darwin-x64") == LayoutPlatform::MacOS &&
                         !branding::TargetLayoutPlatform("linux-arm64") && !branding::TargetLayoutPlatform("win32"),
                     "the four desktop platforms");
        const Bytes pe = SyntheticPe();
        suite.Expect(!branding::ExecutableTargetMismatch(pe, "win32-x64"), "an x64 PE is a win32-x64 Player");
        suite.Expect(branding::ExecutableTargetMismatch(pe, "linux-x64").value_or("").find("Windows") != std::string::npos,
                     "a PE is not a Linux Player");
        Bytes elf(64, 0);
        elf[0] = 0x7F;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2;
        elf[5] = 1;
        Put16(elf, 18, 0x3E);
        suite.Expect(!branding::ExecutableTargetMismatch(elf, "linux-x64") &&
                         branding::ExecutableTargetMismatch(elf, "darwin-arm64"),
                     "an x86-64 ELF is only a linux-x64 Player");
        Bytes macho(32, 0);
        Put32(macho, 0, 0xFEEDFACF);
        Put32(macho, 4, 0x0100000C);
        suite.Expect(!branding::ExecutableTargetMismatch(macho, "darwin-arm64") &&
                         branding::ExecutableTargetMismatch(macho, "darwin-x64"),
                     "an arm64 Mach-O is only a darwin-arm64 Player");
        Bytes universal{0xCA, 0xFE, 0xBA, 0xBE, 0, 0, 0, 2, 1, 0, 0, 7};
        universal.resize(8 + 20, 0);
        universal.insert(universal.end(), {1, 0, 0, 12});
        universal.resize(8 + 40, 0);
        suite.Expect(!branding::ExecutableTargetMismatch(universal, "darwin-x64") &&
                         !branding::ExecutableTargetMismatch(universal, "darwin-arm64"),
                     "a universal binary serves both Macs");
        suite.Expect(branding::ExecutableTargetMismatch(Bytes{'#', '!'}, "linux-x64").has_value(),
                     "a script is no Player");
    });

#ifdef _WIN32
    suite.Run("WindowsSubsystems", [&] {
        suite.Require(argc >= 3, "PrismatiXPlayer and PrismatiXPlayerQA paths are passed");
        suite.Expect(branding::PeSubsystem(ReadBytes(px::PathFromUtf8(argv[1]))) == std::uint16_t{2},
                     "the shipped Player is a GUI (WIN32 subsystem) program");
        suite.Expect(branding::PeSubsystem(ReadBytes(px::PathFromUtf8(argv[2]))) == std::uint16_t{3},
                     "the QA Player stays a console program");
    });

    suite.Run("WindowsBundledLayoutResources", [&] {
        suite.Require(argc >= 2, "the PrismatiXPlayer path is passed");
        const auto staging = fixture.path / "windows";
        std::filesystem::create_directories(staging);
        std::filesystem::copy_file(px::PathFromUtf8(argv[1]), staging / "Hikari.exe");
        Write(staging / "SDL3.dll", "dll");
        Write(staging / "avcodec-62.dll", "dll");
        Write(staging / "Content.pdx", "pdx");
        Write(staging / "Package/manifest.json", "{}");
        Write(staging / "Notices/LICENSE", "license");
        branding::LayoutOutcome outcome;
        std::string error;
        suite.Require(branding::ApplyLayout(staging, {.platform = branding::LayoutPlatform::Windows,
                                                      .playerFile = "Hikari.exe", .displayName = "光之軌跡",
                                                      .version = "1.2.0", .companyName = "Hikari Works",
                                                      .copyright = "© 2026 Hikari Works", .engineFolder = "Engine",
                                                      .icon = iconPath},
                                            outcome, error),
                      "the Windows bundled layout applies", error);
        std::vector<std::string> root;
        for (const auto& entry : std::filesystem::directory_iterator(staging))
            root.push_back(px::PathToUtf8(entry.path().filename()));
        std::ranges::sort(root);
        suite.Equal("Engine\nHikari.exe\n", Lines(root), "only the executable and the engine folder at the top");
        for (const char* path : {"Engine/SDL3.dll", "Engine/Content.pdx", "Engine/Package/manifest.json",
                                 "Engine/Notices/LICENSE", "Engine/Engine.manifest"})
            suite.Expect(std::filesystem::is_regular_file(staging / path), std::string("engine folder has ") + path);
        suite.Expect(Read(staging / "Engine/Engine.manifest").find("<file name=\"avcodec-62.dll\"/>") != std::string::npos,
                     "the private assembly lists the DLLs");
        suite.Expect(px::sdk::DistributionDataRoot(staging) == staging / "Engine", "tools find the engine folder");

        const auto exe = staging / "Hikari.exe";
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
        std::vector<std::uint8_t> info(size);
        suite.Require(size > 0 && GetFileVersionInfoW(exe.c_str(), 0, size, info.data()), "Windows reads VERSIONINFO");
        const auto query = [&](const wchar_t* key) {
            wchar_t* value = nullptr;
            UINT length = 0;
            const std::wstring path = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
            if (!VerQueryValueW(info.data(), path.c_str(), reinterpret_cast<void**>(&value), &length) || !value)
                return std::string{};
            return px::PathToUtf8(std::filesystem::path(std::wstring(value)));
        };
        suite.Equal("光之軌跡", query(L"ProductName"), "ProductName");
        suite.Equal("光之軌跡", query(L"FileDescription"), "FileDescription");
        suite.Equal("Hikari Works", query(L"CompanyName"), "CompanyName");
        suite.Equal("© 2026 Hikari Works", query(L"LegalCopyright"), "LegalCopyright");
        suite.Equal("1.2.0", query(L"ProductVersion"), "ProductVersion");
        suite.Equal("1.2.0.0", query(L"FileVersion"), "FileVersion");
        suite.Equal("Hikari.exe", query(L"OriginalFilename"), "OriginalFilename");

        HMODULE module = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        suite.Require(module != nullptr, "the branded executable loads as a resource file");
        const auto resource = [&](LPCWSTR type, LPCWSTR name) {
            HRSRC found = FindResourceW(module, name, type);
            if (!found) return std::string{};
            HGLOBAL loaded = LoadResource(module, found);
            return std::string(static_cast<const char*>(LockResource(loaded)), SizeofResource(module, found));
        };
        const std::string group = resource(MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1));
        suite.Expect(group.size() == 6 + 14 * 7, "one icon group with seven sizes");
        const std::string manifest = resource(MAKEINTRESOURCEW(24), MAKEINTRESOURCEW(1));
        suite.Expect(manifest.find("<assemblyIdentity type=\"win32\" name=\"Engine\"") != std::string::npos,
                     "the application manifest depends on the engine folder assembly");
        suite.Equal("Engine", branding::LayoutDataDirectory(resource(MAKEINTRESOURCEW(10), branding::kLayoutResourceName))
                                  .value_or(""),
                    "the Player finds its data through PRISMATIX_LAYOUT");
        FreeLibrary(module);
    });
#else
    (void)argc;
    (void)argv;
#endif

    return suite.Finish();
}
