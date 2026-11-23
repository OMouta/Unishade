# Unishade and Setup draw their windows with Dear ImGui and unpack effect packages with miniz.
set(IMGUI_DIR "${CMAKE_BINARY_DIR}/imgui")
set(IMGUI_URL "https://raw.githubusercontent.com/ocornut/imgui/v1.92.9b")
fetch_file("${IMGUI_URL}/imgui.h" 0d8db1045db01d908853adfd26ae07c5bc5ab4789d4515f6ea34234a69ade0ca "${IMGUI_DIR}/imgui.h")
fetch_file("${IMGUI_URL}/imgui.cpp" 5a1e5128c0305f50b6556a77f71fde35836505883b74c1d6ae44723243f1e8de "${IMGUI_DIR}/imgui.cpp")
fetch_file("${IMGUI_URL}/imgui_internal.h" efba9bccc971cc49ec3da3168968ba412c7ea89745c104a7f5c004c8349bc71f "${IMGUI_DIR}/imgui_internal.h")
fetch_file("${IMGUI_URL}/imgui_draw.cpp" 83d30419a8e06a5f0a8692ee6de186f7ecfddecdc33769e76244837e2975b400 "${IMGUI_DIR}/imgui_draw.cpp")
fetch_file("${IMGUI_URL}/imgui_tables.cpp" b5deabe5b569ab712c11b6556562a646bf88327320fc60a451071b2a36498d72 "${IMGUI_DIR}/imgui_tables.cpp")
fetch_file("${IMGUI_URL}/imgui_widgets.cpp" a8a0b2f65caa5711467d9e3ca4028fded48329ea65e11ec92a968293dc2aa231 "${IMGUI_DIR}/imgui_widgets.cpp")
fetch_file("${IMGUI_URL}/imconfig.h" 5755e1b8d6ab0d7811a9d7cac0f509878b51fc5ee77e09bd0f235bcb414971e7 "${IMGUI_DIR}/imconfig.h")
fetch_file("${IMGUI_URL}/imstb_rectpack.h" 889b396795202d1457560a797a7242e96f6f132d4b88ca2d69be58bf05e1771f "${IMGUI_DIR}/imstb_rectpack.h")
fetch_file("${IMGUI_URL}/imstb_textedit.h" a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741 "${IMGUI_DIR}/imstb_textedit.h")
fetch_file("${IMGUI_URL}/imstb_truetype.h" c51a0f7e7ea760f2366bd3752635ec58e21fccfec4a832501639990ba6ce0528 "${IMGUI_DIR}/imstb_truetype.h")

set(MINIZ_DIR "${CMAKE_BINARY_DIR}/miniz")
if(NOT EXISTS "${MINIZ_DIR}/miniz.c")
    fetch_file("https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip"
        f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a "${CMAKE_BINARY_DIR}/miniz-3.1.2.zip")
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/miniz-3.1.2.zip" DESTINATION "${MINIZ_DIR}" PATTERNS miniz.c miniz.h)
endif()

