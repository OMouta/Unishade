# Writes the official add-ons into a source file of the program, at build time: the list of their files, and each
# one's manifest and icon. LIST is official.ini, OUTPUT the source file, VERSION the version, BINARIES the add-ons this
# build made, as name=path separated by |, and FOLDERS the folders with each add-on's addon.ini and icon.png,
# separated by |.

# Run with -P this file has no policies set, and a CMake older than 4 then reads "@VERSION@" as the variable's value,
# so the placeholder would be replaced by itself and stay in the list.
cmake_minimum_required(VERSION 3.20)
file(READ "${LIST}" text)
# Without its comments, which name the placeholders.
string(REGEX REPLACE "(^|\n);[^\n]*" "" text "${text}")
string(REPLACE "@VERSION@" "${VERSION}" text "${text}")
string(REPLACE "|" ";" binaries "${BINARIES}")
foreach(binary IN LISTS binaries)
    string(FIND "${binary}" "=" equals)
    string(SUBSTRING "${binary}" 0 ${equals} name)
    math(EXPR start "${equals} + 1")
    string(SUBSTRING "${binary}" ${start} -1 path)
    file(SHA256 "${path}" hash)
    string(REPLACE "@${name}@" "${hash}" text "${text}")
endforeach()
set(source "extern const char kOfficialAddons[];\nconst char kOfficialAddons[] = R\"list(${text})list\";\n")

string(REPLACE "|" ";" folders "${FOLDERS}")
set(manifests "")
set(icons "")
set(sizes "")
set(count 0)
foreach(folder IN LISTS folders)
    file(READ "${folder}/addon.ini" manifest)
    string(REGEX REPLACE "(^|\n);[^\n]*" "" manifest "${manifest}")
    string(REGEX REPLACE "^\n+" "" manifest "${manifest}")
    string(REPLACE "@VERSION@" "${VERSION}" manifest "${manifest}")
    file(READ "${folder}/icon.png" icon HEX)
    string(REGEX REPLACE "(..)" "0x\\1," icon "${icon}")
    string(APPEND source "static const unsigned char kIcon${count}[] = {${icon}};\n")
    string(APPEND manifests "    R\"manifest(${manifest})manifest\",\n")
    string(APPEND icons "    kIcon${count},\n")
    string(APPEND sizes "    sizeof(kIcon${count}),\n")
    math(EXPR count "${count} + 1")
endforeach()
# Each array ends with an entry that is not an add-on's, so none of them is empty.
string(APPEND source "extern const char* const kOfficialManifests[];\nconst char* const kOfficialManifests[] = {\n${manifests}    nullptr,\n};\n")
string(APPEND source "extern const unsigned char* const kOfficialIcons[];\nconst unsigned char* const kOfficialIcons[] = {\n${icons}    nullptr,\n};\n")
string(APPEND source "extern const unsigned kOfficialIconSizes[];\nconst unsigned kOfficialIconSizes[] = {\n${sizes}    0,\n};\n")
string(FIND "${source}" "@VERSION@" unfilled)
if(NOT unfilled EQUAL -1)
    message(FATAL_ERROR "@VERSION@ is still in the official add-ons, so their downloads would not be found.")
endif()
file(WRITE "${OUTPUT}" "${source}")
