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

.PARAMETER AutoStart
    Click through the game menu (planetary battles -> START) to start the battle.
    The game does not enter the planetary battle module on its own, so without
    this the run just sits in the main menu. Coordinates are calibrated for
    2560x1440; pass -AutoStart:$false to drive the menu by hand.

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
    [int]$TraceBot = 0,
    [int]$TraceFrom = 0,
    [int]$TraceTo = 0,
    [int]$CmdBot = 0,
    [string]$CmdLog = "",
    [string]$CmdReplay = "",
    [switch]$Net,
    [int]$NetPort = 27777,
    [int]$NetLocalSide = 0,
    [int]$NetHashPeriod = 50,
    [int]$NetWaitMs = 60000,
    [string]$NetLog = "",
    [string]$NetHashLog = "",
    [bool]$AutoStart = $true,
    [switch]$NoDraw,
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
if ($Step -gt 0) { $env:MATRIXGAME_FIXEDSTEP = "$Step" } else { Remove-Item Env:\MATRIXGAME_FIXEDSTEP -ErrorAction SilentlyContinue }
$env:MATRIXGAME_MAXTICKS = "$Ticks"
$env:MATRIXGAME_MAP = $Map
$env:MATRIXGAME_HASHLOG = Join-Path $logs "$Name.txt"
$env:MATRIXGAME_OBJLOG = Join-Path $logs "$Name-obj.txt"
$env:MATRIXGAME_TRACE = Join-Path $logs "$Name-trace.txt"
if ($NoDraw) { $env:MATRIXGAME_NODRAW = '1' } else { Remove-Item Env:\MATRIXGAME_NODRAW -ErrorAction SilentlyContinue }
if ($HashStep -gt 0) { $env:MATRIXGAME_HASHSTEP = "$HashStep" } else { Remove-Item Env:\MATRIXGAME_HASHSTEP -ErrorAction SilentlyContinue }
if ($RandLog) {
    $env:MATRIXGAME_RANDLOG = Join-Path $logs "$Name-rand.txt"
}
else {
    Remove-Item Env:\MATRIXGAME_RANDLOG -ErrorAction SilentlyContinue
}
if ($Fill) { $env:MATRIXGAME_FILL = $Fill } else { Remove-Item Env:\MATRIXGAME_FILL -ErrorAction SilentlyContinue }
if ($Poison -gt 0) { $env:MATRIXGAME_POISON = "$Poison" } else { Remove-Item Env:\MATRIXGAME_POISON -ErrorAction SilentlyContinue }
if ($TraceBot -gt 0) {
    $env:MATRIXGAME_TRACEBOT = "$TraceBot"
    $env:MATRIXGAME_TRACEFROM = "$TraceFrom"
    $env:MATRIXGAME_TRACETO = "$TraceTo"
    $env:MATRIXGAME_TRACEBOTLOG = Join-Path $logs "$Name-bot.txt"
}
else {
    Remove-Item Env:\MATRIXGAME_TRACEBOT, Env:\MATRIXGAME_TRACEFROM, Env:\MATRIXGAME_TRACETO, Env:\MATRIXGAME_TRACEBOTLOG -ErrorAction SilentlyContinue
}

# Command layer (phase 2): the bot plays the player side with the same commands a
# human would issue, and writes them to a log; a replay run reads that log back
# and must reproduce the battle bit for bit.
$cmdLogPath = ""
if ($CmdLog) { $cmdLogPath = [System.IO.Path]::GetFullPath($CmdLog) }
elseif ($CmdBot -gt 0) { $cmdLogPath = Join-Path $logs "$Name-cmd.txt" }

if ($cmdLogPath) { $env:MATRIXGAME_CMDLOG = $cmdLogPath } else { Remove-Item Env:\MATRIXGAME_CMDLOG -ErrorAction SilentlyContinue }
if ($CmdReplay) { $env:MATRIXGAME_CMDREPLAY = [System.IO.Path]::GetFullPath($CmdReplay) } else { Remove-Item Env:\MATRIXGAME_CMDREPLAY -ErrorAction SilentlyContinue }
if ($CmdBot -gt 0) { $env:MATRIXGAME_CMDBOT = "$CmdBot" } else { Remove-Item Env:\MATRIXGAME_CMDBOT -ErrorAction SilentlyContinue }

