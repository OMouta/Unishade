cmake_minimum_required(VERSION 3.20)
if(WIN32)
    project(Unishade VERSION 0.6.2 LANGUAGES C CXX RC)
else()
    project(Unishade VERSION 0.6.2 LANGUAGES C CXX)
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
    target_compile_definitions(launcher_ui_tests PRIVATE IMGUI_USE_WCHAR32)
    if(WIN32)
        target_compile_definitions(launcher_ui_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME launcher_ui_tests COMMAND launcher_ui_tests)
endif()

# macOS and Linux have their own host in src/posix. ReShade itself only runs on Windows, so it uses ReShade's
# effect compiler with Vulkan instead.
if(NOT WIN32)
    include(src/posix/posix.cmake)
    return()
endif()

set(RESHADE_INCLUDE "${CMAKE_BINARY_DIR}/reshade/include")
set(RESHADE_HEADERS_URL "https://raw.githubusercontent.com/crosire/reshade/v6.8.0/include")
fetch_file("${RESHADE_HEADERS_URL}/reshade.hpp" cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c "${RESHADE_INCLUDE}/reshade.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api.hpp" 8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb "${RESHADE_INCLUDE}/reshade_api.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_device.hpp" a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0 "${RESHADE_INCLUDE}/reshade_api_device.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_format.hpp" 68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7 "${RESHADE_INCLUDE}/reshade_api_format.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_pipeline.hpp" 5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f "${RESHADE_INCLUDE}/reshade_api_pipeline.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_resource.hpp" 32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3 "${RESHADE_INCLUDE}/reshade_api_resource.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_events.hpp" 2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89 "${RESHADE_INCLUDE}/reshade_events.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_overlay.hpp" ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79 "${RESHADE_INCLUDE}/reshade_overlay.hpp")

set(ONNXRUNTIME_INCLUDE "${CMAKE_BINARY_DIR}/onnxruntime/include")
set(ONNXRUNTIME_HEADERS_URL "https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/session")
fetch_file("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_c_api.h" 9ed0d7054a4e74249467365b25b415d36f51a44a6349e2a994a1812e4723d1e2 "${ONNXRUNTIME_INCLUDE}/onnxruntime_c_api.h")
fetch_file("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_ep_c_api.h" e94ac3490697bdb107844fa9d403952ee7136134132a057eae5c1207b57ce352 "${ONNXRUNTIME_INCLUDE}/onnxruntime_ep_c_api.h")
fetch_file("https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/providers/dml/dml_provider_factory.h" 893ddb645545bddad8c3f24bb768e97651c9ce45ec22076b435439889e4228c8 "${ONNXRUNTIME_INCLUDE}/dml_provider_factory.h")

# The in-game menu draws through ReShade's own Dear ImGui, so it needs the header of the exact version ReShade
# exports its functions for. Only the header: the functions are ReShade's.
set(RESHADE_IMGUI_INCLUDE "${CMAKE_BINARY_DIR}/reshade-imgui")
fetch_file("https://raw.githubusercontent.com/ocornut/imgui/v1.92.5-docking/imgui.h" 07562842049eaca4e47c3fcdcc9d8eafd442695b9856d6f6639e92e1bb6c737e "${RESHADE_IMGUI_INCLUDE}/imgui.h")
fetch_file("https://raw.githubusercontent.com/ocornut/imgui/v1.92.5-docking/imconfig.h" 6e5687893594ebfaf8569280cfea83d025904a8a4e23bec073086f6c5bc8f7fb "${RESHADE_IMGUI_INCLUDE}/imconfig.h")

add_executable(Unishade WIN32
    src/Unishade.rc
    src/main.cpp
    src/addon.cpp
    src/capture.cpp
    src/config.cpp
    src/discord.cpp
    src/game_integration.cpp
    src/hdr.cpp
    src/launcher.cpp
    src/log.cpp
    src/menu.cpp
    src/depth/depth.cpp
    src/depth/depth_model.cpp
    src/overlay.cpp
    src/reshade_config.cpp
    src/setup_check.cpp
    src/update.cpp
    src/net.cpp)