# ReShade's effect compiler, which turns .fx files into SPIR-V.
set(RESHADEFX_DIR "${CMAKE_BINARY_DIR}/reshadefx")
set(RESHADEFX_URL "https://raw.githubusercontent.com/crosire/reshade/v6.8.0/source")
fetch_file("${RESHADEFX_URL}/effect_codegen.hpp" 5eab66de38d61b8cf8ef036085e2914a0d20c81691bd9691229d95532e3bc532 "${RESHADEFX_DIR}/effect_codegen.hpp")
fetch_file("${RESHADEFX_URL}/effect_codegen_spirv.cpp" ac90b11eb9d3c917aa5ee4b6129b141e624c255e7fdecc0251c176db4e9165eb "${RESHADEFX_DIR}/effect_codegen_spirv.cpp")
fetch_file("${RESHADEFX_URL}/effect_expression.cpp" e41ac27190d9eb30e2227f5ae2d205e6b32708e08d0044e44937e4c757435662 "${RESHADEFX_DIR}/effect_expression.cpp")
fetch_file("${RESHADEFX_URL}/effect_expression.hpp" 7f652ce4af688ef7c1a16bfda251a855ad8c0b9dbea5e85047a28bedf50d2c34 "${RESHADEFX_DIR}/effect_expression.hpp")
fetch_file("${RESHADEFX_URL}/effect_lexer.cpp" 03612358eb582d77b185bb3c5379e9cb54deaf076a6d0ec6d797d2b018b9427b "${RESHADEFX_DIR}/effect_lexer.cpp")
fetch_file("${RESHADEFX_URL}/effect_lexer.hpp" 80d464c306678c8029c4185a593f2d7d6261f135dbb5f892554b29625e896f10 "${RESHADEFX_DIR}/effect_lexer.hpp")
fetch_file("${RESHADEFX_URL}/effect_module.hpp" 383e91286d8351ebb62ed7b500aced349fbbfba205cb65917473842ced6a01c7 "${RESHADEFX_DIR}/effect_module.hpp")
fetch_file("${RESHADEFX_URL}/effect_parser.hpp" c708941973667946c89de3e0f4f257badfc1e79f297edc5494de88b2729cabcd "${RESHADEFX_DIR}/effect_parser.hpp")
fetch_file("${RESHADEFX_URL}/effect_parser_exp.cpp" 8bea9ec2b0abb1982fe561cadae3b8c04e5d46fe8712684144301fc74206709c "${RESHADEFX_DIR}/effect_parser_exp.cpp")
fetch_file("${RESHADEFX_URL}/effect_parser_stmt.cpp" 5ebfa9ed1784ad6bd3f8eaeac88e0d6308d0777ba3ad3841e720a8f7fb86cfb3 "${RESHADEFX_DIR}/effect_parser_stmt.cpp")
fetch_file("${RESHADEFX_URL}/effect_preprocessor.cpp" 9137b441bd7b111fa6067f0a2eef510193d2f45441e2588965ea09596565a863 "${RESHADEFX_DIR}/effect_preprocessor.cpp")
fetch_file("${RESHADEFX_URL}/effect_preprocessor.hpp" 265a25de71e6ed96db4fd1c8fc2831c588969dc5a57a8305a869b5dc85465357 "${RESHADEFX_DIR}/effect_preprocessor.hpp")
fetch_file("${RESHADEFX_URL}/effect_symbol_table.cpp" d94dfbef056dad76043288f2e8894efc9b9c3bce3010720ccf6dada558442a49 "${RESHADEFX_DIR}/effect_symbol_table.cpp")
fetch_file("${RESHADEFX_URL}/effect_symbol_table.hpp" ab1e4eaed12469b2aa3669375044f82c16efc5257b1e27d03b3796eb915e94ce "${RESHADEFX_DIR}/effect_symbol_table.hpp")
fetch_file("${RESHADEFX_URL}/effect_symbol_table_intrinsics.inl" 5e49a5e74f0b9ee68ccb1d0dc92ba8d330bb378946e46cb3c22a7d83755846b3 "${RESHADEFX_DIR}/effect_symbol_table_intrinsics.inl")
fetch_file("${RESHADEFX_URL}/effect_token.hpp" 2635a1f013eda6a49cb5e87a28a3dad1f0ee1870ef6645a59c4c69ecbb028666 "${RESHADEFX_DIR}/effect_token.hpp")
# The SPIR-V headers commit ReShade v6.8.0 pins as a submodule.
set(SPIRV_URL "https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/7845730cab6ebbdeb621e7349b7dc1a59c3377be/include/spirv/unified1")
fetch_file("${SPIRV_URL}/spirv.hpp" 43f4dcb231a8d61da50043f46e3aa04fa8b7c0ea53bb8b66c5ce70ac4ada51fe "${RESHADEFX_DIR}/spirv/spirv.hpp")
fetch_file("${SPIRV_URL}/GLSL.std.450.h" 20f32378793c5f416bc0704f44345c2a14c99cba3f411e3beaf1bcea372d58ba "${RESHADEFX_DIR}/spirv/GLSL.std.450.h")

