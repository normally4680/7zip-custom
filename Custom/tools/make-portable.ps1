# Build the portable 7zip-custom package from _out\x64.
# Used both locally and by .github/workflows/release.yml.
param(
    [string]$OutDir = "dist",
    [string]$Version = $env:GITHUB_REF_NAME
)

$ErrorActionPreference = "Stop"

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$bin = Join-Path $repo "_out\x64"
$assets = Join-Path $repo "Custom\portable"
$out = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repo $OutDir }
$stage = Join-Path $out "7zip-custom-portable"

if (-not $Version) { $Version = Get-Date -Format "yyyy-MM-dd" }
$Version = $Version -replace '[^\w\.\-]', '-'

$files = @("7zFM.exe", "7zG.exe", "7z.exe", "7z.dll", "7zVault.exe", "7zVaultUi.dll")

Write-Host "== staging portable package ($Version)"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

foreach ($f in $files) {
    $src = Join-Path $bin $f
    if (-not (Test-Path $src)) { throw "missing build output: $src (run build-msvc.bat first)" }
    Copy-Item $src (Join-Path $stage $f)
}

$static = @(
    "7zVault.exe.manifest",
    "config.ini"
)
# "使用说明.txt" built from code points so this script stays ASCII-only
# (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
$static += (-join ([char[]](0x4F7F, 0x7528, 0x8BF4, 0x660E))) + ".txt"
foreach ($f in $static) {
    Copy-Item (Join-Path $assets $f) (Join-Path $stage $f)
}
Copy-Item (Join-Path $repo "DOC\License.txt") (Join-Path $stage "License.txt")
Copy-Item (Join-Path $repo "Custom\tools\diagnose.bat") (Join-Path $stage "diagnose.bat")

New-Item -ItemType Directory -Path (Join-Path $stage "Lang") | Out-Null
Copy-Item (Join-Path $repo "Custom\lang\zh-cn.txt") (Join-Path $stage "Lang\zh-cn.txt")

# Empty data\ marks portable mode (vault + logs live here).
New-Item -ItemType Directory -Path (Join-Path $stage "data") | Out-Null

# ------------------------------------------------------------------- zip
$zip = Join-Path $out "7zip-custom-portable-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }

$sevenZip = Join-Path $bin "7z.exe"
Push-Location $out
try {
    & $sevenZip a -tzip -mx=9 (Split-Path $zip -Leaf) "7zip-custom-portable" -bd -bso0 -bsp0
    if ($LASTEXITCODE -ne 0) { throw "7z.exe failed to create $zip" }
}
finally {
    Pop-Location
}

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$shaFile = "$zip.sha256.txt"
"$hash  $(Split-Path $zip -Leaf)" | Set-Content -Path $shaFile -Encoding ASCII

Write-Host ""
Write-Host "package : $zip"
Write-Host "size    : $((Get-Item $zip).Length) bytes"
Write-Host "sha256  : $hash"
