# The macOS and Linux host. Included from the top-level CMakeLists.txt, which already fetched Dear ImGui's core
# files and miniz.
#
# Linux:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
#         Needs the Vulkan loader and headers and the X11 development packages GLFW builds against.
# macOS:  brew install molten-vk vulkan-headers, then the same commands. The build is Unishade.app, with
#         MoltenVK copied inside so it runs without Homebrew.

if(APPLE)
    # Objective-C first: the language enabled first owns the .m extension, and GLFW's .m files are not C++.
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")
endif()

set(POSIX_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ReShade's effect compiler, which turns .fx files into SPIR-V. The same version the Windows host runs.
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

# ReShade's compiler is built with MSVC. A few lines are changed for other systems, in copies under patched/, and
# configuring stops if a line is no longer there, so an update cannot drop a change unnoticed.
function(patch_source name)
    file(READ "${RESHADEFX_DIR}/${name}" source)
    math(EXPR last "${ARGC} - 1")
    foreach(index RANGE 1 ${last} 2)
        math(EXPR next "${index} + 1")
        set(old "${ARGV${index}}")
        set(new "${ARGV${next}}")
        string(FIND "${source}" "${old}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "ReShade's ${name} changed. Update its patch in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}.")
        endif()
        string(REPLACE "${old}" "${new}" source "${source}")
    endforeach()
    set(patched "${RESHADEFX_DIR}/patched/${name}")
    set(existing "")
    if(EXISTS "${patched}")
        file(READ "${patched}" existing)
    endif()
    if(NOT existing STREQUAL source)
        file(WRITE "${patched}" "${source}")
    endif()
endfunction()
# Effects written on Windows include files as ".\Folder\File.fxh". Outside Windows backslashes are not
# separators, so the preprocessor's path helper turns them into slashes.
patch_source(effect_preprocessor.cpp
    "\t#define u8path(p) path(p)\n"
    "\tstatic std::string posix_path(std::string p) { for (char &c : p) if (c == '\\\\') c = '/'; return p; }\n\t#define u8path(p) path(posix_path(p))\n")
# malloc.h is Windows' and glibc's home of alloca, alloca.h everyone's.
patch_source(effect_symbol_table.cpp "#include <malloc.h> // alloca" "#include <alloca.h> // alloca")
# Apple's C++ library has no std::from_chars for floats before macOS 15's.
patch_source(effect_lexer.cpp
    "#include <charconv> // std::from_chars" "#include <charconv> // std::from_chars\n#include <cstdlib> // std::strtof\n#include <string>"
    "std::from_chars(begin, end, tok.literal_as_float);" "tok.literal_as_float = std::strtof(std::string(begin, end).c_str(), nullptr);")

add_library(reshadefx STATIC
    "${RESHADEFX_DIR}/effect_codegen_spirv.cpp"
    "${RESHADEFX_DIR}/effect_expression.cpp"
    "${RESHADEFX_DIR}/patched/effect_lexer.cpp"
    "${RESHADEFX_DIR}/effect_parser_exp.cpp"
    "${RESHADEFX_DIR}/effect_parser_stmt.cpp"
    "${RESHADEFX_DIR}/patched/effect_preprocessor.cpp"
    "${RESHADEFX_DIR}/patched/effect_symbol_table.cpp")
target_include_directories(reshadefx PUBLIC "${RESHADEFX_DIR}" PRIVATE "${RESHADEFX_DIR}/spirv")
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
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glfw)
if(APPLE)
    get_target_property(GLFW_SOURCES glfw SOURCES)
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

add_library(posix_libraries STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
    "${MINIZ_DIR}/miniz.c"
    "${POSIX_DIR}/stb.cpp")
target_include_directories(posix_libraries PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${MINIZ_DIR}" "${STB_DIR}")
# Vulkan only: without GLFW_INCLUDE_NONE GLFW's header pulls in OpenGL's. Dear ImGui's 32-bit characters let emoji
# outside the first 65536 code points show.
target_compile_definitions(posix_libraries PUBLIC MINIZ_NO_TIME GLFW_INCLUDE_NONE IMGUI_USE_WCHAR32)
target_compile_options(posix_libraries PRIVATE -w)
target_link_libraries(posix_libraries PUBLIC glfw unishade_vulkan)

# The logo the launcher and the menu draw goes into the program as the bytes of its PNG, as the resource does on
# Windows. Written again only when the picture changes.
set(LOGO_PNG "${CMAKE_SOURCE_DIR}/assets/Unishade.png")
set(LOGO_SOURCE "${CMAKE_BINARY_DIR}/logo.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LOGO_PNG}")
file(READ "${LOGO_PNG}" logo_bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," logo_bytes "${logo_bytes}")
file(CONFIGURE OUTPUT "${LOGO_SOURCE}" CONTENT
    "extern const unsigned char kLogoPng[];\nextern const unsigned kLogoPngSize;\nconst unsigned char kLogoPng[] = {${logo_bytes}};\nconst unsigned kLogoPngSize = sizeof(kLogoPng);\n"
    @ONLY)

# Everything but main, so tests can link it.
set(POSIX_SOURCES
    "${LOGO_SOURCE}"
    "${POSIX_DIR}/app.cpp"
    "${POSIX_DIR}/config.cpp"
    "${POSIX_DIR}/effects.cpp"
    "${POSIX_DIR}/games.cpp"
    "${POSIX_DIR}/gpu.cpp"
    "${POSIX_DIR}/hotkeys.cpp"
    "${POSIX_DIR}/log.cpp"
    "${POSIX_DIR}/setup.cpp"
    "${POSIX_DIR}/ui.cpp"
    # The launcher every platform shares.
    "${POSIX_DIR}/../ui/kit.cpp"
    "${POSIX_DIR}/../ui/launcher_ui.cpp")
if(APPLE)
    list(APPEND POSIX_SOURCES "${POSIX_DIR}/platform_macos.mm")
else()
    list(APPEND POSIX_SOURCES "${POSIX_DIR}/platform_x11.cpp")
endif()

add_library(unishade_core STATIC ${POSIX_SOURCES})
target_include_directories(unishade_core PUBLIC "${POSIX_DIR}" "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_definitions(unishade_core PUBLIC UNISHADE_VERSION="${PROJECT_VERSION}")
target_compile_options(unishade_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_source_files_properties("${POSIX_DIR}/effects.cpp" PROPERTIES COMPILE_DEFINITIONS UNISHADE_COMPILER_ID="${RESHADEFX_ID}")
target_link_libraries(unishade_core PUBLIC reshadefx posix_libraries Threads::Threads)
if(APPLE)
    target_compile_options(unishade_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(unishade_core PUBLIC "-framework AppKit" "-framework Carbon" "-framework CoreGraphics"
        "-framework CoreMedia" "-framework CoreVideo" "-framework QuartzCore" "-framework ScreenCaptureKit" "-framework Metal"
        "-framework IOSurface" "-framework IOKit")
else()
    target_link_libraries(unishade_core PUBLIC X11::X11 X11::Xcomposite X11::Xext)
    # XRes tells which process owns a window even for sandboxed programs such as Flatpak apps. It is loaded at
    # run time when present, so only its header is needed here.
    # DRI3 shares the window's picture on the graphics card. Also loaded at run time when present.
    find_path(DRI3_INCLUDE_DIR xcb/dri3.h)
    find_path(XLIB_XCB_INCLUDE_DIR X11/Xlib-xcb.h)
    if(DRI3_INCLUDE_DIR AND XLIB_XCB_INCLUDE_DIR)
        target_compile_definitions(unishade_core PRIVATE UNISHADE_HAVE_DRI3)
    else()
        message(WARNING "Without libxcb-dri3 and libx11-xcb headers, frames are always copied through memory.")
    endif()
    if(X11_XRes_INCLUDE_PATH)
        target_compile_definitions(unishade_core PRIVATE UNISHADE_HAVE_XRES)
    endif()
    target_link_libraries(unishade_core PUBLIC ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    # Finder and the Dock show the logo from an icon file made of it, which Info.plist names.
    set(APP_ICON "${CMAKE_BINARY_DIR}/Unishade.icns")
    add_custom_command(OUTPUT "${APP_ICON}"
        COMMAND sips -s format icns "${LOGO_PNG}" --out "${APP_ICON}"
        DEPENDS "${LOGO_PNG}"
        VERBATIM)
    add_executable(Unishade MACOSX_BUNDLE "${POSIX_DIR}/main.cpp" "${APP_ICON}")
    set_source_files_properties("${APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(Unishade PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${POSIX_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH ON)
    # MoltenVK goes inside the app, which is then signed again since changing it breaks the signature.
    add_custom_command(TARGET Unishade POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DAPP=$<TARGET_BUNDLE_DIR:Unishade> -DEXE=$<TARGET_FILE:Unishade> -DMOLTENVK=${MOLTENVK_LIBRARY}
                -P "${POSIX_DIR}/bundle_macos.cmake"
        VERBATIM)
else()
    add_executable(unishade "${POSIX_DIR}/main.cpp")
    set_target_properties(unishade PROPERTIES OUTPUT_NAME unishade)
    # Run paths into the build folder are relative to the executable, so the folder can move. Effects and presets
    # are always in the data folder from config.h, never beside the executable.
    set_target_properties(unishade PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
endif()
if(APPLE)
    target_link_libraries(Unishade PRIVATE unishade_core)
    target_compile_options(Unishade PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
else()
    target_link_libraries(unishade PRIVATE unishade_core)
    target_compile_options(unishade PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(ini_text_tests "${CMAKE_SOURCE_DIR}/tests/ini_text_tests.cpp")
    # GNU mode defines unix as a macro, which the test uses as a name.
    set_target_properties(ini_text_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME ini_text_tests COMMAND ini_text_tests)

    add_executable(posix_tests "${CMAKE_SOURCE_DIR}/tests/posix_tests.cpp")
    target_link_libraries(posix_tests PRIVATE unishade_core)
    add_test(NAME posix_tests COMMAND posix_tests)

    add_executable(posix_effects_tests "${CMAKE_SOURCE_DIR}/tests/posix_effects_tests.cpp")
    target_link_libraries(posix_effects_tests PRIVATE unishade_core)
    target_compile_options(posix_effects_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME posix_effects_tests COMMAND posix_effects_tests)

    add_executable(names_tests "${CMAKE_SOURCE_DIR}/tests/names_tests.cpp")
    target_compile_options(names_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME names_tests COMMAND names_tests)

    add_executable(package_files_tests "${CMAKE_SOURCE_DIR}/tests/package_files_tests.cpp")
    target_link_libraries(package_files_tests PRIVATE posix_libraries)
    target_compile_options(package_files_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME package_files_tests COMMAND package_files_tests)
endif()
