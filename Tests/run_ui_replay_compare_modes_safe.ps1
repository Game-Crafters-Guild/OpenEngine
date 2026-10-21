param(
  [string]$EditorExe = "",
  [ValidateSet("Debug", "DebugFast", "RelWithDebInfo", "Release")]
  [string]$Config = "RelWithDebInfo",
  [string]$ProjectRoot,
  [string]$OutDir,
  [string]$Preset,
  [int]$ExitAfterFrames = 900,
  # Before each run, stop editors this runner started that are still running, and an earlier run's
  # still writing a replay log under the same OutDir (see Stop-EditorIfRunning). Off by default.
  [switch]$KillExistingEditor,
  # When enabled, writes the Editor process stdout and stderr for each run
  # next to the JSONL log. This helps diagnose random crashes or black frames.
  [switch]$CaptureStdout = $true,
  # Optional: override the default scenario list. Each element may hold several comma-separated paths:
  # through `powershell -File` a value after the first binds to the next free positional parameter or
  # is dropped, so callers on a command line pass one comma-joined string. A path that contains a comma
  # is not supported. Passing -Scenarios with no path is an error.
  [string[]]$Scenarios = @()
)

# Compare runner:
# - Runs selected scenarios under GE_UI_CORRECTNESS_MODE=0 and =1
# - Never throws (keeps automation sessions alive)
# - Writes separate logs per mode
# - Prints a compact summary (failures, fast-path hints, scroll ranges)
# - Gives each editor its own MCP debug port from 9960-9979 (--debug-port), the first one with no
#   listener when the editor starts, so it never lands on another editor's port (9999 is the default;
#   run_ui_replay_tests_safe.ps1 owns 9900-9919)

$ErrorActionPreference = "Continue"

# Resolve repo root from this script's location (Tests/run_ui_replay_compare_modes_safe.ps1 → ..).
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = $repoRoot }
if ([string]::IsNullOrWhiteSpace($OutDir))      { $OutDir      = Join-Path $repoRoot 'Tests\UIReplay\out_compare' }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# Absolute, so the editor (which resolves a relative --ui-replay-log against its project root) writes
# the files this runner reads, and Test-RunnerOwnedCommandLine compares absolute paths.
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath

if ([string]::IsNullOrWhiteSpace($EditorExe)) {
  $presets = @($Preset, 'vs2026-x64-local', 'vs2026-x64-local-unity',
               'vs2026-arm64-local', 'vs2022-x64-local', 'vs2022-x64-local-unity') |
             Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  foreach ($p in $presets) {
    $candidate = Join-Path $repoRoot "build\$p\bin\$Config\Apps\Editor\Editor.exe"
    if (Test-Path $candidate) { $EditorExe = $candidate; break }
  }
  if ([string]::IsNullOrWhiteSpace($EditorExe)) {
    $EditorExe = Join-Path $repoRoot "build\vs2026-x64-local\bin\$Config\Apps\Editor\Editor.exe"
  }
}

$extraFrames = 180

$scenarioDir = Join-Path $repoRoot 'Tests\UIReplay'
$defaultScenarios = @(
  'smoke_present_not_black.json',
  'smoke_present_resize_fuzz.json',
  'repro_hierarchy_multi_create.json',
  'repro_hierarchy_multi_create_cubes.json',
  'repro_inspector_scroll_after_create_cube.json',
  # Assets ListView mode: wheel scroll + horizontal scrollbar drag should pass in both modes.
  'repro_assets_listview_wheel_scroll_delay.json',
  'repro_assets_listview_horizontal_scrollbar_drag.json'
) | ForEach-Object { Join-Path $scenarioDir $_ }

