#!/bin/sh
# Packs Unishade.app into the disk image a Mac release is: a window with the app and the Applications folder to drag it
# to. The Build and release workflow runs it on the signed app. dmgbuild writes the image, installed at a fixed version
# into a folder beside the image, so nothing is installed on the Mac itself.
#
#   ./scripts/dmg.sh build/Unishade.app build/Unishade-macOS.dmg
set -eu

if [ $# -ne 2 ]; then
    echo "Name the app and the disk image to make: ./scripts/dmg.sh build/Unishade.app build/Unishade-macOS.dmg" >&2
    exit 1
fi
repo=$(cd "$(dirname "$0")/.." && pwd)
app=$1
dmg=$2
if [ ! -d "$app/Contents/MacOS" ]; then
    echo "$app is not an app. Build Unishade first." >&2
    exit 1
fi

# dmgbuild needs Python 3.11 or newer, which the one that comes with Xcode is not.
python=
for candidate in python3 python3.14 python3.13 python3.12 python3.11; do
    if command -v "$candidate" > /dev/null && "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 11))'; then
        python=$candidate
        break
    fi
done
if [ -z "$python" ]; then
    echo "The disk image needs Python 3.11 or newer. Install it, such as with: brew install python" >&2
    exit 1
fi

tools="$(dirname "$dmg")/dmg-tools"
if [ ! -x "$tools/bin/python" ]; then
    rm -rf "$tools"
    "$python" -m venv "$tools"
fi
"$tools/bin/python" -m pip install --quiet --disable-pip-version-check --only-binary :all: --require-hashes \
    -r "$repo/scripts/dmg-requirements.txt"

rm -f "$dmg"
"$tools/bin/dmgbuild" -s "$repo/scripts/dmg_settings.py" -D "app=$app" -D "background=$repo/assets/dmg/background.png" Unishade "$dmg"
echo "Packed $dmg"
