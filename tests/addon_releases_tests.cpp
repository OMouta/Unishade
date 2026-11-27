// Tests for finding the official add-ons' releases in GitHub's list of releases: which tags are an add-on's, and
// which releases each kind of build is offered.

#include "addons/addon_releases.h"

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

// A release as GitHub lists it, with what Unishade reads and some of what it must not trip over.
std::string Listed(const std::string& tag, const std::string& flags, const std::string& asset = "addon.ini")
{
    return R"({"url":"https://api.github.com/repos/OMouta/Unishade/releases/1","tag_name":")" + tag + R"(","name":")" + tag + R"(",)" + flags +
           R"(,"author":{"login":"OMouta"},"assets":[{"name":"SHA256SUMS.txt","size":1},{"name":")" + asset + R"(","size":2}],"body":"\"tag_name\" in text"},)";
}

// The tags a build is offered, in the list's order, separated by spaces.
std::string Offered(const std::string& json, bool beta)
{
    std::string tags;
    for (const addon::Release& release : addon::ReadReleases("[" + json + "]", beta).offered)
        tags += (tags.empty() ? "" : " ") + release.tag;
    return tags;
}
} // namespace

int main()
{
    using namespace addon;
    bool ok = true;

    const std::optional<addon::Release> version = ReadTag("unishade.depth-v1.2.0");
    ok &= Check(version && version->id == "unishade.depth" && version->version.Text() == "1.2.0" && version->tag == "unishade.depth-v1.2.0",
                "reads an add-on's Id and version out of its release's tag");
    const std::optional<addon::Release> beta = ReadTag("unishade.depth-v1.2.0-beta.3");
    ok &= Check(beta && beta->id == "unishade.depth" && beta->version.beta == 3, "and a beta's");
    const std::optional<addon::Release> hyphens = ReadTag("someone.my-very-own-v2-v0.1.0");
    ok &= Check(hyphens && hyphens->id == "someone.my-very-own-v2" && hyphens->version.Text() == "0.1.0", "an Id can have -v in it");
    for (const char* tag : { "v0.7.0", "v0.7.0-beta.2", "depth-assets", "dlss5-assets", "unishade.depth", "unishade.depth-v1.2", "unishade.depth-1.2.0",
                             "Unishade.Depth-v1.2.0", "unishade.depth-v1.2.0-rc.1", "unishade.depth-v1.2.0/..", "../x-v1.2.0", "-v1.2.0", "" })
        ok &= Check(!ReadTag(tag), std::string("the tag ") + tag + " is no add-on's release");
    ok &= Check(ReleaseTag("unishade.depth", ParseVersion("1.2.0-beta.3").value()) == "unishade.depth-v1.2.0-beta.3", "names an add-on's release");
    ok &= Check(ReleaseFile("unishade.depth-v1.2.0", "addon.ini") == "https://github.com/OMouta/Unishade/releases/download/unishade.depth-v1.2.0/addon.ini",
                "a release's files download from the release");

    const std::string shown = R"("draft":false,"prerelease":false)";
    const std::string early = R"("draft":false,"prerelease":true)";
    const std::string list = Listed("unishade.depth-v1.1.0-beta.1", early) + Listed("v0.7.0", shown, "Unishade-Setup-0.7.0.exe") +
                             Listed("unishade.dlss5-v1.0.0", shown) + Listed("unishade.depth-v1.0.0", shown) + Listed("depth-assets", shown, "onnxruntime.dll") +
                             Listed("unishade.depth-v1.0.0-beta.2", early);
    ok &= Check(Offered(list, false) == "unishade.dlss5-v1.0.0 unishade.depth-v1.0.0", "a version of Unishade is offered the add-ons' versions, not their betas");
    ok &= Check(Offered(list, true) == "unishade.depth-v1.1.0-beta.1 unishade.dlss5-v1.0.0 unishade.depth-v1.0.0 unishade.depth-v1.0.0-beta.2",
                "a beta of Unishade is offered both");
    ok &= Check(ReadReleases("[" + list + "]", false).listed == 6 && ReadReleases("[]", true).listed == 0,
                "counts every release of a page, which says whether another page follows");

    // What is never offered, to a build of either kind.
    for (const bool build : { false, true })
    {
        ok &= Check(Offered(Listed("unishade.depth-v2.0.0", R"("draft":true,"prerelease":false)"), build).empty(), "skips a draft");
        ok &= Check(Offered(Listed("unishade.depth-v2.0.0", early), build).empty(), "skips a version GitHub has as a prerelease");
        ok &= Check(Offered(Listed("unishade.depth-v2.0.0", R"("draft":false)"), build).empty(), "takes a missing prerelease flag for set");
        ok &= Check(Offered(Listed("unishade.depth-v2.0.0", shown, "Depth.dll"), build).empty(), "skips a release without an addon.ini");
    }
    ok &= Check(Offered(Listed("unishade.depth-v2.0.0-beta.1", shown), true) == "unishade.depth-v2.0.0-beta.1",
                "a beta is one by its version, whatever GitHub has it as");

    std::vector<addon::Release> releases = ReadReleases("[" + list + "]", true).offered;
    NewestFirst(releases);
    std::string order;
    for (const addon::Release& release : releases)
        order += release.tag + " ";
    ok &= Check(order == "unishade.depth-v1.1.0-beta.1 unishade.depth-v1.0.0 unishade.depth-v1.0.0-beta.2 unishade.dlss5-v1.0.0 ",
                "orders each add-on's releases from the newest, a version after its betas");

    return ok ? 0 : 1;
}