# PowerShell variable names are case-insensitive: this list must not be named $scenarios, or it
# overwrites the -Scenarios parameter before the override is read.
$scenarioPaths = $defaultScenarios
$requestedScenarios = @($Scenarios | ForEach-Object { $_ -split ',' } |
                        ForEach-Object { $_.Trim() } |
                        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($PSBoundParameters.ContainsKey('Scenarios')) {
  if ($requestedScenarios.Count -eq 0) {
    Write-Host "[UIReplayCompare] -Scenarios names no scenario path: pass one or more comma-separated paths, or omit -Scenarios to run the default list."
    exit 2
  }
  $scenarioPaths = $requestedScenarios
}
Write-Host ("[UIReplayCompare] {0} scenarios" -f @($scenarioPaths).Count)

function Get-ScenarioMinFrames([string]$path) {
  try {
    $j = Get-Content -Raw -Path $path | ConvertFrom-Json
    if ($null -ne $j -and $null -ne $j.minFrames) { return [int]$j.minFrames }
  } catch { }
  return 0
}

function Parse-FailMessage([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -match '"kind":"ui_replay_fail"') {
      # crude JSON parse (message is safe)
      if ($line -match '"message":"(?<m>[^"]+)"') { return $Matches["m"] }
      return "ui_replay_fail"
    }
    if ($line -match '"kind":"ui_replay_end"') {
      return $null
    }
  }
  return $null
}

function Get-ReplayResult([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -match '"kind":"ui_replay_end"') {
      if ($line -match '"result":"(?<r>[^"]+)"') { return $Matches["r"] }
      return "unknown"
    }
    if ($line -match '"kind":"ui_replay_fail"') {
      return "fail"
    }
  }
  return $null
}

function Parse-ScrollYRange([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  $minY = $null
  $maxY = $null
  $reScrollY = [regex]('"probe":\{.*?"scrollView":\{[^}]*"scrollY":(?<y>-?[0-9]+(\.[0-9]+)?)')
  foreach ($line in Get-Content -Path $logPath) {
    $m = $reScrollY.Match($line)
    if (-not $m.Success) { continue }
    $y = [double]$m.Groups["y"].Value
    if ($null -eq $minY -or $y -lt $minY) { $minY = $y }
    if ($null -eq $maxY -or $y -gt $maxY) { $maxY = $y }
  }
  if ($null -eq $minY -or $null -eq $maxY) { return $null }
  return @{ min = $minY; max = $maxY }
}

function Parse-FastPathCounts([string]$logPath) {
  if (-not (Test-Path $logPath)) { return @{} }
  $counts = @{
    tookVisualPatchOnly = 0
    tookNoOpFastPath = 0
    tookCapturedMoveFastPath = 0
    tookPassiveMoveFastPath = 0
    ranHeavyPass = 0
  }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -match '"tookVisualPatchOnly":true') { $counts.tookVisualPatchOnly++ }
    if ($line -match '"tookNoOpFastPath":true') { $counts.tookNoOpFastPath++ }
    if ($line -match '"tookCapturedMoveFastPath":true') { $counts.tookCapturedMoveFastPath++ }
    if ($line -match '"tookPassiveMoveFastPath":true') { $counts.tookPassiveMoveFastPath++ }
    if ($line -match '"ranHeavyPass":true') { $counts.ranHeavyPass++ }
  }
  return $counts
}

function Parse-PixelHashMap([string]$logPath) {
  $map = @{}
  if (-not (Test-Path $logPath)) { return $map }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -notmatch '"kind":"ui_replay_pixel_hash"') { continue }
    $f = $null
    $h = $null
    if ($line -match '"frame":(?<f>[0-9]+)') { $f = [int]$Matches["f"] }
    if ($line -match '"hash64":"(?<h>[0-9a-fA-F]+)"') { $h = $Matches["h"] }
    # 8e-7: hashes carry the producing arm/source. Frames hashed on different
    # arms or sources (old backbuffer bytes vs RG2 FinalLinear) are not
    # comparable — key them so the diff treats cross-arm pairs as SKIPPED.
    $arm = "old"
    $src = "backbuffer"
    if ($line -match '"arm":"(?<a>[a-z0-9_]+)"') { $arm = $Matches["a"] }
    if ($line -match '"src":"(?<s>[a-z0-9_]+)"') { $src = $Matches["s"] }
    if ($null -ne $f -and $null -ne $h) { $map[$f] = @{ hash = $h; arm = $arm; src = $src } }
  }
  return $map
}

# Editors this run started, held as Process objects so a recycled PID never matches.
$startedEditors = New-Object System.Collections.Generic.List[System.Diagnostics.Process]

