// Tests for the effect runtime's parts that need no graphics card: where uniform values go, the compile cache,
// preset definitions and the order techniques run in.

#include "effects.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

fx::Uniform MakeUniform(reshadefx::type::datatype base, unsigned rows, unsigned cols, unsigned arrayLength, uint32_t offset)
{
    fx::Uniform uniform;
    uniform.type = {};
    uniform.type.base = base;
    uniform.type.rows = rows;
    uniform.type.cols = cols;
    uniform.type.array_length = arrayLength;
    uniform.offset = offset;
    return uniform;
}

void Write(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary) << text;
}

fx::Technique MakeTechnique(const std::string& name, const std::string& label, size_t effect, size_t index)
{
    fx::Technique technique;
    technique.name = name;
    technique.label = label;
    technique.effect = effect;
    technique.index = index;
    return technique;
}

std::string Names(const std::vector<fx::Technique>& techniques)
{
    std::string names;
    for (const fx::Technique& technique : techniques)
        names += technique.name + " ";
    return names;
}
} // namespace

int main()
{
    bool ok = true;

    // Uniform values in ReShade's layout: array elements and matrix rows start on 16 bytes.
    const fx::Uniform scalar = MakeUniform(reshadefx::type::t_float, 1, 1, 0, 32);
    ok &= Check(fx::ComponentCount(scalar) == 1 && fx::ComponentOffset(scalar, 0) == 32, "a scalar is at its offset");
    const fx::Uniform vector = MakeUniform(reshadefx::type::t_float, 3, 1, 0, 16);
    ok &= Check(fx::ComponentCount(vector) == 3 && fx::ComponentOffset(vector, 2) == 24, "vector components are packed");
    const fx::Uniform array = MakeUniform(reshadefx::type::t_float, 2, 1, 3, 0);
    ok &= Check(fx::ComponentCount(array) == 6, "an array counts every element's components");
    ok &= Check(fx::ComponentOffset(array, 1) == 4 && fx::ComponentOffset(array, 2) == 16 && fx::ComponentOffset(array, 5) == 36,
                "array elements start on 16 bytes");
    const fx::Uniform matrix = MakeUniform(reshadefx::type::t_float, 3, 3, 0, 0);
    ok &= Check(fx::ComponentCount(matrix) == 9, "a 3x3 matrix has nine values");
    ok &= Check(fx::ComponentOffset(matrix, 2) == 8 && fx::ComponentOffset(matrix, 3) == 16 && fx::ComponentOffset(matrix, 8) == 40,
                "matrix rows start on 16 bytes");
    const fx::Uniform matrices = MakeUniform(reshadefx::type::t_float, 4, 4, 2, 64);
    ok &= Check(fx::ComponentCount(matrices) == 32 && fx::ComponentOffset(matrices, 16) == 128 && fx::ComponentOffset(matrices, 31) == 188,
                "arrays of matrices follow each other");

    // Effect files and preset sections compare without case.
    fx::PresetDefinitions definitions;
    definitions.effects["Curves.fx"] = { { "A", "1" } };
    ok &= Check(definitions.effects.count("CURVES.FX") == 1 && definitions.effects.count("curves.fx") == 1, "finds an effect's definitions in any case");
    ok &= Check(definitions.effects.count("Curve.fx") == 0, "other files have none");
    definitions.effects["CURVES.fx"] = { { "B", "1" } };
    ok &= Check(definitions.effects.size() == 1, "a section in another case is the same section");

    // The cache key covers the compiler's macros, the effect and every file it includes.
    fx::CompileOptions options;
    options.width = 1920;
    options.height = 1080;
    const fx::Definitions macros = fx::EffectMacros({ { "QUALITY", "2" } }, options);
    const uint64_t key = fx::CompileKey(macros, { "effect", "include" });
    ok &= Check(key == fx::CompileKey(fx::EffectMacros({ { "QUALITY", "2" } }, options), { "effect", "include" }), "the key is stable");
    ok &= Check(key != fx::CompileKey(macros, { "effect", "include changed" }), "an included file changes the key");
    ok &= Check(key != fx::CompileKey(macros, { "effect changed", "include" }), "the effect changes the key");
    ok &= Check(key != fx::CompileKey(macros, { "effec", "tinclude" }), "where one file ends matters");
    ok &= Check(key != fx::CompileKey(fx::EffectMacros({ { "QUALITY", "3" } }, options), { "effect", "include" }), "a definition changes the key");
    fx::CompileOptions wider = options;
    wider.width = 2560;
    ok &= Check(key != fx::CompileKey(fx::EffectMacros({ { "QUALITY", "2" } }, wider), { "effect", "include" }), "BUFFER_WIDTH changes the key");
    const fx::Definitions overridden = fx::EffectMacros({ { "BUFFER_WIDTH", "1" }, { "EMPTY", "" } }, options);
    ok &= Check(overridden.back() == std::make_pair(std::string("EMPTY"), std::string("1")), "an empty definition means 1");
    ok &= Check(std::find(overridden.begin(), overridden.end(), std::make_pair(std::string("BUFFER_WIDTH"), std::string("1920"))) <
                    std::find(overridden.begin(), overridden.end(), std::make_pair(std::string("BUFFER_WIDTH"), std::string("1"))),
                "ReShade's own macros come first, so they win");

    // Compiling through the cache: the second compile reads the first, and a changed include compiles again.
    const fs::path directory = fs::temp_directory_path() / ("unishade-effects-tests-" + std::to_string(getpid()));
    fs::create_directories(directory / "Shaders");
    Write(directory / "Shaders" / "Value.fxh", "#define VALUE 0.5\n");
    Write(directory / "Shaders" / "Test.fx",
          "#include \"Value.fxh\"\n"
          "#ifndef TEST_QUALITY\n"
          "#define TEST_QUALITY 2\n"
          "#endif\n"
          "uniform float Strength < ui_type = \"slider\"; ui_min = 0.0; ui_max = 1.0; > = VALUE;\n"
          "texture Target { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };\n"
          "void Vertex(uint id : SV_VertexID, out float4 position : SV_Position)\n"
          "{\n"
          "    const float2 uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);\n"
          "    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
          "}\n"
          "float4 Pixel(float4 position : SV_Position) : SV_Target { return Strength; }\n"
          "technique Test < ui_label = \"A test\"; > { pass { VertexShader = Vertex; PixelShader = Pixel; RenderTarget = Target; } }\n");
    options.includePaths = { directory / "Shaders" };
    options.cacheDirectory = directory / "cache";
    const fs::path effectPath = directory / "Shaders" / "Test.fx";
    fx::Effect first, second, defined, third, fourth, missing, stopped;
    ok &= Check(fx::CompileEffect(first, effectPath, {}, options) && first.compiled && !first.cached, "compiles an effect");
    ok &= Check(first.definitions == fx::Definitions{ { "TEST_QUALITY", "2" } }, "lists the definitions an effect checks, with its own value");
    ok &= Check(fx::CompileEffect(defined, effectPath, { { "TEST_QUALITY", "3" } }, options) &&
                    defined.definitions == fx::Definitions{ { "TEST_QUALITY", "3" } },
                "or with the value it was compiled with");
    ok &= Check(fx::CompileEffect(second, effectPath, {}, options) && second.compiled && second.cached, "reads it back from the cache");
    ok &= Check(second.spirv == first.spirv && second.definitions == first.definitions && second.module.techniques.size() == 1 &&
                    second.module.techniques[0].name == "Test" &&
                    second.module.textures.size() == first.module.textures.size() && second.module.textures[0].width == 1920 &&
                    second.uniforms.size() == 1 && second.uniforms[0].min == 0.0f && second.uniforms[0].max == 1.0f &&
                    second.uniformData.size() == first.uniformData.size() && second.module.uniforms[0].initializer_value.as_float[0] == 0.5f,
                "the cached effect is the compiled one");
    Write(directory / "Shaders" / "Value.fxh", "#define VALUE 0.25\n");
    ok &= Check(fx::CompileEffect(third, effectPath, {}, options) && third.compiled && !third.cached &&
                    third.module.uniforms[0].initializer_value.as_float[0] == 0.25f,
                "a changed include compiles again");
    options.width = 1280;
    ok &= Check(fx::CompileEffect(fourth, effectPath, {}, options) && !fourth.cached && fourth.module.textures[0].width == 1280,
                "another size compiles again");
    ok &= Check(!fx::CompileEffect(missing, directory / "Shaders" / "Missing.fx", {}, options), "a missing effect does not compile");
    // An entry in the other folder moves over rather than compiling again.
    fx::CompileOptions moved = options;
    moved.cacheDirectory = directory / "other";
    moved.otherCacheDirectory = options.cacheDirectory;
    fx::Effect fifth;
    ok &= Check(fx::CompileEffect(fifth, effectPath, {}, moved) && fifth.cached && fifth.module.textures[0].width == 1280,
                "takes an entry from the other folder");
    const auto entries = [](const fs::path& folder) {
        std::error_code ignored;
        return std::distance(fs::directory_iterator(folder, ignored), fs::directory_iterator());
    };
    ok &= Check(entries(moved.cacheDirectory) == 1 && entries(options.cacheDirectory) == 2, "and moves it there");
    options.cacheDirectory.clear();
    ok &= Check(!fx::CompileEffect(stopped, effectPath, {}, options, [] { return true; }) && !stopped.compiled, "a cancelled compile stops");
    std::error_code error;
    fs::remove_all(directory, error);

    // Techniques run as the preset sorts them, by key or by name. The rest go by label, each effect's together in
    // its file's order.
    std::vector<fx::Effect> effects(5);
    const char* files[] = { "a.fx", "b.fx", "c.fx", "d.fx", "e.fx" };
    for (size_t i = 0; i < effects.size(); ++i)
        effects[i].file = files[i];
    std::vector<fx::Technique> techniques = {
        MakeTechnique("A", "", 0, 0), MakeTechnique("C", "Zeta", 2, 0), MakeTechnique("E1", "Zed", 4, 0),
        MakeTechnique("E2", "Ann", 4, 1), MakeTechnique("D", "Alpha", 3, 0), MakeTechnique("B", "", 1, 0),
    };
    fx::OrderTechniques(techniques, effects, { "B@b.fx", "A" });
    const std::string order = Names(techniques);
    ok &= Check(order == "B A D E1 E2 C ", "orders techniques as the preset does, then by label");
    std::reverse(techniques.begin(), techniques.end());
    fx::OrderTechniques(techniques, effects, { "B@b.fx", "A" });
    ok &= Check(Names(techniques) == order, "the order does not depend on the order before");

    // A preset saved while effects load keeps the places of techniques whose effect has not arrived.
    const auto pending = [](const std::string& key) { return key.ends_with("@late.fx"); };
    ok &= Check(fx::MergeSorting({ "A@a.fx", "B@b.fx" }, { "A@a.fx", "L@late.fx", "B@b.fx", "Gone@gone.fx" }, pending) ==
                    std::vector<std::string>{ "A@a.fx", "L@late.fx", "B@b.fx" },
                "keeps pending techniques where they were");
    ok &= Check(fx::MergeSorting({ "B@b.fx", "A@a.fx", "C@c.fx" }, { "A@a.fx", "L@late.fx", "B@b.fx" }, pending) ==
                    std::vector<std::string>{ "B@b.fx", "A@a.fx", "L@late.fx", "C@c.fx" },
                "and the loaded ones in their own order");
    ok &= Check(fx::MergeSorting({ "A@a.fx" }, {}, pending) == std::vector<std::string>{ "A@a.fx" }, "with nothing before, the order is the loaded one");

    std::printf(ok ? "All effect tests passed.\n" : "Some effect tests failed.\n");
    return ok ? 0 : 1;
}
