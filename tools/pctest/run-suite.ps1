param(
  [Parameter(Mandatory = $true)][string]$List,     # one case a line: name|steps file|extra ini keys (k=v;k=v), '#' = comment
  [Parameter(Mandatory = $true)][string]$Dll,      # the test build's main.dll
  [Parameter(Mandatory = $true)][string]$Master,   # backup tag: private\{main.dll,ini,save,settings}.bak-<tag> = what is installed now
  [string]$Out = "",                               # results folder (default private\suite-<list name>-<UTC time>)
  [int]$CaseSeconds = 100,                         # a case that has not ended by then is stopped
  [int]$PerLaunch = 40,                            # at most this many cases in one launch
  [switch]$Fresh,                                  # every case in its own launch (the old way; for comparisons)
  [switch]$NoConfirm,                              # don't re-run failed cases in their own launch
  [string]$BadSpawn = "468,1"                      # spawn point(s) to roll again, "x,y;x,y" (30 cm), "" = none
)
# Unattended Half Sword test cases, many to a launch (docs/pctest.md).
# - Starts only if no game runs and the installed dll, ini, save and settings equal the master backup.
# - Stops at once if a game it did not start appears or <Out>\STOP exists (SteamVR alone does not stop it).
# - Stops only the Half Sword processes it started itself, by id.
# - Restores exactly the master backup at the end, and never underneath someone else's game.
# Between cases the mod's "hsvr.case <name>" opens the test room again and puts every made-up input
# back to how a launch starts it. A case that fails (or times out) is run again in a launch of its own,
# so a failure that only comes from sharing a launch shows up as "shared-only".
$ErrorActionPreference = "Stop"
$game = "C:\Games\Steam\steamapps\common\Half Sword"
$mod = "$game\HalfswordUE5\Binaries\Win64\ue4ss\Mods\HalfSwordVR"
$priv = "C:\Users\matth\Documents\Codex\HalfSwordDecomp\private"
$saves = "$env:LOCALAPPDATA\HalfswordUE5\Saved\SaveGames"
$logPath = "$mod\HalfSwordVR.log"
$cmdPath = "$mod\HalfSwordVR_commands.txt"
$replays = "$mod\replays"
if (-not $Out) { $Out = "$priv\suite-" + [IO.Path]::GetFileNameWithoutExtension($List) + "-" + (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHHmmss") + "Z" }
function Get-Sha1($f) { (Get-FileHash $f -Algorithm SHA1).Hash.ToLower() }
$files = @(
  @{ live = "$mod\dlls\main.dll";      bak = "$priv\main.dll.bak-$Master" },
  @{ live = "$mod\HalfSwordVR.ini";    bak = "$priv\ini.bak-$Master" },
  @{ live = "$saves\GameProgress.sav"; bak = "$priv\save.bak-$Master" },
  @{ live = "$saves\Settings.sav";     bak = "$priv\settings.bak-$Master" })
$vrGames = "HalfswordUE5*","BladeAndSorcery*","SaintsAndSinners*","TWD*","Boneworks*","BONELAB*","VRChat*","Beat Saber*","HLA*","hlvr*","Pavlov*"
$bad = @()
foreach ($b in ($BadSpawn -split ";")) { if ($b -match "^\s*(-?\d+)\s*,\s*(-?\d+)\s*$") { $bad += , @([int]$matches[1], [int]$matches[2]) } }

# ---- the case list ------------------------------------------------------------------------------
$cases = @()
foreach ($line in Get-Content $List) {
  $t = $line.Trim()
  if (-not $t -or $t.StartsWith("#")) { continue }
  $p = $t -split "\|", 3
  if ($p.Count -lt 2) { throw "bad line in ${List}: $t" }
  $steps = $p[1].Trim()
  if (-not [IO.Path]::IsPathRooted($steps)) { $steps = Join-Path (Split-Path $List -Parent) $steps }
  if (-not (Test-Path $steps)) { throw "no steps file $steps" }
  $text = @(Get-Content $steps)
  $alone = [bool]($text | Where-Object { $_ -match "^\s*#\s*fresh\b" })
  $cases += [pscustomobject]@{ Name = $p[0].Trim(); Steps = $steps; Extra = $(if ($p.Count -gt 2) { $p[2].Trim() } else { "" }); Alone = $alone }
}
if (-not $cases.Count) { throw "no cases in $List" }
$dupes = @($cases | Group-Object Name | Where-Object Count -gt 1)
if ($dupes) { throw "case names must differ: " + (($dupes | ForEach-Object Name) -join ", ") }

# ---- 0. preconditions -----------------------------------------------------------------------------
if (Get-Process -Name $vrGames -ErrorAction SilentlyContinue) { throw "a game is running; not touching anything" }
foreach ($f in $files) { if ((Get-Sha1 $f.live) -ne (Get-Sha1 $f.bak)) { throw "$($f.live) differs from the master backup; not touching anything" } }
if (Test-Path $cmdPath) { throw "someone left a commands file; not touching anything" }
if (Get-ChildItem "$mod\test-save-*.sav" -ErrorAction SilentlyContinue) { throw "save probes from another run are in the mod folder; not touching anything" }
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item $List "$Out\list.txt"
# Progress goes to the screen and <Out>\run.txt (never into a function's return value).
function Say([string]$t) { Add-Content "$Out\run.txt" $t -Encoding ascii; Write-Host $t }
Say "out $Out"
Say ("test dll " + (Get-Sha1 $Dll))
$results = "$Out\results.csv"
"case,run,launch,case_in_launch,passed,failed,result,seconds,spawn,replays" | Set-Content $results -Encoding ascii

$script:mine = @{}   # pid -> start time of the processes this run started
$script:stopAll = ""
function Assert-Alone {
  if (Test-Path "$Out\STOP") { throw "STOP file" }
  $all = @(Get-CimInstance Win32_Process | Sort-Object CreationDate)
  foreach ($pass in 1, 2) {
    foreach ($p in $all) {
      if ($p.Name -like "HalfSwordUE5*" -and $script:mine.ContainsKey([int]$p.ParentProcessId) -and $p.CreationDate -ge $script:mine[[int]$p.ParentProcessId]) { $script:mine[[int]$p.ProcessId] = $p.CreationDate }
    }
  }
  foreach ($p in $all) {
    $n = $p.Name -replace '\.exe$', ''
    if ($script:mine.ContainsKey([int]$p.ProcessId)) { continue }
    foreach ($g in $vrGames) { if ($n -like $g) { throw "FOREIGN GAME mid-run: $($p.Name) pid $($p.ProcessId) started $($p.CreationDate)" } }
  }
}
function Get-MineAlive {
  foreach ($id in @($script:mine.Keys)) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -like "HalfswordUE5*") { $p }
  }
}

