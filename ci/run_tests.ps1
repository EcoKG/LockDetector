param(
    [string]$BinDir = "build/bin/Release",
    [string]$OutDir = "results",
    [switch]$NoFail,
    [int]$TimeoutSec = 60
)
# Runs InversionDemo scenarios with/without LockDetector.dll and verifies the
# detector log. Prints a PASS/FAIL line per check and a benchmark table.
$ErrorActionPreference = 'Continue'
$BinDir = (Resolve-Path $BinDir).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$demo = Join-Path $BinDir 'InversionDemo.exe'
$dll  = Join-Path $BinDir 'LockDetector.dll'
$script:fails = 0
$script:summary = New-Object System.Collections.ArrayList

function Run-Demo([string[]]$DemoArgs, [string]$Tag, [switch]$Detector, [int]$Timeout = $TimeoutSec, [hashtable]$Env = @{}, [string]$Exe = $demo) {
    $log = Join-Path $OutDir "$Tag.detector.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $all = @($DemoArgs)
    if ($Detector) { $all += @('--detector', $dll, '--log', $log) }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($all | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    foreach ($k in $Env.Keys) { $psi.EnvironmentVariables[$k] = [string]$Env[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $timedOut = -not $p.WaitForExit($Timeout * 1000)
    if ($timedOut) { try { $p.Kill() } catch {} ; $p.WaitForExit() }
    $stdout = $outTask.Result; $stderr = $errTask.Result
    Set-Content -Path (Join-Path $OutDir "$Tag.stdout.txt") -Value ($stdout + $stderr)
    $logText = ''
    $logSize = 0
    if (Test-Path $log) { $logText = Get-Content $log -Raw; $logSize = (Get-Item $log).Length }
    return [pscustomobject]@{ Tag=$Tag; Exit=$(if($timedOut){-1}else{$p.ExitCode}); TimedOut=$timedOut;
                              Stdout=$stdout; Stderr=$stderr; Log=$logText; LogSize=$logSize }
}

function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    $s = if ($Ok) { 'PASS' } else { 'FAIL'; $script:fails++ }
    $line = "[{0}] {1} {2}" -f $s, $Name, $Detail
    Write-Host $line
    [void]$script:summary.Add($line)
}

function Get-Locks($r) {
    if ($r.Stdout -match 'LOCKS A=(\S+) B=(\S+)') { return @($Matches[1], $Matches[2]) }
    return @()
}
function Warnings-For($r) {
    $l = Get-Locks $r
    if ($l.Count -ne 2) { return @() }
    $w = @($r.Log -split "`r?`n" | Where-Object { $_ -match '\[WARNING\]' })
    return @($w | Where-Object { $_ -match [regex]::Escape($l[0]) -and $_ -match [regex]::Escape($l[1]) })
}
function Warnings-All($r) { return @($r.Log -split "`r?`n" | Where-Object { $_ -match '\[WARNING\]' }) }

Write-Host "==== environment ===="
Write-Host ("OS: " + [System.Environment]::OSVersion.VersionString + "  CPUs: " + [Environment]::ProcessorCount)

Write-Host "`n==== 0. diagnostics: minimal thread creation with detector ===="
$r = Run-Demo @('thread-trivial') 'thread-trivial-plain'
Write-Host ("plain    : exit=$($r.Exit) " + $r.Stdout.Trim())
$r = Run-Demo @('thread-trivial') 'thread-trivial-det' -Detector
Check "thread-trivial : process with detector starts a thread and exits cleanly" ($r.Exit -eq 0 -and $r.Stdout -match 'RESULT thread-trivial-finished') "(exit=$($r.Exit))"
if ($r.Exit -ne 0) { Write-Host $r.Stdout }
Write-Host "`n==== 1. lock-order inversion (threads do not overlap) ===="
foreach ($k in 'cs','mutex','srw') {
    $r = Run-Demo @("inversion-$k") "inversion-$k" -Detector
    $w = Warnings-For $r
    Check "inversion-$k : detector reports cycle for the two locks" ($w.Count -ge 1) "(exit=$($r.Exit) matching=$($w.Count) allWarnings=$((Warnings-All $r).Count) logBytes=$($r.LogSize))"
    if ($r.Exit -ne 0) { Write-Host "  stdout: $($r.Stdout)"; Write-Host "  stderr: $($r.Stderr)" }
}

Write-Host "`n==== 2. negative control (consistent order) ===="
$r = Run-Demo @('consistent-order') 'consistent-order' -Detector
$w = Warnings-For $r
Check "consistent-order : no cycle reported for the two locks" ($w.Count -eq 0) "(exit=$($r.Exit) matching=$($w.Count) allWarnings=$((Warnings-All $r).Count))"
$all = Warnings-All $r
if ($all.Count -gt 0) { Write-Host "  sample warnings (not about the demo locks = false positives from internal/system locks):"; $all | Select-Object -First 5 | ForEach-Object { Write-Host "   $_" } }

$r = Run-Demo @('srw-shared-only') 'srw-shared-only' -Detector
$w = Warnings-For $r
Check "srw-shared-only : shared/shared opposite order is not reported" ($w.Count -eq 0 -and $r.Exit -eq 0) "(exit=$($r.Exit) matching=$($w.Count))"

Write-Host "`n==== 3. real deadlock ===="
$r = Run-Demo @('deadlock') 'deadlock-nodetector'
Check "deadlock scenario really deadlocks without detector" ($r.Exit -eq 3) "(exit=$($r.Exit))"
$r = Run-Demo @('deadlock') 'deadlock-detector' -Detector
$w = Warnings-For $r
Check "deadlock : detector reports cycle while the threads are really blocked (exit 3 = hang confirmed)" ($w.Count -ge 1 -and $r.Exit -eq 3) "(exit=$($r.Exit) matching=$($w.Count) allWarnings=$((Warnings-All $r).Count) logBytes=$($r.LogSize))"

Write-Host "`n==== 3b. std::mutex ===="
foreach ($e in @(@('InversionDemo.exe','/MD'), @('InversionDemoMT.exe','/MT'))) {
    $exe = Join-Path $BinDir $e[0]
    $lbl = "std::mutex " + $e[1]
    $r = Run-Demo @('inversion-stdmutex') ("stdmutex-inv-" + $e[0]) -Detector -Exe $exe
    $n = (Warnings-All $r).Count
    Check "$lbl inversion: detector reports it" ($n -ge 1 -and $r.Exit -eq 0) "(exit=$($r.Exit) allWarnings=$n)"
    $r.Stdout -split "`r?`n" | Where-Object { $_ -match '^LOCKS' } | ForEach-Object { Write-Host "  $_" }
    (Warnings-All $r) | Select-Object -First 2 | ForEach-Object { Write-Host "  $_" }
    ($r.Log -split "`r?`n" | Where-Object { $_ -match 'STL|msvcp' }) | ForEach-Object { Write-Host "  $_" }

    $r = Run-Demo @('consistent-stdmutex') ("stdmutex-cons-" + $e[0]) -Detector -Exe $exe
    $n = (Warnings-All $r).Count
    Check "$lbl consistent order: no warning" ($n -eq 0 -and $r.Exit -eq 0) "(exit=$($r.Exit) allWarnings=$n)"

    $r = Run-Demo @('recursive-stdmutex') ("stdmutex-rec-" + $e[0]) -Detector -Exe $exe
    $n = (Warnings-All $r).Count
    Check "$lbl recursive_mutex re-lock: no warning" ($n -eq 0 -and $r.Exit -eq 0) "(exit=$($r.Exit) allWarnings=$n)"

    $r = Run-Demo @('deadlock-stdmutex') ("stdmutex-dl-" + $e[0]) -Detector -Exe $exe
    $n = (Warnings-All $r).Count
    Check "$lbl real deadlock: warning logged while blocked" ($n -ge 1 -and $r.Exit -eq 3) "(exit=$($r.Exit) allWarnings=$n)"
}

Write-Host "`n==== 4. benchmark (ns per lock+unlock, wall time / iters per thread) ===="
$bench = New-Object System.Collections.ArrayList
$cases = @(
  @('cs-own',1,20000), @('cs-own',4,20000), @('cs-shared',4,20000),
  @('srw-own',1,20000), @('mutex-own',1,20000), @('event-wait',1,20000)
)
foreach ($c in $cases) {
    foreach ($variant in 'plain','det','det+verbose') {
        $tag = "bench-$($c[0])-t$($c[1])-$variant"
        $a = @('bench',$c[0],$c[1],$c[2])
        if ($variant -eq 'plain') { $r = Run-Demo $a $tag }
        elseif ($variant -eq 'det') { $r = Run-Demo $a $tag -Detector }
        else { $r = Run-Demo $a $tag -Detector -Env @{ LOCKDETECTOR_VERBOSE = '1' } }
        $ns = $null
        if ($r.Stdout -match 'ns_per_op=([\d\.]+)') { $ns = [double]$Matches[1] }
        $row = [pscustomobject]@{ mode=$c[0]; threads=$c[1]; iters=$c[2]; variant=$variant; ns_per_op=$ns; exit=$r.Exit; timedOut=$r.TimedOut; logBytes=$r.LogSize }
        [void]$bench.Add($row)
        Write-Host ("{0,-11} t={1} iters={2} {3,-12} ns/op={4,-12} exit={5} timeout={6} log={7}B" -f $c[0],$c[1],$c[2],$variant,$ns,$r.Exit,$r.TimedOut,$r.LogSize)
    }
}
Write-Host "`n-- overhead ratio vs plain --"
foreach ($c in $cases) {
    $p  = $bench | Where-Object { $_.mode -eq $c[0] -and $_.threads -eq $c[1] -and $_.variant -eq 'plain' }
    foreach ($v in 'det','det+verbose') {
        $d = $bench | Where-Object { $_.mode -eq $c[0] -and $_.threads -eq $c[1] -and $_.variant -eq $v }
        if ($p.ns_per_op -and $d.ns_per_op) {
            Write-Host ("{0,-11} t={1} {2,-12} plain={3,9:N1} ns  detector={4,11:N1} ns  x{5:N1}" -f $c[0],$c[1],$v,$p.ns_per_op,$d.ns_per_op,($d.ns_per_op/$p.ns_per_op))
        } else { Write-Host ("{0,-11} t={1} {2,-12} no result" -f $c[0],$c[1],$v) }
    }
}

Write-Host "`n==== 5. lock-order graph growth (edges) ===="
foreach ($n in 50,100,200,300) {
    $r = Run-Demo @('edges',$n,3) "edges-$n" -Detector -Timeout 90
    $lines = ($r.Stdout -split "`r?`n" | Where-Object { $_ -match '^EDGES' }) -join ' | '
    Write-Host ("n={0} exit={1} timeout={2} log={3}B  {4}" -f $n,$r.Exit,$r.TimedOut,$r.LogSize,$lines)
}
$r = Run-Demo @('edges',300,3) 'edges-300-plain'
Write-Host ("plain n=300: " + (($r.Stdout -split "`r?`n" | Where-Object { $_ -match '^EDGES' }) -join ' | '))

Write-Host "`n==== summary ===="
$script:summary | ForEach-Object { Write-Host $_ }
if ($env:GITHUB_STEP_SUMMARY) {
    "### LockDetector checks`n" + (($script:summary | ForEach-Object { "- $_" }) -join "`n") | Add-Content $env:GITHUB_STEP_SUMMARY
}
Write-Host "failed checks: $script:fails"
if ($script:fails -gt 0 -and -not $NoFail) { exit 1 }
exit 0
