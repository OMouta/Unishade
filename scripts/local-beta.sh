#!/bin/sh
# Builds a local beta on a Mac: Unishade from this checkout, packed for testers into build/local-beta. The zip has
# Unishade.app and the official add-ons for the Mac as this build made them. It is for handing out a commit that is
# not a release. A beta everyone can download is a release the Build and release workflow publishes.
# scripts/local-beta.ps1 does this on Windows.
#
# Effects and presets are not in the zip: Unishade downloads them, as after any install.
#
#   ./scripts/local-beta.sh
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
out="$repo/build/local-beta"
# A build folder of its own. Building into build would replace the Unishade.app there, and macOS can take the screen
# recording permission from an app that another build replaced.
build="$out/build"
commit=$(git -C "$repo" rev-parse --short HEAD)

cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel
# The version as the build names it: 0.7.0, or 0.7.0-beta.2 with set(UNISHADE_BETA 2).
version=$("$build/Unishade.app/Contents/MacOS/Unishade" --version)

folder="$out/Unishade"
rm -rf "$folder"
mkdir -p "$folder"
ditto "$build/Unishade.app" "$folder/Unishade.app"
cp "$repo/LICENSE" "$repo/CREDITS.txt" "$folder/"

# The Add-ons page installs the official add-ons from this version's release, whose files are not this build's, if
# the release exists at all. They go in the zip from this build instead: each one's files for this Mac, from the
# addresses in official.ini, in a folder named after its Id, with the addon.ini and icon Unishade would write there.
system="macos-$(uname -m | sed 's/x86_64/x64/')"
awk -v wanted="$system]" '
    /^\[/ { id = ($2 == wanted) ? substr($1, 2) : ""; next }
    id != "" && /^[^;=]+=/ { at = index($0, "="); print id, substr($0, 1, at - 1), substr($0, at + 1) }
' "$repo/addons/official.ini" | while read -r id name hash url; do
    file="$folder/Addons/$id/$name"
    mkdir -p "$(dirname "$file")"
    case $hash in
    @*@)
        # Built here. Unishade has the hash of this very file.
        built=${hash#@}
        cp "$build/${built%@}" "$file"
        continue
        ;;
    esac
    kept="$repo/build/local-beta-downloads/$hash"
    if [ ! -f "$kept" ]; then
        mkdir -p "$(dirname "$kept")"
        echo "Downloading $url"
        curl --fail --location --silent --show-error --output "$kept.part" "$url"
        if [ "$(shasum -a 256 "$kept.part" | cut -d ' ' -f 1)" != "$hash" ]; then
            echo "$url is not the file official.ini lists." >&2
            exit 1
        fi
        mv "$kept.part" "$kept"
    fi
    cp "$kept" "$file"
done
for manifest in "$repo"/addons/*/addon.ini; do
    id=$(sed -n 's/^Id=//p' "$manifest")
    # Only the add-ons that have files for this Mac have a folder.
    if [ -z "$id" ] || [ ! -d "$folder/Addons/$id" ]; then continue; fi
    # Without its comments and the empty lines they leave at the top, as the build puts it into Unishade.
    sed -e '/^;/d' -e "s/@VERSION@/$version/g" "$manifest" | sed '/./,$!d' > "$folder/Addons/$id/addon.ini"
    cp "$(dirname "$manifest")/icon.png" "$folder/Addons/$id/"
done

cat > "$folder/README.txt" <<README
Unishade $version, built from commit $commit for testing. It is not a release.

Move Unishade to Applications and open it. macOS stops it the first time:
- On macOS 15 or newer, click Done, then open System Settings > Privacy & Security and click Open Anyway.
- On macOS 13 and 14, right-click Unishade, choose Open, then click Open.
README

# Unishade keeps its add-ons in its data folder, where a zip cannot put them, so the tester runs this once.
if [ -d "$folder/Addons" ]; then
    cat > "$folder/install-addons.sh" <<'INSTALL'
#!/bin/sh
# Puts this build's add-ons into Unishade's data folder, where the Add-ons page puts a release's.
set -eu
cd "$(dirname "$0")"
addons="$HOME/Library/Application Support/Unishade/Addons"
mkdir -p "$addons"
for addon in Addons/*; do
    installed="$addons/$(basename "$addon")"
    rm -rf "$installed"
    cp -R "$addon" "$installed"
    # Takes off the mark macOS puts on downloaded files, which can keep Unishade from loading a library.
    xattr -dr com.apple.quarantine "$installed"
done
echo "The add-ons are installed. Start Unishade again if it is open."
INSTALL
    cat >> "$folder/README.txt" <<'README'

The Add-ons page can't install this build's add-ons, so they come in this folder. To install them, open Terminal,
type "sh " with the space after it, drag install-addons.sh into the Terminal window and press Return.
README
fi

zip="$out/Unishade-$version-local-$commit-macOS.zip"
rm -f "$zip"
ditto -c -k --keepParent "$folder" "$zip"
echo "Packed $zip"
