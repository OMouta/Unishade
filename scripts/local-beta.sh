#!/bin/sh
# Builds a local beta on a Mac or on Linux: Unishade from this checkout, packed for testers into build/local-beta. On
# a Mac the zip has Unishade.app and the official add-ons for the Mac as this build made them. Linux has no official
# add-ons, so its archive has what a release's has. It is for handing out a commit that is not a release. A beta
# everyone can download is a release the Build and release workflow publishes. scripts/local-beta.ps1 does this on
# Windows.
#
# Effects and presets are not in the zip or the archive: Unishade downloads them, as after any install.
#
#   ./scripts/local-beta.sh
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
out="$repo/build/local-beta"
commit=$(git -C "$repo" rev-parse --short HEAD)

# The system as src/addons/addon_manifest.h names it, such as macos-arm64 or linux-x64.
machine=$(uname -m | sed -e 's/x86_64/x64/' -e 's/aarch64/arm64/')
case $(uname -s) in
Darwin)
    system="macos-$machine"
    program=Unishade.app/Contents/MacOS/Unishade
    ;;
Linux)
    system="linux-$machine"
    program=unishade
    ;;
*)
    echo "This packs a local beta on a Mac or on Linux. On Windows, run scripts/local-beta.ps1." >&2
    exit 1
    ;;
esac

# A build folder of its own for each system, so a Mac and a Linux container can share the checkout. Building into
# build would also replace the Unishade.app there, and macOS can take the screen recording permission from an app that
# another build replaced.
build="$out/build-$system"
cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build" --parallel
# The version as the build names it: 0.7.0, or 0.7.0-beta.2 with set(UNISHADE_BETA 2).
version=$("$build/$program" --version)

folder="$out/Unishade"
rm -rf "$folder"
mkdir -p "$folder"
cp "$repo/LICENSE" "$repo/CREDITS.txt" "$folder/"
printf '%s\n' "Unishade $version, built from commit $commit for testing. It is not a release." > "$folder/README.txt"

case $system in
linux-*)
    # What a release's archive for Linux has, and the note.
    cp "$build/$program" "$folder/"
    printf '\n%s\n' 'Run ./unishade in this folder.' >> "$folder/README.txt"
    archive="$out/Unishade-$version-local-$commit-$system.tar.gz"
    tar -czf "$archive" -C "$out" Unishade
    echo "Packed $archive"
    exit 0
    ;;
esac

ditto "$build/Unishade.app" "$folder/Unishade.app"

# The Add-ons page installs the official add-ons from their own releases, whose libraries are not this build's. This
# build's go in the zip instead: each one's files for this Mac, from the addresses in its files.ini, in a folder named
# after its Id, with the addon.ini and icon Unishade would write there.
for list in "$repo"/addons/*/files.ini; do
    source=$(dirname "$list")
    id=$(sed -n 's/^Id=//p' "$source/addon.ini")
    awk -v wanted="[$system]" '
        /^\[/ { inside = ($1 == wanted); next }
        inside && /^[^;=]+=/ { at = index($0, "="); print substr($0, 1, at - 1), substr($0, at + 1) }
    ' "$list" | while read -r name hash url; do
        file="$folder/Addons/$id/$name"
        mkdir -p "$(dirname "$file")"
        case $hash in
        @*@)
            # Built here.
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
                echo "$url is not the file $list lists." >&2
                exit 1
            fi
            mv "$kept.part" "$kept"
        fi
        cp "$kept" "$file"
    done
    # Only the add-ons that have files for this Mac have a folder.
    if [ -z "$id" ] || [ ! -d "$folder/Addons/$id" ]; then continue; fi
    # Without its comments and the empty lines they leave at the top, as a release has it.
    sed -e '/^;/d' "$source/addon.ini" | sed '/./,$!d' > "$folder/Addons/$id/addon.ini"
    cp "$source/icon.png" "$folder/Addons/$id/"
done

cat >> "$folder/README.txt" <<'README'

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

The Add-ons page installs the released add-ons, not this build's, which come in this folder. To install them, open
Terminal, type "sh " with the space after it, drag install-addons.sh into the Terminal window and press Return. Then
click Load for each on the Add-ons page. Update there puts the released add-on in its place.
README
fi

zip="$out/Unishade-$version-local-$commit-$system.zip"
rm -f "$zip"
ditto -c -k --keepParent "$folder" "$zip"
echo "Packed $zip"
