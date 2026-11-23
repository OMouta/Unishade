param(
    [string]$Setup = (Get-ChildItem "$PSScriptRoot/../build/installer/Unishade-Setup-*.exe" |
        Sort-Object LastWriteTime | Select-Object -Last 1).FullName,
    [string]$PresetsBaseUrl
)

$ErrorActionPreference = 'Stop'
if (-not $Setup -or -not (Test-Path $Setup)) { throw 'Build the installer first: cmake --build build --config Release --target installer' }
# On GitHub Actions, presets come from the commit under test, which main may not have yet.
if (-not $PresetsBaseUrl -and $env:GITHUB_REPOSITORY -and $env:GITHUB_SHA) {
    $PresetsBaseUrl = "https://raw.githubusercontent.com/$env:GITHUB_REPOSITORY/$env:GITHUB_SHA/presets"
}
$repo = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $repo ('build/installer-tests/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

# --portable keeps the tests out of the Start menu and Windows' app list.
function Invoke-Setup([string]$Name, [string[]]$Arguments) {
    $arguments = @('--silent', '--log', "`"$testRoot/$Name.log`"") + $Arguments
    if ($PresetsBaseUrl) { $arguments += @('--presets-url', $PresetsBaseUrl) }
    return (Start-Process $Setup -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru).ExitCode
}

# Exit code 2 means Setup installed Unishade, but effects or presets were left out.
function Invoke-TestInstaller(
    [string]$Name, [string]$Components, [bool]$ExpectSuccess = $true, [string[]]$Extra = @(), [int]$ExpectExitCode = 0
) {
    $destination = Join-Path $testRoot $Name
    $arguments = @('--portable', '--components', $Components, '--dir', "`"$destination`"") + $Extra
    $exitCode = Invoke-Setup $Name $arguments
    if ($ExpectSuccess -and $exitCode -ne $ExpectExitCode) {
        throw "$Name ended with exit code $exitCode instead of $ExpectExitCode. See $testRoot/$Name.log"
    }
    if (-not $ExpectSuccess -and ($exitCode -eq 0 -or (Test-Path $destination))) {
        throw "$Name installed although Setup should have stopped."
    }
    return $destination
}

function Assert-File([string]$Directory, [string]$Name, [bool]$Expected = $true) {
    if ((Test-Path (Join-Path $Directory $Name)) -ne $Expected) {
        throw "Unexpected file state: $Directory/$Name; expected present=$Expected"
    }
}

$hostOnly = Invoke-TestInstaller 'host-only' 'host'
Assert-File $hostOnly 'Unishade.exe'
Assert-File $hostOnly 'CREDITS.txt'
Assert-File $hostOnly 'dxgi.dll' $false
Assert-File $hostOnly 'nvngx_dlssnr.dll' $false
Assert-File $hostOnly 'depth-anything-v2-small.onnx' $false
Assert-File $hostOnly 'Unishade-Setup.exe' $false

$hostMetadata = [Diagnostics.FileVersionInfo]::GetVersionInfo("$hostOnly/Unishade.exe")
$setupMetadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($Setup)
if ($hostMetadata.ProductName -ne 'Unishade' -or $hostMetadata.FileDescription -ne 'Unishade' -or
    $hostMetadata.OriginalFilename -ne 'Unishade.exe' -or $setupMetadata.ProductName -ne 'Unishade' -or
    $setupMetadata.FileDescription -ne 'Unishade Setup') {
    throw 'The executable or installer still has incorrect product metadata.'
}

# Uninstalling deletes only files inside the folder. File list entries that point elsewhere are ignored.
$outside = Join-Path $testRoot 'outside.txt'
Set-Content $outside 'must survive'
$escape = Invoke-TestInstaller 'file-list-escape' 'host'
[IO.File]::AppendAllText("$escape/RobloxShadeHost-Setup.files", "..\outside.txt`r`n$outside`r`nsub\..\..\outside.txt`r`n")
if ((Invoke-Setup 'uninstall-file-list-escape' @('--uninstall', '--dir', "`"$escape`"")) -ne 0) {
    throw "Uninstall failed. See $testRoot/uninstall-file-list-escape.log"
}
Assert-File $escape 'Unishade.exe' $false
if (-not (Test-Path $outside)) {
    throw 'Uninstall deleted a file outside the installation folder.'
}
if ((Get-Content "$testRoot/uninstall-file-list-escape.log" -Raw) -notmatch 'outside the installation folder') {
    throw 'Uninstall did not report the file list entries outside the folder.'
}

# Setup deletes nothing in a folder that is not a Unishade installation, which needs the host and Setup's file list.
$notInstalled = Join-Path $testRoot 'not-installed'
New-Item -ItemType Directory -Path "$notInstalled/presets" -Force | Out-Null
Set-Content "$notInstalled/Unishade.exe" 'not the host'
Set-Content "$notInstalled/ReShade.ini" '[GENERAL]'
Set-Content "$notInstalled/presets/Mine.ini" 'Techniques=Mine@Mine.fx'
Set-Content "$notInstalled/notes.txt" 'unrelated'
if ((Invoke-Setup 'uninstall-not-installed' @('--uninstall', '--delete-user-files', '--dir', "`"$notInstalled`"")) -ne 1) {
    throw 'Setup did not refuse to uninstall from a folder that is not a Unishade installation.'
}
foreach ($file in @('Unishade.exe', 'ReShade.ini', 'presets/Mine.ini', 'notes.txt')) { Assert-File $notInstalled $file }
if ((Get-Content "$testRoot/uninstall-not-installed.log" -Raw) -notmatch 'is not a Unishade installation') {
    throw 'Setup did not say why it refused to uninstall.'
}

# Uninstalling removes the "Start with Windows" entry only when it starts the copy being uninstalled. Entries that
# were there before the tests are put back.
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$approvedKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
$savedRun = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).Unishade
$savedApproved = (Get-ItemProperty $approvedKey -ErrorAction SilentlyContinue).Unishade
try {
    $startup = Invoke-TestInstaller 'startup' 'host'
    Set-ItemProperty $runKey -Name Unishade -Value "`"$hostOnly\Unishade.exe`" --minimized"
    if ((Invoke-Setup 'uninstall-startup-other' @('--uninstall', '--dir', "`"$startup`"")) -ne 0) {
        throw "Uninstall failed. See $testRoot/uninstall-startup-other.log"
    }
    if (-not (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).Unishade) {
        throw 'Uninstalling one copy removed the startup entry of another.'
    }
    $startup = Invoke-TestInstaller 'startup' 'host'
    Set-ItemProperty $runKey -Name Unishade -Value "`"$startup\Unishade.exe`" --minimized"
    if (-not (Test-Path $approvedKey)) { New-Item $approvedKey -Force | Out-Null }
    New-ItemProperty $approvedKey -Name Unishade -PropertyType Binary -Value ([byte[]](2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)) -Force | Out-Null
    if ((Invoke-Setup 'uninstall-startup' @('--uninstall', '--dir', "`"$startup`"")) -ne 0) {
        throw "Uninstall failed. See $testRoot/uninstall-startup.log"
    }
    if ((Get-ItemProperty $runKey -ErrorAction SilentlyContinue).Unishade -or
        (Get-ItemProperty $approvedKey -ErrorAction SilentlyContinue).Unishade) {
        throw 'Uninstall did not remove its startup entry.'
    }
} finally {
    if ($null -ne $savedRun) {
        Set-ItemProperty $runKey -Name Unishade -Value $savedRun
    } else {
        Remove-ItemProperty $runKey -Name Unishade -ErrorAction SilentlyContinue
    }
    if ($null -ne $savedApproved) {
        New-ItemProperty $approvedKey -Name Unishade -PropertyType Binary -Value $savedApproved -Force | Out-Null
    } else {
        Remove-ItemProperty $approvedKey -Name Unishade -ErrorAction SilentlyContinue
    }
}

# Upgrade an old installation in place without touching the real app registration or Start menu.
$legacy = Join-Path $testRoot 'legacy'
New-Item -ItemType Directory -Path "$legacy/presets" -Force | Out-Null
Set-Content "$legacy/RobloxShadeHost.exe" 'old host'
Set-Content "$legacy/RobloxShadeHost-Setup.exe" 'old setup'
Set-Content "$legacy/legacy-component.txt" 'previously installed component'
Set-Content "$legacy/RobloxShadeHost-Setup.files" "RobloxShadeHost.exe`nRobloxShadeHost-Setup.exe`nlegacy-component.txt"
Set-Content "$legacy/RobloxShadeHost.ini" "[Input]`nToggleKey=F8`n[Menu]`nAutoSavePreset=0"
Set-Content "$legacy/games.ini" "[Games]`nCount=0"
Set-Content "$legacy/ReShade.ini" "[GENERAL]`nPresetPath=.\presets\Custom.ini"
Set-Content "$legacy/presets/Custom.ini" 'Techniques=Custom@Custom.fx'
$userFiles = @('RobloxShadeHost.ini', 'ReShade.ini', 'presets/Custom.ini', 'games.ini')
$userHashes = @{}
foreach ($file in $userFiles) { $userHashes[$file] = (Get-FileHash "$legacy/$file").Hash }
$null = Invoke-TestInstaller 'legacy' 'host'
Assert-File $legacy 'Unishade.exe'
Assert-File $legacy 'RobloxShadeHost.exe' $false
Assert-File $legacy 'RobloxShadeHost-Setup.exe' $false
foreach ($file in $userFiles) {
    if ((Get-FileHash "$legacy/$file").Hash -ne $userHashes[$file]) {
        throw "Upgrade changed the user's $file."
    }
}
$legacyManifest = Get-Content "$legacy/RobloxShadeHost-Setup.files"
if ($legacyManifest -notcontains 'Unishade.exe' -or $legacyManifest -notcontains 'legacy-component.txt' -or
    $legacyManifest -contains 'RobloxShadeHost.exe' -or $legacyManifest -contains 'RobloxShadeHost-Setup.exe') {
    throw 'Upgrade did not preserve and update the install manifest.'
}
if ((Invoke-Setup 'uninstall-legacy' @('--uninstall', '--dir', "`"$legacy`"")) -ne 0) {
    throw "Uninstall after upgrade failed. See $testRoot/uninstall-legacy.log"
}
Assert-File $legacy 'Unishade.exe' $false
Assert-File $legacy 'legacy-component.txt' $false
foreach ($file in $userFiles) { Assert-File $legacy $file }

# Every package on ReShade's list, downloaded by the installed Unishade, and the presets of the repository.
$effects = Invoke-TestInstaller 'effects' 'host,effects'
Assert-File $effects 'dxgi.dll' $false
Assert-File $effects 'reshade-shaders/Shaders/ReShade.fxh'
Assert-File $effects 'reshade-shaders/Shaders/FXShaders/AdaptiveTonemapper.fx'
Assert-File $effects 'reshade-shaders/Shaders/qUINT/qUINT_common.fxh'
if (-not (Get-ChildItem "$effects/reshade-shaders/Textures" -Filter *.png -Recurse | Select-Object -First 1)) {
    throw 'No textures were installed.'
}
foreach ($preset in Get-ChildItem "$repo/presets/*.ini" -Exclude downloads.ini) {
    if ((Get-FileHash "$effects/presets/$($preset.Name)").Hash -ne (Get-FileHash $preset.FullName).Hash) {
        throw "Installed preset $($preset.Name) does not match the repository."
    }
}
if ((Get-Content "$effects/CREDITS.txt" -Raw) -notmatch 'tiago@mouta.me') {
    throw 'Removal contact is missing from installed credits.'
}
if ((Get-Content "$effects/RobloxShadeHost-Setup.files") -notcontains 'reshade-shaders\Shaders\ReShade.fxh') {
    throw 'The effects Unishade installed are not in the file list.'
}

# Installing again keeps a preset the user changed.
Set-Content "$effects/presets/GenericPreset1.ini" 'Techniques=Edited@Edited.fx'
$null = Invoke-TestInstaller 'effects' 'host,effects'
if ((Get-Content "$effects/presets/GenericPreset1.ini" -Raw) -notmatch 'Edited') {
    throw 'Reinstall overwrote an edited preset.'
}

# A local list with only the standard effects reaches Unishade as a file:/// address.
$standardEffects = Join-Path $testRoot 'standard-effects.ini'
Set-Content $standardEffects -Encoding Ascii -Value @(
    '[00]', 'Required=1', 'PackageName=Standard effects', 'InstallPath=.\reshade-shaders\Shaders',
    'TextureInstallPath=.\reshade-shaders\Textures', 'DownloadUrl=https://github.com/crosire/reshade-shaders/archive/slim.zip')
$standard = Invoke-TestInstaller 'standard' 'host,effects' -Extra @('--effects-url', "`"$standardEffects`"")
Assert-File $standard 'reshade-shaders/Shaders/ReShade.fxh'
Assert-File $standard 'reshade-shaders/Shaders/qUINT/qUINT_common.fxh' $false

# Updating an installation of Unishade 0.6.3 removes what Setup installed for ReShade and the add-ons, which would
# otherwise load into Unishade.exe or sit unused, and keeps the user's files.
$old = Join-Path $testRoot 'old'
New-Item -ItemType Directory -Path "$old/presets" -Force | Out-Null
$leftovers = @('UnishadeUi.dll', 'dxgi.dll', 'ReShade-LICENSE.txt', 'EffectPackages.ini', 'onnxruntime.dll', 'DirectML.dll',
    'depth-anything-v2-small.onnx', 'nvngx_dlssnr.dll', 'renodx-dlss.addon64')
foreach ($file in @('Unishade.exe') + $leftovers) { Set-Content "$old/$file" 'installed by Setup 0.6.3' }
Set-Content "$old/RobloxShadeHost-Setup.files" ((@('Unishade.exe') + $leftovers) -join "`r`n")
Set-Content "$old/RobloxShadeHost.ini" "[Input]`nToggleKey=F8"
Set-Content "$old/ReShade.ini" "[GENERAL]`nPresetPath=.\presets\Custom.ini"
Set-Content "$old/ReShadePreset.ini" 'Techniques='
Set-Content "$old/games.ini" "[Games]`nCount=0"
Set-Content "$old/presets/Custom.ini" 'Techniques=Custom@Custom.fx'
$null = Invoke-TestInstaller 'old' 'host'
foreach ($file in $leftovers) { Assert-File $old $file $false }
foreach ($file in @('Unishade.exe', 'RobloxShadeHost.ini', 'ReShade.ini', 'ReShadePreset.ini', 'games.ini', 'presets/Custom.ini')) {
    Assert-File $old $file
}
if (Compare-Object (Get-Content "$old/RobloxShadeHost-Setup.files") $leftovers -IncludeEqual -ExcludeDifferent) {
    throw 'The file list still names what the update removed.'
}

# Uninstalling removes what Unishade made for itself and keeps the settings, presets, add-ons and effects the user
# added, unless asked to delete those too.
New-Item -ItemType Directory -Path "$effects/cache/effects", "$effects/Addons/Mine" -Force | Out-Null
foreach ($file in @('Unishade.ini', 'Unishade.log', 'Unishade.old.log', 'unishade.lock', 'cache/pipelines.bin', 'cache/effects/a.spv',
        'Addons/Mine.dll', 'Addons/Mine/data.bin', 'reshade-shaders/Shaders/Mine.fx', 'Screenshot.png')) {
    Set-Content "$effects/$file" 'made after Setup'
}
$effectsAll = Join-Path $testRoot 'effects-all'
Copy-Item $effects $effectsAll -Recurse
if ((Invoke-Setup 'uninstall' @('--uninstall', '--dir', "`"$effects`"")) -ne 0) {
    throw "Uninstall failed. See $testRoot/uninstall.log"
}
foreach ($file in @('Unishade.exe', 'CREDITS.txt', 'reshade-shaders/Shaders/ReShade.fxh', 'Unishade.log', 'Unishade.old.log', 'unishade.lock',
        'setup.lock', 'cache', 'RobloxShadeHost-Setup.files')) {
    Assert-File $effects $file $false
}
foreach ($file in @('Unishade.ini', 'presets/GenericPreset1.ini', 'reshade-shaders/Shaders/Mine.fx', 'Addons/Mine.dll', 'Screenshot.png')) {
    Assert-File $effects $file
}
# What is left is no longer an installation, so Setup does not delete it.
if ((Invoke-Setup 'uninstall-again' @('--uninstall', '--delete-user-files', '--dir', "`"$effects`"")) -ne 1) {
    throw 'Setup uninstalled from a folder it had already uninstalled from.'
}
Assert-File $effects 'Unishade.ini'

if ((Invoke-Setup 'uninstall-all' @('--uninstall', '--delete-user-files', '--dir', "`"$effectsAll`"")) -ne 0) {
    throw "Uninstall failed. See $testRoot/uninstall-all.log"
}
foreach ($file in @('Unishade.exe', 'Unishade.ini', 'games.ini', 'presets', 'reshade-shaders', 'Addons', 'cache', 'RobloxShadeHost-Setup.files')) {
    Assert-File $effectsAll $file $false
}
Assert-File $effectsAll 'Screenshot.png'

# --launch starts the installed Unishade when a silent run is done, which an update that Unishade installs itself
# relies on. Its log shows that it started, however long it runs on a machine without a graphics card.
$launched = Invoke-TestInstaller 'launch' 'host' $true @('--launch')
$started = $false
for ($i = 0; $i -lt 300 -and -not $started; $i++) {
    $started = Test-Path (Join-Path $launched 'Unishade.log')
    if (-not $started) { Start-Sleep -Milliseconds 100 }
}
$launchedExe = [IO.Path]::GetFullPath((Join-Path $launched 'Unishade.exe'))
Get-Process Unishade -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $launchedExe } | Stop-Process -Force
if (-not $started) { throw "--launch did not start Unishade. See $testRoot/launch.log" }

Write-Output "Installer checks passed. Test files and logs: $testRoot"
