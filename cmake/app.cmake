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

# The launcher's and the menu's font, Bricolage Grotesque, goes in the same way, so the menu over a game finds it too.
# Dear ImGui draws only the default instance of a variable font, so assets/fonts holds static instances of it.
set(FONTS_SOURCE "${CMAKE_BINARY_DIR}/fonts.cpp")
set(fonts_code "")
foreach(face Regular SemiBold ExtraBold)
    set(font "${CMAKE_SOURCE_DIR}/assets/fonts/BricolageGrotesque-${face}.ttf")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${font}")
    file(READ "${font}" font_bytes HEX)
    string(REGEX REPLACE "(..)" "0x\\1," font_bytes "${font_bytes}")
    string(APPEND fonts_code
        "extern const unsigned char kBricolage${face}[];\nextern const unsigned kBricolage${face}Size;\nconst unsigned char kBricolage${face}[] = {${font_bytes}};\nconst unsigned kBricolage${face}Size = sizeof(kBricolage${face});\n")
endforeach()
file(CONFIGURE OUTPUT "${FONTS_SOURCE}" CONTENT "${fonts_code}" @ONLY)

# AMD's FSR 1 upscales effects that run below the game's resolution. Its headers go into the program the same way, and
# ReShade's effect compiler builds them the first time it upscales. That compiler takes no prototypes, which ffx_fsr1.h
# declares its callbacks with, and two blocks of ffx_a.h that FSR does not use call min on unsigned integers, which it
# has no overload for, so those lines go.
set(FSR_DIR "${CMAKE_BINARY_DIR}/fsr")
set(FSR_URL "https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/v1.0.2/ffx-fsr")
fetch_file("${FSR_URL}/ffx_a.h" f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb "${FSR_DIR}/ffx_a.h")
fetch_file("${FSR_URL}/ffx_fsr1.h" 93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5 "${FSR_DIR}/ffx_fsr1.h")
patch_source("${FSR_DIR}" ffx_a.h
    " #if 1\n  // Designed to be one V_CVT_PKACCUM_U8_F32." " #if 0\n  // Designed to be one V_CVT_PKACCUM_U8_F32."
    " #if 1\n  AU1 ABsc0ToU1" " #if 0\n  AU1 ABsc0ToU1")
patch_source("${FSR_DIR}" ffx_fsr1.h
    " AF4 FsrEasuRF(AF2 p);\n AF4 FsrEasuGF(AF2 p);\n AF4 FsrEasuBF(AF2 p);\n" ""
    " AF4 FsrRcasLoadF(ASU2 p);\n void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b);\n" "")
