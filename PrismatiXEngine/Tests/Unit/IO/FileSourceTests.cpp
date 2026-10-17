#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/FileSource.h"
#include "Engine/IO/VFS.h"
#include "Engine/Support/Utf8Path.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using px::io::Bytes;
using px::io::FileSource;
using px::io::SeekableReadStream;
using px::io::SeekOrigin;

Bytes Pattern(const std::size_t size, const std::uint8_t seed) {
    Bytes bytes(size);
    std::uint32_t state = 0x9e3779b9u ^ seed;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

// Repetitive enough that the archive writer keeps it zstd-compressed.
Bytes Compressible(const std::size_t size) {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::uint8_t>("prismatix-"[index % 10]);
    return bytes;
}

void WriteBytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

Bytes ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

Bytes Drain(SeekableReadStream& stream, const std::size_t chunk = 4096) {
    Bytes out;
    Bytes buffer(chunk);
    while (true) {
        const std::size_t read = stream.Read(buffer.data(), buffer.size());
        if (read == 0) break;
        out.insert(out.end(), buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(read));
    }
    return out;
}

// An APK stand-in: files exist only in memory under paths that are not on
// disk, and every read returns at most a few bytes, like an asset stream that
// is allowed to return short reads. Anything that bypasses the FileSource or
// assumes one read fills the buffer fails against it.
class MemoryFileSource final : public FileSource {
public:
    void Add(std::string path, Bytes bytes) {
        std::scoped_lock lock(m_mutex);
        m_files[std::move(path)] = std::make_shared<const Bytes>(std::move(bytes));
    }

    [[nodiscard]] std::unique_ptr<SeekableReadStream> Open(
        const std::string_view path) const override {
        std::scoped_lock lock(m_mutex);
        ++m_opens;
        const auto found = m_files.find(std::string(path));
        if (found == m_files.end()) return {};
        return std::make_unique<Stream>(found->second);
    }

    [[nodiscard]] std::optional<std::uint64_t> FileSize(
        const std::string_view path) const override {
        std::scoped_lock lock(m_mutex);
        const auto found = m_files.find(std::string(path));
        if (found == m_files.end()) return std::nullopt;
        return found->second->size();
    }

    [[nodiscard]] std::optional<std::vector<std::string>> ListFiles(
        const std::string_view directory) const override {
        std::scoped_lock lock(m_mutex);
        const std::string prefix =
            directory.empty() ? std::string{} : std::string(directory) + '/';
        std::vector<std::string> names;
        for (const auto& [path, bytes] : m_files) {
            if (!path.starts_with(prefix)) continue;
            const auto rest = path.substr(prefix.size());
            if (rest.find('/') == std::string::npos) names.push_back(rest);
        }
        return names;
    }

    [[nodiscard]] std::optional<std::string> CanonicalRoot(
        const std::string_view root) const override {
        return std::string(root == "." ? std::string_view{} : root);
    }

    [[nodiscard]] std::optional<std::string> ResolveUnder(
        const std::string_view root,
        const std::string_view relative) const override {
        if (root.empty()) return std::string(relative);
        return std::string(root) + '/' + std::string(relative);
    }

    [[nodiscard]] std::string_view Name() const override { return "memory"; }

    [[nodiscard]] int Opens() const {
        std::scoped_lock lock(m_mutex);
        return m_opens;
    }

private:
    class Stream final : public SeekableReadStream {
    public:
        explicit Stream(std::shared_ptr<const Bytes> bytes)
            : m_bytes(std::move(bytes)) {}
        [[nodiscard]] std::size_t Read(std::uint8_t* destination,
                                       const std::size_t bytes) override {
            if (!destination || m_position >= m_bytes->size()) return 0;
            const std::size_t count = std::min<std::size_t>(
                {bytes, m_bytes->size() - m_position, 1 + m_position % 7});
            std::copy_n(m_bytes->data() + m_position, count, destination);
            m_position += count;
            return count;
        }
        [[nodiscard]] bool Seek(const std::int64_t offset,
                                const SeekOrigin origin) override {
            const std::int64_t base =
                origin == SeekOrigin::Begin     ? 0
                : origin == SeekOrigin::Current ? static_cast<std::int64_t>(m_position)
                                                : static_cast<std::int64_t>(m_bytes->size());
            const std::int64_t target = base + offset;
            if (target < 0 || static_cast<std::uint64_t>(target) > m_bytes->size())
                return false;
            m_position = static_cast<std::size_t>(target);
            return true;
        }
        [[nodiscard]] std::uint64_t Tell() const override { return m_position; }
        [[nodiscard]] std::uint64_t Size() const override { return m_bytes->size(); }
        [[nodiscard]] bool Failed() const override { return false; }

    private:
        std::shared_ptr<const Bytes> m_bytes;
        std::size_t m_position = 0;
    };

    mutable std::mutex m_mutex;
    mutable int m_opens = 0;
    std::map<std::string, std::shared_ptr<const Bytes>> m_files;
};

struct ArchiveFixture {
    std::map<std::string, Bytes> entries;
    std::filesystem::path plain;
    std::filesystem::path encrypted;
    px::crypto::Key key = px::crypto::DeriveKey("file-source-test-key");
};

ArchiveFixture WriteArchives(const std::filesystem::path& root) {
    ArchiveFixture fixture;
    fixture.entries = {
        {"Content/plain.bin", Pattern(70'000, 1)},
        {"Content/compressed.txt", Compressible(300'000)},
        {"Content/empty.txt", {}},
        // Media entries stay uncompressed and, when encrypted, use seekable
        // chunked AES-GCM records.
        {"Content/Video/op.mp4", Pattern(600'000, 2)},
    };
    fixture.plain = root / "plain.pdx";
    fixture.encrypted = root / "encrypted.pdx";
    px::io::ArchiveWriter plain;
    px::io::ArchiveWriter encrypted;
    encrypted.SetKey(fixture.key);
    for (const auto& [path, bytes] : fixture.entries) {
        plain.Add(path, bytes);
        encrypted.Add(path, bytes);
    }
    if (!plain.Write(px::PathToUtf8(fixture.plain)) ||
        !encrypted.Write(px::PathToUtf8(fixture.encrypted)))
        throw std::runtime_error("archive fixture write failed");
    return fixture;
}

void ExpectArchiveMatches(px::test::Suite& suite, const px::io::Archive& archive,
                          const ArchiveFixture& fixture,
                          const std::string& label) {
    for (const auto& [path, bytes] : fixture.entries) {
        const auto whole = archive.Read(path);
        suite.Expect(whole && *whole == bytes, label + ": Read " + path);
        auto stream = archive.OpenStream(path);
        suite.Require(stream != nullptr, label + ": OpenStream " + path);
        suite.Expect(stream->Size() == bytes.size(), label + ": size " + path);
        suite.Expect(Drain(*stream, 5000) == bytes && !stream->Failed(),
                     label + ": streamed bytes " + path);
        if (bytes.size() > 1000) {
            // Backward seeks restart zstd frames and reload encrypted chunks.
            const std::uint64_t middle = bytes.size() / 2;
            suite.Expect(stream->Seek(static_cast<std::int64_t>(middle),
                                      SeekOrigin::Begin),
                         label + ": backward seek " + path);
            std::array<std::uint8_t, 512> window{};
            suite.Expect(px::io::ReadExact(*stream, window.data(), window.size()) &&
                             std::equal(window.begin(), window.end(),
                                        bytes.begin() +
                                            static_cast<std::ptrdiff_t>(middle)),
                         label + ": bytes after seek " + path);
        }
    }
    suite.Expect(!archive.Read("Content/missing.bin"),
                 label + ": missing entry is absent");
}

}  // namespace

int main() {
    px::test::Suite suite("FileSource");

    suite.Run("PackageSourceIsNativeOffAndroid", [&] {
#if defined(__ANDROID__)
        suite.Equal("sdl", px::io::PackageFileSource()->Name(), "package source");
#else
        suite.Equal("native", px::io::PackageFileSource()->Name(), "package source");
#endif
        suite.Expect(px::io::NativeFileSource() == px::io::NativeFileSource(),
                     "native source is a shared singleton");
    });

    suite.Run("NativeAndSdlAgreeOnFiles", [&] {
        px::test::TempDirectory temp("file-source");
        const Bytes data = Pattern(200'003, 7);
        // Non-ASCII names prove both sources take UTF-8 paths on Windows.
        const auto file = temp.path / px::PathFromUtf8("資料/片段-a.bin");
        WriteBytes(file, data);
        WriteBytes(temp.path / px::PathFromUtf8("資料/b.txt"), Bytes{'b'});
        std::filesystem::create_directories(temp.path / px::PathFromUtf8("資料/sub"));
        const std::string path = px::PathToUtf8(file);
        const std::string directory = px::PathToUtf8(file.parent_path());

        for (const auto& source : {px::io::NativeFileSource(), px::io::SdlFileSource()}) {
            const std::string label(source->Name());
            suite.Expect(source->FileSize(path) == data.size(), label + ": FileSize");
            suite.Expect(!source->IsFile(directory), label + ": directory is not a file");
            suite.Expect(!source->IsFile(directory + "/missing.bin"),
                         label + ": missing file");
            suite.Expect(source->Open(directory + "/missing.bin") == nullptr,
                         label + ": missing file does not open");

            auto stream = source->Open(path);
            suite.Require(stream != nullptr, label + ": Open");
            suite.Expect(stream->Size() == data.size() && stream->Tell() == 0,
                         label + ": initial cursor");
            suite.Expect(Drain(*stream, 777) == data && !stream->Failed(),
                         label + ": sequential read");
            suite.Expect(stream->Tell() == data.size(), label + ": cursor at end");
            std::uint8_t byte = 0;
            suite.Expect(stream->Read(&byte, 1) == 0 && !stream->Failed(),
                         label + ": reading at end is not an error");

            suite.Expect(stream->Seek(-10, SeekOrigin::End) &&
                             stream->Tell() == data.size() - 10,
                         label + ": seek from end");
            suite.Expect(stream->Seek(-5, SeekOrigin::Current) &&
                             stream->Tell() == data.size() - 15,
                         label + ": seek from current");
            std::array<std::uint8_t, 15> tail{};
            suite.Expect(px::io::ReadExact(*stream, tail.data(), tail.size()) &&
                             std::equal(tail.begin(), tail.end(), data.end() - 15),
                         label + ": tail bytes");
            suite.Expect(!stream->Seek(1, SeekOrigin::End) &&
                             !stream->Seek(-1, SeekOrigin::Begin),
                         label + ": out-of-range seeks are rejected");
            suite.Expect(stream->Seek(0, SeekOrigin::Begin) && !stream->Failed(),
                         label + ": rejected seeks leave the stream usable");

            auto second = source->Open(path);
            suite.Require(second != nullptr, label + ": second Open");
            suite.Expect(second->Seek(100, SeekOrigin::Begin) && stream->Tell() == 0,
                         label + ": cursors are independent");

            const auto whole = px::io::ReadFile(*source, path);
            suite.Expect(whole && *whole == data, label + ": ReadFile");
            suite.Expect(!px::io::ReadFile(*source, path, data.size() - 1),
                         label + ": ReadFile honours its limit");
            suite.Expect(!px::io::ReadFile(*source, directory),
                         label + ": ReadFile rejects a directory");

            const auto names = source->ListFiles(directory);
            suite.Expect(names && *names == std::vector<std::string>{
                                               "b.txt", "片段-a.bin"},
                         label + ": ListFiles returns sorted regular files");
            suite.Expect(!source->ListFiles(directory + "/missing"),
                         label + ": listing a missing directory fails");
        }
    });

    suite.Run("SdlRootsJoinLexically", [&] {
        const auto sdl = px::io::SdlFileSource();
        suite.Expect(sdl->CanonicalRoot(".") == std::string{}, "'.' is the asset root");
        suite.Expect(sdl->CanonicalRoot("./") == std::string{}, "'./' is the asset root");
        suite.Expect(sdl->CanonicalRoot("./Game/") == std::string("Game"),
                     "leading ./ and trailing / are dropped");
        suite.Expect(sdl->ResolveUnder("", "Content/a.png") ==
                         std::string("Content/a.png"),
                     "asset root join");
        suite.Expect(sdl->ResolveUnder("Game", "Content/a.png") ==
                         std::string("Game/Content/a.png"),
                     "nested root join");
        suite.Expect(!sdl->ResolveUnder("Game", ""), "empty relative path");
    });

    suite.Run("NativeResolveUnderStaysInsideRoot", [&] {
        px::test::TempDirectory temp("file-source-root");
        const auto native = px::io::NativeFileSource();
        const auto root = native->CanonicalRoot(px::PathToUtf8(temp.path));
        suite.Require(root.has_value(), "canonical root");
        suite.Expect(native->ResolveUnder(*root, "a/b.txt").has_value(),
                     "child path resolves");
        suite.Expect(!native->ResolveUnder(*root, "../escape.txt"),
                     "parent traversal is rejected");
    });

    suite.Run("ArchiveReadsIdenticallyThroughEverySource", [&] {
        px::test::TempDirectory temp("file-source-archive");
        const auto fixture = WriteArchives(temp.path);
        auto memory = std::make_shared<MemoryFileSource>();
        memory->Add("apk/Package/plain.pdx", ReadFileBytes(fixture.plain));
        memory->Add("apk/Package/encrypted.pdx", ReadFileBytes(fixture.encrypted));

        struct Case {
            std::shared_ptr<const FileSource> source;
            std::string plain;
            std::string encrypted;
        };
        const std::vector<Case> cases{
            {px::io::NativeFileSource(), px::PathToUtf8(fixture.plain),
             px::PathToUtf8(fixture.encrypted)},
            {px::io::SdlFileSource(), px::PathToUtf8(fixture.plain),
             px::PathToUtf8(fixture.encrypted)},
            {memory, "apk/Package/plain.pdx", "apk/Package/encrypted.pdx"},
        };
        for (const auto& item : cases) {
            const std::string label(item.source->Name());
            px::io::Archive plain;
            suite.Require(plain.Open(item.source, item.plain), label + ": open plain");
            ExpectArchiveMatches(suite, plain, fixture, label + "/plain");
            px::io::Archive encrypted;
            suite.Require(encrypted.Open(item.source, item.encrypted, &fixture.key),
                          label + ": open encrypted");
            ExpectArchiveMatches(suite, encrypted, fixture, label + "/encrypted");
        }
        suite.Expect(memory->Opens() > 10,
                     "archive streams open the memory source, not the disk");

        px::io::Archive fromDisk;
        suite.Expect(!fromDisk.Open("apk/Package/plain.pdx"),
                     "the in-memory path does not exist for the default source");
    });

    suite.Run("ArchiveRejectsTruncatedSource", [&] {
        px::test::TempDirectory temp("file-source-truncated");
        const auto fixture = WriteArchives(temp.path);
        auto bytes = ReadFileBytes(fixture.plain);
        auto memory = std::make_shared<MemoryFileSource>();
        memory->Add("short.pdx", Bytes(bytes.begin(), bytes.begin() + 20));
        bytes.resize(bytes.size() - 1);
        memory->Add("cut.pdx", bytes);
        px::io::Archive archive;
        suite.Expect(!archive.Open(memory, "short.pdx"), "header shorter than 28 bytes");
        suite.Expect(!archive.Open(memory, "cut.pdx"), "index extends past the end");
        suite.Expect(!archive.Open(memory, "missing.pdx"), "missing archive");
    });

    suite.Run("VfsMountsReadThroughTheirSource", [&] {
        px::test::TempDirectory temp("file-source-vfs");
        const auto fixture = WriteArchives(temp.path);
        const Bytes loose = Pattern(1234, 9);
        WriteBytes(temp.path / "loose/Content/loose.bin", loose);

        auto memory = std::make_shared<MemoryFileSource>();
        memory->Add("Package/plain.pdx", ReadFileBytes(fixture.plain));
        memory->Add("Content/asset-only.txt", Bytes{'o', 'k'});

        px::io::VFS vfs;
        vfs.MountDirectory(px::PathToUtf8(temp.path / "loose"), px::io::SdlFileSource());
        vfs.MountDirectory(".", memory);
        suite.Require(vfs.MountArchive("Package/plain.pdx", nullptr,
                                       px::io::ArchiveMountPriority::Base, {},
                                       memory),
                      "archive mounts from the memory source");

        const auto looseRead = vfs.Read("Content/loose.bin");
        suite.Expect(looseRead && *looseRead == loose, "SDL directory mount read");
        auto looseStream = vfs.Open("Content/loose.bin");
        suite.Expect(looseStream && Drain(*looseStream) == loose,
                     "SDL directory mount stream");
        suite.Expect(vfs.ReadText("Content/asset-only.txt") == std::string("ok"),
                     "memory directory mount read");
        suite.Expect(vfs.Exists("Content/asset-only.txt") &&
                         vfs.Exists("Content/plain.bin") &&
                         !vfs.Exists("Content/nothing.bin"),
                     "Exists consults every mount through its source");
        const auto archived = vfs.Read("Content/compressed.txt");
        suite.Expect(archived && *archived == fixture.entries.at("Content/compressed.txt"),
                     "archive entry through the memory source");
        suite.Expect(!vfs.Read("../loose/Content/loose.bin"),
                     "virtual paths still reject traversal");
    });

    suite.Run("Utf8PathStemKeepsNamesTheAnsiCodePageCannotHold", [&] {
        // A music file name in Japanese on a Big5 (or any non-UTF-8) Windows
        // code page: the narrow std::filesystem conversion threw and ended
        // the Player as soon as the Story played it.
        const std::string path = "Assets/Music/2-01. この櫻ノ詩の下.flac";
        suite.Expect(px::PathStemUtf8(path) ==
                         "2-01. この櫻ノ詩の下",
                     "stem keeps the UTF-8 name and only the last extension goes");
        suite.Expect(px::PathStemUtf8("Assets/Music/theme.v2.ogg") == "theme.v2",
                     "dots inside the name stay");
        suite.Expect(px::PathStemUtf8("Story/第一章.pxstory") ==
                         "第一章",
                     "scene file names round-trip");
    });

    suite.Run("DefaultVfsBehaviourIsUnchanged", [&] {
        px::test::TempDirectory temp("file-source-default");
        const Bytes data = Pattern(4096, 3);
        WriteBytes(temp.path / "Content/a.bin", data);
        std::filesystem::create_directories(temp.path / "Content/folder.bin");
        px::io::VFS vfs;
        vfs.MountDirectory(px::PathToUtf8(temp.path));
        suite.Expect(vfs.Read("Content/a.bin") == std::optional<Bytes>(data),
                     "default mount reads the file system");
        suite.Expect(!vfs.Exists("Content/folder.bin") && !vfs.Open("Content/folder.bin"),
                     "directories are not files");
    });

    return suite.Finish();
}
