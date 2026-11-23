# Builds the example add-on by itself, for a copy of this folder with unishade_addon.h beside it or one folder up:
#
#   cmake -S . -B build && cmake --build build --config Release
#
# The library and addon.ini then go in a folder of Unishade's Addons folder.
cmake_minimum_required(VERSION 3.20)
project(Example LANGUAGES C)

add_library(Example SHARED example.c)
# Named as addon.ini names it on every system, without the lib a library gets elsewhere.
set_target_properties(Example PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
target_include_directories(Example PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
