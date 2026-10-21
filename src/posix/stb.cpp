#include "image.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>

namespace
{
std::atomic<size_t> imageBytes = 0;

bool Reserve(size_t bytes)
{
    size_t used = imageBytes.load(std::memory_order_relaxed);
    do
    {
        if (bytes > image::kMemoryLimit - used)
            return false;
    } while (!imageBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
}

void Release(size_t bytes)
{
    imageBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

struct alignas(std::max_align_t) Allocation
{
    size_t bytes;
};

void* Allocate(size_t bytes)
{
    if (bytes > image::kMaxImageBytes || !Reserve(bytes))
        return nullptr;
    auto* allocation = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + bytes));
    if (!allocation)
    {
        Release(bytes);
        return nullptr;
    }
    allocation->bytes = bytes;
    return allocation + 1;
}

void Deallocate(void* pointer)
{
    if (!pointer)
        return;
    auto* allocation = static_cast<Allocation*>(pointer) - 1;
    Release(allocation->bytes);
    std::free(allocation);
}

void* Reallocate(void* pointer, size_t bytes)
{
    if (!pointer)
        return Allocate(bytes);
    if (!bytes)
    {
        Deallocate(pointer);
        return nullptr;
    }
    auto* allocation = static_cast<Allocation*>(pointer) - 1;
    const size_t previous = allocation->bytes;
    if (bytes > image::kMaxImageBytes || (bytes > previous && !Reserve(bytes - previous)))
        return nullptr;
    auto* resized = static_cast<Allocation*>(std::realloc(allocation, sizeof(Allocation) + bytes));
    if (!resized)
    {
        if (bytes > previous)
            Release(bytes - previous);
        return nullptr;
    }
    if (bytes < previous)
        Release(previous - bytes);
    resized->bytes = bytes;
    return resized + 1;
}
} // namespace

// stb's temporary allocations share the same budget as retained effect images.
#define STBI_MALLOC Allocate
#define STBI_REALLOC Reallocate
#define STBI_FREE Deallocate
#define STBI_MAX_DIMENSIONS image::kMaxDimension
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb_image.h>

