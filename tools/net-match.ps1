<#
.SYNOPSIS
    Launches one side of a network match on this machine.

.DESCRIPTION
    Wraps tools\Run-Sim.ps1 with the right combination of variables for a
    two-machine match, so nobody has to remember the flag soup:

      host      - plays side 1, listens on -Port (bind 0.0.0.0), sends the
                  command stream to the peer;
      client    - plays side 2, greets -Peer, applies both sides' commands;
      observer  - plays nothing, only simulates and compares hashes (the
                  cheapest way to check that two machines stay in sync).

    The battle starts automatically (the menu is clicked for you). Both
    machines must be started within about a minute of each other: the host
    waits up to 60 s for the peer, then falls back to an offline run.

    The harness build of MatrixGame.dll must be installed into the game; pass
    -InstallDll to copy it from build-sim (the game must not be running).

.EXAMPLE
    # машина A
    .\net-match.ps1 -Role host

    # машина B
    .\net-match.ps1 -Role client -Peer 192.168.1.10:27777

    # затем сравнить хеш-логи (оба файла лежат в tools\logs)
    python tools\simdiff.py tools\logs\net-host.txt tools\logs\net-client.txt
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('host', 'client', 'observer')][string]$Role,
    [string]$Peer = "",
    [string]$Map = "armagedd",
    [int]$Seed = 12345,
    [int]$Ticks = 120000,
    [int]$Port = 27777,
    [int]$PeerPort = 27778,
    [string]$Sides = "1,2",
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Space Rangers HD A War Apart",
    [string]$Build = "",
    [switch]$InstallDll,
    [switch]$Bot,
    [int]$TimeoutSec = 3600
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $Build) { $Build = Join-Path $root 'build-sim\MatrixGame\Release\MatrixGame.dll' }

$installed = Join-Path $GameDir 'MatrixGame.dll'
if (-not (Test-Path $installed)) { throw "No MatrixGame.dll in the game folder: $installed" }

if ($InstallDll) {
    if (-not (Test-Path $Build)) { throw "No harness build: $Build" }
    & (Join-Path $PSScriptRoot 'Setup-MatrixGameDll.ps1') -GameDir $GameDir -Build $Build -Apply | Out-Null
}
elseif ((Test-Path $Build) -and ((Get-Item $installed).Length -ne (Get-Item $Build).Length)) {
    Write-Warning "The DLL in the game folder is not the harness build. Add -InstallDll (and close the game first)."
}

$name = "net-$Role"
$common = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $PSScriptRoot 'Run-Sim.ps1'),
    '-Name', $name,
    '-GameDir', $GameDir,
    '-Map', $Map,
    '-Seed', "$Seed",
    '-Ticks', "$Ticks",
    '-HashStep', '500',
    '-Net',
    '-NetSides', $Sides,
    '-NetHashPeriod', '50',
    '-TimeoutSec', "$TimeoutSec"
)

switch ($Role) {
    'host' {
        $args2 = $common + @('-NetBind', '0.0.0.0', '-NetPort', "$Port", '-NetLocalSide', '1')
        if ($Bot) {
            # Играет за обе стороны — нужен, когда вторая машина только наблюдает.
            $args2 += @('-CmdBot', '50', '-CmdBotAll')
        }
        else {
            $args2 += @('-CmdBot', '0')
        }
        "role     : host (side 1), listening on 0.0.0.0:$Port$(if ($Bot) { ', bot plays both sides' } else { '' })"
        "peer     : waiting for a client, up to 60 s"
    }
    'client' {
        if (-not $Peer) { throw "-Peer host:port is required for the client role" }
        $args2 = $common + @('-NetPeer', $Peer, '-NetPort', "$PeerPort", '-NetLocalSide', '2',
                             '-CmdBot', '0')
        "role     : client (side 2), peer $Peer"
    }
    'observer' {
        if (-not $Peer) { throw "-Peer host:port is required for the observer role" }
        $args2 = $common + @('-NetPeer', $Peer, '-NetPort', "$PeerPort", '-NetLocalSide', '0',
                             '-NetObserver')
        "role     : observer, peer $Peer (no local commands, only hash check)"
    }
}

"map/seed : $Map / $Seed, sides $Sides"
"logs     : $(Join-Path $root "tools\logs\$name.txt") (-net.txt for the network log)"
""
"Starting the game; if the menu does not advance by itself, click"
"'planetary battles' and then 'START' in the map list."

& powershell @args2
