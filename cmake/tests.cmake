# The tests in tests/. Each file is a program of its own that includes Unishade's files the way Unishade does, by their
# path from src.
function(unishade_test name)
    add_executable(${name} "${CMAKE_SOURCE_DIR}/tests/${name}.cpp" ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# The launcher without a window, with its own build of Dear ImGui.
unishade_test(launcher_ui_tests src/ui/launcher_ui.cpp src/ui/tray.cpp src/ui/kit.cpp
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp" "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(launcher_ui_tests PRIVATE "${IMGUI_DIR}")
# Dear ImGui's test engine hooks tell the tests where each control is.
target_compile_definitions(launcher_ui_tests PRIVATE IMGUI_USE_WCHAR32 IMGUI_ENABLE_TEST_ENGINE)
if(WIN32)
    target_compile_definitions(launcher_ui_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Windows builds the tests of files that stand alone. The tests that link Unishade's own code are not built there yet.
if(WIN32)
    foreach(name frame_limit_tests frame_stats_tests menu_layout_tests ini_text_tests addon_manifest_tests addon_releases_tests
            preset_ini_tests names_tests game_scan_tests discord_rpc_tests version_tests releases_tests)
        unishade_test(${name})
        target_compile_options(${name} PRIVATE /W4 /permissive- /EHsc)
    endforeach()

    # Windows only, like the ReShade host whose settings it carries over.
    unishade_test(old_settings_tests)
    target_compile_definitions(old_settings_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # The test's names outside ASCII are UTF-8 in the source.
    target_compile_options(old_settings_tests PRIVATE /W4 /permissive- /EHsc /utf-8)

    # miniz comes with Setup's other libraries.
    unishade_test(package_files_tests)
    target_compile_options(package_files_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(package_files_tests PRIVATE setup_libraries)
    return()
endif()

unishade_test(ini_text_tests)
# GNU mode defines unix as a macro, which the test uses as a name.
set_target_properties(ini_text_tests PROPERTIES CXX_EXTENSIONS OFF)

unishade_test(addon_manifest_tests)
target_compile_options(addon_manifest_tests PRIVATE -Wall -Wextra)

unishade_test(addon_releases_tests)
target_compile_options(addon_releases_tests PRIVATE -Wall -Wextra)

unishade_test(preset_ini_tests)
target_compile_options(preset_ini_tests PRIVATE -Wall -Wextra)

unishade_test(names_tests)
target_compile_options(names_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

unishade_test(game_scan_tests)
target_compile_options(game_scan_tests PRIVATE -Wall -Wextra)

unishade_test(version_tests)
target_compile_options(version_tests PRIVATE -Wall -Wextra)

unishade_test(releases_tests)
target_compile_options(releases_tests PRIVATE -Wall -Wextra)

unishade_test(image_tests)
target_link_libraries(image_tests PRIVATE unishade_libraries)

unishade_test(package_files_tests)
target_link_libraries(package_files_tests PRIVATE unishade_libraries)
target_compile_options(package_files_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# Loads the example add-on through Unishade's add-on host, without a graphics card.
unishade_test(addons_tests)
target_link_libraries(addons_tests PRIVATE unishade_core)
target_compile_options(addons_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_compile_definitions(addons_tests PRIVATE EXAMPLE_ADDON="$<TARGET_FILE:example_addon>"
    EXAMPLE_MANIFEST="${CMAKE_SOURCE_DIR}/addons/example/addon.ini")
add_dependencies(addons_tests example_addon)

# Packs releases of add-ons with cmake/addon_release.cmake and has Unishade find them, without GitHub.
unishade_test(official_addons_tests)
target_link_libraries(official_addons_tests PRIVATE unishade_core)
target_compile_options(official_addons_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_compile_definitions(official_addons_tests PRIVATE CMAKE_COMMAND="${CMAKE_COMMAND}"
    PACK_SCRIPT="${CMAKE_SOURCE_DIR}/cmake/addon_release.cmake" ADDONS_FOLDER="${CMAKE_SOURCE_DIR}/addons")

# Settings, games, shortcuts and the rules for macOS windows.
unishade_test(host_tests)
target_link_libraries(host_tests PRIVATE unishade_core)

unishade_test(effects_tests)
target_link_libraries(effects_tests PRIVATE unishade_core)
target_compile_options(effects_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
