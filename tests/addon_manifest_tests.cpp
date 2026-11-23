#include "../src/addon_manifest.h"
#include <cstdio>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

const char kManifest[] = "[Addon]\n"
                         "Id=someone.my-addon\n"
                         "Name = My add-on\n"
                         "Version=1.2.0\n"
                         "Author=Someone\n"
                         "Description=What it does.\n"
                         "Icon=art/icon.png\n"
                         "Website=https://example.com/my-addon\n"
                         "License=MIT\n"
                         "Api=1\n"
                         "\n"
                         "[windows-x64]\n"
                         "Library=MyAddon.dll\n"
                         "\n"
                         "[macos]\n"
                         "Library=MyAddon.dylib\n"
                         "\n"
                         "[macos-x64]\n"
                         "Library=intel/MyAddon.dylib\n";
} // namespace

int main()
{
    using namespace addon;
    bool ok = true;

    ok &= Check(ValidId("unishade.depth") && ValidId("a") && ValidId("3d-lut_2"), "takes lowercase letters, digits, dots, hyphens and underscores");
    ok &= Check(!ValidId("") && !ValidId("Depth") && !ValidId(".depth") && !ValidId("my addon") && !ValidId("a/b") && !ValidId(std::string(65, 'a')),
                "refuses anything else as an Id");
    ok &= Check(!ValidId("bloom.fx") && !ValidId("bloom.addonfx"), "an Id cannot be named as an effect's section in a preset is");

    ok &= Check(InsideFolder("a.dll") && InsideFolder("bin/a.dll"), "a library can be in a folder of the add-on's");
    ok &= Check(!InsideFolder("") && !InsideFolder("../a.dll") && !InsideFolder("/a.dll") && !InsideFolder("bin//a.dll") && !InsideFolder("bin/./a.dll") &&
                    !InsideFolder("C:/a.dll") && !InsideFolder("bin\\a.dll") && !InsideFolder("a.dll/"),
                "a path cannot leave the add-on's folder");

    Manifest manifest;
    ok &= Check(ReadManifest(kManifest, "windows-x64", manifest).empty(), "reads a manifest");
    ok &= Check(manifest.id == "someone.my-addon" && manifest.name == "My add-on" && manifest.version == "1.2.0" && manifest.author == "Someone" &&
                    manifest.description == "What it does." && manifest.icon == "art/icon.png" && manifest.website == "https://example.com/my-addon" &&
                    manifest.license == "MIT" && manifest.api == 1,
                "with everything it says");
    ok &= Check(manifest.library == "MyAddon.dll", "the library is the system's");
    ok &= Check(ReadManifest(kManifest, "macos-arm64", manifest).empty() && manifest.library == "MyAddon.dylib",
                "a section for the system alone is for every processor");
    ok &= Check(ReadManifest(kManifest, "macos-x64", manifest).empty() && manifest.library == "intel/MyAddon.dylib",
                "a section for the processor comes first");
    ok &= Check(ReadManifest(kManifest, "linux-x64", manifest).empty() && manifest.library.empty() && manifest.id == "someone.my-addon",
                "an add-on without a library for the system still says what it is");

    ok &= Check(ReadManifest("[Addon]\nId=mine\nApi=2\n[linux]\nLibrary=mine.so\n", "linux-x64", manifest).empty() && manifest.name == "mine" &&
                    manifest.api == 2 && manifest.library == "mine.so" && manifest.icon.empty(),
                "only Id, Api and the library are needed, and the name is then the Id");
    ok &= Check(!ReadManifest("[Addon]\nName=Mine\nApi=1\n", "linux-x64", manifest).empty(), "a manifest needs an Id");
    ok &= Check(!ReadManifest("[Addon]\nId=My Addon\nApi=1\n", "linux-x64", manifest).empty(), "and one that is valid");
    ok &= Check(!ReadManifest("[Addon]\nId=mine\n", "linux-x64", manifest).empty(), "and the version of the interface");
    ok &= Check(!ReadManifest("[Addon]\nId=mine\nApi=1\n[linux]\nLibrary=../../evil.so\n", "linux-x64", manifest).empty(),
                "a library outside the folder is refused");
    ok &= Check(ReadManifest("[Addon]\nId=mine\nApi=1\nIcon=../icon.png\nWebsite=file:///etc/passwd\n", "linux-x64", manifest).empty() &&
                    manifest.icon.empty() && manifest.website.empty(),
                "an icon outside the folder and a website that is not one are left out");
    ok &= Check(ReadManifest("[Addon]\nId=mine\nApi=1\nName=" + std::string(63, 'a') + "\xC3\xA9" + "b\n", "linux-x64", manifest).empty() &&
                    manifest.name == std::string(63, 'a'),
                "a long name is cut between characters");

    const std::string hash(64, 'a');
    const std::string list = "; comment\n"
                             "[unishade.depth windows-x64]\n"
                             "Depth.dll=" + std::string(64, 'A') + " https://example.com/Depth.dll\n"
                             "models/depth.onnx=" + hash + " https://example.com/depth.onnx\n"
                             "\n"
                             "[unishade.depth macos-arm64]\n"
                             "Depth.dylib=" + hash + " https://example.com/Depth.dylib\n"
                             "\n"
                             "[unishade.dlss5 windows-x64]\n"
                             "Unishade_nvngx.dll=@Unishade_nvngx.dll@ https://example.com/Unishade_nvngx.dll\n"
                             "\n"
                             "[unishade.plain windows-x64]\n"
                             "Plain.dll=" + hash + " http://example.com/Plain.dll\n"
                             "\n"
                             "[unishade.out windows-x64]\n"
                             "../Out.dll=" + hash + " https://example.com/Out.dll\n";
    const std::vector<Files> windows = ReadFiles(list, "windows-x64");
    ok &= Check(windows.size() == 1 && windows[0].id == "unishade.depth" && windows[0].files.size() == 2,
                "lists the add-ons for the system whose files all have a SHA-256, a secure address and a place in the folder");
    ok &= Check(windows.size() == 1 && windows[0].files[0].path == "Depth.dll" && windows[0].files[0].sha256 == hash &&
                    windows[0].files[0].url == "https://example.com/Depth.dll" && windows[0].files[1].path == "models/depth.onnx",
                "with each file's path, SHA-256 in lowercase and address");
    const std::vector<Files> mac = ReadFiles(list, "macos-arm64");
    ok &= Check(mac.size() == 1 && mac[0].files.size() == 1 && mac[0].files[0].path == "Depth.dylib", "each system has its own files");
    ok &= Check(ReadFiles(list, "linux-x64").empty(), "a system without add-ons has none");

    return ok ? 0 : 1;
}
