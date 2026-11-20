<#
.SYNOPSIS
    Runs one deterministic simulation pass and waits for the game to exit.

.DESCRIPTION
    Sets the MATRIXGAME_* environment variables expected by the harness, starts
    the game, waits for it to finish and prints the tail of the hash log.
    Requires MatrixGame.dll built with -DMATRIXGAME_SIM_HARNESS=ON to be
    installed into the game folder (see Setup-MatrixGameDll.ps1).

.PARAMETER Name
    Log name prefix; files land in tools\logs\<Name>.txt and <Name>-obj.txt.

.PARAMETER Fill
    Byte used to fill fresh allocations (hex, e.g. CD or AA). Empty = harness
    default (00). Comparing two runs with different fills shows which fields are
    read before being initialized.

.EXAMPLE
    .\Run-Sim.ps1 -Name run19a -Fill CD
    .\Run-Sim.ps1 -Name run19b -Fill AA
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Space Rangers HD A War Apart",
    [string]$Map = "armagedd",
    [int]$Seed = 12345,
    [int]$Ticks = 30000,
    [int]$Step = 10,
    [string]$Fill = "",
    [int]$HashStep = 0,
    [switch]$RandLog,
    [int]$Poison = 0,
    [int]$TimeoutSec = 900
)

$ErrorActionPreference = 'Stop'

$logs = Join-Path $PSScriptRoot 'logs'
if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Path $logs | Out-Null }

$exe = Join-Path $GameDir 'Rangers.exe'
if (-not (Test-Path $exe)) { throw "No game executable: $exe" }

$dll = Join-Path $GameDir 'MatrixGame.dll'
if (-not (Test-Path $dll)) { throw "No MatrixGame.dll in the game folder: $dll" }

$env:MATRIXGAME_SIM = '1'
$env:MATRIXGAME_SEED = "$Seed"
$env:MATRIXGAME_FIXEDSTEP = "$Step"
$env:MATRIXGAME_MAXTICKS = "$Ticks"
$env:MATRIXGAME_MAP = $Map
$env:MATRIXGAME_HASHLOG = Join-Path $logs "$Name.txt"
$env:MATRIXGAME_OBJLOG = Join-Path $logs "$Name-obj.txt"
$env:MATRIXGAME_TRACE = Join-Path $logs "$Name-trace.txt"
if ($HashStep -gt 0) { $env:MATRIXGAME_HASHSTEP = "$HashStep" } else { Remove-Item Env:\MATRIXGAME_HASHSTEP -ErrorAction SilentlyContinue }
if ($RandLog) {
    $env:MATRIXGAME_RANDLOG = Join-Path $logs "$Name-rand.txt"
}
else {
    Remove-Item Env:\MATRIXGAME_RANDLOG -ErrorAction SilentlyContinue
}
if ($Fill) { $env:MATRIXGAME_FILL = $Fill } else { Remove-Item Env:\MATRIXGAME_FILL -ErrorAction SilentlyContinue }
if ($Poison -gt 0) { $env:MATRIXGAME_POISON = "$Poison" } else { Remove-Item Env:\MATRIXGAME_POISON -ErrorAction SilentlyContinue }

"run      : $Name"
"map/seed : $Map / $Seed, step $Step ms, max $Ticks ms, hash step $(if ($HashStep -gt 0) { $HashStep } else { 100 }) ms"
"alloc fill: $(if ($Fill) { "0x$Fill" } else { '00 (default)' })$(if ($Poison -gt 0) { ", stack poison $Poison B" } else { '' })$(if ($RandLog) { ', rand log on' } else { '' })"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    $proc.Kill()
    throw "Timeout after $TimeoutSec s"
}
$sw.Stop()

"exit     : $($proc.ExitCode) in $([int]$sw.Elapsed.TotalSeconds) s"

if (Test-Path $env:MATRIXGAME_TRACE) {
    $trace = Get-Content $env:MATRIXGAME_TRACE -ErrorAction SilentlyContinue
    if ($trace) { "trace    :"; $trace | Select-Object -Last 5 | ForEach-Object { "  $_" } }
}

if (Test-Path $env:MATRIXGAME_HASHLOG) {
    $lines = Get-Content $env:MATRIXGAME_HASHLOG
    "hash log : $($lines.Count) lines"
    $lines | Select-Object -Last 2 | ForEach-Object { "  $_" }
}
else {
    "hash log : MISSING ($($env:MATRIXGAME_HASHLOG))"
}

if ($RandLog -and (Test-Path $env:MATRIXGAME_RANDLOG)) {
    "rand log : $($env:MATRIXGAME_RANDLOG)"
}
