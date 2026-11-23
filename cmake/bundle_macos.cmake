# Copies MoltenVK into Unishade.app, so the app runs without Homebrew or the Vulkan SDK, then signs the app
# again. Run after every build with -DAPP=<bundle> -DEXE=<executable> -DMOLTENVK=<libMoltenVK.dylib>.

get_filename_component(MOLTENVK_REAL "${MOLTENVK}" REALPATH)
set(FRAMEWORKS "${APP}/Contents/Frameworks")
file(MAKE_DIRECTORY "${FRAMEWORKS}")
configure_file("${MOLTENVK_REAL}" "${FRAMEWORKS}/libMoltenVK.dylib" COPYONLY)

# The executable names MoltenVK by the path it was linked from, which only exists on the build machine.
execute_process(COMMAND otool -D "${MOLTENVK_REAL}" OUTPUT_VARIABLE id_output OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" id_lines "${id_output}")
list(GET id_lines -1 old_id)
execute_process(COMMAND install_name_tool -id "@rpath/libMoltenVK.dylib" "${FRAMEWORKS}/libMoltenVK.dylib")
execute_process(COMMAND install_name_tool -change "${old_id}" "@rpath/libMoltenVK.dylib" "${EXE}")

# Apple silicon refuses to run changed binaries whose signature no longer matches. An ad-hoc signature is
# enough to run locally built copies.
execute_process(COMMAND codesign --force --sign - "${FRAMEWORKS}/libMoltenVK.dylib")
execute_process(COMMAND codesign --force --sign - "${APP}" RESULT_VARIABLE sign_result)
if(NOT sign_result EQUAL 0)
    message(WARNING "Could not sign ${APP}")
endif()
