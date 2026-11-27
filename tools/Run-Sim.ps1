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
    [switch]$CmdBotAll,
    [string]$CmdLog = "",
    [string]$CmdReplay = "",
    [switch]$Net,
    [int]$NetPort = 27777,
    [int]$NetLocalSide = 0,
    [int]$NetHashPeriod = 50,
    [int]$NetWaitMs = 60000,
    [string]$NetLog = "",
    [string]$NetHashLog = "",
    [string]$NetPeer = "",
    [string]$NetSides = "",
    [switch]$NetObserver,
    [string]$NetBind = "",
    [int]$WindowX = -100000,
    [int]$WindowY = 0,
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

# Stale files from a previous run of the same name would look like "the battle
# already started" to the autostart loop below.
Remove-Item $env:MATRIXGAME_HASHLOG, $env:MATRIXGAME_OBJLOG, $env:MATRIXGAME_TRACE -ErrorAction SilentlyContinue
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
if ($CmdBotAll) { $env:MATRIXGAME_CMDBOT_ALL = '1' } else { Remove-Item Env:\MATRIXGAME_CMDBOT_ALL -ErrorAction SilentlyContinue }

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

if ($NetPeer) { $env:MATRIXGAME_NET_PEER = $NetPeer } else { Remove-Item Env:\MATRIXGAME_NET_PEER -ErrorAction SilentlyContinue }
if ($NetSides) { $env:MATRIXGAME_NET_SIDES = $NetSides } else { Remove-Item Env:\MATRIXGAME_NET_SIDES -ErrorAction SilentlyContinue }
if ($NetObserver) { $env:MATRIXGAME_NET_OBSERVER = '1' } else { Remove-Item Env:\MATRIXGAME_NET_OBSERVER -ErrorAction SilentlyContinue }
if ($NetBind) { $env:MATRIXGAME_NET_BIND = $NetBind } else { Remove-Item Env:\MATRIXGAME_NET_BIND -ErrorAction SilentlyContinue }

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
if ($CmdBotAll -and $CmdBot -gt 0) { "commands : bot plays every human side (offline reference)" }
if ($CmdReplay) { "commands : replay from $($env:MATRIXGAME_CMDREPLAY)" }
if ($Net) { "net      : udp $NetPort, local side $NetLocalSide, log $netLogPath" }
if ($NetPeer) { "net peer : $NetPeer" }
if ($NetSides) { "net sides: $NetSides (human-controlled)" }
if ($NetObserver) { "net      : observer mode, no local commands" }
if ($NetBind) { "net bind : $NetBind" }
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
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);

  // MainWindowHandle can point at a splash or a hidden 160x28 window, and with
  // two instances running it is easy to grab the wrong one. Pick the biggest
  // visible window of the process instead.
  public static IntPtr FindBigWindow(int pid, int minW, int minH) {
    IntPtr best = IntPtr.Zero;
    int bestArea = 0;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p;
      GetWindowThreadProcessId(h, out p);
      if (p != (uint)pid) return true;
      if (!IsWindowVisible(h)) return true;
      RECT r;
      if (!GetClientRect(h, out r)) return true;
      int w = r.Right - r.Left;
      int hh = r.Bottom - r.Top;
      if (w < minW || hh < minH) return true;
      if (w * hh > bestArea) { bestArea = w * hh; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }
}
'@ -ErrorAction SilentlyContinue

    $deadline = (Get-Date).AddSeconds(180)
    $hwnd = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline) {
        $proc.Refresh()
        if ($proc.HasExited) {
            # The usual reason is Steam: without a signed-in account that owns
            # the game, Rangers.exe exits at once with code 0 and writes nothing.
            throw "Game exited immediately (code $($proc.ExitCode)). Is Steam signed in with the account that owns the game?"
        }
        $hwnd = [SimClick]::FindBigWindow($proc.Id, 640, 480)
        if ($hwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 300
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        # A stuck process holds the DLL and blocks the next run, so kill it first.
        if (-not $proc.HasExited) { $proc.Kill() }
        throw "Game window did not appear"
    }
    Start-Sleep -Seconds 15  # the menu is not drawn instantly

    # Two copies of the game may run at once (netplay test): put this one at a
    # known place so its clicks do not land on the other window.
    if ($WindowX -gt -100000) {
        [void][SimClick]::SetWindowPos($hwnd, [IntPtr]::Zero, $WindowX, $WindowY, 0, 0, 0x0001 -bor 0x0004)
        Start-Sleep -Milliseconds 500
    }

    # Clicks are fractions of the client area of *this* window, not of the screen.
    $clientRect = New-Object SimClick+RECT
    [void][SimClick]::GetClientRect($hwnd, [ref]$clientRect)
    $origin = New-Object SimClick+POINT
    $origin.X = 0
    $origin.Y = 0
    [void][SimClick]::ClientToScreen($hwnd, [ref]$origin)
    $clientW = $clientRect.Right - $clientRect.Left
    $clientH = $clientRect.Bottom - $clientRect.Top
    "window   : $hwnd at $($origin.X),$($origin.Y) size $($clientW)x$($clientH)"

    [void][SimClick]::SetForegroundWindow($hwnd)

    function ClickAt([double]$fx, [double]$fy) {
        $x = $origin.X + [int]($clientW * $fx)
        $y = $origin.Y + [int]($clientH * $fy)
        [void][SimClick]::SetForegroundWindow($hwnd)
        [void][SimClick]::SetCursorPos($x, $y)
        Start-Sleep -Milliseconds 250
        [SimClick]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 90
        [SimClick]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 600
    }

    # The menu sometimes takes longer than 15 s to draw, and then the clicks land
    # on the loading screen and nothing happens. Click, wait for the harness trace
    # (it is written as soon as the battle starts) and try again if needed.
    $tracePath = $env:MATRIXGAME_TRACE
    $started = $false
    for ($attempt = 0; $attempt -lt 6 -and -not $started; $attempt++) {
        if ($attempt -gt 0) { Start-Sleep -Seconds 4 }

        ClickAt 0.0438 0.948   # "planetary battles"
        Start-Sleep -Milliseconds 900
        ClickAt 0.5902 0.6493  # "START" in the map list

        if (-not $tracePath) { $started = $true; break }

        for ($i = 0; $i -lt 24; $i++) {
            Start-Sleep -Milliseconds 500
            $proc.Refresh()
            if ($proc.HasExited) { break }
            if (Test-Path $tracePath) { $started = $true; break }
        }
        if (-not $started) { "autostart: attempt $($attempt + 1) did not start the battle, retrying" }
    }
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
