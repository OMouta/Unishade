#!/bin/sh
# Has Apple's notary service check a signed Unishade.app or disk image, and attaches the ticket it answers with to the
# file, so a Mac opens the download without a warning, with or without a network. The Build and release workflow runs
# it. The App Store Connect API key comes from the environment: NOTARY_KEY is the path of its .p8 file and
# NOTARY_KEY_ID its ID. NOTARY_ISSUER_ID is the issuer of a team's key, and stays empty for an individual's.
#
#   NOTARY_KEY=AuthKey.p8 NOTARY_KEY_ID=... NOTARY_ISSUER_ID=... ./scripts/notarize-macos.sh build/Unishade.app
set -eu

if [ $# -ne 1 ] || [ ! -e "$1" ]; then
    echo "Name the app or the disk image to notarize: ./scripts/notarize-macos.sh build/Unishade.app" >&2
    exit 1
fi
if [ ! -f "${NOTARY_KEY:-}" ] || [ -z "${NOTARY_KEY_ID:-}" ]; then
    echo "Set NOTARY_KEY to the App Store Connect API key's .p8 file and NOTARY_KEY_ID to the key's ID." >&2
    exit 1
fi
file=$1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The service takes one file, so an app goes there in a zip. The ticket is attached to the app itself.
upload=$file
case $file in
*.app)
    upload="$work/$(basename "$file" .app).zip"
    ditto -c -k --keepParent "$file" "$upload"
    ;;
esac

# The key, as each call of notarytool takes it.
set -- --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID"
if [ -n "${NOTARY_ISSUER_ID:-}" ]; then
    set -- "$@" --issuer "$NOTARY_ISSUER_ID"
fi

# notarytool can succeed while the service refuses the file, so its answer is read.
echo "Sending $file to Apple's notary service, which can take a while."
if ! xcrun notarytool submit "$upload" "$@" --wait --timeout 45m --output-format plist > "$work/answer.plist"; then
    cat "$work/answer.plist" >&2
    echo "Could not send $file to Apple's notary service." >&2
    exit 1
fi
status=$(plutil -extract status raw "$work/answer.plist" 2> /dev/null || true)
if [ "$status" != Accepted ]; then
    id=$(plutil -extract id raw "$work/answer.plist" 2> /dev/null || true)
    echo "Apple's notary service did not accept $file. Its answer was: ${status:-none}." >&2
    # The service's log names each file it objects to and why.
    if [ -n "$id" ]; then
        xcrun notarytool log "$id" "$@" >&2 || true
    fi
    exit 1
fi

xcrun stapler staple "$file"
xcrun stapler validate "$file"
