param(
  [Parameter(Mandatory = $true)][string]$Name,   # A, B, C ...: one copy per test instance
  [string]$Root = "C:\HSTest",
  [switch]$Refresh                               # copy his mod folder, save and settings again (after an install or a game update)
)
# A test copy of Half Sword that never shares a file the game or the mod WRITES with his own game (docs/pctest.md).
#   <Root>\<Name>\game  the game: files of 50 MB and more are hard links to his install (paks, exe: read only,
#                       no disk used), everything else a real copy; the ue4ss folder (mod dll, ini, log,
#                       commands, replays) is always a real copy.
#   <Root>\<Name>\user  the game's -UserDir: its own Saved\SaveGames and Saved\Config (picture settings).
# Reads his files, never writes them. Runs only while no game runs from this copy.
$ErrorActionPreference = "Stop"
$src = "C:\Games\Steam\steamapps\common\Half Sword"
$hisSaved = "$env:LOCALAPPDATA\HalfswordUE5\Saved"
$copy = Join-Path $Root $Name
$game = "$copy\game"
$user = "$copy\user"
$here = Split-Path $MyInvocation.MyCommand.Path -Parent

if (Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($game, [StringComparison]::OrdinalIgnoreCase) }) {
  throw "a game runs from $game; not touching it"
}
if ((Split-Path $src -Qualifier) -ne (Split-Path $Root -Qualifier)) { throw "hard links need $Root on the same drive as $src" }

$big = 50MB
$ue4ss = "HalfswordUE5\Binaries\Win64\ue4ss"
$fresh = -not (Test-Path "$game\HalfSwordUE5.exe")
if ($fresh -or $Refresh) {
  New-Item -ItemType Directory -Force $game | Out-Null
  $linked = 0; $copied = 0
  foreach ($f in Get-ChildItem $src -Recurse -File) {
    $rel = $f.FullName.Substring($src.Length + 1)
    if ($rel.StartsWith($ue4ss + "\", [StringComparison]::OrdinalIgnoreCase)) { continue }
    $dst = Join-Path $game $rel
    New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
    if (Test-Path $dst) {
      # A file from before an update stays linked to the old version: make it again.
      if ((Get-Item $dst).Length -eq $f.Length -and (Get-Item $dst).LastWriteTimeUtc -eq $f.LastWriteTimeUtc) { continue }
      Remove-Item $dst -Force
    }
    if ($f.Length -ge $big) { New-Item -ItemType HardLink -Path $dst -Target $f.FullName | Out-Null; $linked++ }
    else { Copy-Item $f.FullName $dst; $copied++ }
  }
  # The mod folder: always a real copy (the runner puts the test dll and ini in it).
  if (Test-Path "$game\$ue4ss") { Remove-Item "$game\$ue4ss" -Recurse -Force }
  Copy-Item "$src\$ue4ss" "$game\$ue4ss" -Recurse
  $m = "$game\$ue4ss\Mods\HalfSwordVR"
  foreach ($junk in "HalfSwordVR_commands.txt", "replays", "logs", "save_backups") { if (Test-Path "$m\$junk") { Remove-Item "$m\$junk" -Recurse -Force } }
  Get-ChildItem $m -Filter "test-save-*.sav" -ErrorAction SilentlyContinue | Remove-Item -Force
  Write-Host "game: $linked linked, $copied copied, ue4ss copied"
}

# Its own save and settings, from his (read only).
New-Item -ItemType Directory -Force "$user\Saved\SaveGames", "$user\Saved\Config\Windows" | Out-Null
foreach ($s in "GameProgress.sav", "Settings.sav") {
  if ($fresh -or $Refresh -or -not (Test-Path "$user\Saved\SaveGames\$s")) { Copy-Item "$hisSaved\SaveGames\$s" "$user\Saved\SaveGames\$s" -Force }
}
# Lowest picture: every quality group 0, a small window, and the render-only Engine.ini.
$gus = @(Get-Content "$hisSaved\Config\Windows\GameUserSettings.ini" -ErrorAction SilentlyContinue)
$set = [ordered]@{
  "sg.ResolutionQuality" = "50"; "sg.ViewDistanceQuality" = $null; "sg.AntiAliasingQuality" = "0"; "sg.ShadowQuality" = "0"
  "sg.GlobalIlluminationQuality" = "0"; "sg.ReflectionQuality" = "0"; "sg.PostProcessQuality" = "0"; "sg.TextureQuality" = "0"
  "sg.EffectsQuality" = "0"; "sg.FoliageQuality" = $null; "sg.ShadingQuality" = "0"; "sg.LandscapeQuality" = "0"
  "FullscreenMode" = "2"; "LastConfirmedFullscreenMode" = "2"; "PreferredFullscreenMode" = "2"
  "ResolutionSizeX" = "1280"; "ResolutionSizeY" = "720"; "bUseVSync" = "False"; "FrameRateLimit" = "0.000000"
}
# sg.ViewDistanceQuality and sg.FoliageQuality stay as he has them: they change what is drawn far away and
# which foliage exists, and foliage can carry collision (see testcopy-Engine.ini).
$out = @()
$seen = @{}
foreach ($l in $gus) {
  $k = ($l -split "=", 2)[0]
  if ($set.Contains($k) -and $null -ne $set[$k]) { $out += "$k=$($set[$k])"; $seen[$k] = 1 } else { $out += $l }
}
$missing = @($set.Keys | Where-Object { -not $seen[$_] -and $null -ne $set[$_] })
if ($missing) { throw "his GameUserSettings.ini has no " + ($missing -join ", ") + ": check it before making a copy" }
Set-Content "$user\Saved\Config\Windows\GameUserSettings.ini" $out -Encoding ascii
Copy-Item "$here\testcopy-Engine.ini" "$user\Saved\Config\Windows\Engine.ini" -Force
"$(Get-Date -Format o) from $src" | Set-Content "$copy\made.txt"
Write-Host "test copy $copy ready (user dir $user)"