# The mod's log, read from where the last read stopped (it only grows during a launch).
$script:logPos = 0L
$script:logText = New-Object System.Text.StringBuilder
function Read-Log {
  if (-not (Test-Path $logPath)) { return }
  $fs = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
  try {
    if ($fs.Length -lt $script:logPos) { $script:logPos = 0L }
    [void]$fs.Seek($script:logPos, [IO.SeekOrigin]::Begin)
    $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
    $new = $sr.ReadToEnd()
    $script:logPos = $fs.Position
    [void]$script:logText.Append($new)
  } finally { $fs.Dispose() }
}
# Waits until the log after character $from matches $pattern; returns the match's end or -1 (timeout / game gone).
function Wait-Log($pattern, [int]$from, [double]$seconds) {
  $t0 = Get-Date
  $rx = New-Object Text.RegularExpressions.Regex($pattern)
  while ($true) {
    Read-Log
    $m = $rx.Match($script:logText.ToString(), [Math]::Min($from, $script:logText.Length))
    if ($m.Success) { return $m.Index + $m.Length }
    if (((Get-Date) - $t0).TotalSeconds -gt $seconds) { return -1 }
    if (-not (Get-MineAlive)) { return -1 }
    Start-Sleep -Milliseconds 500
    Assert-Alone
  }
}
# Steps written so the mod counts them from now (each write of the file is a new batch).
function Send-Steps([string[]]$lines) {
  Set-Content $cmdPath ($lines -join "`r`n") -Encoding ascii
}
function Spawn-Of([int]$from) {
  $ms = [regex]::Matches($script:logText.ToString().Substring([Math]::Min($from, $script:logText.Length)), "Pose: actor \((-?\d+), (-?\d+),")
  if ($ms.Count) { return @([int]$ms[$ms.Count - 1].Groups[1].Value, [int]$ms[$ms.Count - 1].Groups[2].Value) }
  return $null
}
function Is-Bad($xy) {
  if (-not $xy) { return $false }
  foreach ($b in $bad) { if ([Math]::Abs($xy[0] - $b[0]) -lt 30 -and [Math]::Abs($xy[1] - $b[1]) -lt 30) { return $true } }
  return $false
}
# A case's steps: "quit" dropped (the runner ends the launch), "hsvr.case_end" after the last step.
function Case-Lines($c) {
  $lines = @()
  $last = 0.0
  foreach ($l in Get-Content $c.Steps) {
    if ($l -match "^\s*(M\+)?([0-9.]+)\s*:\s*(.*)$") {
      if ($matches[1]) { throw "$($c.Steps): M+ steps only work at launch" }
      $at = [double]$matches[2]
      if ($matches[3].Trim() -eq "quit") { if ($at -gt $last) { $last = $at }; continue }
      if ($at -gt $last) { $last = $at }
    }
    $lines += $l
  }
  $lines += ("{0}:hsvr.case_end" -f ($last + 0.5).ToString([Globalization.CultureInfo]::InvariantCulture))
  return $lines
}
function Note-Result($c, $run, $launch, $k, $pass, $fail, $result, $secs, $spawn, $reps) {
  $sp = $(if ($spawn) { "$($spawn[0]) $($spawn[1])" } else { "" })
  "$($c.Name),$run,$launch,$k,$pass,$fail,$result,$([int]$secs),$sp,$reps" | Add-Content $results -Encoding ascii
  Say ("{0,-40} {1,-7} {2,-9} pass {3} fail {4} {5,4} s spawn {6}" -f $c.Name, $run, $result, $pass, $fail, [int]$secs, $sp)
}

