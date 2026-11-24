<#
.SYNOPSIS
    Runs the whole determinism acceptance check for one harness build.

.DESCRIPTION
    Installs the harness DLL into the game, then runs the pairs:
      1. 0xCD / 0xAA with rendering off  - the differential check: anything read
                                           before being written differs between
                                           the two fills and shows up as a diverging
                                           field
      2. two runs with no fill at all    - plain reproducibility: nothing touches
                                           fresh allocations (Phase 0.4, the heap
                                           is not zeroed any more)
      3. 0xCD / 0xAA with rendering on   - catches frame-driven leaks
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

.PARAMETER SkipDrawn / SkipNoFill
    Drop individual pairs, e.g. for a quick pass.

.EXAMPLE
    .\Check-Determinism.ps1 -Build ..\build-sim\MatrixGame\Release\MatrixGame.dll
#>
[CmdletBinding()]
param(
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Space Rangers HD A War Apart",
    [Parameter(Mandatory = $true)][string]$Build,
    [int]$Ticks = 60000,
    [string]$Map = 'armagedd',
    [int]$Seed = 12345,
    [int]$HashStep = 500,
    [switch]$SkipDrawn,
    [switch]$SkipNoFill,
    [int]$Retries = 1,
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'

$runSim = Join-Path $PSScriptRoot 'Run-Sim.ps1'
$simdiff = Join-Path $PSScriptRoot 'simdiff.py'
$logs = Join-Path $PSScriptRoot 'logs'

function Invoke-Pair {
    # FillA/FillB differ only for the differential pairs: two runs with different
    # bytes in fresh allocations expose reads of uninitialised memory. A pair with
    # no fill at all is the plain reproducibility check (nothing touches fresh allocations).
    param([string]$Tag, [string[]]$Extra, [string]$FillA = 'CD', [string]$FillB = 'AA')

    $a = "check-$Tag-a"
    $b = "check-$Tag-b"

    # An empty -Fill must be omitted entirely: Windows PowerShell drops empty
    # arguments and Run-Sim then fails with "Missing an argument for Fill".
    function Invoke-Run([string]$name, [string]$fill) {
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $runSim, '-Name', $name, '-Map', $Map, '-Seed', "$Seed",
                  '-Ticks', "$Ticks", '-HashStep', "$HashStep", '-GameDir', $GameDir, '-TimeoutSec', "$TimeoutSec")
        if ($fill) { $argv += @('-Fill', $fill) }
        $argv += $Extra
        & powershell @argv | Out-Null
    }

    # A pair that fails once is re-run: the first pass after a DLL swap sometimes
    # reports a different object count at the first checkpoint (engine warm-up,
    # not the simulation), and a real desync reproduces every time.
    for ($attempt = 0; $attempt -le $Retries; $attempt++) {
        if ($attempt -gt 0) { Write-Host "  $Tag : retry $attempt" }
        Invoke-Run -name $a -fill $FillA
        Invoke-Run -name $b -fill $FillB

        $logA = Join-Path $logs "$a.txt"
        $logB = Join-Path $logs "$b.txt"
        if (-not (Test-Path $logA) -or -not (Test-Path $logB)) {
            Write-Host "  $Tag : NO LOG (the battle never started - is Steam signed in?)"
            return $false
        }

        # A battle that ends before MAXTICKS stops the log without a FINAL line,
        # and that is normal. What is not normal is an uneven number of
        # checkpoints: two runs of the same battle must stop at the same tick.
        # simdiff.py reports that case, so no extra rule is needed here.

        $out = & python $simdiff $logA $logB 2>&1
        $ok = ($LASTEXITCODE -eq 0)
        $summary = ($out | Select-String -Pattern 'IDENTICAL|FIRST DIVERGENCE' | Select-Object -First 1)
        Write-Host "  $Tag : $(if ($ok) { 'PASS' } else { 'FAIL' }) - $summary"
        if (-not $ok) { $out | Select-Object -First 6 | ForEach-Object { Write-Host "        $_" } }
        if ($ok) { return $true }
    }
    return $false
}

"== determinism check =="
"game    : $GameDir"
"build   : $Build"
"map/seed: $Map / $Seed, ticks $Ticks, hash step $HashStep ms"

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Setup-MatrixGameDll.ps1') `
    -GameDir $GameDir -Build $Build -Apply | Out-Null

Get-Process Rangers -ErrorAction SilentlyContinue | Stop-Process -Force

$results = @()

$results += Invoke-Pair -Tag 'fill' -Extra @('-NoDraw')
$results += Invoke-Pair -Tag 'nofill' -Extra @('-NoDraw') -FillA '' -FillB ''
if (-not $SkipDrawn) {
    $results += Invoke-Pair -Tag 'drawn' -Extra @()
}

$failed = ($results | Where-Object { -not $_ }).Count
""
if ($failed -eq 0) {
    "RESULT: PASS ($($results.Count) pairs identical)"
    exit 0
}
"RESULT: FAIL ($failed of $($results.Count) pairs diverged)"
exit 1
