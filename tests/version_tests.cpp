// Tests for reading and ordering Unishade's versions, which decide what the update check offers.

#include "net/version.h"

#include <cstdio>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

Version Read(const char* text)
{
    return ParseVersion(text).value();
}
} // namespace

int main()
{
    bool ok = true;

    ok &= Check(Read("0.7.0") == Version{ { 0, 7, 0 }, 0 } && Read("12.0.345") == Version{ { 12, 0, 345 }, 0 }, "reads a version");
    ok &= Check(Read("0.7.0-beta.2") == Version{ { 0, 7, 0 }, 2 } && Read("1.0.0-beta.10") == Version{ { 1, 0, 0 }, 10 }, "reads a beta");
    for (const char* text : { "", "0.7", "0.7.", "0.7.0.", "0.7.0.1", "v0.7.0", "0.7.0 ", " 0.7.0", "0..0", "a.b.c", "depth-assets", "0.7.0-beta", "0.7.0-beta.",
                              "0.7.0-beta.0", "0.7.0-beta.x", "0.7.0-beta.1.2", "0.7.0-rc.1", "0.7.0beta.1", "0.7.0-beta.1/..", "1234567890.0.0",
                              "0.7.0-beta.1234567890" })
        ok &= Check(!ParseVersion(text), text);

    ok &= Check(Read("0.7.0").Text() == "0.7.0" && Read("0.7.0-beta.2").Text() == "0.7.0-beta.2" && Read("007.01.0").Text() == "7.1.0",
                "writes a version as its tag has it");

    // The order the update check goes by.
    const char* const order[] = { "0.6.3", "0.7.0-beta.1", "0.7.0-beta.2", "0.7.0-beta.10", "0.7.0", "0.7.1-beta.1", "0.7.1", "0.10.0", "1.0.0-beta.1", "1.0.0" };
    for (size_t earlier = 0; earlier < std::size(order); ++earlier)
        for (size_t later = 0; later < std::size(order); ++later)
            ok &= Check((Read(order[earlier]) < Read(order[later])) == (earlier < later), order[earlier]);

    return ok ? 0 : 1;
}
