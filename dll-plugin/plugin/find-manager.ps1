# find-manager.ps1 (dot-sourced): the Mod Manager folder whose FrostyCore.dll / FrostySdk.dll the Press Box plugin,
# its harness and the app's install tests compile against. READ ONLY: nothing is ever written there.
# Order: the given folder, %MMC_MODMANAGER_DIR%, then a search of the Desktop as Windows records it (two levels down)
# for a folder holding MMCModManager.exe + FrostyCore.dll (the newest one when there are several).
function Find-ManagerRefs([string]$given) {
  $ok = { param($d) $d -and (Test-Path -LiteralPath (Join-Path $d 'FrostyCore.dll')) -and (Test-Path -LiteralPath (Join-Path $d 'FrostySdk.dll')) }
  if ($given) {
    if (& $ok $given) { return (Resolve-Path -LiteralPath $given).Path }
    throw "FrostyCore.dll / FrostySdk.dll not found in '$given' - pass -ManagerDir <the MMC_ModManager folder> (or set MMC_MODMANAGER_DIR)."
  }
  if ($env:MMC_MODMANAGER_DIR -and (& $ok $env:MMC_MODMANAGER_DIR)) { return $env:MMC_MODMANAGER_DIR }
  $desk = [Environment]::GetFolderPath('Desktop')
  $hits = @()
  if ($desk -and (Test-Path -LiteralPath $desk)) {
    $hits = @(Get-ChildItem -LiteralPath $desk -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $_; Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue } |
      Where-Object { (Test-Path -LiteralPath (Join-Path $_.FullName 'MMCModManager.exe')) -and (& $ok $_.FullName) } |
      Sort-Object { (Get-Item -LiteralPath (Join-Path $_.FullName 'FrostyCore.dll')).LastWriteTime } -Descending)
  }
  if ($hits.Count -gt 0) { return $hits[0].FullName }
  throw 'No Mod Manager folder found to compile against - pass -ManagerDir <the MMC_ModManager folder> (or set MMC_MODMANAGER_DIR).'
}