target_include_directories(Unishade PRIVATE "${RESHADE_INCLUDE}" "${RESHADE_IMGUI_INCLUDE}" "${ONNXRUNTIME_INCLUDE}")
target_compile_definitions(Unishade PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(Unishade PRIVATE UNISHADE_VERSION="${PROJECT_VERSION}")
# src/net.cpp sends this for the update check.
target_compile_definitions(Unishade PRIVATE UNISHADE_USER_AGENT="Unishade/${PROJECT_VERSION}")
target_compile_definitions(Unishade PRIVATE UNISHADE_VERSION_RESOURCE=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
target_compile_options(Unishade PRIVATE /W4 /permissive- /EHsc /bigobj)
target_link_libraries(Unishade PRIVATE d3d11 d3d12 d3dcompiler dxgi dwmapi windowsapp gdi32 version winhttp bcrypt windowscodecs)

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

# The launcher's window, which Unishade.exe loads. It has a Dear ImGui of its own and draws on the CPU, since the
# host's ImGui and graphics card belong to ReShade. See src/ui/launcher_module.h.
set(UI_SOURCES src/ui/launcher_module.cpp src/ui/launcher_ui.cpp src/ui/kit.cpp src/ui/soft_renderer.cpp)
add_library(UnishadeUi SHARED
    ${UI_SOURCES}
    src/ui/UnishadeUi.rc
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")
target_include_directories(UnishadeUi PRIVATE src "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
# 32-bit characters, so emoji outside the first 65536 code points show too.
target_compile_definitions(UnishadeUi PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX UNISHADE_UI_EXPORTS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD IMGUI_USE_WCHAR32)
target_compile_definitions(UnishadeUi PRIVATE UNISHADE_VERSION="${PROJECT_VERSION}")
target_compile_definitions(UnishadeUi PRIVATE UNISHADE_VERSION_RESOURCE=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
target_compile_options(UnishadeUi PRIVATE /EHsc)
set_source_files_properties(${UI_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4;/permissive-")
target_link_libraries(UnishadeUi PRIVATE dwmapi imm32)
add_dependencies(Unishade UnishadeUi)

# Built on request, like the Inno Setup installer it replaces: cmake --build build --config Release --target installer
add_executable(installer WIN32 EXCLUDE_FROM_ALL
    installer/main.cpp
    installer/install.cpp
    src/net.cpp
    installer/setup.rc)
# setup.rc embeds Unishade.exe and UnishadeUi.dll, found through the include path.
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
add_dependencies(installer Unishade UnishadeUi)
# The generator expression keeps multi-config generators from adding a Release folder.
set_target_properties(installer PROPERTIES
    OUTPUT_NAME "Unishade-Setup-${PROJECT_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/installer$<0:>")

if(BUILD_TESTING)
    add_executable(log_tests tests/log_tests.cpp src/log.cpp)
    target_compile_definitions(log_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX UNISHADE_VERSION="${PROJECT_VERSION}")
    target_compile_options(log_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME log_tests COMMAND log_tests)
    set_tests_properties(log_tests PROPERTIES TIMEOUT 45)

    add_executable(frame_statistics_tests tests/frame_statistics_tests.cpp)
    target_compile_options(frame_statistics_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME frame_statistics_tests COMMAND frame_statistics_tests)

    add_executable(frame_limit_tests tests/frame_limit_tests.cpp)
    target_compile_options(frame_limit_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME frame_limit_tests COMMAND frame_limit_tests)

    add_executable(menu_layout_tests tests/menu_layout_tests.cpp)
    target_compile_options(menu_layout_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME menu_layout_tests COMMAND menu_layout_tests)

    add_executable(hotkey_tests tests/hotkey_tests.cpp)
    target_compile_definitions(hotkey_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(hotkey_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME hotkey_tests COMMAND hotkey_tests)

    add_executable(ini_text_tests tests/ini_text_tests.cpp)
    target_compile_options(ini_text_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME ini_text_tests COMMAND ini_text_tests)

    add_executable(names_tests tests/names_tests.cpp)
    target_compile_options(names_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME names_tests COMMAND names_tests)

    add_executable(discord_rpc_tests tests/discord_rpc_tests.cpp)
    target_compile_options(discord_rpc_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME discord_rpc_tests COMMAND discord_rpc_tests)

    # miniz comes with Setup's other libraries.
    add_executable(package_files_tests tests/package_files_tests.cpp)
    target_compile_options(package_files_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(package_files_tests PRIVATE setup_libraries)
    add_test(NAME package_files_tests COMMAND package_files_tests)

    add_executable(game_window_tests tests/game_window_tests.cpp src/game_integration.cpp)
    target_compile_definitions(game_window_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(game_window_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(game_window_tests PRIVATE dwmapi)
    add_test(NAME game_window_tests COMMAND game_window_tests)
    set_tests_properties(game_window_tests PROPERTIES TIMEOUT 30)
endif()
