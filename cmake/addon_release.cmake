# Packs the release of an official add-on, as src/addons/addon_releases.h describes it. Run with -P:
#
#   cmake -DADDON=addons/depth "-DBINARIES=Depth.dll=build/Release/Depth.dll|Depth.dylib=build/Depth.dylib" -DOUTPUT=upload -P cmake/addon_release.cmake
#
# ADDON is the add-on's folder, with its addon.ini, files.ini and icon. BINARIES are the libraries built for it, as
# name=path separated by |: every one its files.ini has a placeholder for. OUTPUT is the folder the release's files go
# into: the libraries, addon.ini, files.ini with each library's SHA-256 and the release's tag filled in, the icon and
# SHA256SUMS.txt. Without OUTPUT, only the release's tag is printed, which the workflow that publishes it reads.

# Run with -P this file has no policies set, and a CMake older than 4 then reads "@TAG@" as the variable's value, so
# the placeholder would be replaced by itself and stay in the list.
cmake_minimum_required(VERSION 3.20)

# Without its comments and the empty lines they leave at the top.
function(read_ini path text)
    file(READ "${path}" read)
    string(REGEX REPLACE "(^|\n);[^\n]*" "" read "${read}")
    string(REGEX REPLACE "^\n+" "" read "${read}")
    set(${text} "${read}" PARENT_SCOPE)
endfunction()

read_ini("${ADDON}/addon.ini" manifest)
if(NOT manifest MATCHES "(^|\n)Id=([a-z0-9][a-z0-9._-]*)\n")
    message(FATAL_ERROR "${ADDON}/addon.ini has no Id that Unishade takes.")
endif()
set(id "${CMAKE_MATCH_2}")
# As src/net/version.h reads a version: 1.2.0, or 1.2.0-beta.3 for a beta.
if(NOT manifest MATCHES "(^|\n)Version=([0-9]+\\.[0-9]+\\.[0-9]+(-beta\\.[1-9][0-9]*)?)\n")
    message(FATAL_ERROR "The Version in ${ADDON}/addon.ini is not one like 1.2.0 or 1.2.0-beta.3.")
endif()
set(tag "${id}-v${CMAKE_MATCH_2}")
if(NOT OUTPUT)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${tag}")
    return()
endif()

# A folder given from where the script runs, as the workflow gives it. Listing its files below needs the whole path.
get_filename_component(OUTPUT "${OUTPUT}" ABSOLUTE)
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
read_ini("${ADDON}/files.ini" files)
string(REPLACE "@TAG@" "${tag}" files "${files}")
string(REPLACE "|" ";" binaries "${BINARIES}")
foreach(binary IN LISTS binaries)
    string(FIND "${binary}" "=" equals)
    string(SUBSTRING "${binary}" 0 ${equals} name)
    math(EXPR start "${equals} + 1")
    string(SUBSTRING "${binary}" ${start} -1 path)
    string(FIND "${files}" "@${name}@" listed)
    if(listed EQUAL -1)
        message(FATAL_ERROR "${ADDON}/files.ini has no @${name}@, so ${path} is not a file of this add-on.")
    endif()
    configure_file("${path}" "${OUTPUT}/${name}" COPYONLY)
    file(SHA256 "${OUTPUT}/${name}" hash)
    string(REPLACE "@${name}@" "${hash}" files "${files}")
endforeach()
if(files MATCHES "@([^@\n]+)@")
    message(FATAL_ERROR "The release of ${id} needs ${CMAKE_MATCH_1}, which is not among the libraries given, so its downloads would not be found.")
endif()
file(WRITE "${OUTPUT}/addon.ini" "${manifest}")
file(WRITE "${OUTPUT}/files.ini" "${files}")
# The release has the icon under its file's name, wherever in the add-on's folder the manifest puts it.
if(manifest MATCHES "(^|\n)Icon=([^\n]+)\n")
    get_filename_component(icon "${CMAKE_MATCH_2}" NAME)
    configure_file("${ADDON}/${CMAKE_MATCH_2}" "${OUTPUT}/${icon}" COPYONLY)
endif()

# The SHA-256 of every file, in the format sha256sum -c reads. Unishade checks what it reads of the release against it.
file(GLOB released RELATIVE "${OUTPUT}" "${OUTPUT}/*")
list(SORT released)
set(sums "")
foreach(file IN LISTS released)
    file(SHA256 "${OUTPUT}/${file}" hash)
    string(APPEND sums "${hash}  ${file}\n")
endforeach()
if(NOT sums)
    message(FATAL_ERROR "No file of the release was found in ${OUTPUT} to list in SHA256SUMS.txt.")
endif()
file(WRITE "${OUTPUT}/SHA256SUMS.txt" "${sums}")
message(STATUS "Packed ${tag} into ${OUTPUT}")
