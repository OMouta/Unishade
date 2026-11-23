// Tests for installing effect packages, shared by Setup on Windows and the macOS and Linux host: what a package's
// zip may hold and where its files may go.

#include "setup/package_files.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

template <class Function>
bool Throws(Function function)
{
    try
    {
        function();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    return false;
}

using Files = std::vector<std::pair<std::string, std::string>>;

// A zip of the files in memory. Their contents are compressed, as in the packages on ReShade's list.
std::string Zip(const Files& files)
{
    mz_zip_archive zip{};
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& [name, contents] : files)
        mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(), MZ_BEST_SPEED);
    void* buffer = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    mz_zip_writer_end(&zip);
    std::string data(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    return data;
}

// miniz writes no names that start with a slash, so they are changed afterwards.
void Rename(std::string& zip, const std::string& from, const std::string& to)
{
    for (size_t at = zip.find(from); at != std::string::npos; at = zip.find(from, at + to.size()))
        zip.replace(at, from.size(), to);
}

// Changes the unpacked size the zip's directory states for a file, which is what the limits check.
void SetUnpackedSize(std::string& zip, const std::string& name, uint32_t size)
{
    // Each file's entry in the directory starts with PK\1\2, and holds the size at 24, the name's length at 28 and the name at 46.
    for (size_t at = zip.find("PK\x01\x02"); at != std::string::npos; at = zip.find("PK\x01\x02", at + 4))
        if (size_t(uint8_t(zip[at + 28]) | uint8_t(zip[at + 29]) << 8) == name.size() && zip.compare(at + 46, name.size(), name) == 0)
            for (size_t byte = 0; byte < 4; ++byte)
                zip[at + 24 + byte] = static_cast<char>((size >> (byte * 8)) & 0xFF);
}

void Write(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;
}
} // namespace

