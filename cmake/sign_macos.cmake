# Signs Unishade.app. Run with -DAPP=<bundle>, and with -DIDENTITY=<certificate> for an app that is handed out: the
# name or the SHA-1 of a Developer ID Application certificate in the keychain. Without one the signature is ad hoc,
# which is enough to run a build on the Mac that made it.

if(NOT IDENTITY)
    set(IDENTITY "-")
endif()

# Add-ons are libraries that Unishade did not come with and that anyone can make, which the hardened runtime only
# loads with this entitlement. An ad-hoc build also lets a debugger and Instruments attach, which the hardened runtime
# stops otherwise. Apple's notary service refuses an app that allows it, so a certificate's build does not.
set(entitlements "    <key>com.apple.security.cs.disable-library-validation</key>\n    <true/>\n")
set(timestamp "")
if(IDENTITY STREQUAL "-")
    string(APPEND entitlements "    <key>com.apple.security.get-task-allow</key>\n    <true/>\n")
else()
    # The notary service asks for the time of signing from Apple's server.
    set(timestamp --timestamp)
endif()
get_filename_component(folder "${APP}" DIRECTORY)
set(ENTITLEMENTS "${folder}/Unishade.entitlements")
file(WRITE "${ENTITLEMENTS}"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n${entitlements}</dict>\n</plist>\n")

# The hardened runtime is what the notary service asks of an app. Every build has it, so what it stops shows up
# before a release does. What is inside the app is signed first, since the app's signature covers it.
execute_process(COMMAND codesign --force --sign "${IDENTITY}" --options runtime ${timestamp} "${APP}/Contents/Frameworks/libMoltenVK.dylib"
    RESULT_VARIABLE library_result)
execute_process(COMMAND codesign --force --sign "${IDENTITY}" --options runtime ${timestamp} --entitlements "${ENTITLEMENTS}" "${APP}"
    RESULT_VARIABLE app_result)
if(NOT library_result EQUAL 0 OR NOT app_result EQUAL 0)
    if(IDENTITY STREQUAL "-")
        message(WARNING "Could not sign ${APP}")
    else()
        message(FATAL_ERROR "Could not sign ${APP} with ${IDENTITY}. The certificate and its key have to be in a keychain that is unlocked.")
    endif()
endif()
