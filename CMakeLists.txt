cmake_minimum_required(VERSION 3.20)
# The Mac app runs on macOS 13 and later. project() reads the target when it first enables a language, so it is set
# before, where APPLE is not known yet. An empty value, as an older build folder may hold, counts as unset, and a value
# given on the command line or in MACOSX_DEPLOYMENT_TARGET wins.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "The oldest macOS the app runs on" FORCE)
endif()
if(WIN32)
    project(Unishade VERSION 0.7.0 LANGUAGES C CXX RC)
else()
    project(Unishade VERSION 0.7.0 LANGUAGES C CXX)
endif()
# Which beta of that version this is: 2 builds 0.7.0-beta.2, with the beta icon and a BETA badge in the launcher, and
# the release workflow publishes it as a prerelease. 0 builds the version itself. The workflow reads this line.
set(UNISHADE_BETA 3)
if(UNISHADE_BETA GREATER 0)
    set(UNISHADE_VERSION "${PROJECT_VERSION}-beta.${UNISHADE_BETA}")
else()
    set(UNISHADE_VERSION "${PROJECT_VERSION}")
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

if(APPLE)
    # Objective-C first: the language enabled first owns the .m extension, and GLFW's .m files are not C++.
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

# The build, one part to a file in cmake/. They share this folder's variables and targets, so their order matters.
# Third-party files are downloaded, not vendored.
include(cmake/fetch.cmake)
# The libraries Unishade is built on.
include(cmake/libraries.cmake)
include(CTest)
# The add-ons in addons/: the example, Depth and DLSS5.
include(cmake/addons.cmake)
# Unishade itself, in src/. It compiles effects with ReShade's effect compiler and runs them on Vulkan.
include(cmake/app.cmake)
# Setup for Windows, in installer/.
if(WIN32)
    include(cmake/installer.cmake)
endif()
if(BUILD_TESTING)
    include(cmake/tests.cmake)
endif()
