#pragma once

#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Installs the effect packages from ReShade's list, for Setup on Windows and for the macOS and Linux host: unpacks a
// package's zip, finds its effects and textures the way ReShade's own installer does, and copies them into
// reshade-shaders. Errors are thrown as std::runtime_error with a readable message, which each platform reports in
// its own way. Text is UTF-8, as zip files and EffectPackages.ini hold it.

namespace fs = std::filesystem;

// Limits for an effect package's zip, checked before anything is extracted. The largest packages on the list hold
// about 200 files and 45 MB.
constexpr mz_uint kZipEntries = 20000;
constexpr uint64_t kZipEntrySize = 128ull << 20;
constexpr uint64_t kZipTotalSize = 1ull << 30;

// What effect packages may put into reshade-shaders: effect sources, the textures ReShade loads, and plain-text
// licenses and readmes. Anything else, such as add-ons, libraries or programs, stays out.
inline constexpr const char* kPackageExtensions[] = { ".fx",  ".fxh", ".png", ".jpg",  ".jpeg", ".bmp",
                                                      ".tga", ".dds", ".hdr", ".cube", ".txt",  ".md" };
inline constexpr const char* kPackageTextNames[] = { "LICENSE", "LICENCE", "COPYING", "NOTICE", "README" };

// A path from UTF-8 text. Windows would otherwise read narrow text in its ANSI code page.
inline fs::path Utf8Path(std::string_view text)
{
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// A path as UTF-8 text, for messages.
inline std::string Utf8Text(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Names compared as Windows compares file names: ASCII letters without case.
inline bool SameFileName(const fs::path& a, const fs::path& b)
{
    using Char = fs::path::value_type;
    const auto lower = [](Char c) { return c >= 'A' && c <= 'Z' ? static_cast<Char>(c - 'A' + 'a') : c; };
    const fs::path::string_type& x = a.native();
    const fs::path::string_type& y = b.native();
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [&](Char c, Char d) { return lower(c) == lower(d); });
}

// Whether path is folder or inside it, after resolving . and .. in both. Windows ignores case in file names.
inline bool IsInside(const fs::path& path, const fs::path& folder)
{
    if (folder.empty())
        return false;
    const fs::path child = path.lexically_normal();
    auto part = child.begin();
    for (const fs::path& parent : folder.lexically_normal())
    {
        // A separator at the end of folder leaves an empty last part.
        if (parent.empty())
            continue;
#ifdef _WIN32
        const bool same = part != child.end() && SameFileName(*part, parent);
#else
        const bool same = part != child.end() && part->native() == parent.native();
#endif
        if (!same)
            return false;
        ++part;
    }
    return true;
}

inline bool AllowedPackageFile(const fs::path& name)
{
    const fs::path extension = name.extension();
    for (const char* allowed : kPackageExtensions)
        if (SameFileName(extension, allowed))
            return true;
    for (const char* text : kPackageTextNames)
        if (SameFileName(name, text))
            return true;
    return false;
}

// Extracts the files of the types effect packages may install. name is the package's, for messages.
inline void ExtractZip(std::string_view data, const fs::path& destination, const std::string& name)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
        throw std::runtime_error("The download of " + name + " is not a zip file.");
    struct Closer
    {
        mz_zip_archive* zip;
        ~Closer() { mz_zip_reader_end(zip); }
    } closer{ &zip };

    // The sizes in the zip's directory are what extracting allocates and writes, so they are checked first.
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kZipEntries)
        throw std::runtime_error(name + " contains more files than an effect package can.");
    uint64_t total = 0;
    for (mz_uint index = 0; index < count; ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat))
            throw std::runtime_error("Could not read " + name + ".");
        total += stat.m_uncomp_size;
        if (stat.m_uncomp_size > kZipEntrySize || total > kZipTotalSize)
            throw std::runtime_error(name + " unpacks to more data than an effect package can.");
    }

    for (mz_uint index = 0; index < count; ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_directory)
            continue;
        // Backslashes separate folders everywhere, as they do on Windows. A colon would name a drive, or an NTFS
        // stream, which writes into another file.
        std::string entry = stat.m_filename;
        std::replace(entry.begin(), entry.end(), '\\', '/');
        const fs::path target = (destination / Utf8Path(entry)).lexically_normal();
        if (entry.find(':') != std::string::npos || !IsInside(target, destination))
            throw std::runtime_error(name + " contains a file outside its own folder.");
        if (!AllowedPackageFile(target.filename()))
            continue;
        fs::create_directories(target.parent_path());
        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
        if (!bytes)
            throw std::runtime_error("Could not extract " + name + ".");
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        mz_free(bytes);
        file.close();
        if (!file)
            throw std::runtime_error("Could not write " + Utf8Text(target) + ".");
    }
}

