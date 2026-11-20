<#
.SYNOPSIS
    Подменяет MatrixGame.dll в установленной Space Rangers 2 / SR2 HD на нашу сборку.

.DESCRIPTION
    Сверяет экспорты оригинальной и новой DLL, делает бэкап оригинала с меткой времени
    и (при -Apply) копирует нашу сборку на место оригинала.

.PARAMETER GameDir
    Папка установленной игры (там, где лежит MatrixGame.dll).

.PARAMETER Build
    Путь к нашей собранной MatrixGame.dll. По умолчанию берётся самая свежая
    MatrixGame.dll, найденная в каталогах сборки репозитория.

.PARAMETER Apply
    Без этого ключа скрипт только проверяет и показывает, что будет сделано.

.EXAMPLE
    .\Setup-MatrixGameDll.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Space Rangers HD A War Apart"

.EXAMPLE
    .\Setup-MatrixGameDll.ps1 -GameDir "..." -Apply
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Build = "",
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'

$dumpbin = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.37.32822\bin\Hostx64\x86\dumpbin.exe"
$target = Join-Path $GameDir "MatrixGame.dll"

if (-not (Test-Path $GameDir)) { throw "Нет такой папки: $GameDir" }
if (-not (Test-Path $target)) { throw "В папке игры нет MatrixGame.dll: $target" }

if (-not $Build) {
    $root = Split-Path -Parent $PSScriptRoot
    $newest = Get-ChildItem -Path $root -Recurse -Filter "MatrixGame.dll" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\run' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $newest) { throw "Не нашёл собранную MatrixGame.dll. Соберите проект или укажите путь через -Build." }
    $Build = $newest.FullName
}

if (-not (Test-Path $Build)) { throw "Нет нашей сборки: $Build" }

function Get-Exports([string]$Path) {
    if (-not (Test-Path $dumpbin)) { return $null }
    $lines = & $dumpbin /nologo /exports $Path 2>&1
    $lines | Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)$' } |
        ForEach-Object { $Matches[1] } | Sort-Object -Unique
}

"Оригинал : {0} ({1:N0} Б)" -f $target, (Get-Item $target).Length
"Наша     : {0} ({1:N0} Б)" -f $Build, (Get-Item $Build).Length

$orig = Get-Exports $target
$mine = Get-Exports $Build

if ($null -ne $orig -and $null -ne $mine) {
    ""
    "Экспорты оригинала : $($orig -join ', ')"
    "Экспорты нашей DLL : $($mine -join ', ')"
    $missing = Compare-Object $orig $mine | Where-Object SideIndicator -eq '<='
    if ($missing) {
        "ВНИМАНИЕ: наша DLL не экспортирует: $($missing.InputObject -join ', ')"
        "Подмена, скорее всего, сломает загрузку игры."
    }
    else {
        "Все экспорты оригинала присутствуют в нашей DLL."
    }
}
else {
    "dumpbin не найден — сверку экспортов пропускаю."
}

$backup = Join-Path $GameDir ("MatrixGame.dll.orig-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
""
"Бэкап оригинала: $backup"

if (-not $Apply) {
    ""
    "Режим проверки. Чтобы подменить, запустите с -Apply."
    return
}

Copy-Item $target $backup -Force
Copy-Item $Build $target -Force
""
"Готово. Оригинал сохранён, наша DLL на месте."
"Откат: Copy-Item '{0}' '{1}' -Force" -f $backup, $target
