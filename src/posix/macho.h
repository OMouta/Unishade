#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

// The SDK a Mac program was built with, read from its executable. macOS 26 draws the windows of programs built with
// an older SDK with the earlier look, a shorter title bar and smaller corners, so the overlay needs to know which
// look the game has. Plain C++ without Apple's headers, so tests can check it anywhere.
namespace macho
{
namespace detail
{
constexpr uint32_t kFatMagic = 0xCAFEBABE;
constexpr uint32_t kFatMagic64 = 0xCAFEBABF;
constexpr uint32_t kMagic32 = 0xFEEDFACE;
constexpr uint32_t kMagic64 = 0xFEEDFACF;
constexpr uint32_t kCpuArm64 = 0x0100000C;
constexpr uint32_t kCpuX86_64 = 0x01000007;
constexpr uint32_t kBuildVersion = 0x32;     // LC_BUILD_VERSION
constexpr uint32_t kVersionMinMacOS = 0x24;  // LC_VERSION_MIN_MACOSX, from before LC_BUILD_VERSION

// Fat headers are big-endian, Mach-O headers of Apple's processors little-endian.
inline uint32_t Big32(const std::vector<uint8_t>& b, size_t at)
{
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
inline uint64_t Big64(const std::vector<uint8_t>& b, size_t at)
{
    return uint64_t(Big32(b, at)) << 32 | Big32(b, at + 4);
}
inline uint32_t Little32(const std::vector<uint8_t>& b, size_t at)
{
    return uint32_t(b[at + 3]) << 24 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 1]) << 8 | b[at];
}

// read(offset, size, bytes) fills bytes with exactly size bytes from offset, or returns false.
template <typename Read>
uint32_t Sdk(Read read)
{
    std::vector<uint8_t> b;
    if (!read(0, 8, b))
        return 0;
    uint64_t slice = 0;
    const uint32_t magic = Big32(b, 0);
    if (magic == kFatMagic || magic == kFatMagic64)
    {
        // Java class files share the magic, with a version where the count would be, so a large count is no fat file.
        const uint32_t count = Big32(b, 4);
        const size_t entry = magic == kFatMagic64 ? 32 : 20;
        if (count == 0 || count > 32 || !read(8, count * entry, b))
            return 0;
        // The slice Apple silicon runs, or the one Rosetta does.
        bool found = false;
        for (uint32_t cpu : { kCpuArm64, kCpuX86_64 })
            for (uint32_t i = 0; i < count && !found; ++i)
                if (Big32(b, i * entry) == cpu)
                {
                    slice = magic == kFatMagic64 ? Big64(b, i * entry + 8) : Big32(b, i * entry + 8);
                    found = true;
                }
        if (!found)
            return 0;
    }
    if (!read(slice, 28, b))
        return 0;
    const uint32_t thin = Little32(b, 0);
    if (thin != kMagic32 && thin != kMagic64)
        return 0;
    const uint32_t commands = Little32(b, 16), size = Little32(b, 20);
    const uint64_t first = slice + (thin == kMagic64 ? 32 : 28);
    if (size > (16u << 20) || !read(first, size, b))
        return 0;
    for (size_t at = 0, i = 0; i < commands && at + 8 <= b.size(); ++i)
    {
        const uint32_t command = Little32(b, at), length = Little32(b, at + 4);
        if (length < 8 || at + length > b.size())
            return 0;
        // The SDK is encoded as xxxx.yy.zz in nibbles, the major version in the top 16 bits.
        if (command == kBuildVersion && length >= 20)
            return Little32(b, at + 16) >> 16;
        if (command == kVersionMinMacOS && length >= 16)
            return Little32(b, at + 12) >> 16;
        at += length;
    }
    return 0;
}
} // namespace detail

// The major version of the SDK the executable was built with, such as 26, or 0 when the bytes are no Mach-O file or
// do not say. A fat file is read for its arm64 slice, or its x86_64 one when it has none.
inline uint32_t SdkMajor(const std::vector<uint8_t>& file)
{
    return detail::Sdk([&](uint64_t offset, size_t size, std::vector<uint8_t>& bytes) {
        if (offset > file.size() || size > file.size() - offset)
            return false;
        bytes.assign(file.begin() + std::ptrdiff_t(offset), file.begin() + std::ptrdiff_t(offset + size));
        return true;
    });
}

// The same for a file, reading only its headers. 0 when it cannot be read.
inline uint32_t SdkMajorOfFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return 0;
    return detail::Sdk([&](uint64_t offset, size_t size, std::vector<uint8_t>& bytes) {
        bytes.resize(size);
        file.clear();
        file.seekg(std::streamoff(offset));
        return file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)) && size_t(file.gcount()) == size;
    });
}
} // namespace macho