# ReShade's compiler is built with MSVC, so a few of its lines change for other systems. Effects written on Windows
# include files as ".\Folder\File.fxh". Outside Windows backslashes are not separators, so the preprocessor's path
# helper turns them into slashes.
if(WIN32)
    set(RESHADEFX_PATCHED "${RESHADEFX_DIR}")
else()
    patch_source("${RESHADEFX_DIR}" effect_preprocessor.cpp
        "\t#define u8path(p) path(p)\n"
        "\tstatic std::string posix_path(std::string p) { for (char &c : p) if (c == '\\\\') c = '/'; return p; }\n\t#define u8path(p) path(posix_path(p))\n")
    # malloc.h is Windows' and glibc's home of alloca, alloca.h everyone's.
    patch_source("${RESHADEFX_DIR}" effect_symbol_table.cpp "#include <malloc.h> // alloca" "#include <alloca.h> // alloca")
    # Apple's C++ library has no std::from_chars for floats before macOS 15's.
    patch_source("${RESHADEFX_DIR}" effect_lexer.cpp
        "#include <charconv> // std::from_chars" "#include <charconv> // std::from_chars\n#include <cstdlib> // std::strtof\n#include <string>"
        "std::from_chars(begin, end, tok.literal_as_float);" "tok.literal_as_float = std::strtof(std::string(begin, end).c_str(), nullptr);")
    set(RESHADEFX_PATCHED "${RESHADEFX_DIR}/patched")
endif()

add_library(reshadefx STATIC
    "${RESHADEFX_DIR}/effect_codegen_spirv.cpp"
    "${RESHADEFX_DIR}/effect_expression.cpp"
    "${RESHADEFX_PATCHED}/effect_lexer.cpp"
    "${RESHADEFX_DIR}/effect_parser_exp.cpp"
    "${RESHADEFX_DIR}/effect_parser_stmt.cpp"
    "${RESHADEFX_PATCHED}/effect_preprocessor.cpp"
    "${RESHADEFX_PATCHED}/effect_symbol_table.cpp")
target_include_directories(reshadefx PUBLIC "${RESHADEFX_DIR}" PRIVATE "${RESHADEFX_DIR}/spirv")
# On Windows the preprocessor takes the UTF-8 strings of paths from u8string, which returns char8_t from C++20 on.
# ReShade builds it as C++17.
if(WIN32)
    set_target_properties(reshadefx PROPERTIES CXX_STANDARD 17)
endif()
# Third-party code: its warnings are not ours to fix.
target_compile_options(reshadefx PRIVATE -w)
# Compiled effects are cached for the compiler that made them, so a hash of its files names it.
get_target_property(RESHADEFX_SOURCES reshadefx SOURCES)
file(GLOB RESHADEFX_HEADERS "${RESHADEFX_DIR}/*.hpp" "${RESHADEFX_DIR}/*.inl" "${RESHADEFX_DIR}/spirv/*")
set(RESHADEFX_ID "")
foreach(source IN LISTS RESHADEFX_SOURCES RESHADEFX_HEADERS)
    file(SHA256 "${source}" hash)
    string(APPEND RESHADEFX_ID "${hash}")
endforeach()
string(SHA256 RESHADEFX_ID "${RESHADEFX_ID}")

