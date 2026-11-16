# Builds Unishade and packs a portable copy for testers into build/beta. The zip has every official effect and the
# presets. Testers unzip it, run Unishade.exe and install the add-ons from its Add-ons page.
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

# Setup's uninstall list means nothing outside an installation.
Remove-Item (Join-Path $folder 'RobloxShadeHost-Setup.files') -ErrorAction SilentlyContinue

$zip = Join-Path $out "Unishade-$version-beta-$commit.zip"
Compress-Archive -Path $folder -DestinationPath $zip -Force
Write-Host "Packed $zip"