# Network layer (phase 3): UDP socket, command exchange with a peer, tick barrier,
# world-hash comparison.
$netLogPath = ""
if ($Net) {
    if ($NetLog) { $netLogPath = [System.IO.Path]::GetFullPath($NetLog) }
    else { $netLogPath = Join-Path $logs "$Name-net.txt" }
    $env:MATRIXGAME_NET = '1'
    $env:MATRIXGAME_NET_PORT = "$NetPort"
    $env:MATRIXGAME_NET_LOCAL_SIDE = "$NetLocalSide"
    $env:MATRIXGAME_NET_HASH_PERIOD = "$NetHashPeriod"
    $env:MATRIXGAME_NET_WAIT_MS = "$NetWaitMs"
    $env:MATRIXGAME_NETLOG = $netLogPath
}
else {
    Remove-Item Env:\MATRIXGAME_NET, Env:\MATRIXGAME_NET_PORT, Env:\MATRIXGAME_NET_LOCAL_SIDE, Env:\MATRIXGAME_NET_HASH_PERIOD, Env:\MATRIXGAME_NET_WAIT_MS, Env:\MATRIXGAME_NETLOG -ErrorAction SilentlyContinue
}

# The net world hash is written even without a socket, so an offline run gives the
# reference sequence that a networked run is compared against.
$netHashPath = ""
if ($NetHashLog) { $netHashPath = [System.IO.Path]::GetFullPath($NetHashLog) }
elseif ($Net) { $netHashPath = Join-Path $logs "$Name-nethash.txt" }
if ($netHashPath) { $env:MATRIXGAME_NETHASHLOG = $netHashPath } else { Remove-Item Env:\MATRIXGAME_NETHASHLOG -ErrorAction SilentlyContinue }

"run      : $Name"
"map/seed : $Map / $Seed, step $(if ($Step -gt 0) { "$Step ms" } else { "measured (whole ticks)" }), max $Ticks ms, hash step $(if ($HashStep -gt 0) { $HashStep } else { 100 }) ms"
"alloc fill: $(if ($Fill) { "0x$Fill" } else { '00 (default)' })$(if ($Poison -gt 0) { ", stack poison $Poison B" } else { '' })$(if ($RandLog) { ', rand log on' } else { '' })"
"autostart: $(if ($AutoStart) { 'menu -> planetary battles -> START' } else { 'off' })$(if ($NoDraw) { ', no draw' } else { '' })"
if ($CmdBot -gt 0) { "commands : bot every $CmdBot ticks -> $cmdLogPath" }
if ($CmdReplay) { "commands : replay from $($env:MATRIXGAME_CMDREPLAY)" }
if ($Net) { "net      : udp $NetPort, local side $NetLocalSide, log $netLogPath" }
if ($netHashPath) { "nethash  : $netHashPath" }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru

# The planetary battle can only be started from the game menu: bottom-left icon
# "planetary battles", then "START". Coordinates are fractions of the client
# area, calibrated on 2560x1440; recalibrate for another resolution.
if ($AutoStart) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class SimClick {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@ -ErrorAction SilentlyContinue

    # The window handle can be zero for the first seconds and it is not an
    # integer, so compare against IntPtr::Zero explicitly.
    $deadline = (Get-Date).AddSeconds(120)
    $hwnd = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline) {
        $proc.Refresh()
        if ($proc.HasExited) {
            # The usual reason is Steam: without a signed-in account that owns
            # the game, Rangers.exe exits at once with code 0 and writes nothing.
            throw "Game exited immediately (code $($proc.ExitCode)). Is Steam signed in with the account that owns the game?"
        }
        $h = $proc.MainWindowHandle
        if ($h -ne $null -and $h -ne [IntPtr]::Zero) { $hwnd = $h; break }
        Start-Sleep -Milliseconds 300
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "Game window did not appear" }
    Start-Sleep -Seconds 15  # the menu is not drawn instantly

    [void][SimClick]::SetForegroundWindow($hwnd)

    function ClickAt([double]$fx, [double]$fy) {
        $x = [int](2560 * $fx)
        $y = [int](1440 * $fy)
        [void][SimClick]::SetCursorPos($x, $y)
        Start-Sleep -Milliseconds 250
        [SimClick]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 90
        [SimClick]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 600
    }

    ClickAt 0.0438 0.948   # "planetary battles"
    ClickAt 0.5902 0.6493  # "START" in the map list
}

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

if ($cmdLogPath -and (Test-Path $cmdLogPath)) {
    $cmdLines = Get-Content $cmdLogPath
    "cmd log  : $cmdLogPath ($($cmdLines.Count) lines)"
    $cmdLines | Select-Object -Last 2 | ForEach-Object { "  $_" }
}

if ($netLogPath -and (Test-Path $netLogPath)) {
    $netLines = Get-Content $netLogPath
    "net log  : $netLogPath ($($netLines.Count) lines)"
    $netLines | Select-Object -Last 4 | ForEach-Object { "  $_" }
}