int main()
{
    bool ok = true;
    const fs::path root = fs::temp_directory_path() / ("unishade-package-tests-" + std::to_string(std::random_device{}()));
    const fs::path extracted = root / "package";
    fs::create_directories(root);

    ok &= Check(IsInside(root / "a" / "b", root / "a") && IsInside(root / "a", root / "a") && IsInside(root / "a" / "", root / "a" / ""),
                "a folder holds what is in it, and itself");
    ok &= Check(!IsInside(root / "ab", root / "a") && !IsInside(root / "a" / ".." / "b", root / "a") && !IsInside(root, root / "a"),
                "nothing beside or above a folder is inside it");
    ok &= Check(!IsInside(root / "a", ""), "nothing is inside an empty path");
#ifdef _WIN32
    ok &= Check(IsInside("C:\\Games\\Unishade\\dxgi.dll", "c:/games/UNISHADE") && !IsInside("D:\\Games\\Unishade", "C:\\Games\\Unishade"),
                "Windows compares folders without case, and drives too");
#else
    ok &= Check(!IsInside("/games/Unishade/a.fx", "/games/unishade"), "macOS and Linux compare folders as written");
#endif

    ok &= Check(AllowedPackageFile("Bloom.fx") && AllowedPackageFile("ReShade.FXH") && AllowedPackageFile("lut.cube") &&
                    AllowedPackageFile("LICENSE") && AllowedPackageFile("readme"),
                "effects, textures and licenses may be installed, whatever their case");
    ok &= Check(!AllowedPackageFile("depth.addon64") && !AllowedPackageFile("dxgi.dll") && !AllowedPackageFile("run.exe") &&
                    !AllowedPackageFile("Bloom.fx.exe") && !AllowedPackageFile("LICENSE.exe") && !AllowedPackageFile(".fx"),
                "add-ons, libraries and programs may not");

    // Only the allowed files are unpacked, and backslashes separate folders as on Windows.
    std::string zip = Zip({ { "pkg-main/Shaders/Bloom.fx", "technique Bloom {}" },
                            { "pkg-main/Shaders/Sub/Common.FXH", "#pragma once" },
                            { "pkg-main/Shaders/../Shaders/Inside.fx", "technique Inside {}" },
                            { "pkg-main\\Textures\\Lens.png", "not really a png" },
                            { "pkg-main/LICENSE", "MIT" },
                            { "pkg-main/Shaders/evil.dll", "MZ...." },
                            { "pkg-main/Shaders/depth.addon64", "MZ...." },
                            { "pkg-main/run.exe", "MZ...." } });
    ok &= Check(!Throws([&] { ExtractZip(zip, extracted, "Test"); }), "unpacks a package");
    const fs::path package = extracted / "pkg-main";
    ok &= Check(fs::exists(package / "Shaders" / "Bloom.fx") && fs::exists(package / "Shaders" / "Sub" / "Common.FXH") &&
                    fs::exists(package / "Shaders" / "Inside.fx") && fs::exists(package / "Textures" / "Lens.png") && fs::exists(package / "LICENSE"),
                "unpacks effects, textures and licenses");
    ok &= Check(!fs::exists(package / "Shaders" / "evil.dll") && !fs::exists(package / "Shaders" / "depth.addon64") &&
                    !fs::exists(package / "run.exe"),
                "leaves out add-ons, libraries and programs");

    const PackageFolders folders = FindPackageFolders(extracted, "Test");
    ok &= Check(folders.shaders == package / "Shaders" && folders.textures == package / "Textures", "finds the Shaders and Textures folders");

    // Names that lead out of the package's folder, onto a drive or into a stream of another file.
    const fs::path outside = root / "outside";
    const std::u8string below = (outside / "evil.fx").relative_path().generic_u8string();
    const std::string absolute = "/" + std::string(reinterpret_cast<const char*>(below.data()), below.size());
    for (const std::string name : { "../outside/evil.fx", "pkg/../../outside/evil.fx", "..\\outside\\evil.fx", "pkg\\..\\..\\outside\\evil.fx",
                                    "\\outside\\evil.fx", "C:/evil.fx", "C:evil.fx", "c:\\evil.fx", "LICENSE.txt:evil.fx", "Shaders/a.fx:$DATA" })
    {
        fs::remove_all(extracted);
        ok &= Check(Throws([&] { ExtractZip(Zip({ { name, "technique Evil {}" } }), extracted, "Test"); }), name.c_str());
    }
    zip = Zip({ { "_" + absolute.substr(1), "technique Evil {}" } });
    Rename(zip, "_" + absolute.substr(1), absolute);
    ok &= Check(Throws([&] { ExtractZip(zip, extracted, "Test"); }), "rejects an absolute path");
    ok &= Check(!fs::exists(outside) && !fs::exists(root / "evil.fx"), "writes nothing outside the package's folder");

    // The limits are checked before anything is unpacked.
    fs::remove_all(extracted);
    ok &= Check(Throws([] { ExtractZip("not a zip", fs::path(), "Test"); }), "rejects what is not a zip");
    Files many(kZipEntries, { "", "" });
    for (size_t index = 0; index < many.size(); ++index)
        many[index].first = "file" + std::to_string(index) + ".bin";
    ok &= Check(!Throws([&] { ExtractZip(Zip(many), extracted, "Test"); }), "takes as many files as the limit");
    many.push_back({ "Bloom.fx", "technique Bloom {}" });
    ok &= Check(Throws([&] { ExtractZip(Zip(many), extracted, "Test"); }), "rejects more files");

    // Files the limits reject come after one that would be unpacked, and the unpacked sizes are only stated, so
    // nothing large is written.
    zip = Zip({ { "first.fx", "technique First {}" }, { "big.bin", "contents" } });
    SetUnpackedSize(zip, "big.bin", static_cast<uint32_t>(kZipEntrySize));
    ok &= Check(!Throws([&] { ExtractZip(zip, extracted / "entry", "Test"); }) && fs::exists(extracted / "entry" / "first.fx"),
                "takes a file as large as the limit");
    SetUnpackedSize(zip, "big.bin", static_cast<uint32_t>(kZipEntrySize + 1));
    ok &= Check(Throws([&] { ExtractZip(zip, extracted / "larger", "Test"); }) && !fs::exists(extracted / "larger"), "rejects a larger file");

    Files parts(static_cast<size_t>(kZipTotalSize / kZipEntrySize), { "", "contents" });
    for (size_t index = 0; index < parts.size(); ++index)
        parts[index].first = "part" + std::to_string(index) + ".bin";
    zip = Zip(parts);
    for (const auto& part : parts)
        SetUnpackedSize(zip, part.first, static_cast<uint32_t>(kZipEntrySize));
    ok &= Check(!Throws([&] { ExtractZip(zip, extracted / "total", "Test"); }), "takes as much data as the limit");
    parts.insert(parts.begin(), { "first.fx", "technique First {}" });
    zip = Zip(parts);
    for (const auto& part : parts)
        if (part.first != "first.fx")
            SetUnpackedSize(zip, part.first, static_cast<uint32_t>(kZipEntrySize));
    ok &= Check(Throws([&] { ExtractZip(zip, extracted / "more", "Test"); }) && !fs::exists(extracted / "more"), "rejects more data");

    // Packages without Shaders and Textures folders use the shallowest folders with effects and images.
    fs::remove_all(extracted);
    Write(extracted / "pkg" / "effects" / "deeper" / "B.fx", "");
    Write(extracted / "pkg" / "effects" / "A.FX", "");
    Write(extracted / "pkg" / "images" / "a.JPG", "");
    const PackageFolders shallowest = FindPackageFolders(extracted, "Test");
    ok &= Check(shallowest.shaders == extracted / "pkg" / "effects" && shallowest.textures == extracted / "pkg" / "images",
                "finds the shallowest folders with effects and images");
    Write(extracted / "pkg" / "sHaDeRs" / "readme.txt", "");
    ok &= Check(FindPackageFolders(extracted, "Test").shaders == extracted / "pkg" / "sHaDeRs", "a Shaders folder comes first, whatever its case");
    fs::remove_all(extracted);
    Write(extracted / "pkg" / "readme.txt", "");
    ok &= Check(Throws([&] { FindPackageFolders(extracted, "Test"); }), "a package needs effects");

    // Copying leaves out the files the list denies, compared without case, and what may not be installed.
    fs::remove_all(extracted);
    for (const char* name : { "A.fx", "B.fx", "C.fxh", "Sub/D.fx", "Sub/E.fx", "evil.dll" })
        Write(extracted / name, "");
    const fs::path shaders = root / "reshade-shaders" / "Shaders" / "Test";
    CopyPackageFiles(extracted, shaders, " a.fx ,,sub/e.fx, d.FX");
    ok &= Check(fs::exists(shaders / "B.fx") && fs::exists(shaders / "C.fxh") && fs::exists(shaders / "Sub" / "E.fx"), "copies the package's files");
    ok &= Check(!fs::exists(shaders / "A.fx") && !fs::exists(shaders / "Sub" / "D.fx") && !fs::exists(shaders / "evil.dll"),
                "leaves out denied files and what may not be installed");

    // EffectPackages.ini names install folders the Windows way, and they must be in reshade-shaders.
    const fs::path effects = root / "reshade-shaders";
    ok &= Check(PackageDestination(root, ".\\reshade-shaders\\Shaders\\OtisFX", "Shaders", "Test") == effects / "Shaders" / "OtisFX" &&
                    PackageDestination(root, ".\\reshade-shaders\\Textures", "Textures", "Test") == effects / "Textures",
                "reads install folders");
    for (const char* relative : { "", ".\\reshade-shaders\\Textures\\OtisFX", ".\\reshade-shaders\\Shaders\\..\\..\\OtisFX",
                                  "..\\reshade-shaders\\Shaders", "C:\\Windows", "/etc" })
        ok &= Check(Throws([&] { PackageDestination(root, relative, "Shaders", "Test"); }), "rejects install folders elsewhere");

    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::printf(ok ? "All tests passed.\n" : "Some tests failed.\n");
    return ok ? 0 : 1;
}
