#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace image
{
constexpr size_t kMaxFileBytes = 128ull << 20;
constexpr size_t kMaxImageBytes = 128ull << 20;
constexpr size_t kMemoryLimit = 512ull << 20;
constexpr unsigned kMaxDimension = 16384;

// Accounts for image buffers outside stb, including decoded effect images waiting for upload.
class Memory
{
public:
    Memory() = default;
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&& other) noexcept;
    Memory& operator=(Memory&& other) noexcept;
    ~Memory();
    bool Resize(size_t bytes);

private:
    size_t bytes = 0;
};

bool ByteSize(unsigned width, unsigned height, unsigned depth, size_t pixelBytes, size_t& bytes);
void Free(void* pixels) noexcept;

struct Pixels
{
    std::unique_ptr<void, void (*)(void*)> data{ nullptr, Free };
    int width = 0, height = 0, depth = 1;
    std::string error;
};

// Checks dimensions and DDS payloads before decoding. Both forms return RGBA, or RGBA floats when requested.
Pixels Decode(std::string_view data, bool floating = false);
Pixels Read(const std::filesystem::path& path, bool floating = false);
} // namespace image