# True when the command line passes --ui-replay-log an absolute path under this runner's OutDir.
# A relative or drive-relative path never matches: its editor resolved it against a directory this
# runner cannot see, so it may name another tree's out folder.
function Test-RunnerOwnedCommandLine([string]$commandLine) {
  if ([string]::IsNullOrWhiteSpace($commandLine)) { return $false }
  $m = [regex]::Match($commandLine, '--ui-replay-log\s+(?:"(?<p>[^"]+)"|(?<p>\S+))')
  if (-not $m.Success) { return $false }
  $logArg = $m.Groups["p"].Value
  if ($logArg -notmatch '^(?:[A-Za-z]:[\\/]|[\\/]{2})') { return $false }
  try {
    $logPath = [IO.Path]::GetFullPath($logArg)
    $outPrefix = [IO.Path]::GetFullPath($OutDir).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  } catch {
    return $false
  }
  return $logPath.StartsWith($outPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

# Stops only editors this runner owns: those this run started (by the process it recorded), and
# leftovers of an earlier run into the same OutDir (matched by Test-RunnerOwnedCommandLine). Never
# selects by process name alone: other editors on the machine are not the runner's to stop. An editor's
# CompileServerHost children stop with it (the engine binds them to the editor process).
function Stop-EditorIfRunning() {
  $owned = @{}
  foreach ($started in $startedEditors) {
    if (-not $started.HasExited) { $owned[$started.Id] = $started }
  }
  foreach ($e in @(Get-CimInstance Win32_Process -Filter "Name = 'Editor.exe'" -ErrorAction SilentlyContinue)) {
    if ($owned.ContainsKey([int]$e.ProcessId)) { continue }
    if (-not (Test-RunnerOwnedCommandLine $e.CommandLine)) { continue }
    $leftover = Get-Process -Id $e.ProcessId -ErrorAction SilentlyContinue
    if ($null -ne $leftover) { $owned[$leftover.Id] = $leftover }
  }
  if ($owned.Count -eq 0) { return }
  $procs = @($owned.Values)
  Write-Host ("[UIReplayCompare] Stopping runner-owned Editor pid(s) {0}" -f (($procs | ForEach-Object { $_.Id }) -join ", "))
  $procs | Stop-Process -Force -ErrorAction SilentlyContinue
  # A force-stopped editor can take 30 to 60 s to tear down. One still exiting after the wait is reported,
  # the next run launches beside it, and it is found and stopped again before the run after that.
  try { $procs | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue } catch { }
  foreach ($proc in $procs) {
    $proc.Refresh()
    $state = if ($proc.HasExited) { "stopped" } else { "still exiting after 5 s" }
    Write-Host ("[UIReplayCompare] Runner-owned Editor pid={0}: {1}" -f $proc.Id, $state)
  }
  Start-Sleep -Milliseconds 200
}

$runnerDebugPortFirst = 9960
$runnerDebugPortLast = 9979

# Returns the first port in the runner's range with no TCP listener, or $null when all are taken.
function Get-FreeRunnerDebugPort() {
  $listening = @{}
  foreach ($endpoint in [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()) {
    $listening[$endpoint.Port] = $true
  }
  for ($port = $runnerDebugPortFirst; $port -le $runnerDebugPortLast; $port++) {
    if (-not $listening.ContainsKey($port)) { return $port }
  }
  return $null
}

# Start-Process joins -ArgumentList with spaces and quotes nothing, so a path with a space would split.
# Inside the quotes, trailing backslashes are doubled: the C runtime reads \" as a literal quote.
function Join-EditorArguments([object[]]$arguments) {
  return @($arguments | ForEach-Object { $a = [string]$_; if ($a -match '\s') { '"{0}"' -f ($a -replace '(\\+)$', '$1$1') } else { $a } })
}

function Clean-DiffArtifacts([string]$scenarioName) {
  # Diff images are generated out-of-band (python script) and can become stale.
  # Remove previous diff PNGs for this scenario so manual inspection can’t be misled.
  $pat = ("diff_{0}.frame*.png" -f $scenarioName)
  Get-ChildItem -Path $OutDir -Filter $pat -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

$results = @()

# The editors read these from the runner's environment. Their previous values are restored before the
# runner exits, so a session that runs it with & keeps its own.
$runnerEnvironmentNames = @('GE_EDITOR_USER_DATA_ROOT', 'GE_FORCE_HDR_OUTPUT', 'GE_UI_CORRECTNESS_MODE')
$savedEnvironment = @{}
foreach ($envName in $runnerEnvironmentNames) {
  $savedEnvironment[$envName] = [Environment]::GetEnvironmentVariable($envName, 'Process')
}

# The editors keep preferences, recent projects and caches under OutDir, never in the developer's own
# editor user data. The editor needs an absolute path here.
$env:GE_EDITOR_USER_DATA_ROOT = Join-Path $OutDir 'EditorUserData'
# Keep HDR output off whatever the project or the developer's settings request.
$env:GE_FORCE_HDR_OUTPUT = "Off"

foreach ($s in $scenarioPaths) {
  $name = [IO.Path]::GetFileNameWithoutExtension($s)
  $minFrames = Get-ScenarioMinFrames $s
  $scenarioExit = [Math]::Max($ExitAfterFrames, ($minFrames + $extraFrames))

  Clean-DiffArtifacts $name

  foreach ($mode in @("0","1")) {
    if ($KillExistingEditor) {
      Stop-EditorIfRunning
    }

    $env:GE_UI_CORRECTNESS_MODE = $mode
    $out = Join-Path $OutDir ("{0}.{1}.mode{2}.jsonl" -f $name, $Config, $mode)
    $stdout = Join-Path $OutDir ("{0}.{1}.mode{2}.stdout.txt" -f $name, $Config, $mode)
    $stderr = Join-Path $OutDir ("{0}.{1}.mode{2}.stderr.txt" -f $name, $Config, $mode)
    $editorLog = Join-Path $OutDir ("{0}.{1}.mode{2}.editor.log" -f $name, $Config, $mode)
    # An earlier run's logs would be read as this run's result when the editor never writes its own.
    Remove-Item -LiteralPath $out, $editorLog -Force -ErrorAction SilentlyContinue
    Write-Host ("[UIReplayCompare] Running {0} mode={1} (config={2})" -f $name, $mode, $Config)

    $p = $null
    try {
      # Checked free at launch time; the editor binds it seconds later. An editor that loses the port to
      # another process logs "Failed to start MCP debug server" and runs without one, which the replay
      # does not use.
      $debugPort = Get-FreeRunnerDebugPort
      if ($null -eq $debugPort) {
        throw ("no free debug port in {0}-{1}" -f $runnerDebugPortFirst, $runnerDebugPortLast)
      }
      $editorArgs = Join-EditorArguments @("--project", $ProjectRoot, "--ui-replay", $s, "--ui-replay-log", $out,
                                           "--exit-after-frames", $scenarioExit, "-logfile", $editorLog,
                                           "--debug-port", $debugPort)
      if ($CaptureStdout) {
        $p = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
      } else {
        $p = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -NoNewWindow -PassThru
      }
      $startedEditors.Add($p)
      # Holding the handle keeps ExitCode readable after the process exits.
      $null = $p.Handle
      Write-Host ("[UIReplayCompare] Editor pid={0} debug port={1}" -f $p.Id, $debugPort)
      $p | Wait-Process
    } catch {
      Write-Host ("[UIReplayCompare] Could not launch {0} mode={1}: {2}" -f $name, $mode, $_)
      $p = $null
    }

    # Not launched: Start-Process sets no exit code, so record the failure explicitly.
    $code = 1
    if ($null -ne $p) {
      try { $p.Refresh() } catch { }
      try { $code = $p.ExitCode } catch { $code = 1 }
    }
    $result = Get-ReplayResult $out

    $failMsg = Parse-FailMessage $out
    $scrollRange = Parse-ScrollYRange $out
    $fastCounts = Parse-FastPathCounts $out

    $results += [PSCustomObject]@{
      scenario = $name
      mode = $mode
      exitCode = $code
      result = $result
      log = $out
      stdout = $stdout
      stderr = $stderr
      fail = $failMsg
      scrollMin = if ($null -ne $scrollRange) { $scrollRange.min } else { $null }
      scrollMax = if ($null -ne $scrollRange) { $scrollRange.max } else { $null }
      ranHeavyPass = $fastCounts.ranHeavyPass
      tookVisualPatchOnly = $fastCounts.tookVisualPatchOnly
      tookNoOpFastPath = $fastCounts.tookNoOpFastPath
      tookCapturedMoveFastPath = $fastCounts.tookCapturedMoveFastPath
      tookPassiveMoveFastPath = $fastCounts.tookPassiveMoveFastPath
    }
  }
}

Write-Host ""
Write-Host "[UIReplayCompare] Summary:"
foreach ($r in $results) {
  $scroll = if ($null -ne $r.scrollMin -and $null -ne $r.scrollMax) { ("scrollY=[{0},{1}]" -f $r.scrollMin, $r.scrollMax) } else { "scrollY=(n/a)" }
  $fp = ("heavy={0} vp={1} noop={2} capMove={3} passMove={4}" -f $r.ranHeavyPass, $r.tookVisualPatchOnly, $r.tookNoOpFastPath, $r.tookCapturedMoveFastPath, $r.tookPassiveMoveFastPath)
  $ok = ($r.result -eq "pass")
  $status = if ($ok) { "OK" } else { ("FAIL result={0} exit={1} msg={2}" -f $r.result, $r.exitCode, $r.fail) }
  if ($CaptureStdout) {
    Write-Host ("  - {0} mode={1}: {2} {3} {4} stdout={5} stderr={6}" -f $r.scenario, $r.mode, $status, $scroll, $fp, $r.stdout, $r.stderr)
  } else {
    Write-Host ("  - {0} mode={1}: {2} {3} {4}" -f $r.scenario, $r.mode, $status, $scroll, $fp)
  }
}

# Pixel-hash comparison (mode0 vs mode1), per scenario.
Write-Host ""
Write-Host "[UIReplayCompare] Pixel hash (mode0 vs mode1):"
foreach ($s in ($results | Select-Object -ExpandProperty scenario -Unique)) {
  $r0 = $results | Where-Object { $_.scenario -eq $s -and $_.mode -eq "0" } | Select-Object -First 1
  $r1 = $results | Where-Object { $_.scenario -eq $s -and $_.mode -eq "1" } | Select-Object -First 1
  if ($null -eq $r0 -or $null -eq $r1) { continue }
  if ($r0.result -ne "pass" -or $r1.result -ne "pass") {
    Write-Host ("  - {0}: (skipped; non-pass result)" -f $s)
    continue
  }

  $m0 = Parse-PixelHashMap $r0.log
  $m1 = Parse-PixelHashMap $r1.log
  $common = @()
  foreach ($k in $m0.Keys) { if ($m1.ContainsKey($k)) { $common += $k } }
  $common = $common | Sort-Object
  if ($common.Count -eq 0) {
    Write-Host ("  - {0}: (no pixel hashes logged)" -f $s)
    continue
  }

  $mismatches = @()
  $skippedCrossArm = 0
  foreach ($f in $common) {
    # Different arm/source = not comparable (different bytes by design) — skip.
    if ($m0[$f].arm -ne $m1[$f].arm -or $m0[$f].src -ne $m1[$f].src) {
      $skippedCrossArm++
      continue
    }
    if ($m0[$f].hash -ne $m1[$f].hash) { $mismatches += $f }
  }
  Write-Host ("  - {0}: mismatches={1}/{2} (skipped cross-arm: {3})" -f $s, $mismatches.Count, $common.Count, $skippedCrossArm)
  if ($mismatches.Count -gt 0) {
    $show = $mismatches | Select-Object -First 5
    foreach ($f in $show) {
      Write-Host ("      frame {0}: mode0={1} mode1={2}" -f $f, $m0[$f].hash, $m1[$f].hash)
    }
  }
}

foreach ($envName in $runnerEnvironmentNames) {
  [Environment]::SetEnvironmentVariable($envName, $savedEnvironment[$envName], 'Process')
}

# Always exit success to keep automation sessions alive.
exit 0

