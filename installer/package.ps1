# Packages the plugin build output into the release ZIP and (optionally) an
# NSIS installer. Run from the repo root after building:
#
#   cmake -B plugin/build -S plugin -A x64
#   cmake --build plugin/build --config Release
#   powershell -File installer/package.ps1 -Version 1.0.0
#
# Produces: dist/NotepadPlusPlusSync-v<Version>-win64.zip (+ .sha256)
param(
    [Parameter(Mandatory=$true)][string]$Version,
    [string]$BuildDir = "plugin/build/Release",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"

$dll = Join-Path $BuildDir "NppSync.dll"
if (-not (Test-Path $dll)) { throw "Plugin DLL not found at $dll - build first." }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$staging = Join-Path $OutDir "staging"
Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$staging/NppSync" | Out-Null

# Layout mirrors what Notepad++ expects under <NPP>\plugins\NppSync\
Copy-Item $dll "$staging/NppSync/NppSync.dll"
Copy-Item "README.md" "$staging/README.txt"
Copy-Item "LICENSE" "$staging/LICENSE.txt"

$zipName = "NotepadPlusPlusSync-v$Version-win64.zip"
$zipPath = Join-Path $OutDir $zipName
Remove-Item $zipPath -ErrorAction SilentlyContinue
Compress-Archive -Path "$staging/*" -DestinationPath $zipPath

# Checksums (verified on the release page / by users).
$hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLower()
"$hash  $zipName" | Out-File -Encoding ascii "$zipPath.sha256"

Remove-Item -Recurse -Force $staging
Write-Host "Created $zipPath"
Write-Host "SHA256: $hash"
