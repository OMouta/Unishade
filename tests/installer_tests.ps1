param(
    [string]$Setup = (Get-ChildItem "$PSScriptRoot/../build/installer/Unishade-Setup-*.exe" |
        Sort-Object LastWriteTime | Select-Object -Last 1).FullName,
    [switch]$DownloadDLSS,
    [switch]$DownloadDepth,
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

# Exit code 2 means Setup installed, but left out an effect package, a preset or the add-on.
function Invoke-TestInstaller(
    [string]$Name, [string]$Components, [bool]$AcceptLicense = $true,
    [bool]$ExpectSuccess = $AcceptLicense, [string[]]$Extra = @(), [int]$ExpectExitCode = 0
) {
    $destination = Join-Path $testRoot $Name
    $arguments = @('--portable', '--components', $Components, '--dir', "`"$destination`"") + $Extra
    if ($AcceptLicense) { $arguments += '--accept-reshade-license' }
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
Assert-File $hostOnly 'UnishadeUi.dll'
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

$null = Invoke-TestInstaller 'no-license' 'reshade' $false
$reshade = Invoke-TestInstaller 'reshade' 'reshade,presets'
Assert-File $reshade 'dxgi.dll'
Assert-File $reshade 'ReShade-LICENSE.txt'
Assert-File $reshade 'renodx-dlss.addon64' $false
Assert-File $reshade 'onnxruntime.dll' $false
# ReShade's installer leaves these next to the exe; only its DLL and settings are installed.
Assert-File $reshade 'ReShadePreset.ini' $false
Assert-File $reshade 'ReShade.log' $false
Assert-File $reshade 'reshade-shaders/Shaders/ReShade.fxh'
Assert-File $reshade 'reshade-shaders/Shaders/FXShaders/AdaptiveTonemapper.fx'
Assert-File $reshade 'reshade-shaders/Shaders/qUINT/qUINT_common.fxh'
if (-not (Get-ChildItem "$reshade/reshade-shaders/Textures" -Filter *.png -Recurse | Select-Object -First 1)) {
    throw 'No textures were installed.'
}
function Assert-SearchPaths([string]$Ini) {
    if ($Ini -notmatch '(?m)^EffectSearchPaths=\.\\reshade-shaders\\Shaders\\\*\*\r?$' -or
        $Ini -notmatch '(?m)^TextureSearchPaths=\.\\reshade-shaders\\Textures\\\*\*\r?$') {
        throw 'ReShade.ini does not have the expected effect search paths.'
    }
}

$reshadeIni = Get-Content "$reshade/ReShade.ini" -Raw
Assert-SearchPaths $reshadeIni
if ($reshadeIni -notmatch '(?m)^PresetPath=\.\\presets\\ReShadePreset\.ini\r?$') {
    throw 'The initial preset browser path is not in the presets folder.'
}
$iniBytes = [IO.File]::ReadAllBytes("$reshade/ReShade.ini")
if ($iniBytes.Length -lt 3 -or $iniBytes[0] -ne 0xEF -or $iniBytes[1] -ne 0xBB -or $iniBytes[2] -ne 0xBF) {
    throw 'ReShade.ini lost its byte order mark.'
}
if (([regex]::Matches($reshadeIni, '(?m)^\[GENERAL\]')).Count -ne 1) {
    throw 'ReShade.ini has a duplicated GENERAL section.'
}
foreach ($preset in Get-ChildItem "$repo/presets/*.ini" -Exclude downloads.ini) {
    if ((Get-FileHash "$reshade/presets/$($preset.Name)").Hash -ne (Get-FileHash $preset.FullName).Hash) {
        throw "Installed preset $($preset.Name) does not match the repository."
    }
}
if ((Get-Content "$reshade/CREDITS.txt" -Raw) -notmatch 'tiago@mouta.me') {
    throw 'Removal contact is missing from installed credits.'
}

Add-Content "$reshade/ReShade.ini" "`n[InstallerTest]`nPreserve=1"
$originalHash = (Get-FileHash "$reshade/ReShade.ini").Hash
Set-Content "$reshade/presets/GenericPreset1.ini" 'Techniques=Edited@Edited.fx'
Set-Content "$reshade/ReShadePreset.ini" 'Techniques=Mine@Mine.fx'
$null = Invoke-TestInstaller 'reshade' 'reshade,presets'
if ((Get-FileHash "$reshade/ReShade.ini").Hash -ne $originalHash) {
    throw 'Reinstall changed the existing ReShade configuration.'
}
if ((Get-Content "$reshade/ReShadePreset.ini" -Raw) -notmatch 'Mine') {
    throw "Reinstall overwrote the user's ReShadePreset.ini."
}

# An install from an earlier installer kept ReShade's doubled search paths. Reinstalling repairs
# those lines and nothing else.
$brokenIni = $reshadeIni -replace '(?m)^((?:Effect|Texture)SearchPaths=.*\\\*\*)(?=\r?$)', '$1\**'
$brokenIni += "`n[InstallerTest]`nPreserve=1`n"
[IO.File]::WriteAllText("$reshade/ReShade.ini", $brokenIni.TrimStart([char]0xFEFF), [Text.UTF8Encoding]::new($true))
$null = Invoke-TestInstaller 'reshade' 'reshade,presets'
$repairedIni = Get-Content "$reshade/ReShade.ini" -Raw
Assert-SearchPaths $repairedIni
if ($repairedIni -notmatch '(?m)^Preserve=1' -or ([regex]::Matches($repairedIni, '(?m)^\[GENERAL\]')).Count -ne 1) {
    throw 'Repairing the search paths did not preserve the rest of ReShade.ini.'
}
if ((Get-Content "$reshade/presets/GenericPreset1.ini" -Raw) -notmatch 'Edited') {
    throw 'Reinstall overwrote an edited preset.'
}

$missingManifests = @(
    '--dlss5-manifest', 'https://github.com/OMouta/Unishade/releases/download/dlss5-assets/not-present.ini',
    '--depth-manifest', 'https://github.com/OMouta/Unishade/releases/download/depth-assets/not-present.ini')
# Both add-ons can be picked. One that is skipped keeps the files it had.
New-Item -ItemType Directory -Path "$testRoot/missing-addons" -Force | Out-Null
Set-Content "$testRoot/missing-addons/depth-anything-v2-small.onnx" 'kept'
$missing = Invoke-TestInstaller 'missing-addons' 'reshade,dlss5,depth' -Extra $missingManifests -ExpectExitCode 2
Assert-File $missing 'Unishade.exe'
Assert-File $missing 'dxgi.dll'
Assert-File $missing 'nvngx_dlssnr.dll' $false
Assert-File $missing 'renodx-dlss.addon64' $false
Assert-File $missing 'onnxruntime.dll' $false
Assert-File $missing 'DirectML.dll' $false
Assert-File $missing 'depth-anything-v2-small.onnx'
$missingLog = Get-Content "$testRoot/missing-addons.log" -Raw
if ($missingLog -notmatch 'DLSS5 skipped:' -or $missingLog -notmatch 'Depth estimation skipped:') {
    throw 'Missing add-on downloads were not reported.'
}

# Each add-on file must match the SHA-256 in the download list. This list points nvngx_dlssnr.dll at a small file of
# the depth model's repository, which does not match, so DLSS5 is skipped without downloading its real files. A local
# effect list with only the standard effects keeps this quick.
$standardEffects = Join-Path $testRoot 'standard-effects.ini'
Set-Content $standardEffects -Encoding Ascii -Value @(
    '[00]', 'Required=1', 'PackageName=Standard effects', 'InstallPath=.\reshade-shaders\Shaders',
    'TextureInstallPath=.\reshade-shaders\Textures', 'DownloadUrl=https://github.com/crosire/reshade-shaders/archive/slim.zip')
$otherFile = 'https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/config.json'
$dlss5List = Get-Content "$repo/vendor/dlss5/downloads.ini" -Raw
$wrongDlss5List = $dlss5List -replace '(?m)^url=.*/nvngx_dlssnr\.dll(?=\r?$)', "url=$otherFile"
if ($wrongDlss5List -eq $dlss5List) { throw 'Could not change the address in the DLSS5 download list.' }
Set-Content "$testRoot/wrong-dlss5.ini" -Encoding Ascii -Value $wrongDlss5List
$mismatched = Invoke-TestInstaller 'mismatched-dlss5' 'reshade,dlss5' -ExpectExitCode 2 -Extra @(
    '--effects-url', "`"$standardEffects`"", '--dlss5-manifest', "`"$testRoot/wrong-dlss5.ini`"")
Assert-File $mismatched 'dxgi.dll'
Assert-File $mismatched 'reshade-shaders/Shaders/ReShade.fxh'
Assert-File $mismatched 'nvngx_dlssnr.dll' $false
Assert-File $mismatched 'renodx-dlss.addon64' $false
if ((Get-Content "$testRoot/mismatched-dlss5.log" -Raw) -notmatch 'DLSS5 skipped:.*does not match its checksum') {
    throw 'DLSS5 was not skipped although a download does not match the SHA-256 in its download list.'
}

if ($DownloadDLSS) {
    # Installing DLSS5 without depth estimation removes the depth files.
    New-Item -ItemType Directory -Path "$testRoot/full" -Force | Out-Null
    Set-Content "$testRoot/full/depth-anything-v2-small.onnx" 'stale'
    $full = Invoke-TestInstaller 'full' 'reshade,dlss5'
    Assert-File $full 'depth-anything-v2-small.onnx' $false
    Assert-File $full 'nvngx_dlssnr.dll'
    Assert-File $full 'renodx-dlss.addon64'
    $manifest = Get-Content "$repo/vendor/dlss5/downloads.ini" -Raw
    foreach ($file in @('nvngx_dlssnr.dll', 'renodx-dlss.addon64')) {
        $pattern = '(?ms)^\[' + [regex]::Escape($file) + '\]\r?\n.*?^sha256=([a-f0-9]{64})'
        $expectedHash = [regex]::Match($manifest, $pattern).Groups[1].Value
        if ((Get-FileHash "$full/$file").Hash -ne $expectedHash) {
            throw "$file does not match the repository manifest."
        }
    }
}

if ($DownloadDepth) {
    # A download that does not match its hash also skips the add-on. This one points onnxruntime.dll at another
    # file of the model's repository.
    $depthList = Get-Content "$repo/vendor/depth/downloads.ini" -Raw
    $tamperedList = $depthList -replace '(?m)^url=.*/onnxruntime\.dll(?=\r?$)', "url=$otherFile"
    if ($tamperedList -eq $depthList) { throw 'Could not change the address in the depth download list.' }
    Set-Content "$testRoot/tampered-depth.ini" -Encoding Ascii -Value $tamperedList
    $tampered = Invoke-TestInstaller 'tampered-depth' 'reshade,depth' -ExpectExitCode 2 -Extra @(
        '--effects-url', "`"$standardEffects`"", '--depth-manifest', "`"$testRoot/tampered-depth.ini`"")
    Assert-File $tampered 'onnxruntime.dll' $false
    if ((Get-Content "$testRoot/tampered-depth.log" -Raw) -notmatch 'Depth estimation skipped:.*does not match its checksum') {
        throw 'Depth estimation was not skipped although a download does not match its hash.'
    }

    # Installing depth estimation without DLSS5 removes the DLSS5 files.
    New-Item -ItemType Directory -Path "$testRoot/depth" -Force | Out-Null
    Set-Content "$testRoot/depth/renodx-dlss.addon64" 'stale'
    $depth = Invoke-TestInstaller 'depth' 'reshade,depth'
    Assert-File $depth 'renodx-dlss.addon64' $false
    $manifest = Get-Content "$repo/vendor/depth/downloads.ini" -Raw
    foreach ($file in @('onnxruntime.dll', 'DirectML.dll', 'depth-anything-v2-small.onnx')) {
        Assert-File $depth $file
        $pattern = '(?ms)^\[' + [regex]::Escape($file) + '\]\r?\n.*?^sha256=([a-f0-9]{64})'
        $expectedHash = [regex]::Match($manifest, $pattern).Groups[1].Value
        if ((Get-FileHash "$depth/$file").Hash -ne $expectedHash) {
            throw "$file does not match the repository manifest."
        }
    }
}

# Uninstalling keeps ReShade settings and presets unless asked to delete them too.
if ((Invoke-Setup 'uninstall' @('--uninstall', '--dir', "`"$reshade`"")) -ne 0) {
    throw "Uninstall failed. See $testRoot/uninstall.log"
}
foreach ($file in @('Unishade.exe', 'dxgi.dll', 'CREDITS.txt', 'reshade-shaders')) { Assert-File $reshade $file $false }
Assert-File $reshade 'ReShade.ini'
Assert-File $reshade 'presets/GenericPreset1.ini'
# What is left is no longer an installation, so Setup does not delete it.
if ((Invoke-Setup 'uninstall-again' @('--uninstall', '--delete-user-files', '--dir', "`"$reshade`"")) -ne 1) {
    throw 'Setup uninstalled from a folder it had already uninstalled from.'
}
Assert-File $reshade 'ReShade.ini'

# Deleting the user's files too removes the settings, logs, presets and reshade-shaders, and leaves other files.
New-Item -ItemType Directory -Path "$missing/presets/Game" -Force | Out-Null
Set-Content "$missing/presets/Game/Mine.ini" 'Techniques=Mine@Mine.fx'
Set-Content "$missing/reshade-shaders/Shaders/Mine.fx" '// added by the user'
Set-Content "$missing/ReShadePreset.ini" 'Techniques='
Set-Content "$missing/RobloxShadeHost.ini" "[Input]`nToggleKey=F8"
Set-Content "$missing/games.ini" "[Games]`nCount=0"
Set-Content "$missing/ReShade.log" 'log'
Set-Content "$missing/Unishade.log" 'log'
Set-Content "$missing/Screenshot.png" 'not created by Setup'
if ((Invoke-Setup 'uninstall-all' @('--uninstall', '--delete-user-files', '--dir', "`"$missing`"")) -ne 0) {
    throw "Uninstall failed. See $testRoot/uninstall-all.log"
}
foreach ($file in @('Unishade.exe', 'dxgi.dll', 'ReShade.ini', 'ReShadePreset.ini', 'RobloxShadeHost.ini', 'games.ini', 'ReShade.log',
        'Unishade.log', 'presets', 'reshade-shaders', 'RobloxShadeHost-Setup.files')) {
    Assert-File $missing $file $false
}
Assert-File $missing 'Screenshot.png'

Write-Output "Installer checks passed. Test files and logs: $testRoot"
