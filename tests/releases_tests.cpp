// Tests for picking an update out of GitHub's list of releases: which release each kind of build is offered, and
// which ones never count.

#include "net/releases.h"

#include <cstdio>
#include <string>

namespace
{
bool Check(bool condition, const std::string& what)
{
    if (!condition)
        std::printf("Failed: %s\n", what.c_str());
    return condition;
}

// A release as GitHub lists it, with what the update check reads and some of what it must not trip over.
std::string Release(const std::string& tag, const std::string& flags, const std::string& asset)
{
    return R"({"url":"https://api.github.com/repos/OMouta/Unishade/releases/1","tag_name":")" + tag + R"(","name":")" + tag + R"(",)" + flags +
           R"(,"author":{"login":"OMouta"},"assets":[{"name":"SHA256SUMS.txt","size":1},{"name":")" + asset + R"(","size":2}],"body":"\"tag_name\" in text"},)";
}

std::string Offered(const std::string& json, const char* current)
{
    return NewestRelease("[" + json + "]", ParseVersion(current).value());
}
} // namespace

int main()
{
    bool ok = true;
    const std::string shown = R"("draft":false,"prerelease":false)";
    const std::string beta = R"("draft":false,"prerelease":true)";

    ok &= Check(SetupName("v0.7.0") == "Unishade-Setup-0.7.0.exe" && SetupName("v0.7.0-beta.2") == "Unishade-Setup-0.7.0-beta.2.exe", "names a release's Setup");

    // 0.7.0 has betas, then 0.7.1 has its first.
    const std::string during = Release("v0.7.0-beta.2", beta, "Unishade-Setup-0.7.0-beta.2.exe") + Release("v0.7.0-beta.1", beta, "Unishade-Setup-0.7.0-beta.1.exe") +
                               Release("depth-assets", shown, "onnxruntime.dll") + Release("v0.6.3", shown, "Unishade-Setup-0.6.3.exe");
    const std::string after = Release("v0.7.1-beta.1", beta, "Unishade-Setup-0.7.1-beta.1.exe") + Release("v0.7.0", shown, "Unishade-Setup-0.7.0.exe") + during;

    ok &= Check(Offered(during, "0.6.2") == "v0.6.3" && Offered(during, "0.6.3").empty(), "a version is not offered betas");
    ok &= Check(Offered(during, "0.7.0-beta.1") == "v0.7.0-beta.2" && Offered(during, "0.7.0-beta.2").empty(), "a beta is offered the betas after it");
    ok &= Check(Offered(after, "0.6.3") == "v0.7.0" && Offered(after, "0.7.0").empty(), "a version is offered the version after it");
    ok &= Check(Offered(after, "0.7.0-beta.2") == "v0.7.1-beta.1" && Offered(after, "0.7.1-beta.1").empty(), "a beta is offered the newest release of either kind");
    ok &= Check(Offered(Release("v0.7.0", shown, "Unishade-Setup-0.7.0.exe") + during, "0.7.0-beta.2") == "v0.7.0", "a beta is offered the version it led to");

    // What never counts, for a build of either kind.
    for (const char* current : { "0.6.3", "0.7.0-beta.1" })
    {
        ok &= Check(Offered(Release("v0.8.0", R"("draft":true,"prerelease":false)", "Unishade-Setup-0.8.0.exe"), current).empty(), "skips a draft");
        ok &= Check(Offered(Release("v0.8.0", beta, "Unishade-Setup-0.8.0.exe"), current).empty(), "skips a version GitHub has as a prerelease");
        ok &= Check(Offered(Release("v0.8.0", R"("draft":false)", "Unishade-Setup-0.8.0.exe"), current).empty(), "takes a missing prerelease flag for set");
        ok &= Check(Offered(Release("v0.8.0", shown, "Unishade.exe"), current).empty(), "skips a release without Setup");
        ok &= Check(Offered(Release("v0.8.0", shown, "Unishade-Setup-0.9.0.exe"), current).empty(), "skips a release with another version's Setup");
        for (const char* tag : { "0.8.0", "v0.8", "v0.8.0/../..", "v0.8.0-rc.1", "v0.8.0?x", "V0.8.0", "depth-assets" })
            ok &= Check(Offered(Release(tag, shown, "Unishade-Setup-0.8.0.exe"), current).empty(), std::string("skips the tag ") + tag);
    }
    // A beta's tag makes it a beta, whatever its prerelease flag says.
    ok &= Check(Offered(Release("v0.8.0-beta.1", shown, "Unishade-Setup-0.8.0-beta.1.exe"), "0.7.0").empty() &&
                    Offered(Release("v0.8.0-beta.1", shown, "Unishade-Setup-0.8.0-beta.1.exe"), "0.7.0-beta.1") == "v0.8.0-beta.1",
                "goes by the tag for what is a beta");
    ok &= Check(Offered("", "0.7.0").empty() && NewestRelease("not json", ParseVersion("0.7.0").value()).empty(), "reads nothing from an answer without releases");

    // SHA256SUMS.txt, as sha256sum writes it.
    const std::string a(64, 'a'), b = "0123456789ABCDEF" + std::string(48, '0');
    const std::string sums = a + "  Unishade.exe\n" + b + " *Unishade-Setup-0.7.0.exe\r\n" + a + "  Unishade-Setup-0.7.0.exe.sig\n" + std::string(64, 'g') + "  Bad.exe\n" + a + "  Last.exe";
    ok &= Check(ListedSha256(sums, "Unishade.exe") == a && ListedSha256(sums, "Last.exe") == a, "finds a file's SHA-256");
    ok &= Check(ListedSha256(sums, "Unishade-Setup-0.7.0.exe") == "0123456789abcdef" + std::string(48, '0'), "reads binary mode, Windows line ends and capitals");
    ok &= Check(ListedSha256(sums, "Setup-0.7.0.exe").empty() && ListedSha256(sums, "Unishade-Setup-0.7.0").empty() && ListedSha256(sums, "Missing.exe").empty() &&
                    ListedSha256("", "Unishade.exe").empty(),
                "goes by the whole name");
    ok &= Check(ListedSha256(sums, "Bad.exe").empty() && ListedSha256(std::string(63, 'a') + "  Short.exe", "Short.exe").empty(), "takes nothing but a SHA-256");

    return ok ? 0 : 1;
}
