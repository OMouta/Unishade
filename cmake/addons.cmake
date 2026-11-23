# The Depth add-on on macOS, Depth.dylib, runs ONNX Runtime's library for macOS, which Microsoft publishes for Apple
# silicon only. So only a build for arm64 alone has it, and Unishade offers it only there.
set(DEPTH_MACOS OFF)
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")))
    set(DEPTH_MACOS ON)
endif()

# The example add-on, addons/example, in C, so every build checks that unishade_addon.h is the C it says it is. The
# host's tests load it.
add_library(example_addon SHARED addons/example/example.c)
set_target_properties(example_addon PROPERTIES OUTPUT_NAME Example PREFIX "" C_STANDARD 11 C_VISIBILITY_PRESET hidden)
target_include_directories(example_addon PRIVATE addons)
if(MSVC)
    target_compile_options(example_addon PRIVATE /W4)
else()
    target_compile_options(example_addon PRIVATE -Wall -Wextra -pedantic)
endif()

# ONNX Runtime's headers come from its NuGet package for DirectML, whose three files are the ones of ONNX Runtime 1.24.4
# with Windows line endings. The package is the one download whose hash is checked, and only the headers leave it. The
# Mac takes the same C API header, since the Core ML provider needs no header of its own.
if(WIN32 OR DEPTH_MACOS)
    set(ONNXRUNTIME_DIR "${CMAKE_BINARY_DIR}/onnxruntime")
    set(ONNXRUNTIME_INCLUDE "${ONNXRUNTIME_DIR}/build/native/include")
    if(NOT EXISTS "${ONNXRUNTIME_INCLUDE}/dml_provider_factory.h")
        fetch_file("https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg"
            57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681 "${CMAKE_BINARY_DIR}/onnxruntime-directml-1.24.4.zip")
        file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/onnxruntime-directml-1.24.4.zip" DESTINATION "${ONNXRUNTIME_DIR}"
            PATTERNS build/native/include/onnxruntime_c_api.h build/native/include/onnxruntime_ep_c_api.h build/native/include/dml_provider_factory.h)
    endif()
endif()

