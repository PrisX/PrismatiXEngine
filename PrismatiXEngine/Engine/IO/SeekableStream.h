#pragma once

#include <cstddef>
#include <cstdint>

namespace px::io {

enum class SeekOrigin : std::uint8_t {
    Begin,
    Current,
    End,
};

// Read-only, package-relative byte stream used by native decoders. Implementations
// must keep an independent cursor so a decoder can seek without mutating VFS state.
class SeekableReadStream {
public:
    virtual ~SeekableReadStream() = default;

    [[nodiscard]] virtual std::size_t Read(std::uint8_t* destination,
                                           std::size_t bytes) = 0;
    [[nodiscard]] virtual bool Seek(std::int64_t offset, SeekOrigin origin) = 0;
    [[nodiscard]] virtual std::uint64_t Tell() const = 0;
    [[nodiscard]] virtual std::uint64_t Size() const = 0;
    [[nodiscard]] virtual bool Failed() const = 0;
};

}  // namespace px::io
