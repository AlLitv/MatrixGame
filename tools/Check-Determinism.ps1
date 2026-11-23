<#
.SYNOPSIS
    Runs the whole determinism acceptance check for one harness build.

.DESCRIPTION
    Installs the harness DLL into the game, then runs the pairs:
      1. 0xCD / 0xAA with rendering off        - the main check
      2. 0xCD / 0xAA with rendering on         - catches frame-driven leaks
      3. 0xCD / 0xAA with heap zeroing off     - proves the allocator workaround
                                                 is no longer needed (Phase 0.4)
    Every pair is compared with simdiff.py and the exit code is non-zero if any
    pair diverges.

.PARAMETER GameDir
    Installed Space Rangers HD folder (Steam).

.PARAMETER Build
    Path to MatrixGame.dll built with -DMATRIXGAME_SIM_HARNESS=ON.

.PARAMETER Ticks
    Game time per run, ms.

.PARAMETER Seed
    Fixed logic seed.

.PARAMETER SkipDrawn / SkipNoZero
    Drop individual pairs, e.g. for a quick pass.

.EXAMPLE
    .\Check-Determinism.ps1 -Build ..\build-sim\MatrixGame\Release\MatrixGame.dll
#>
[CmdletBinding()]
param(
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Space Rangers HD A War Apart",
    [Parameter(Mandatory = $true)][string]$Build,
    [int]$Ticks = 60000,
    [int]$Seed = 12345,
    [int]$HashStep = 500,
    [switch]$SkipDrawn,
    [switch]$SkipNoZero,
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'

$runSim = Join-Path $PSScriptRoot 'Run-Sim.ps1'
$simdiff = Join-Path $PSScriptRoot 'simdiff.py'
$logs = Join-Path $PSScriptRoot 'logs'

function Invoke-Pair {
    param([string]$Tag, [string[]]$Extra)

    $a = "check-$Tag-cd"
    $b = "check-$Tag-aa"

    & powershell -NoProfile -ExecutionPolicy Bypass -File $runSim -Name $a -Fill CD -Seed $Seed `
        -Ticks $Ticks -HashStep $HashStep -GameDir $GameDir -TimeoutSec $TimeoutSec @Extra | Out-Null
    & powershell -NoProfile -ExecutionPolicy Bypass -File $runSim -Name $b -Fill AA -Seed $Seed `
        -Ticks $Ticks -HashStep $HashStep -GameDir $GameDir -TimeoutSec $TimeoutSec @Extra | Out-Null

    $logA = Join-Path $logs "$a.txt"
    $logB = Join-Path $logs "$b.txt"
    if (-not (Test-Path $logA) -or -not (Test-Path $logB)) {
        Write-Host "  $Tag : NO LOG (the battle never started - is Steam signed in?)"
        return $false
    }

    $out = & python $simdiff $logA $logB 2>&1
    $ok = ($LASTEXITCODE -eq 0)
    $summary = ($out | Select-String -Pattern 'IDENTICAL|FIRST DIVERGENCE' | Select-Object -First 1)
    Write-Host "  $Tag : $(if ($ok) { 'PASS' } else { 'FAIL' }) - $summary"
    if (-not $ok) { $out | Select-Object -First 6 | ForEach-Object { Write-Host "        $_" } }
    return $ok
}

"== determinism check =="
"game    : $GameDir"
"build   : $Build"
"map/seed: armagedd / $Seed, ticks $Ticks, hash step $HashStep ms"

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Setup-MatrixGameDll.ps1') `
    -GameDir $GameDir -Build $Build -Apply | Out-Null

Get-Process Rangers -ErrorAction SilentlyContinue | Stop-Process -Force

$results = @()

$results += Invoke-Pair -Tag 'nodraw' -Extra @('-NoDraw')
if (-not $SkipDrawn) {
    $results += Invoke-Pair -Tag 'drawn' -Extra @()
}
if (-not $SkipNoZero) {
    $results += Invoke-Pair -Tag 'nozero' -Extra @('-NoDraw', '-NoZero')
}

$failed = ($results | Where-Object { -not $_ }).Count
""
if ($failed -eq 0) {
    "RESULT: PASS ($($results.Count) pairs identical)"
    exit 0
}
"RESULT: FAIL ($failed of $($results.Count) pairs diverged)"
exit 1