# Depth estimation on macOS, Depth.dylib. Its model and libonnxruntime.dylib go in its folder with it. The linker signs
# it ad hoc, which is all Apple silicon asks of a library a program without the hardened runtime loads, so the file the
# official list hashes is the file published.
if(DEPTH_MACOS)
    add_library(depth_addon SHARED addons/depth/depth_addon_macos.mm addons/depth/depth_model.cpp)
    set_target_properties(depth_addon PROPERTIES OUTPUT_NAME Depth PREFIX "")
    target_include_directories(depth_addon PRIVATE addons "${ONNXRUNTIME_INCLUDE}")
    target_compile_options(depth_addon PRIVATE -Wall -Wextra -Wno-missing-field-initializers $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(depth_addon PRIVATE "-framework Foundation" "-framework Metal")
endif()

if(WIN32)
    # Depth estimation as an add-on of the Vulkan host, Depth.dll. Its model and ONNX Runtime go in its folder with it.
    add_library(depth_addon SHARED addons/depth/depth_addon.cpp addons/depth/depth_model.cpp)
    set_target_properties(depth_addon PROPERTIES OUTPUT_NAME Depth)
    target_include_directories(depth_addon PRIVATE addons "${ONNXRUNTIME_INCLUDE}")
    target_compile_definitions(depth_addon PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(depth_addon PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(depth_addon PRIVATE d3d11 d3d12 d3dcompiler dxgi windowsapp)

    # DLSS5 calls NVIDIA's nvngx_dlssnr.dll directly. Unishade_nvngx.dll is also the DLSS5 add-on of the Vulkan host, which
    # takes nvngx_dlssnr.dll from its own folder. A program that runs an image through it is built on request:
    # cmake --build build --config Release --target dlss5_check
    set(NGX_INCLUDE "${CMAKE_BINARY_DIR}/ngx")
    set(NGX_URL "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/include")
    fetch_file("${NGX_URL}/nvsdk_ngx_defs.h" ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e "${NGX_INCLUDE}/nvsdk_ngx_defs.h")
    fetch_file("${NGX_URL}/nvsdk_ngx_params.h" 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc "${NGX_INCLUDE}/nvsdk_ngx_params.h")

    # Its path has to contain nvngx.dll. See addons/dlss5/dlss5.cpp.
    add_library(Unishade_nvngx SHARED addons/dlss5/dlss5.cpp addons/dlss5/dlss5_addon.cpp)
    target_include_directories(Unishade_nvngx PRIVATE addons "${NGX_INCLUDE}")
    target_compile_definitions(Unishade_nvngx PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX UNISHADE_DLSS5_EXPORTS)
    target_compile_definitions(Unishade_nvngx PRIVATE UNISHADE_VERSION="${PROJECT_VERSION}")
    target_compile_options(Unishade_nvngx PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(Unishade_nvngx PRIVATE d3d12 dxgi windowsapp)

    add_executable(dlss5_check EXCLUDE_FROM_ALL addons/dlss5/check.cpp)
    target_include_directories(dlss5_check PRIVATE "${STB_DIR}")
    target_compile_definitions(dlss5_check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dlss5_check PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(dlss5_check PRIVATE d3d12 windowsapp)
    add_dependencies(dlss5_check Unishade_nvngx)
endif()

# The official add-ons go into the program too: the list of their files, with the SHA-256 of the add-ons this build
# makes, so they are official to the Unishade built with them, and each one's manifest and icon, which Unishade writes
# to the add-on's folder when it installs it. Each add-on this build makes goes in OFFICIAL_ADDONS_BINARIES as its file
# name, =, and the file, separated by |, and its target in OFFICIAL_ADDONS_TARGETS.
set(OFFICIAL_ADDONS_SOURCE "${CMAKE_BINARY_DIR}/official_addons.cpp")
set(OFFICIAL_ADDONS_FOLDERS "${CMAKE_SOURCE_DIR}/addons/depth|${CMAKE_SOURCE_DIR}/addons/dlss5")
set(OFFICIAL_ADDONS_BINARIES "")
set(OFFICIAL_ADDONS_TARGETS "")
if(WIN32)
    set(OFFICIAL_ADDONS_BINARIES "Depth.dll=$<TARGET_FILE:depth_addon>|Unishade_nvngx.dll=$<TARGET_FILE:Unishade_nvngx>")
    set(OFFICIAL_ADDONS_TARGETS depth_addon Unishade_nvngx)
elseif(DEPTH_MACOS)
    set(OFFICIAL_ADDONS_BINARIES "Depth.dylib=$<TARGET_FILE:depth_addon>")
    set(OFFICIAL_ADDONS_TARGETS depth_addon)
endif()
add_custom_command(OUTPUT "${OFFICIAL_ADDONS_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DLIST=${CMAKE_SOURCE_DIR}/addons/official.ini" "-DOUTPUT=${OFFICIAL_ADDONS_SOURCE}"
            "-DVERSION=${UNISHADE_VERSION}" "-DBINARIES=${OFFICIAL_ADDONS_BINARIES}" "-DFOLDERS=${OFFICIAL_ADDONS_FOLDERS}"
            -P "${CMAKE_SOURCE_DIR}/cmake/official_addons.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/addons/official.ini" "${CMAKE_SOURCE_DIR}/cmake/official_addons.cmake"
            "${CMAKE_SOURCE_DIR}/addons/depth/addon.ini" "${CMAKE_SOURCE_DIR}/addons/depth/icon.png"
            "${CMAKE_SOURCE_DIR}/addons/dlss5/addon.ini" "${CMAKE_SOURCE_DIR}/addons/dlss5/icon.png" ${OFFICIAL_ADDONS_TARGETS}
    VERBATIM)
