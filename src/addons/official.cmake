# Writes the official add-on list into a source file of the program, at build time. LIST is official.ini, OUTPUT the
# source file, VERSION the version, and BINARIES the add-ons this build made, as name=path separated by |.
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
file(WRITE "${OUTPUT}" "extern const char kOfficialAddons[];\nconst char kOfficialAddons[] = R\"list(${text})list\";\n")
