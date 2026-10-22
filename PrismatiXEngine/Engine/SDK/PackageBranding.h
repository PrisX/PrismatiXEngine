#pragma once

// Distribution branding and platform layout for the native Packager:
// executable icons from a PNG or .ico, the Windows VERSIONINFO resource, the
// macOS .app bundle (Info.plist, .icns) and the Linux .desktop entry. The
// byte and text builders are platform neutral so every host's tests cover
// every platform's output; only WriteWindowsResources needs Windows.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk::branding {

using Bytes = std::vector<std::uint8_t>;

// Straight (not premultiplied) 8-bit RGBA, row-major, top row first.
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

// Decodes a PNG with SDL's built-in loader. Returns nullopt and sets error
// for anything that is not a readable PNG of at most 4096 x 4096.
[[nodiscard]] std::optional<RgbaImage> DecodePng(const Bytes& png, std::string& error);
[[nodiscard]] Bytes EncodePng(const RgbaImage& image, std::string& error);

// The largest image of a Windows .ico (PNG or 32-bit DIB frames), so a
// project with only an .ico still gets macOS and Linux icons.
[[nodiscard]] std::optional<RgbaImage> DecodeIcoLargest(const Bytes& ico, std::string& error);

// Centers a non-square image on a transparent square, then resamples to
// size x size (area average when shrinking, bilinear when enlarging).
[[nodiscard]] RgbaImage ResizeSquare(const RgbaImage& image, int size);

// A Windows .ico with 16, 24, 32, 48, 64, 128 and 256 pixel frames: 32-bit
// DIBs up to 48 px, PNG from 64 px (the form Explorer expects for 256).
[[nodiscard]] Bytes BuildIco(const RgbaImage& source, std::string& error);

// A macOS .icns with PNG members from 16 to 512 px (1024 for a 1024 px
// source), including the @2x variants.
[[nodiscard]] Bytes BuildIcns(const RgbaImage& source, std::string& error);

// Linux hicolor sizes written as icons/hicolor/<n>x<n>/apps/<name>.png.
inline constexpr std::array<int, 7> kLinuxIconSizes{16, 24, 32, 48, 64, 128, 256};

struct VersionStrings {
    std::string productName;
    std::string fileDescription;
    std::string companyName;
    std::string legalCopyright;
    std::string productVersion;   // as authored, e.g. "1.2.0-beta.1"
    std::string originalFilename; // e.g. "Hikari.exe"
    std::string internalName;     // the file name without extension
};

// "1.2.3-beta" -> {1, 2, 3, 0}. Missing or non-numeric parts are 0 and each
// part is clamped to 65535, the range of VS_FIXEDFILEINFO.
[[nodiscard]] std::array<std::uint16_t, 4> NumericVersion(std::string_view version);
// "1.2.3-beta" -> "1.2.3"; at least "x.y", as CFBundleShortVersionString wants.
[[nodiscard]] std::string ShortVersion(std::string_view version);

// The binary RT_VERSION resource (VS_VERSIONINFO) for US English / Unicode.
[[nodiscard]] Bytes BuildVersionResource(const VersionStrings& strings);

// Reads the strings and the fixed file version back from VS_VERSIONINFO
// bytes; used by tests and the Packager's own post-write check.
struct ParsedVersionResource {
    std::array<std::uint16_t, 4> fileVersion{};
    std::vector<std::pair<std::string, std::string>> strings;
};
[[nodiscard]] std::optional<ParsedVersionResource> ParseVersionResource(const Bytes& resource);

struct MacBundleInfo {
    std::string bundleName;        // CFBundleName / CFBundleDisplayName
    std::string executable;        // CFBundleExecutable
    std::string identifier;        // CFBundleIdentifier
    std::string version;           // authored version; both version keys derive from it
    std::string iconFile;          // CFBundleIconFile, empty when no icon
    std::string minimumSystemVersion;
    std::string copyright;         // NSHumanReadableCopyright, optional
};
[[nodiscard]] std::string InfoPlist(const MacBundleInfo& info);

// LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX of a (fat or thin) Mach-O
// executable as "major.minor[.patch]", or nullopt when absent.
[[nodiscard]] std::optional<std::string> MachOMinimumSystemVersion(const Bytes& executable);

// A reverse-DNS identifier from a project id: kept when it already has a dot,
// else com.example.<id>; characters outside [A-Za-z0-9.-] become '-'.
[[nodiscard]] std::string DefaultBundleIdentifier(std::string_view projectId);
[[nodiscard]] bool ValidBundleIdentifier(std::string_view identifier);

struct DesktopEntryInfo {
    std::string name;
    std::string executable;
    std::string icon; // icon theme name, empty when no icon
    std::string comment;
};
[[nodiscard]] std::string DesktopEntry(const DesktopEntryInfo& info);

// Windows bundled layout: the engine folder is a private side-by-side
// assembly of the same name. The executable's manifest depends on it, and
// <folder>/<folder>.manifest lists the DLLs the loader redirects into it.
[[nodiscard]] bool ValidEngineFolder(std::string_view folder);
[[nodiscard]] std::string AssemblyManifest(std::string_view folder, std::string_view architecture,
                                           const std::vector<std::string>& files);
