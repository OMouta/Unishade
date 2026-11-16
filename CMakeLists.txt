cmake_minimum_required(VERSION 3.20)
# The Mac app runs on macOS 13 and later. project() reads the target when it first enables a language, so it is set
# before, where APPLE is not known yet. An empty value, as an older build folder may hold, counts as unset, and a value
# given on the command line or in MACOSX_DEPLOYMENT_TARGET wins.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "The oldest macOS the app runs on" FORCE)
endif()
if(WIN32)
    project(Unishade VERSION 0.6.3 LANGUAGES C CXX RC)
else()
    project(Unishade VERSION 0.6.3 LANGUAGES C CXX)
endif()

# Without a build type, single-config generators build without optimisation and with the asserts of the effect
# compiler, stb and Dear ImGui on. The build type only takes effect when the build files are generated, so it can be
# set after project(), where GENERATOR_IS_MULTI_CONFIG is known. Multi-config generators pick it per build instead.
get_property(UNISHADE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT UNISHADE_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Third-party files are downloaded once at configure time instead of being vendored. The download gets its final name
# only after its hash matched, since CMake keeps a file that failed the check.
function(fetch_file url hash destination)
    if(NOT EXISTS "${destination}")
        file(REMOVE "${destination}.part")
        file(DOWNLOAD "${url}" "${destination}.part" EXPECTED_HASH "SHA256=${hash}" STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${destination}.part")
            list(GET status 1 message)
            message(FATAL_ERROR "Could not download ${url}: ${message}")
        endif()
        file(RENAME "${destination}.part" "${destination}")
    endif()
endfunction()

# Setup and the macOS and Linux host draw their windows with Dear ImGui and unpack effect packages with miniz.
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

include(CTest)
if(BUILD_TESTING)
    add_executable(launcher_ui_tests tests/launcher_ui_tests.cpp src/ui/launcher_ui.cpp src/ui/kit.cpp
        "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp" "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp")
    target_include_directories(launcher_ui_tests PRIVATE src "${IMGUI_DIR}")
    # Dear ImGui's test engine hooks tell the tests where each control is.
    target_compile_definitions(launcher_ui_tests PRIVATE IMGUI_USE_WCHAR32 IMGUI_ENABLE_TEST_ENGINE)
    if(WIN32)
        target_compile_definitions(launcher_ui_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME launcher_ui_tests COMMAND launcher_ui_tests)
endif()

# The host, in src/posix for every system. It compiles effects with ReShade's effect compiler and runs them on Vulkan.
include(src/posix/posix.cmake)
if(NOT WIN32)
    return()
endif()

set(ONNXRUNTIME_INCLUDE "${CMAKE_BINARY_DIR}/onnxruntime/include")
set(ONNXRUNTIME_HEADERS_URL "https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/session")
fetch_file("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_c_api.h" 9ed0d7054a4e74249467365b25b415d36f51a44a6349e2a994a1812e4723d1e2 "${ONNXRUNTIME_INCLUDE}/onnxruntime_c_api.h")
fetch_file("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_ep_c_api.h" e94ac3490697bdb107844fa9d403952ee7136134132a057eae5c1207b57ce352 "${ONNXRUNTIME_INCLUDE}/onnxruntime_ep_c_api.h")
fetch_file("https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/providers/dml/dml_provider_factory.h" 893ddb645545bddad8c3f24bb768e97651c9ce45ec22076b435439889e4228c8 "${ONNXRUNTIME_INCLUDE}/dml_provider_factory.h")

# Depth estimation as an add-on of the Vulkan host, Depth.dll. Its model and ONNX Runtime go in Addons/Depth.
add_library(depth_addon SHARED src/depth/depth_addon.cpp src/depth/depth_model.cpp)
set_target_properties(depth_addon PROPERTIES OUTPUT_NAME Depth)
target_include_directories(depth_addon PRIVATE src "${ONNXRUNTIME_INCLUDE}")
target_compile_definitions(depth_addon PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(depth_addon PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(depth_addon PRIVATE d3d11 d3d12 d3dcompiler dxgi windowsapp)

# Setup's window uses Dear ImGui's Win32 and Direct3D 11 backends.
fetch_file("${IMGUI_URL}/backends/imgui_impl_win32.h" 2736595b8de1e56871661d5d9b96deec567d35521746fbe2ade8f0543acdfd2b "${IMGUI_DIR}/backends/imgui_impl_win32.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_win32.cpp" 6a602ce28470bb209636c40633156e63a530acd49f137819d837d80688d52df6 "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")
fetch_file("${IMGUI_URL}/backends/imgui_impl_dx11.h" fb8a314a6a2904dcc5bfeb938a169389e170f233ad367d33f1ce5c7bde586a7c "${IMGUI_DIR}/backends/imgui_impl_dx11.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_dx11.cpp" 749d12b01011f504284497db21cb52b3c6f73c59612e7115f46f41e0c4ea1c49 "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")

add_library(setup_libraries STATIC EXCLUDE_FROM_ALL
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${MINIZ_DIR}/miniz.c")
target_include_directories(setup_libraries PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${MINIZ_DIR}")
target_compile_definitions(setup_libraries PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME)
# Otherwise the backend loads XInput by its bare name, which could find a DLL next to Setup.
target_compile_definitions(setup_libraries PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Built on request, like the Inno Setup installer it replaces: cmake --build build --config Release --target installer
add_executable(installer WIN32 EXCLUDE_FROM_ALL
    installer/main.cpp
    installer/install.cpp
    src/net.cpp
    installer/setup.rc)
# setup.rc embeds Unishade.exe, found through the include path.
target_include_directories(installer PRIVATE "$<TARGET_FILE_DIR:Unishade>")
target_compile_definitions(installer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(installer PRIVATE UNISHADE_VERSION="${PROJECT_VERSION}")
target_compile_definitions(installer PRIVATE UNISHADE_USER_AGENT="Unishade-Setup/${PROJECT_VERSION}")
target_compile_definitions(installer PRIVATE UNISHADE_VERSION_RESOURCE=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
target_compile_options(installer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(installer PRIVATE setup_libraries d3d11 dxgi d3dcompiler dwmapi windowscodecs winhttp bcrypt imm32 delayimp)
# Setup's copy lives next to ReShade's dxgi.dll, and Windows looks for DLLs in the exe's folder first. Its imports
# load from System32 instead: the loader flag covers DLLs loaded at startup (Windows 10 1607 and later), and the DLLs
# that are not KnownDLLs are delay-loaded, so they load after wWinMain has limited the search to System32.
target_link_options(installer PRIVATE /DEPENDENTLOADFLAG:0x800 /DELAYLOAD:d3d11.dll /DELAYLOAD:d3dcompiler_47.dll
    /DELAYLOAD:dwmapi.dll /DELAYLOAD:winhttp.dll /DELAYLOAD:bcrypt.dll)
add_dependencies(installer Unishade)
# The generator expression keeps multi-config generators from adding a Release folder.
set_target_properties(installer PROPERTIES
    OUTPUT_NAME "Unishade-Setup-${PROJECT_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/installer$<0:>")

# DLSS5 calls NVIDIA's nvngx_dlssnr.dll directly. Unishade_nvngx.dll is also the DLSS5 add-on of the Vulkan host, which
# takes nvngx_dlssnr.dll from Addons/Unishade_nvngx. A program that runs an image through it is built on request:
# cmake --build build --config Release --target dlss5_check
set(NGX_INCLUDE "${CMAKE_BINARY_DIR}/ngx")
set(NGX_URL "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include")
fetch_file("${NGX_URL}/nvsdk_ngx_defs.h" ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e "${NGX_INCLUDE}/nvsdk_ngx_defs.h")
fetch_file("${NGX_URL}/nvsdk_ngx_params.h" 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc "${NGX_INCLUDE}/nvsdk_ngx_params.h")

# Its path has to contain nvngx.dll. See src/dlss5/dlss5.cpp.
add_library(Unishade_nvngx SHARED src/dlss5/dlss5.cpp src/dlss5/dlss5_addon.cpp)
target_include_directories(Unishade_nvngx PRIVATE src "${NGX_INCLUDE}")
target_compile_definitions(Unishade_nvngx PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX UNISHADE_DLSS5_EXPORTS)
target_compile_definitions(Unishade_nvngx PRIVATE UNISHADE_VERSION="${PROJECT_VERSION}")
target_compile_options(Unishade_nvngx PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(Unishade_nvngx PRIVATE d3d12 dxgi windowsapp)

set(STB_DIR "${CMAKE_BINARY_DIR}/stb")
set(STB_URL "https://raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c")
fetch_file("${STB_URL}/stb_image.h" 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 "${STB_DIR}/stb_image.h")
fetch_file("${STB_URL}/stb_image_write.h" cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05 "${STB_DIR}/stb_image_write.h")

add_executable(dlss5_check EXCLUDE_FROM_ALL src/dlss5/check.cpp)
target_include_directories(dlss5_check PRIVATE "${STB_DIR}")
target_compile_definitions(dlss5_check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(dlss5_check PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(dlss5_check PRIVATE d3d12 windowsapp)
add_dependencies(dlss5_check Unishade_nvngx)

if(BUILD_TESTING)
    add_executable(frame_limit_tests tests/frame_limit_tests.cpp)
    target_compile_options(frame_limit_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME frame_limit_tests COMMAND frame_limit_tests)

    add_executable(menu_layout_tests tests/menu_layout_tests.cpp)
    target_compile_options(menu_layout_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME menu_layout_tests COMMAND menu_layout_tests)

    add_executable(ini_text_tests tests/ini_text_tests.cpp)
    target_compile_options(ini_text_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME ini_text_tests COMMAND ini_text_tests)

    add_executable(preset_ini_tests tests/preset_ini_tests.cpp)
    target_compile_options(preset_ini_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME preset_ini_tests COMMAND preset_ini_tests)

    add_executable(names_tests tests/names_tests.cpp)
    target_compile_options(names_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME names_tests COMMAND names_tests)

    # Windows only, like the ReShade host whose settings it carries over.
    add_executable(old_settings_tests tests/old_settings_tests.cpp)
    target_compile_definitions(old_settings_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # The test's names outside ASCII are UTF-8 in the source.
    target_compile_options(old_settings_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME old_settings_tests COMMAND old_settings_tests)

    add_executable(discord_rpc_tests tests/discord_rpc_tests.cpp)
    target_compile_options(discord_rpc_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME discord_rpc_tests COMMAND discord_rpc_tests)

    # miniz comes with Setup's other libraries.
    add_executable(package_files_tests tests/package_files_tests.cpp)
    target_compile_options(package_files_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(package_files_tests PRIVATE setup_libraries)
    add_test(NAME package_files_tests COMMAND package_files_tests)
endif()
