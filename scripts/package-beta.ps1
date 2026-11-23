# Builds Unishade and packs a portable copy for testers into build/beta. The zip has every official effect, the
# presets and the official add-ons. Testers unzip it and run Unishade.exe.
#
#   ./scripts/package-beta.ps1

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build'
$out = Join-Path $build 'beta'

$version = [regex]::Match((Get-Content "$repo/CMakeLists.txt" -Raw), 'project\(Unishade VERSION ([\d.]+)').Groups[1].Value
$commit = git -C $repo rev-parse --short HEAD

cmake -S $repo -B $build -A x64
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
cmake --build $build --config Release --target installer --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }

# Setup does the actual install, the same way it does for users. --portable keeps it out of the Start menu and
# Windows' app list.
$folder = Join-Path $out 'Unishade'
$log = Join-Path $out 'setup.log'
if (Test-Path $folder) { Remove-Item $folder -Recurse -Force }
New-Item -ItemType Directory -Path $out -Force | Out-Null
$arguments = @('--silent', '--portable', '--dir', "`"$folder`"", '--log', "`"$log`"")
# Presets come from main on the GitHub repository origin points at, which may still have its old name. Without a
# GitHub origin, Setup uses its own presets address.
$origin = [string](git -C $repo config --get remote.origin.url)
$github = [regex]::Match($origin, 'github\.com[:/]([^/]+/[^/]+?)(\.git)?/?$')
if ($github.Success) {
    $arguments += @('--presets-url', "https://raw.githubusercontent.com/$($github.Groups[1].Value)/main/presets")
} else {
    Write-Warning "origin is not a GitHub repository, so Setup downloads the presets from its default address."
}
$setup = Join-Path $build "installer/Unishade-Setup-$version.exe"
$exitCode = (Start-Process $setup -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru).ExitCode
# 2 means Setup left out effects or presets, which a beta should not ship without.
if ($exitCode -eq 2) { throw "Setup left out effects or presets. See $log" }
if ($exitCode -ne 0) { throw "Setup failed with exit code $exitCode. See $log" }

# The Add-ons page installs the official add-ons from this version's release, which a beta does not have. They go in
# from this build instead: each one's files for Windows, from the addresses in official.ini, in a folder of the Addons
# folder named after its Id, with the addon.ini and icon Unishade would write there.
$addons = Join-Path $folder 'Addons'
$id = $null
foreach ($line in Get-Content "$repo/src/addons/official.ini") {
    if ($line -match '^\[(\S+) (\S+)\]') { $id = if ($Matches[2] -eq 'windows-x64') { $Matches[1] } else { $null }; continue }
    if (-not $id -or $line -notmatch '^([^;\[=]+)=(\S+) (\S+)$') { continue }
    $file = Join-Path $addons "$id/$($Matches[1])"
    $hash = $Matches[2]
    $url = $Matches[3]
    New-Item -ItemType Directory -Path (Split-Path $file -Parent) -Force | Out-Null
    if ($hash -match '^@(.+)@$') {
        # Built here. Unishade.exe has the hash of this very file.
        Copy-Item (Join-Path $build "Release/$($Matches[1])") $file
        continue
    }
    $kept = Join-Path $build "beta-downloads/$hash"
    if (-not (Test-Path $kept)) {
        New-Item -ItemType Directory -Path (Split-Path $kept -Parent) -Force | Out-Null
        Write-Host "Downloading $url"
        curl.exe --fail --location --silent --show-error --output "$kept.part" $url
        if ($LASTEXITCODE) { throw "Could not download $url" }
        if ((Get-FileHash "$kept.part" -Algorithm SHA256).Hash -ne $hash) { throw "$url is not the file official.ini lists." }
        Move-Item "$kept.part" $kept -Force
    }
    Copy-Item $kept $file
}
foreach ($manifest in Get-ChildItem "$repo/src/addons/*/addon.ini") {
    $text = (Get-Content $manifest -Raw).Replace('@VERSION@', $version)
    $id = [regex]::Match($text, '(?m)^Id=(\S+)').Groups[1].Value
    $target = Join-Path $addons $id
    # Only the add-ons that have files for Windows have a folder.
    if (-not (Test-Path $target)) { continue }
    Set-Content (Join-Path $target 'addon.ini') $text -NoNewline
    Copy-Item (Join-Path $manifest.Directory 'icon.png') $target
}

# Setup's uninstall list means nothing outside an installation.
Remove-Item (Join-Path $folder 'RobloxShadeHost-Setup.files') -ErrorAction SilentlyContinue

$zip = Join-Path $out "Unishade-$version-beta-$commit.zip"
Compress-Archive -Path $folder -DestinationPath $zip -Force
Write-Host "Packed $zip"
