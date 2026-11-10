# Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
# Packs and signs RPCS3 for Game Vault on the Game Vault PC from the cloud build's unsigned folder
# (.github/workflows/build-gamevault.yml, artifact "rpcs3-gamevault-layout"). The signing key never leaves this PC.
# Usage: pack_gamevault.ps1 <run id> <version, e.g. 0.1.35.1600>   -> dist\RPCS3ForGameVault_<version>_x64.appx
param([Parameter(Mandatory)][string]$Run, [Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$work = Join-Path $repo "dist\layout-$Run"
$sdk = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
$gv = "C:\Users\GamerDad\XboxEmu\GameVault"
if (-not (Test-Path $work)) {
    New-Item -ItemType Directory -Force $work | Out-Null
    gh run download $Run -R skitzellez/RPCS3-GameVault -n rpcs3-gamevault-layout -D $work
    if ($LASTEXITCODE) { throw "download failed" }
}
# libsmb2 (LGPL-2.1) for network games: the same smb2.dll Game Vault ships (loaded only when one starts)
Copy-Item -Force "$gv\pkg\smb2.dll" "$work\smb2.dll"
Copy-Item -Force "C:\Users\GamerDad\XboxEmu\deps\libsmb2\COPYING" "$work\libsmb2-LICENSE.txt"
# the manifest from this checkout (it may be newer than the one the build used), with the version asked for
$manifest = Get-Content "$repo\uwp\package\AppxManifest.xml" -Raw
$manifest = $manifest -replace '(<Identity [^>]*Version=")[^"]*(")', "`${1}$Version`${2}"
[IO.File]::WriteAllText("$work\AppxManifest.xml", $manifest)
New-Item -ItemType Directory -Force "$repo\dist" | Out-Null
$out = "$repo\dist\RPCS3ForGameVault_${Version}_x64.appx"
& "$sdk\makeappx.exe" pack /o /d $work /p $out | Select-Object -Last 2
if ($LASTEXITCODE) { throw "makeappx failed" }
$pass = & powershell -NoProfile -ExecutionPolicy Bypass -File "$gv\pfx_password.ps1"
& "$sdk\signtool.exe" sign /fd SHA256 /a /f "$gv\cert\GameVault.pfx" /p $pass $out | Select-Object -Last 1
if ($LASTEXITCODE) { throw "signing failed" }
Get-Item $out | Select-Object Name, Length
