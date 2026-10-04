#include "../src/json.h"
#include <cstdio>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

bool Fails(std::string_view text)
{
    try
    {
        json::Parse(text);
        return false;
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
}
} // namespace

int main()
{
    bool ok = true;

    // An answer like the presets API's list.
    const json::Value list = json::Parse(R"({"presets":[{"id":12,"name":"Crisp \"night\"","effects":["CAS.fx","FXAA.fx"],"dlss5":null,
        "needsDepth":true,"saves":3}],"next":null})");
    const json::Value& preset = list["presets"].items.at(0);
    ok &= Check(preset["id"].Integer() == 12, "reads a number");
    ok &= Check(preset["name"].String() == "Crisp \"night\"", "reads an escaped quote");
    ok &= Check(preset["effects"].items.size() == 2 && preset["effects"].items[1].String() == "FXAA.fx", "reads an array");
    ok &= Check(preset["dlss5"].IsNull() && list["next"].IsNull(), "reads null");
    ok &= Check(preset["needsDepth"].Bool(), "reads true");
    ok &= Check(preset["missing"].IsNull() && preset["missing"].String().empty(), "a missing member reads as null");

    ok &= Check(json::Parse(R"("Techniques=A@A.fx\n\n[A.fx]\r\nX=1")").String() == "Techniques=A@A.fx\n\n[A.fx]\r\nX=1", "reads line breaks");
    ok &= Check(json::Parse(R"("caf\u00e9 \ud83d\ude00 \ud800")").String() == "caf\xC3\xA9 \xF0\x9F\x98\x80 \xEF\xBF\xBD",
                "reads escaped characters, pairs of halves, and a lone half");
    ok &= Check(json::Parse("-1.5e2").number == -150, "reads exponents");

    ok &= Check(Fails("{\"a\":1"), "an unclosed object fails");
    ok &= Check(Fails("[1,]"), "a trailing comma fails");
    ok &= Check(Fails("\"a\nb\""), "a raw line break in a string fails");
    ok &= Check(Fails("{} x"), "text after the value fails");
    ok &= Check(Fails(std::string(100, '[') + std::string(100, ']')), "deep nesting fails");

    ok &= Check(json::Quote("a\"b\\c\n") == R"("a\"b\\c\u000a")", "quotes text");

    return ok ? 0 : 1;
}