# Dear ImGui's GLFW and Vulkan backends, from the same version as the core.
fetch_file("${IMGUI_URL}/backends/imgui_impl_glfw.h" ec95fe696c025dbf5fe6d24b958d7531d8cfbac0cb306a5435899d3526f64c4e "${IMGUI_DIR}/backends/imgui_impl_glfw.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_glfw.cpp" 41b11f71c17e05a748d4eaf8905a5fe6924d3ac42f69230feec4e585fd84900f "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp")
fetch_file("${IMGUI_URL}/backends/imgui_impl_vulkan.h" 351aa104d643f8a575b891d99645389d45d321bcbd33d41270d2ebf0276351f3 "${IMGUI_DIR}/backends/imgui_impl_vulkan.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_vulkan.cpp" 659590d8b74bcbd2ba6612d84a6d8871f97314f04da06a8a05508244adabf086 "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")

# stb_image loads the textures effects bring along and stb_image_resize2 sizes them, stb_image_write saves
# screenshots. ReShade's own addition to stb_image reads DDS files.
set(STB_DIR "${CMAKE_BINARY_DIR}/stb")
set(STB_URL "https://raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c")
fetch_file("${STB_URL}/stb_image.h" 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 "${STB_DIR}/stb_image.h")
fetch_file("${STB_URL}/stb_image_write.h" cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05 "${STB_DIR}/stb_image_write.h")
fetch_file("${STB_URL}/stb_image_resize2.h" af5fbe1ed423c44cec8155bd0dfea2b5ae6191912790b3a95e15b0b4f610d4df "${STB_DIR}/stb_image_resize2.h")
fetch_file("https://raw.githubusercontent.com/crosire/reshade/v6.8.0/deps/stb_image/stb_image_dds.h"
    6740af5e5d6e6bda48ad25b7f5fd990e5cd2f2c0442f9ba060700171f76a2ea0 "${STB_DIR}/stb_image_dds.h")

# GLFW 3.4 is the first version that can pass mouse input through a window, which the overlay needs. Linux
# builds only the X11 backend: Wayland does not let a window place itself over another program's window, while
# X11 programs, including those running through XWayland, can.
include(FetchContent)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
# CMake before 3.24, as Ubuntu 22.04 packages, does not know DOWNLOAD_EXTRACT_TIMESTAMP and reads it as part of the hash.
set(GLFW_EXTRACT_TIMESTAMP "")
if(NOT CMAKE_VERSION VERSION_LESS 3.24)
    set(GLFW_EXTRACT_TIMESTAMP DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    ${GLFW_EXTRACT_TIMESTAMP})