// Needs stb_image's internals, so it goes in the same file.
#define STB_IMAGE_DDS_IMPLEMENTATION
#include <stb_image_dds.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBIR_MALLOC(size, context) Allocate(size)
#define STBIR_FREE(pointer, context) Deallocate(pointer)
#include <stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace image
{
Memory::Memory(Memory&& other) noexcept : bytes(std::exchange(other.bytes, 0)) {}

Memory& Memory::operator=(Memory&& other) noexcept
{
    if (this != &other)
    {
        Release(bytes);
        bytes = std::exchange(other.bytes, 0);
    }
    return *this;
}

Memory::~Memory() { Release(bytes); }

bool Memory::Resize(size_t next)
{
    if (next > bytes && !Reserve(next - bytes))
        return false;
    if (next < bytes)
        Release(bytes - next);
    bytes = next;
    return true;
}

bool ByteSize(unsigned width, unsigned height, unsigned depth, size_t pixelBytes, size_t& bytes)
{
    if (!width || !height || !depth || !pixelBytes || width > kMaxDimension || height > kMaxDimension || depth > kMaxDimension)
        return false;
    bytes = pixelBytes;
    for (unsigned dimension : { width, height, depth })
    {
        if (bytes > kMaxImageBytes / dimension)
            return false;
        bytes *= dimension;
    }
    return true;
}

void Free(void* pixels) noexcept { stbi_image_free(pixels); }

namespace
{
// This decoder does not check its reads. Validate every byte it will consume, including faces and mip skips.
bool DdsInfo(std::string_view data, int& width, int& height, int& depth)
{
    if (data.size() < 4 + sizeof(DDS_HEADER))
        return false;
    DDS_HEADER header;
    std::memcpy(&header, data.data() + 4, sizeof(header));
    const unsigned flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    if (header.dwSize != sizeof(header) || (header.dwFlags & flags) != flags || header.sPixelFormat.dwSize != sizeof(header.sPixelFormat) ||
        !(header.sCaps.dwCaps1 & DDSCAPS_TEXTURE) || !(header.sPixelFormat.dwFlags & (DDPF_FOURCC | DDPF_RGB | DDPF_LUMINANCE | DDPF_YUV)))
        return false;
    size_t offset = 4 + sizeof(header), blockBytes = 0;
    if (header.sPixelFormat.dwFlags & DDPF_FOURCC)
    {
        const unsigned format = header.sPixelFormat.dwFourCC;
        if (format == MAKEFOURCC('D', 'X', '1', '0'))
        {
            if (data.size() - offset < sizeof(DDS_HEADER_DXT10))
                return false;
            DDS_HEADER_DXT10 extended;
            std::memcpy(&extended, data.data() + offset, sizeof(extended));
            offset += sizeof(extended);
            if (extended.arraySize != 1)
                return false;
            switch (extended.dxgiFormat)
            {
            case 28: case 49: case 61: break;
            case 71: blockBytes = 8; break;
            case 74: case 77: blockBytes = 16; break;
            default: return false;
            }
        }
        else if (format == MAKEFOURCC('D', 'X', 'T', '1'))
            blockBytes = 8;
        else if (format == MAKEFOURCC('D', 'X', 'T', '2') || format == MAKEFOURCC('D', 'X', 'T', '3') ||
                 format == MAKEFOURCC('D', 'X', 'T', '4') || format == MAKEFOURCC('D', 'X', 'T', '5'))
            blockBytes = 16;
        else
            return false;
    }
    const bool cube = (header.sCaps.dwCaps2 & DDSCAPS2_CUBEMAP) != 0;
    const unsigned slices = header.dwDepth > 1 ? header.dwDepth : cube ? 6 : 1;
    size_t decoded = 0;
    if ((cube && (header.dwWidth != header.dwHeight || header.dwDepth > 1)) ||
        !ByteSize(header.dwWidth, header.dwHeight, slices, 4, decoded))
        return false;
    unsigned channels = header.sPixelFormat.dwRGBBitCount ? header.sPixelFormat.dwRGBBitCount / 8 :
                        (header.sPixelFormat.dwFlags & DDPF_ALPHAPIXELS) ? 4 : 3;
    if (!blockBytes && (channels < 1 || channels > 4 || header.sPixelFormat.dwRGBBitCount % 8 != 0))
        return false;
    unsigned levels = 1;
    if ((header.sCaps.dwCaps1 & DDSCAPS_MIPMAP) && header.dwMipMapCount > 1)
    {
        unsigned maxLevels = 1;
        for (unsigned size = std::max(header.dwWidth, header.dwHeight); size > 1; size >>= 1)
            ++maxLevels;
        if (header.dwMipMapCount > maxLevels)
            return false;
        levels = header.dwMipMapCount;
    }
    size_t faceBytes = blockBytes ? size_t((header.dwWidth + 3) / 4) * ((header.dwHeight + 3) / 4) * blockBytes :
                                   size_t(header.dwWidth) * header.dwHeight * channels;
    for (unsigned level = 1; level < levels; ++level)
    {
        const unsigned shift = level + (blockBytes ? 2 : 0);
        faceBytes += size_t(std::max(1u, header.dwWidth >> shift)) * std::max(1u, header.dwHeight >> shift) *
                     (blockBytes ? blockBytes : channels);
    }
    if (faceBytes > (data.size() - offset) / slices)
        return false;
    width = static_cast<int>(header.dwWidth);
    height = static_cast<int>(header.dwHeight);
    depth = static_cast<int>(slices);
    return true;
}
} // namespace

Pixels Decode(std::string_view data, bool floating)
{
    Pixels result;
    if (data.empty() || data.size() > kMaxFileBytes)
    {
        result.error = "image file is empty or exceeds the size limit";
        return result;
    }
    const bool dds = data.starts_with("DDS ");
    const auto* bytes = reinterpret_cast<const stbi_uc*>(data.data());
    const int length = static_cast<int>(data.size());
    int channels = 0;
    size_t decoded = 0;
    if ((dds ? floating || !DdsInfo(data, result.width, result.height, result.depth) :
               !stbi_info_from_memory(bytes, length, &result.width, &result.height, &channels)) ||
        !ByteSize(result.width, result.height, result.depth, floating ? 16 : 4, decoded))
    {
        result.error = "invalid image or dimensions beyond the size limit";
        return result;
    }
    result.data.reset(floating ? static_cast<void*>(stbi_loadf_from_memory(bytes, length, &result.width, &result.height, &channels, 4)) :
                      dds ? stbi_dds_load_from_memory(bytes, length, &result.width, &result.height, &result.depth, &channels, 4) :
                            stbi_load_from_memory(bytes, length, &result.width, &result.height, &channels, 4));
    if (!result.data)
        result.error = "could not decode image within the memory limit";
    return result;
}

Pixels Read(const std::filesystem::path& path, bool floating)
{
    Pixels result;
    try
    {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(path, error);
        if (error || !size || size > kMaxFileBytes)
        {
            result.error = "image file is missing or exceeds the size limit";
            return result;
        }
        Memory memory;
        if (!memory.Resize(static_cast<size_t>(size)))
        {
            result.error = "image memory limit reached";
            return result;
        }
        std::string data(static_cast<size_t>(size), '\0');
        std::ifstream input(path, std::ios::binary);
        if (!input.read(data.data(), static_cast<std::streamsize>(data.size())))
        {
            result.error = "could not read image file";
            return result;
        }
        return Decode(data, floating);
    }
    catch (const std::bad_alloc&)
    {
        result.error = "out of memory";
        return result;
    }
}
} // namespace image
