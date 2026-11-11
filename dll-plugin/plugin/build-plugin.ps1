# Builds the Mod Settings Mod Manager plugin into plugin\out\Plugins (inside modsettings-dll\plugin only; never into any live
# folder). out\Plugins is wiped first, so the package holds exactly what this run built.
#   out\Plugins\ModSettingsPlugin.dll         the .NET loader (csc from .NET Framework 4.x, x64 like the manager)
#   out\Plugins\ModSettings\plugin.cfg        the launcher settings (shipped defaults)
#   out\Plugins\ModSettings\README.txt        this folder's README.md
# modsettings.dll is NOT built here: ..\dll\build.js builds it; ..\tools\package.js assembles the package.
#
# FrostyCore.dll / FrostySdk.dll are referenced IN PLACE from a Mod Manager folder: READ ONLY, nothing is copied from
# or written to that folder. Which folder: -ManagerDir, else %MMC_MODMANAGER_DIR%, else the first folder on the
# Desktop (as Windows records it) up to two levels down that holds MMCModManager.exe + FrostyCore.dll.
#   powershell -ExecutionPolicy Bypass -File plugin\build-plugin.ps1 [-ManagerDir <MMC_ModManager folder>]
param(
  [string]$ManagerDir = $env:MMC_MODMANAGER_DIR
)
$ErrorActionPreference = 'Stop'
$plug = $PSScriptRoot
. (Join-Path $plug 'find-manager.ps1')
$ManagerDir = Find-ManagerRefs $ManagerDir

$out = Join-Path $plug 'out'
$pl = Join-Path $out 'Plugins'
$sub = Join-Path $pl 'ModSettings'
if (Test-Path $pl) { Remove-Item -Recurse -Force $pl }
New-Item -ItemType Directory -Force $sub | Out-Null

$version = '0.1.0'
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc (.NET Framework 4.x)" }

$sources = Get-ChildItem (Join-Path $plug 'src\*.cs') | ForEach-Object { $_.FullName }
Write-Host "--- ModSettingsPlugin.dll (csc, x64, refs in place from $ManagerDir) ---"
& $csc /nologo /target:library /platform:x64 /optimize+ /warn:4 /warnaserror+ `
  "/out:$pl\ModSettingsPlugin.dll" `
  "/r:$ManagerDir\FrostyCore.dll" "/r:$ManagerDir\FrostySdk.dll" `
  $sources
if ($LASTEXITCODE -ne 0) { throw 'csc failed' }

# A managed assembly imports at most mscoree.dll (the CLR shim; an x64 image has no import table at all). Confirm that,
# so an accidental native dependency is caught.
$imports = objdump -p "$pl\ModSettingsPlugin.dll" | Select-String 'DLL Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
$extra = $imports | Where-Object { $_ -notin 'mscoree.dll', 'MSCOREE.DLL' }
if ($extra) { throw "ModSettingsPlugin.dll imports $($extra -join ', '); a managed plugin should import only mscoree.dll" }
Write-Host "imports: $(if ($imports) { $imports -join ', ' } else { 'none (a pure managed x64 image)' })"

Write-Host '--- plugin.cfg + README ---'
Copy-Item (Join-Path $plug 'plugin.cfg') $sub -Force
Copy-Item (Join-Path $plug 'README.md') (Join-Path $sub 'README.txt') -Force

Write-Host '--- package ---'
Get-ChildItem -Recurse $pl | Where-Object { -not $_.PSIsContainer } | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName.Substring($out.Length + 1) }
$hash = (Get-FileHash -Algorithm SHA256 "$pl\ModSettingsPlugin.dll").Hash.ToLowerInvariant()
Write-Host "sha256 ModSettingsPlugin.dll: $hash"
Write-Host "Mod Settings plugin $version built into $pl"
