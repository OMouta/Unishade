#include "../src/posix/image.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

void Word(std::string& data, size_t index, uint32_t value)
{
    for (size_t byte = 0; byte < 4; ++byte)
        data[index * 4 + byte] = static_cast<char>(value >> (8 * byte));
}

std::string Dds(unsigned width, unsigned height, unsigned channels = 4)
{
    std::string data(128, '\0');
    data.replace(0, 4, "DDS ");
    Word(data, 1, 124); // header size
    Word(data, 2, 0x1007); // caps, height, width, pixel format
    Word(data, 3, height);
    Word(data, 4, width);
    Word(data, 19, 32); // pixel format size
    Word(data, 20, 0x40 | (channels == 4 ? 1 : 0)); // RGB, alpha
    Word(data, 22, channels * 8);
    Word(data, 27, 0x1000); // texture
    return data;
}

bool RejectsTruncations(const std::string& data)
{
    for (size_t size = 0; size < data.size(); ++size)
        if (image::Decode(std::string_view(data.data(), size)).data)
            return false;
    return true;
}
} // namespace

int main()
{
    bool ok = true;
    std::string raw = Dds(1, 1);
    raw.append("\x11\x22\x33\x80", 4);
    {
        const image::Pixels pixels = image::Decode(raw);
        const unsigned char expected[] = { 0x33, 0x22, 0x11, 0x80 };
        ok &= Check(pixels.data && pixels.width == 1 && pixels.height == 1 && pixels.depth == 1 &&
                        std::memcmp(pixels.data.get(), expected, sizeof(expected)) == 0,
                    "valid DDS decodes to RGBA");
    }
    ok &= Check(RejectsTruncations(raw), "every truncated raw DDS is rejected");
    ok &= Check(!image::Decode(Dds(16384, 16384)).data, "oversized DDS is rejected before allocation");
    ok &= Check(!image::Decode(Dds(0, 1)).data && !image::Decode(Dds(1, 0)).data, "zero dimensions are rejected");
    ok &= Check(!image::Decode(Dds(std::numeric_limits<unsigned>::max(), 1)).data, "overflowing dimensions are rejected");
    ok &= Check(!image::Decode(raw, true).data, "DDS does not enter the float decoder");

    std::string tga(18, '\0');
    tga[2] = 2; // uncompressed color
    tga[12] = tga[14] = 1;
    tga[16] = 32;
    tga[17] = 8; // alpha bits
    tga.append("\xFF\xFF\xFF\x80", 4);
    {
        const image::Pixels pixels = image::Decode(tga, true);
        const auto* values = static_cast<const float*>(pixels.data.get());
        ok &= Check(values && values[0] == 1 && values[1] == 1 && values[2] == 1 && values[3] > 0.5f && values[3] < 0.51f,
                    "ordinary images still decode to RGBA floats");
    }

    for (char format : { '1', '2', '3', '4', '5' })
    {
        std::string compressed = Dds(3, 3);
        Word(compressed, 20, 4); // FOURCC
        const std::string fourcc = std::string("DXT") + format;
        compressed.replace(84, 4, fourcc);
        compressed.append(format == '1' ? 8 : 16, '\0');
        ok &= Check(bool(image::Decode(compressed).data), "valid DXT block decodes, including partial edges");
        ok &= Check(RejectsTruncations(compressed), "every truncated DXT block is rejected");
    }

    std::string cube = Dds(1, 1);
    Word(cube, 28, 0x200); // cubemap
    cube.append(6 * 4, '\0');
    ok &= Check(image::Decode(cube).depth == 6 && image::Decode(cube).data, "all six cubemap faces decode");
    ok &= Check(RejectsTruncations(cube), "missing cubemap faces are rejected");
    std::string volume = Dds(1, 1);
    Word(volume, 6, 2);
    volume.append(2 * 4, '\0');
    ok &= Check(image::Decode(volume).depth == 2 && image::Decode(volume).data, "volume slices decode");
    ok &= Check(RejectsTruncations(volume), "missing volume slices are rejected");

    std::string mipmaps = Dds(2, 2);
    Word(mipmaps, 7, 2);
    Word(mipmaps, 27, 0x401000); // texture, mipmaps
    mipmaps.append(4 * 4 + 4, '\0');
    ok &= Check(bool(image::Decode(mipmaps).data), "complete mip chain decodes");
    ok &= Check(RejectsTruncations(mipmaps), "truncated mip chain is rejected");
    Word(mipmaps, 7, std::numeric_limits<uint32_t>::max());
    ok &= Check(!image::Decode(mipmaps).data, "excessive mip counts are rejected");
    std::string extended = Dds(1, 1);
    Word(extended, 20, 4);
    extended.replace(84, 4, "DX10");
    ok &= Check(!image::Decode(extended).data, "missing DX10 header is rejected");
    extended.append(20, '\0');
    Word(extended, 32, 28); // RGBA8
    Word(extended, 35, 1); // one array entry
    extended.append(4, '\0');
    ok &= Check(bool(image::Decode(extended).data) && RejectsTruncations(extended), "DX10 headers and payloads are checked");
    Word(extended, 35, 2);
    ok &= Check(!image::Decode(extended).data, "unsupported DDS arrays are rejected");

    size_t bytes = 0;
    ok &= Check(image::ByteSize(8192, 4096, 1, 4, bytes) && bytes == image::kMaxImageBytes, "output at the byte limit is accepted");
    ok &= Check(!image::ByteSize(8192, 4097, 1, 4, bytes), "oversized resize output is rejected");
    ok &= Check(!image::ByteSize(16384, 16384, 16384, 16, bytes), "volume and float output sizes are bounded");
    ok &= Check(!image::ByteSize(1, 1, 1, std::numeric_limits<size_t>::max(), bytes), "byte calculations cannot overflow");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("unishade-image-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory))
        return 1;
    const auto file = directory / "image.tga";
    std::ofstream(file, std::ios::binary) << tga;
    ok &= Check(bool(image::Read(file).data), "reads a valid image from a file");
    {
        image::Memory memory;
        ok &= Check(memory.Resize(image::kMemoryLimit), "memory budget can be reserved without allocating it");
        ok &= Check(!image::Decode(raw).data, "decoder respects retained images' memory budget");
        ok &= Check(!image::Read(file).data, "file buffers respect the memory budget too");
        image::Memory moved = std::move(memory);
        ok &= Check(!memory.Resize(1), "moving a reservation preserves the budget");
        ok &= Check(moved.Resize(0) && bool(image::Decode(raw).data), "releasing memory permits another decode");
    }
    std::ofstream(file, std::ios::binary | std::ios::trunc);
    ok &= Check(!image::Read(file).data, "empty image files are rejected");
    std::filesystem::remove(file);
    std::filesystem::remove(directory);

    std::printf(ok ? "All image tests passed.\n" : "Some image tests failed.\n");
    return ok ? 0 : 1;
}
