# Packages a built NppSync.dll into the release ZIP and a double-click
# NSIS installer. Run from anywhere; paths are relative to the repo root.
#
#   powershell -File installer/package.ps1 -Version 1.2.0
#
# The installer is packaging only. It does not rebuild the plugin and it
# does not take a server address. To wrap the DLL already published in
# NotepadPlusPlusSync-v1.2.0-win64.zip (no new product version):
#
#   powershell -File installer/package.ps1 -Version 1.2.0 -BuildDir <extracted>\NppSync -InstallerOnly
#
# That writes dist\NotepadPlusPlusSync-v1.2.0-win64-setup.exe
# NSIS 3 must be installed (makensis on PATH). https://nsis.sourceforge.io/
param(
    [Parameter(Mandatory=$true)][string]$Version,
    [string]$BuildDir = "plugin/build/Release",
    [string]$OutDir = "dist",
    [switch]$SkipInstaller,
    [switch]$InstallerOnly
)

$ErrorActionPreference = "Stop"

if ($SkipInstaller -and $InstallerOnly) {
    throw "Use only one of -SkipInstaller or -InstallerOnly."
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $RepoRoot

function Find-Makensis {
    $cmd = Get-Command makensis -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    $candidates = @()
    if (${env:ProgramFiles(x86)}) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"
    }
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles "NSIS\makensis.exe"
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Write-Checksum([string]$Path) {
    $name = Split-Path -Leaf $Path
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLower()
    "$hash  $name" | Out-File -Encoding ascii -FilePath "$Path.sha256"
    Write-Host "SHA256: $hash  $name"
}

$dll = Join-Path $BuildDir "NppSync.dll"
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Plugin DLL not found at $dll - build first, or point -BuildDir at the NppSync folder from the release ZIP."
}
$dll = (Resolve-Path -LiteralPath $dll).Path

$depsDir = Join-Path $BuildDir "deps"
$hasDeps = $false
if (Test-Path -LiteralPath $depsDir) {
    $depFiles = @(Get-ChildItem -LiteralPath $depsDir -Recurse -File -ErrorAction SilentlyContinue)
    if ($depFiles.Count -gt 0) { $hasDeps = $true }
}
if ($hasDeps) {
    $depsDir = (Resolve-Path -LiteralPath $depsDir).Path
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outFull = (Resolve-Path -LiteralPath $OutDir).Path
$license = Join-Path $RepoRoot "LICENSE"
if (-not (Test-Path -LiteralPath $license)) { throw "LICENSE not found at $license" }

if (-not $InstallerOnly) {
    $staging = Join-Path $outFull "staging"
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path "$staging/NppSync" | Out-Null

    # Layout mirrors what Notepad++ expects under <NPP>\plugins\NppSync\
    Copy-Item -LiteralPath $dll -Destination "$staging/NppSync/NppSync.dll"
    if ($hasDeps) {
        Copy-Item -LiteralPath $depsDir -Destination "$staging/NppSync/deps" -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $RepoRoot "README.md") -Destination "$staging/README.txt"
    Copy-Item -LiteralPath $license -Destination "$staging/LICENSE.txt"

    $zipName = "NotepadPlusPlusSync-v$Version-win64.zip"
    $zipPath = Join-Path $outFull $zipName
    Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$zipPath.sha256" -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path "$staging/*" -DestinationPath $zipPath
    Remove-Item -LiteralPath $staging -Recurse -Force
    Write-Host "Created $zipPath"
    Write-Checksum $zipPath
}

if (-not $SkipInstaller) {
    $makensis = Find-Makensis
    if (-not $makensis) {
        throw "makensis was not found. Install NSIS 3 so makensis is on PATH (https://nsis.sourceforge.io/), or pass -SkipInstaller to build only the ZIP."
    }

    $versionVi = "0.0.0.0"
    if ($Version -match '^\d+\.\d+\.\d+$') {
        $versionVi = "$Version.0"
    } elseif ($Version -match '^\d+\.\d+\.\d+\.\d+$') {
        $versionVi = $Version
    }

    $exeName = "NotepadPlusPlusSync-v$Version-win64-setup.exe"
    $exePath = Join-Path $outFull $exeName
    Remove-Item -LiteralPath $exePath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$exePath.sha256" -Force -ErrorAction SilentlyContinue

    $nsi = Join-Path $RepoRoot "installer\NppSync.nsi"
    $makensisArgs = @(
        "-DVERSION=$Version",
        "-DVERSION_VI=$versionVi",
        "-DPLUGIN_DLL=$dll",
        "-DLICENSE_FILE=$license",
        "-DOUTFILE=$exePath"
    )
    if ($hasDeps) {
        $makensisArgs += "-DINCLUDE_DEPS"
        $makensisArgs += "-DPLUGIN_DEPS=$depsDir"
    }
    $makensisArgs += $nsi

    & $makensis @makensisArgs
    if ($LASTEXITCODE -ne 0) { throw "makensis failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $exePath)) { throw "Installer was not created at $exePath" }
    Write-Host "Created $exePath"
    Write-Checksum $exePath
}
