param(
  [Parameter(Mandatory = $true)][string]$List,   # same list format as run-suite.ps1
  [Parameter(Mandatory = $true)][string]$Dll,    # the test build's main.dll
  [int]$Slots = 2,                               # test copies used while he is away (A, B, ...)
  [string]$Root = "C:\HSTest",
  [int]$Chunk = 8,                               # cases handed to a copy at a time (small = even sharing)
  [int]$IdleMinutes = 10,                        # no keyboard/mouse for this long and his game closed = he is away
  [double]$MinFps = 65,
  [int]$MaxFps = 72,
  [int]$SlowTries = 3,                           # a case that keeps running on a starved game is reported, not retried forever
  [string]$Out = ""
)
# Runs a case list on test copies (docs/pctest.md, "Test copies"). Matthew 2026-11-23:
#   at the PC (keyboard or mouse used in the last -IdleMinutes, or his own Half Sword running): ONE test game,
#   in the background, so he can play or playtest meanwhile;
#   away: up to -Slots test games at once.
# Each copy runs run-suite.ps1 -Copy on a chunk of the list. His install, ini and save are never touched.
# A case that ran on a starved game (below -MinFps) is SLOW: not counted, queued again.
# <Out>\STOP stops handing out chunks and stops every copy's run.
$ErrorActionPreference = "Stop"
$here = Split-Path $MyInvocation.MyCommand.Path -Parent
$priv = "C:\Users\matth\Documents\Codex\HalfSwordDecomp\private"
if (-not $Out) { $Out = "$priv\parallel-" + [IO.Path]::GetFileNameWithoutExtension($List) + "-" + (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHHmmss") + "Z" }
New-Item -ItemType Directory -Force $Out | Out-Null
function Say([string]$t) { $line = (Get-Date).ToUniversalTime().ToString("HH:mm:ss") + "Z " + $t; Add-Content "$Out\run.txt" $line -Encoding ascii; Write-Host $line }

Add-Type -Namespace HsvrPc -Name Idle -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
public static double Seconds() { var i = new LASTINPUTINFO(); i.cbSize = (uint)Marshal.SizeOf(i); GetLastInputInfo(ref i); return ((uint)Environment.TickCount - i.dwTime) / 1000.0; }
'@
# He is using the PC for games: his own Half Sword (not from a test copy) or SteamVR is running.
function His-GameRuns { [bool](Get-CimInstance Win32_Process | Where-Object { $_.Name -like "vrserver*" -or $_.Name -like "vrcompositor*" -or ($_.Name -like "HalfswordUE5*" -and -not ($_.ExecutablePath -and $_.ExecutablePath -like "$Root\*")) }) }
function Allowed {
  if (His-GameRuns) { return 1 }
  if ([HsvrPc.Idle]::Seconds() -lt $IdleMinutes * 60) { return 1 }
  return $Slots
}

# ---- the list, with steps paths made absolute so chunk lists can live in <Out> ----------------------
$lines = @()
foreach ($line in Get-Content $List) {
  $t = $line.Trim()
  if (-not $t -or $t.StartsWith("#")) { continue }
  $p = $t -split "\|", 3
  $steps = $p[1].Trim()
  if (-not [IO.Path]::IsPathRooted($steps)) { $steps = Join-Path (Split-Path (Resolve-Path $List) -Parent) $steps }
  $lines += [pscustomobject]@{ Name = $p[0].Trim(); Line = ($p[0].Trim() + "|" + $steps + $(if ($p.Count -gt 2) { "|" + $p[2] } else { "" })); Tries = 0 }
}
if (-not $lines.Count) { throw "no cases in $List" }
Copy-Item $List "$Out\list.txt"
$queue = [System.Collections.ArrayList]@()
for ($i = 0; $i -lt $lines.Count; $i += $Chunk) { [void]$queue.Add(@($lines[$i..([Math]::Min($i + $Chunk, $lines.Count) - 1)])) }

# ---- copies -------------------------------------------------------------------------------------------
$names = @(65..(64 + $Slots) | ForEach-Object { [string][char]$_ })
foreach ($n in $names) { if (-not (Test-Path "$Root\$n\game\HalfSwordUE5.exe")) { & "$here\new-testcopy.ps1" -Name $n -Root $Root } }

$results = "$Out\results.csv"
"copy,chunk,case,run,launch,case_in_launch,passed,failed,result,seconds,spawn,replays,min_fps" | Set-Content $results -Encoding ascii
$running = @{}   # copy name -> @{ proc; out; chunk; cases }
$chunkNo = 0
$tAll = Get-Date
$lastAllowed = -1
try {
  while ($queue.Count -or $running.Count) {
    if (Test-Path "$Out\STOP") {
      Say "STOP file: no more chunks; stopping every copy's run"
      foreach ($r in $running.Values) { New-Item -ItemType File -Force "$($r.out)\STOP" | Out-Null }
      $queue.Clear()
    }
    # Finished copies: collect, queue SLOW cases again.
    foreach ($n in @($running.Keys)) {
      $r = $running[$n]
      if (-not $r.proc.HasExited) { continue }
      $running.Remove($n)
      if (Test-Path "$($r.out)\results.csv") {
        foreach ($row in @(Get-Content "$($r.out)\results.csv" | Select-Object -Skip 1)) { "$n,$($r.chunk),$row" | Add-Content $results -Encoding ascii }
      }
      $tail = @(Get-Content "$($r.out)\run.txt" -ErrorAction SilentlyContinue | Select-Object -Last 3) -join " / "
      Say "copy $n chunk $($r.chunk) done: $tail"
      if (Test-Path "$($r.out)\slow.txt") {
        $again = @()
        foreach ($s in @(Get-Content "$($r.out)\slow.txt")) {
          $c = $r.cases | Where-Object Name -eq $s | Select-Object -First 1
          if (-not $c) { continue }
          $c.Tries++
          if ($c.Tries -lt $SlowTries) { $again += $c } else { Say "  $s ran SLOW $($c.Tries) times: reported as SLOW, not run again" }
        }
        if ($again.Count) { [void]$queue.Add($again); Say ("  queued again (SLOW): " + (($again | ForEach-Object Name) -join ", ")) }
      }
      if ((Select-String -Path "$($r.out)\run.txt" -Pattern "RUN ERROR" -Quiet -ErrorAction SilentlyContinue)) {
        Say "  copy $n had a run error: not using it again this run"
        $names = @($names | Where-Object { $_ -ne $n })
      }
    }
    # Hand out chunks up to what is allowed right now (a running copy is never stopped for this:
    # chunks are short, so a second game ends within a few minutes after he comes back).
    $allowed = Allowed
    if ($allowed -ne $lastAllowed) { Say "test games allowed now: $allowed"; $lastAllowed = $allowed }
    foreach ($n in $names) {
      if (-not $queue.Count -or $running.Count -ge $allowed) { break }
      if ($running.ContainsKey($n)) { continue }
      $cases = $queue[0]; $queue.RemoveAt(0)
      $chunkNo++
      $chunkList = "$Out\chunk-$chunkNo.txt"
      @($cases | ForEach-Object Line) | Set-Content $chunkList -Encoding ascii
      $co = "$Out\chunk-$chunkNo-$n"
      $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$here\run-suite.ps1`"", "-List", "`"$chunkList`"", "-Dll", "`"$Dll`"",
                   "-Copy", "`"$Root\$n`"", "-Out", "`"$co`"", "-MinFps", $MinFps, "-MaxFps", $MaxFps)
      $proc = Start-Process powershell -ArgumentList $argList -WindowStyle Minimized -PassThru
      $running[$n] = @{ proc = $proc; out = $co; chunk = $chunkNo; cases = $cases }
      Say ("copy $n chunk ${chunkNo}: " + (($cases | ForEach-Object Name) -join ", "))
    }
    if (-not $names.Count -and $queue.Count) { throw "no usable test copy left" }
    Start-Sleep -Seconds 5
  }
}
catch { Say "RUN ERROR: $_" }
finally {
  $rows = @(Import-Csv $results)
  $sum = $rows | Group-Object result | ForEach-Object { "$($_.Name) $($_.Count)" }
  Say ("all done in " + [int]((Get-Date) - $tAll).TotalMinutes + " min: " + ($sum -join ", ") + "; results $results")
}