# One launch: the given cases one after another. Returns the cases it did not get to.
$script:launchNo = 0
function Run-Launch($group, [string]$run) {
  $script:launchNo++
  $L = $script:launchNo
  $extra = $group[0].Extra
  $set = @{ dev_fake_headset = "1"; dev_pose_fake_tracking = "1"; dev_fake_still = "1"; pose_body = "1"; log_frames = "0"; replay_capture = "1"; test_room = "1"; test_room_foes = "1"; dev_script = "" }
  foreach ($kv in ($extra -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { $set[$matches[1].Trim()] = $matches[2] } }
  $ini = @(Get-Content "$priv\ini.bak-$Master" | Where-Object { $k = ($_ -split '=', 2)[0]; -not $set.ContainsKey($k) })
  foreach ($k in $set.Keys) { $ini += "$k=$($set[$k])" }
  Set-Content "$mod\HalfSwordVR.ini" $ini -Encoding ascii
  Copy-Item $Dll "$mod\dlls\main.dll" -Force
  Assert-Alone
  if (Get-Process -Name $vrGames -ErrorAction SilentlyContinue) { throw "FOREIGN: a game started meanwhile; not launching" }
  $script:logPos = 0L
  [void]$script:logText.Clear()
  $launchDir = "$Out\launch-$L"
  New-Item -ItemType Directory -Force $launchDir | Out-Null
  $t0 = Get-Date
  $lp = Start-Process "$game\HalfswordUE5.exe" -PassThru
  $script:mine[[int]$lp.Id] = $t0.AddSeconds(-2)
  $left = @()
  try {
    # The log of this launch: wait until it is newer than the launch, then for the room.
    while ($true) {
      Start-Sleep -Seconds 1
      Assert-Alone
      $li = Get-Item $logPath -ErrorAction SilentlyContinue
      if ($li -and $li.LastWriteTime -gt $t0.AddSeconds(1)) { break }
      if (((Get-Date) - $t0).TotalSeconds -gt 60) { throw "no log after 60 s" }
      if (-not (Get-MineAlive)) { throw "game exited before its log" }
    }
    $script:logPos = 0L
    [void]$script:logText.Clear()
    $at = Wait-Log "TestRoom: \d+ passive foe" 0 150
    if ($at -lt 0) { throw "no test room after 150 s" }
    Say ("launch $L ($run): test room after " + [int]((Get-Date) - $t0).TotalSeconds + " s")
    for ($k = 0; $k -lt $group.Count; $k++) {
      $c = $group[$k]
      $caseDir = "$Out\$($c.Name)" + $(if ($run -ne "shared") { "-$run" } else { "" })
      New-Item -ItemType Directory -Force $caseDir | Out-Null
      Copy-Item $c.Steps "$caseDir\steps.txt"
      $tc = Get-Date
      $from = $(if ($k -eq 0) { 0 } else { $script:logText.Length })   # the first case's log starts at the launch
      $spawn = $null
      for ($roll = 1; $roll -le 6; $roll++) {
        if ($k -gt 0 -or $roll -gt 1) {
          # A fresh room for this case (and again for a bad spawn point).
          $mark = $script:logText.Length
          Send-Steps @("0:hsvr.case $($c.Name)")
          $b = Wait-Log ("Case: begin " + [regex]::Escape($c.Name) + " ") $mark 20
          if ($b -lt 0) { throw "the mod did not start case $($c.Name)" }
          $from = $b
          $at = Wait-Log "TestRoom: \d+ passive foe" $b 60
          if ($at -lt 0) { throw "no test room for case $($c.Name) after 60 s" }
        }
        # As after a launch: 4 s for the weapons and foes to settle.
        Start-Sleep -Seconds 4
        Read-Log
        $spawn = Spawn-Of $from
        if (-not (Is-Bad $spawn)) { break }
        Say "  $($c.Name): bad spawn point $($spawn[0]), $($spawn[1]); room again"
      }
      if (Is-Bad $spawn) { throw "bad spawn point six times" }
      $mark = $script:logText.Length
      $repsBefore = @(Get-ChildItem $replays -Filter "replay-*.csv" -ErrorAction SilentlyContinue | ForEach-Object FullName)
      Send-Steps (@("0:hsvr.case_here $($c.Name)") + (Case-Lines $c))
      $end = Wait-Log ("Case: end " + [regex]::Escape($c.Name) + " .*") $mark $CaseSeconds
      Read-Log
      $all = $script:logText.ToString()
      $slice = $all.Substring($from, $(if ($end -gt 0) { $end } else { $all.Length }) - $from)
      Set-Content "$caseDir\HalfSwordVR.log" $slice -Encoding utf8 -NoNewline
      Start-Sleep -Milliseconds 300
      Get-ChildItem "$mod\test-save-*.sav" -ErrorAction SilentlyContinue | ForEach-Object { Move-Item $_.FullName $caseDir }
      # A replay of a failed check is written 5 s after the mark: wait for it before the room reopens.
      $reps = @()
      if ($end -gt 0 -and $slice -match "replay marked") {
        $tw = Get-Date
        while (((Get-Date) - $tw).TotalSeconds -lt 12) {
          Read-Log
          if ($script:logText.ToString().IndexOf("Replay: saved", $mark) -ge 0) { break }
          Start-Sleep -Milliseconds 500
        }
      }
      foreach ($r in @(Get-ChildItem $replays -Filter "replay-*.csv" -ErrorAction SilentlyContinue)) {
        if ($repsBefore -notcontains $r.FullName) { Copy-Item $r.FullName $caseDir; $reps += $r.Name }
      }
      $secs = ((Get-Date) - $tc).TotalSeconds
      if ($end -lt 0) {
        Note-Result $c $run $L ($k + 1) 0 0 "TIMEOUT" $secs $spawn ($reps -join " ")
        $left = @()
        if ($k + 1 -lt $group.Count) { $left = @($group[($k + 1)..($group.Count - 1)]) }
        if (-not (Get-MineAlive)) { Say "  game exited during $($c.Name)" }
        return , @{ Left = $left; Failed = @($c) }
      }
      $m = [regex]::Match($slice, "Case: end .*: (\d+) check\(s\) passed, (\d+) failed")
      $pass = [int]$m.Groups[1].Value; $fail = [int]$m.Groups[2].Value
      $res = $(if ($fail -gt 0) { "FAIL" } else { "pass" })
      Note-Result $c $run $L ($k + 1) $pass $fail $res $secs $spawn ($reps -join " ")
      if ($fail -gt 0) { $script:failed += $c }
    }
    return , @{ Left = @(); Failed = @() }
  }
  finally {
    if (Get-MineAlive) {
      Send-Steps @("0:quit")
      $tq = Get-Date
      while ((Get-MineAlive) -and ((Get-Date) - $tq).TotalSeconds -lt 20) { Start-Sleep -Milliseconds 500 }
    }
    $still = @(Get-MineAlive)
    if ($still) { $still | Stop-Process -Force -ErrorAction SilentlyContinue; $still | Wait-Process -Timeout 60 -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
    if (Test-Path $logPath) { Copy-Item $logPath $launchDir }
    if (Test-Path "$game\HalfswordUE5\Binaries\Win64\ue4ss\UE4SS.log") { Copy-Item "$game\HalfswordUE5\Binaries\Win64\ue4ss\UE4SS.log" $launchDir }
    Remove-Item $cmdPath -ErrorAction SilentlyContinue
    Get-ChildItem "$mod\test-save-*.sav" -ErrorAction SilentlyContinue | ForEach-Object { Move-Item $_.FullName $launchDir }
    if (Test-Path "$saves\GameProgress.sav") { Copy-Item "$saves\GameProgress.sav" "$launchDir\after-GameProgress.sav" -ErrorAction SilentlyContinue }
    # Each launch starts from his exact files again.
    if (-not (Get-Process -Name "HalfswordUE5*" -ErrorAction SilentlyContinue)) {
      foreach ($f in $files[2..3]) { Copy-Item $f.bak $f.live -Force }
    }
  }
}

# Groups: cases next to each other with the same extra ini keys share launches (at most $PerLaunch);
# a "# fresh" case, or every case with -Fresh, has a launch of its own.
function Make-Groups($list) {
  $groups = @()
  $cur = @()
  foreach ($c in $list) {
    if ($Fresh -or $c.Alone) {
      if ($cur.Count) { $groups += , $cur; $cur = @() }
      $groups += , @($c)
      continue
    }
    if ($cur.Count -and ($cur[0].Extra -ne $c.Extra -or $cur.Count -ge $PerLaunch)) { $groups += , $cur; $cur = @() }
    $cur += $c
  }
  if ($cur.Count) { $groups += , $cur }
  return , $groups
}

$script:failed = @()
$tAll = Get-Date
try {
  $queue = [System.Collections.ArrayList]@(Make-Groups $cases)
  $run = $(if ($Fresh) { "fresh" } else { "shared" })
  while ($queue.Count) {
    $g = $queue[0]; $queue.RemoveAt(0)
    $r = Run-Launch $g $run
    $script:failed += $r.Failed
    if ($r.Left.Count) { $queue.Insert(0, @($r.Left)) }   # after a stuck case: the rest in a new launch
  }
  # Failed or stuck cases once more, each in a launch of its own (not with -Fresh: they already were).
  if (-not $Fresh -and -not $NoConfirm -and $script:failed.Count) {
    Say "confirming $($script:failed.Count) failed case(s) in launches of their own"
    $again = @($script:failed | Select-Object -Unique)
    foreach ($c in $again) { [void](Run-Launch @($c) "fresh") }
  }
}
catch { $script:stopAll = "$_"; Say "RUN ERROR: $_" }
finally {
  $other = @(Get-Process -Name "HalfswordUE5*" -ErrorAction SilentlyContinue)
  if ($other) {
    Say ("RESTORE PENDING: a Half Sword this run did not start is running (pid " + (($other | ForEach-Object Id) -join ",") + "); nothing restored")
  }
  else {
    foreach ($f in $files) {
      Copy-Item $f.bak $f.live -Force
      Say ("restored " + (Split-Path $f.live -Leaf) + ": " + ((Get-Sha1 $f.live) -eq (Get-Sha1 $f.bak)))
    }
  }
  Say ("all done in " + [int]((Get-Date) - $tAll).TotalMinutes + " min; results $results")
}