// Where an unpacked package keeps its effects and its textures.
struct PackageFolders
{
    fs::path shaders;
    fs::path textures;
};

// Collects the first folders named Shaders and Textures, and the shallowest folders with effect or image files.
inline void SearchPackageFolders(const fs::path& directory, PackageFolders& named, PackageFolders& shallowest)
{
    for (const auto& entry : fs::directory_iterator(directory))
    {
        const fs::path& path = entry.path();
        if (entry.is_directory())
        {
            if (named.shaders.empty() && SameFileName(path.filename(), "Shaders"))
                named.shaders = path;
            if (named.textures.empty() && SameFileName(path.filename(), "Textures"))
                named.textures = path;
            SearchPackageFolders(path, named, shallowest);
            continue;
        }
        const fs::path extension = path.extension();
        const bool image = SameFileName(extension, ".png") || SameFileName(extension, ".jpg") || SameFileName(extension, ".jpeg");
        const auto shallower = [&](const fs::path& current) { return current.empty() || directory.native().size() < current.native().size(); };
        if (SameFileName(extension, ".fx") && shallower(shallowest.shaders))
            shallowest.shaders = directory;
        if (image && shallower(shallowest.textures))
            shallowest.textures = directory;
    }
}

// Matches ReShade's own installer: folders named Shaders and Textures first, otherwise the shallowest folder with
// effect or image files. Throws when the package has no effects; textures may be empty.
inline PackageFolders FindPackageFolders(const fs::path& extracted, const std::string& name)
{
    PackageFolders named, shallowest;
    SearchPackageFolders(extracted, named, shallowest);
    if (named.shaders.empty())
        named.shaders = shallowest.shaders;
    if (named.textures.empty())
        named.textures = shallowest.textures;
    if (named.shaders.empty())
        throw std::runtime_error("The effect package " + name + " contains no effects.");
    return named;
}

// Copies the files effect packages may install, folders included, except those named in denied, a comma-separated
// list such as DenyEffectFiles in EffectPackages.ini.
inline void CopyPackageFiles(const fs::path& source, const fs::path& destination, std::string_view denied)
{
    std::vector<fs::path> deniedNames;
    for (size_t start = 0; start < denied.size();)
    {
        const size_t end = std::min(denied.find(',', start), denied.size());
        std::string deniedName(denied.substr(start, end - start));
        deniedName.erase(0, deniedName.find_first_not_of(' '));
        deniedName.erase(deniedName.find_last_not_of(' ') + 1);
        if (!deniedName.empty())
            deniedNames.push_back(Utf8Path(deniedName));
        start = end + 1;
    }
    fs::create_directories(destination);
    for (const auto& entry : fs::directory_iterator(source))
    {
        const fs::path name = entry.path().filename();
        if (entry.is_directory())
        {
            CopyPackageFiles(entry.path(), destination / name, denied);
            continue;
        }
        const auto isName = [&](const fs::path& deniedName) { return SameFileName(deniedName, name); };
        const bool skip = !AllowedPackageFile(name) || std::any_of(deniedNames.begin(), deniedNames.end(), isName);
        if (!skip)
            fs::copy_file(entry.path(), destination / name, fs::copy_options::overwrite_existing);
    }
}

// The folder a package's effects or textures go to, from a path such as .\reshade-shaders\Shaders\OtisFX in
// EffectPackages.ini, relative to root, the folder holding reshade-shaders. It must be in reshade-shaders' Shaders
// or Textures folder, as kind says.
inline fs::path PackageDestination(const fs::path& root, std::string relative, const char* kind, const std::string& package)
{
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const fs::path destination = (root / Utf8Path(relative)).lexically_normal();
    if (relative.empty() || !IsInside(destination, root / "reshade-shaders" / kind))
        throw std::runtime_error("The effect package " + package + " has an invalid install folder.");
    return destination;
}