set(FSR_SOURCE "${CMAKE_BINARY_DIR}/fsr.cpp")
file(READ "${FSR_DIR}/patched/ffx_a.h" ffx_a_bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," ffx_a_bytes "${ffx_a_bytes}")
file(READ "${FSR_DIR}/patched/ffx_fsr1.h" ffx_fsr1_bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," ffx_fsr1_bytes "${ffx_fsr1_bytes}")
file(CONFIGURE OUTPUT "${FSR_SOURCE}" CONTENT
    "extern const unsigned char kFfxA[];\nextern const unsigned kFfxASize;\nconst unsigned char kFfxA[] = {${ffx_a_bytes}};\nconst unsigned kFfxASize = sizeof(kFfxA);\nextern const unsigned char kFfxFsr1[];\nextern const unsigned kFfxFsr1Size;\nconst unsigned char kFfxFsr1[] = {${ffx_fsr1_bytes}};\nconst unsigned kFfxFsr1Size = sizeof(kFfxFsr1);\n"
    @ONLY)

# Everything but main, so tests can link it.
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(CORE_SOURCES
    "${LOGO_SOURCE}"
    "${FONTS_SOURCE}"
    "${FSR_SOURCE}"
    "${OFFICIAL_ADDONS_SOURCE}"
    "${SRC_DIR}/addons/addon_frames.cpp"
    "${SRC_DIR}/addons/addon_host.cpp"
    "${SRC_DIR}/addons/addons.cpp"
    "${SRC_DIR}/app/app.cpp"
    "${SRC_DIR}/app/config.cpp"
    "${SRC_DIR}/app/hotkeys.cpp"
    "${SRC_DIR}/app/log.cpp"
    "${SRC_DIR}/effects/effects.cpp"
    "${SRC_DIR}/effects/uimask.cpp"
    "${SRC_DIR}/games/game_scan.cpp"
    "${SRC_DIR}/games/games.cpp"
    "${SRC_DIR}/gpu/backdrop.cpp"
    "${SRC_DIR}/gpu/gpu.cpp"
    "${SRC_DIR}/gpu/upscale.cpp"
    "${SRC_DIR}/setup/setup.cpp"
    "${SRC_DIR}/ui/kit.cpp"
    "${SRC_DIR}/ui/launcher_ui.cpp"
    "${SRC_DIR}/ui/tray.cpp"
    "${SRC_DIR}/ui/ui.cpp")
if(APPLE)
    list(APPEND CORE_SOURCES "${SRC_DIR}/platform/macos/platform_macos.mm" "${SRC_DIR}/platform/macos/emoji_macos.cpp")
elseif(WIN32)
    # Effects download through WinHTTP, with the same code as Setup.
    list(APPEND CORE_SOURCES "${SRC_DIR}/platform/windows/platform_win32.cpp" "${SRC_DIR}/net/net.cpp"
        "${SRC_DIR}/discord/discord_win32.cpp" "${SRC_DIR}/net/update.cpp")
else()
    list(APPEND CORE_SOURCES "${SRC_DIR}/platform/linux/platform_x11.cpp")
endif()
if(MSVC)
    # Names that shadow others are fine, as with GCC and Clang at -Wall -Wextra.
    set(HOST_COMPILE_OPTIONS /W4 /wd4458 /wd4459 /bigobj)
else()
    set(HOST_COMPILE_OPTIONS -Wall -Wextra -Wno-missing-field-initializers)
endif()

add_library(unishade_core STATIC ${CORE_SOURCES})
# Files are included by their path from src, and the add-on interface by its name.
target_include_directories(unishade_core PUBLIC "${SRC_DIR}" "${CMAKE_SOURCE_DIR}/addons")
target_compile_definitions(unishade_core PUBLIC UNISHADE_VERSION="${UNISHADE_VERSION}")
if(UNISHADE_BETA GREATER 0)
    target_compile_definitions(unishade_core PUBLIC UNISHADE_BETA=${UNISHADE_BETA})
endif()
target_compile_options(unishade_core PRIVATE ${HOST_COMPILE_OPTIONS})
set_source_files_properties("${SRC_DIR}/effects/effects.cpp" PROPERTIES COMPILE_DEFINITIONS UNISHADE_COMPILER_ID="${RESHADEFX_ID}")
target_link_libraries(unishade_core PUBLIC reshadefx unishade_libraries Threads::Threads)
if(APPLE)
    target_compile_options(unishade_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(unishade_core PUBLIC "-framework AppKit" "-framework Carbon" "-framework CoreGraphics"
        "-framework CoreMedia" "-framework CoreText" "-framework CoreVideo" "-framework QuartzCore" "-framework ScreenCaptureKit" "-framework Metal"
        "-framework IOSurface" "-framework IOKit")
elseif(WIN32)
    # The standard and POSIX C functions MSVC warns about are used as on the other systems.
    target_compile_definitions(unishade_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    target_compile_definitions(unishade_core PRIVATE UNISHADE_USER_AGENT="Unishade/${UNISHADE_VERSION}")
    target_link_libraries(unishade_core PUBLIC d3d11 d3dcompiler dxgi dwmapi windowsapp winhttp bcrypt comdlg32)
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
    # Finder and the Dock show the logo from an icon file made of it, which Info.plist names. It holds every size
    # Finder draws, each scaled down from the 512-pixel logo, so small icons are not shrunk from the largest on the fly.
    set(APP_ICON "${CMAKE_BINARY_DIR}/Unishade.icns")
    set(APP_ICONSET "${CMAKE_BINARY_DIR}/Unishade.iconset")
    set(ICON_COMMANDS COMMAND "${CMAKE_COMMAND}" -E rm -rf "${APP_ICONSET}" COMMAND "${CMAKE_COMMAND}" -E make_directory "${APP_ICONSET}")
    foreach(size 16 32 128 256 512)
        list(APPEND ICON_COMMANDS COMMAND sips -z ${size} ${size} "${LOGO_PNG}" --out "${APP_ICONSET}/icon_${size}x${size}.png")
        math(EXPR double "${size} * 2")
        if(double LESS_EQUAL 512)
            list(APPEND ICON_COMMANDS COMMAND sips -z ${double} ${double} "${LOGO_PNG}" --out "${APP_ICONSET}/icon_${size}x${size}@2x.png")
        endif()
    endforeach()
    add_custom_command(OUTPUT "${APP_ICON}"
        ${ICON_COMMANDS}
        COMMAND iconutil -c icns "${APP_ICONSET}" -o "${APP_ICON}"
        DEPENDS "${LOGO_PNG}"
        VERBATIM)
    add_executable(Unishade MACOSX_BUNDLE "${SRC_DIR}/main.cpp" "${APP_ICON}")
    set_source_files_properties("${APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(Unishade PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${SRC_DIR}/platform/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${UNISHADE_VERSION}"
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH ON)
    # MoltenVK goes inside the app, which is then signed again since changing it breaks the signature.
    add_custom_command(TARGET Unishade POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DAPP=$<TARGET_BUNDLE_DIR:Unishade> -DEXE=$<TARGET_FILE:Unishade> -DMOLTENVK=${MOLTENVK_LIBRARY}
                -P "${CMAKE_SOURCE_DIR}/cmake/bundle_macos.cmake"
        VERBATIM)
elseif(WIN32)
    # The manifest makes UTF-8 the program's code page, which the narrow file and path functions then use, as they do
    # on the other systems.
    add_executable(Unishade WIN32 "${SRC_DIR}/main.cpp" "${SRC_DIR}/platform/windows/Unishade.manifest" "${SRC_DIR}/platform/windows/Unishade.rc")
    target_compile_definitions(Unishade PRIVATE
        UNISHADE_VERSION_RESOURCE=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
    # A program without a console window that still starts at main.
    target_link_options(Unishade PRIVATE /ENTRY:mainCRTStartup)
    # vulkan-1.dll comes with the graphics driver. It loads when Vulkan is first used instead of with the program, so
    # Unishade starts without it: Setup has it download the effects, which needs no graphics card, and where Vulkan is
    # missing Unishade says so itself instead of Windows showing an error about a DLL.
    target_link_options(Unishade PRIVATE /DELAYLOAD:vulkan-1.dll)
    target_link_libraries(Unishade PRIVATE delayimp)
else()
    add_executable(unishade "${SRC_DIR}/main.cpp")
    set_target_properties(unishade PROPERTIES OUTPUT_NAME unishade)
    # Run paths into the build folder are relative to the executable, so the folder can move. Effects and presets
    # are always in the data folder from config.h, never beside the executable.
    set_target_properties(unishade PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
endif()
if(APPLE)
    target_link_libraries(Unishade PRIVATE unishade_core)
    target_compile_options(Unishade PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(WIN32)
    target_link_libraries(Unishade PRIVATE unishade_core)
    target_compile_options(Unishade PRIVATE ${HOST_COMPILE_OPTIONS})
else()
    target_link_libraries(unishade PRIVATE unishade_core)
    target_compile_options(unishade PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