FetchContent_MakeAvailable(glfw)
if(APPLE)
    # The overlay has to take the keyboard for the menu without making Unishade the active program, which would take
    # the user off a game's full-screen Space. Only a panel made non-activating when it is created can, and GLFW
    # creates its windows itself, so glfwCocoaPanelHint makes the next window it creates such a panel.
    patch_source("${glfw_SOURCE_DIR}/src" cocoa_window.m
        "@interface GLFWWindow : NSWindow {}\n@end\n"
        "@interface GLFWWindow : NSWindow {}\n@end\n\n@interface GLFWPanel : NSPanel {}\n@end\n\n@implementation GLFWPanel\n\n- (BOOL)canBecomeKeyWindow\n{\n    return YES;\n}\n\n@end\n\nstatic GLFWbool panelHint = GLFW_FALSE;\n\nGLFWAPI void glfwCocoaPanelHint(int panel)\n{\n    panelHint = panel ? GLFW_TRUE : GLFW_FALSE;\n}\n"
        "    window->ns.object = [[GLFWWindow alloc]\n        initWithContentRect:contentRect\n                  styleMask:styleMask\n"
        "    const GLFWbool panel = panelHint;\n    panelHint = GLFW_FALSE;\n    window->ns.object = [[(panel ? [GLFWPanel class] : [GLFWWindow class]) alloc]\n        initWithContentRect:contentRect\n                  styleMask:styleMask | (panel ? NSWindowStyleMaskNonactivatingPanel : 0)\n")
    get_target_property(GLFW_SOURCES glfw SOURCES)
    list(TRANSFORM GLFW_SOURCES REPLACE "^(.*/)?cocoa_window\\.m$" "${glfw_SOURCE_DIR}/src/patched/cocoa_window.m")
    if(NOT "${glfw_SOURCE_DIR}/src/patched/cocoa_window.m" IN_LIST GLFW_SOURCES)
        message(FATAL_ERROR "GLFW no longer builds cocoa_window.m. Update its patch in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
    set_target_properties(glfw PROPERTIES SOURCES "${GLFW_SOURCES}")
    list(FILTER GLFW_SOURCES INCLUDE REGEX "\\.m$")
    set_source_files_properties(${GLFW_SOURCES} TARGET_DIRECTORY glfw PROPERTIES LANGUAGE OBJC)
endif()

# Vulkan: the system loader on Linux. On macOS MoltenVK is linked directly, so the app carries its own
# Vulkan implementation and needs no loader or driver manifest.
if(APPLE)
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/include" /opt/homebrew/include /usr/local/include REQUIRED)
    find_library(MOLTENVK_LIBRARY MoltenVK HINTS "$ENV{VULKAN_SDK}/lib" /opt/homebrew/lib /usr/local/lib REQUIRED)
    add_library(unishade_vulkan INTERFACE)
    target_include_directories(unishade_vulkan INTERFACE "${VULKAN_INCLUDE_DIR}")
    target_link_libraries(unishade_vulkan INTERFACE "${MOLTENVK_LIBRARY}")
elseif(WIN32)
    # Every graphics driver installs the loader, vulkan-1.dll. Linking it takes an import library, which is made from
    # the loader's own list of exports, so the build needs no Vulkan SDK.
    set(VULKAN_DIR "${CMAKE_BINARY_DIR}/vulkan-headers")
    set(VULKAN_URL "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/v1.4.366/include")
    fetch_file("${VULKAN_URL}/vulkan/vulkan.h" ace769f4cd4c69a846c1778be95bfcd326038c8adc7630584ba33bcadd2f70a5 "${VULKAN_DIR}/include/vulkan/vulkan.h")
    fetch_file("${VULKAN_URL}/vulkan/vk_platform.h" a2cd9085c66776845d2524c3b0e73ccf4827ba046aa6f676c4b8ddbed47d6552 "${VULKAN_DIR}/include/vulkan/vk_platform.h")
    fetch_file("${VULKAN_URL}/vulkan/vulkan_core.h" 272ced8df5a2b2218aa8d5461ab3ba9c04bd562088630070c2dac928b0ddd6c9 "${VULKAN_DIR}/include/vulkan/vulkan_core.h")
    fetch_file("${VULKAN_URL}/vulkan/vulkan_win32.h" 68ffd3b0481ea8be18b5d183634d97924617ac23f8c465095ef40e1de300d272 "${VULKAN_DIR}/include/vulkan/vulkan_win32.h")
    foreach(video
            vulkan_video_codecs_common.h:3fc937e75411bf82fd8dfaa721b07b2bb53307c891a347cc010d4747ce28f971
            vulkan_video_codec_h264std.h:072e863dd05e8151b9e0f5b18a4cb52acbe618079b4ace71e3b60cce1ec9ccbc
            vulkan_video_codec_h264std_decode.h:e401c6aea14f2e5d33cc8215d6a6725a9a6659e266a4f0b371c91d2a4cbde505
            vulkan_video_codec_h264std_encode.h:30ef5fc7be25aec0fbebde8c4908d631690f2063d33c10ccea9edaa2ed0d728a
            vulkan_video_codec_h265std.h:ba1668e412d3483d945fc3707e3b25b30c0c43ffd347d9b8b536a1cc622000a8
            vulkan_video_codec_h265std_decode.h:2da3a4c697f9b0b75a11181880e082ab13553ed5d915782c841e01be4793dfb0
            vulkan_video_codec_h265std_encode.h:bbc4a232434a2268c3689a676a488a13bf1912bfa4ff9aeb3a54077905316bd1
            vulkan_video_codec_av1std.h:83bc45ed585e664ccd57f112ad0eaf2a83c12276bf5a30081ae9e742b8ffeb6c
            vulkan_video_codec_av1std_decode.h:e37184b12c11fe5c4990133040158c0e5ecdbafc52285880276cddee6aecb8d5
            vulkan_video_codec_av1std_encode.h:c0388defe123f7e5fc02908d6ba868f58479a323da31d8e825b5fe2ac0523acb
            vulkan_video_codec_vp9std.h:1c33ed11eda1225ba52250221378930129b9c78c4aecc91bb368fa3d00f9cf36
            vulkan_video_codec_vp9std_decode.h:37d5b7847a9dda12480ea6b27dfedb4c509eb61b91090b5b119a12bbb5c5b190)
        string(REPLACE ":" ";" video "${video}")
        list(GET video 0 name)
        list(GET video 1 hash)
        fetch_file("${VULKAN_URL}/vk_video/${name}" ${hash} "${VULKAN_DIR}/include/vk_video/${name}")
    endforeach()
    fetch_file("https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/v1.4.366/loader/vulkan-1.def"
        9ba339b7f5ee2df28487698a6840ecf095fac58b415c2c89ad9f161e9d316378 "${VULKAN_DIR}/vulkan-1.def")
    set(VULKAN_LIBRARY "${VULKAN_DIR}/${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}/vulkan-1.lib")
    if(NOT EXISTS "${VULKAN_LIBRARY}")
        get_filename_component(library_dir "${VULKAN_LIBRARY}" DIRECTORY)
        file(MAKE_DIRECTORY "${library_dir}")
        execute_process(COMMAND "${CMAKE_AR}" /nologo "/def:${VULKAN_DIR}/vulkan-1.def" "/out:${VULKAN_LIBRARY}" "/machine:${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}"
            RESULT_VARIABLE result OUTPUT_QUIET)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Could not make vulkan-1.lib from the loader's exports.")
        endif()
    endif()
    add_library(unishade_vulkan INTERFACE)
    target_include_directories(unishade_vulkan INTERFACE "${VULKAN_DIR}/include")
    target_link_libraries(unishade_vulkan INTERFACE "${VULKAN_LIBRARY}")
else()
    find_package(Vulkan REQUIRED)
    find_package(X11 REQUIRED)
    if(NOT X11_Xcomposite_FOUND OR NOT X11_Xext_FOUND)
        message(FATAL_ERROR "Unishade needs the Xcomposite and Xext development files (libxcomposite-dev, libxext-dev).")
    endif()
    add_library(unishade_vulkan INTERFACE)
    target_link_libraries(unishade_vulkan INTERFACE Vulkan::Vulkan)
endif()
find_package(Threads REQUIRED)

# What Unishade is built on besides ReShade's compiler, and stb's code with it. Tests of files that need them link it.
add_library(unishade_libraries STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
    "${MINIZ_DIR}/miniz.c"
    "${CMAKE_SOURCE_DIR}/src/base/stb.cpp")
target_include_directories(unishade_libraries PUBLIC "${CMAKE_SOURCE_DIR}/src" "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${MINIZ_DIR}" "${STB_DIR}")
# Vulkan only: without GLFW_INCLUDE_NONE GLFW's header pulls in OpenGL's. Dear ImGui's 32-bit characters let emoji
# outside the first 65536 code points show.
target_compile_definitions(unishade_libraries PUBLIC MINIZ_NO_TIME GLFW_INCLUDE_NONE IMGUI_USE_WCHAR32)
target_compile_options(unishade_libraries PRIVATE -w)
target_link_libraries(unishade_libraries PUBLIC glfw unishade_vulkan)
