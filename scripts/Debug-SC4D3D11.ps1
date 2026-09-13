param(
    [string]$GameExe = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe',
    [string]$UserDir = 'D:\OneDrive - Maplix\SimCity 4\',
    [string]$SymbolDir = 'C:\Users\caspe\CLionProjects\scd3d11\build\review',
    [string]$LogPath = "$env:TEMP\cdb-sc4.log",
    [int]$Width = 1024,
    [int]$Height = 768,
    [string]$Commands = 'sxe av; g; .echo ===CRASH===; r; kb 60; .ecxr; kb 60; lm m SCD3D11; q',
    [string]$ScriptFile = ''
)

# Runs SimCity 4 under cdb so an access violation produces a symbolised stack
# instead of SC4's own (symbol-less) exception report.

$ErrorActionPreference = 'Stop'
$cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe'
$gameDir = Split-Path -Parent $GameExe
Remove-Item -LiteralPath $LogPath -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $UserDir 'SC4D3D11.log') -ErrorAction SilentlyContinue

$arguments = @(
    '-logo', $LogPath,
    '-y', "$SymbolDir;srv*C:\symbols*https://msdl.microsoft.com/download/symbols"
)
if ($ScriptFile) { $arguments += @('-cf', $ScriptFile) } else { $arguments += @('-c', $Commands) }
$arguments += @(
    $GameExe,
    "-UserDir:$UserDir",
    '-CPUCount:1',
    '-CustomResolution:enabled',
    "-r${Width}x${Height}x32",
    '-w'
)

Push-Location $gameDir
try {
    & $cdb @arguments
} finally {
    Pop-Location
}
Write-Output "cdb log: $LogPath"
