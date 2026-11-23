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
    src/net/net.cpp
    # The launcher's look and its font, so Setup's window is drawn as Unishade's is.
    src/ui/kit.cpp
    "${FONTS_SOURCE}"
    installer/setup.rc)
# setup.rc embeds Unishade.exe, found through the include path.
target_include_directories(installer PRIVATE src "$<TARGET_FILE_DIR:Unishade>")
target_compile_definitions(installer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(installer PRIVATE UNISHADE_VERSION="${UNISHADE_VERSION}")
if(UNISHADE_BETA GREATER 0)
    target_compile_definitions(installer PRIVATE UNISHADE_BETA=${UNISHADE_BETA})
endif()
target_compile_definitions(installer PRIVATE UNISHADE_USER_AGENT="Unishade-Setup/${UNISHADE_VERSION}")
target_compile_definitions(installer PRIVATE UNISHADE_VERSION_RESOURCE=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
target_compile_options(installer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(installer PRIVATE setup_libraries d3d11 dxgi d3dcompiler dwmapi windowscodecs winhttp bcrypt imm32 delayimp)
# Setup's copy lives in the installation folder, where Unishade 0.6.3 and earlier had ReShade's dxgi.dll, and Windows
# looks for DLLs in the exe's folder first. Its imports load from System32 instead: the loader flag covers DLLs loaded
# at startup (Windows 10 1607 and later), and the DLLs that are not KnownDLLs are delay-loaded, so they load after
# wWinMain has limited the search to System32.
target_link_options(installer PRIVATE /DEPENDENTLOADFLAG:0x800 /DELAYLOAD:d3d11.dll /DELAYLOAD:d3dcompiler_47.dll
    /DELAYLOAD:dwmapi.dll /DELAYLOAD:winhttp.dll /DELAYLOAD:bcrypt.dll)
add_dependencies(installer Unishade)
# The generator expression keeps multi-config generators from adding a Release folder.
set_target_properties(installer PROPERTIES
    OUTPUT_NAME "Unishade-Setup-${UNISHADE_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/installer$<0:>")