// Inserts the dependency on the engine folder assembly before </assembly>;
// an empty input yields a complete manifest. nullopt when the input has no
// closing assembly element.
[[nodiscard]] std::optional<std::string> AddAssemblyDependency(std::string_view manifest,
                                                              std::string_view folder,
                                                              std::string_view architecture);
// "amd64", "x86" or "arm64" from a PE image's COFF machine, else nullopt.
[[nodiscard]] std::optional<std::string> PeArchitecture(const Bytes& executable);
// IMAGE_OPTIONAL_HEADER.Subsystem of a PE image (2 = GUI, 3 = console).
[[nodiscard]] std::optional<std::uint16_t> PeSubsystem(const Bytes& executable);

// The RT_RCDATA resource the shipped Player reads to find its game data in
// the bundled layout (Applications/PlayerMain.cpp).
inline constexpr wchar_t kLayoutResourceName[] = L"PRISMATIX_LAYOUT";
[[nodiscard]] std::string LayoutResource(std::string_view dataDirectory);
[[nodiscard]] std::optional<std::string> LayoutDataDirectory(std::string_view resource);

// One resource of a PE image: type, name (an integer id, or a string when
// `name` is not empty) and language, as the Win32 resource API sees them.
struct PeResourceKey {
    std::uint16_t id = 0;
    std::u16string name;
    friend bool operator==(const PeResourceKey&, const PeResourceKey&) = default;
};
struct PeResource {
    PeResourceKey type;
    PeResourceKey name;
    std::uint16_t language = 0;
    Bytes data;
    std::uint32_t codePage = 0;
};
// Every resource of a PE image, read from its resource directory without the
// Win32 loader. An image without resources yields an empty list.
[[nodiscard]] std::optional<std::vector<PeResource>> ReadPeResources(const Bytes& image, std::string& error);
// The image with its resource section rebuilt to hold exactly `resources`.
// The section keeps its address; a base relocation section after it moves
// up, the headers (sizes, data directories, checksum) follow and an
// Authenticode signature, which no longer matches, is dropped. Images whose
// resource section is followed by anything else are refused.
[[nodiscard]] std::optional<Bytes> ReplacePeResources(const Bytes& image, const std::vector<PeResource>& resources,
                                                      std::string& error);
// The data of the first resource with this type and name (any language).
[[nodiscard]] std::optional<Bytes> FindPeResource(const std::vector<PeResource>& resources, const PeResourceKey& type,
                                                  const PeResourceKey& name);
// The PE checksum (IMAGE_OPTIONAL_HEADER.CheckSum) of an image.
[[nodiscard]] std::optional<std::uint32_t> PeChecksum(const Bytes& image);

struct WindowsResources {
    Bytes ico;                       // empty: keep the existing icons
    Bytes version;                   // empty: keep any existing version resource
    std::string engineFolder;        // empty: flat layout, manifest untouched
    std::string architecture;        // with engineFolder
};

// Rewrites the given resources of a PE file in one update, on any host
// (ReplacePeResources), the way UpdateResource would: replaced icons and
// version information are removed in every language first.
[[nodiscard]] bool WriteWindowsResources(const std::filesystem::path& executable,
                                         const WindowsResources& resources, std::string& error);

enum class LayoutPlatform { Windows, MacOS, Linux, Other };
[[nodiscard]] LayoutPlatform HostLayoutPlatform();

// The desktop platforms a distribution can be packed for, as Node names them
// (`${process.platform}-${process.arch}`): win32-x64, linux-x64, darwin-x64
// and darwin-arm64. nullopt for anything else.
[[nodiscard]] std::optional<LayoutPlatform> TargetLayoutPlatform(std::string_view target);
// Why `executable` cannot be the Player of `target` (a PE image of the wrong
// machine, an ELF file for macOS, ...), or nullopt when it can.
[[nodiscard]] std::optional<std::string> ExecutableTargetMismatch(const Bytes& executable, std::string_view target);

// Turns a flat staged distribution (Player, runtime libraries, archives,
// Package/ and Notices/ side by side) into the platform's shipping layout.
struct LayoutRequest {
    LayoutPlatform platform = LayoutPlatform::Other;
    std::string playerFile;        // the Player's file name in the staging root
    std::string displayName;       // product name shown to players
    std::string version;
    std::string companyName;
    std::string copyright;
    std::string engineFolder;      // Windows "bundled" layout when not empty
    bool platformLayout = false;   // macOS .app, Linux lib/ + .desktop
    std::filesystem::path icon;    // optional .ico or .png
    std::string bundleIdentifier;  // macOS
    std::string appName;           // macOS: <appName>.app
};
struct LayoutOutcome {
    std::filesystem::path player;  // absolute path of the laid-out Player
    std::vector<std::string> notes;
};
[[nodiscard]] bool ApplyLayout(const std::filesystem::path& staging, const LayoutRequest& request,
                               LayoutOutcome& outcome, std::string& error);
// Where ApplyLayout puts the Player, relative to the distribution root.
[[nodiscard]] std::filesystem::path LaidOutPlayer(const LayoutRequest& request);

}  // namespace px::sdk::branding
