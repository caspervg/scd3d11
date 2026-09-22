param(
    [string]$GameExe = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe',
    # Used only to locate plugins/logs. Do not override the game's user directory.
    [string]$UserDir = 'C:\Users\caspe\Documents\SimCity 4\',
    [string[]]$GameArguments = @('-CPUCount:8', '-w', '-r3200x1800x32', '-CustomResolution:enabled', '-Intro:off', '-NativeShadowMasks:all'),
    [string]$CaptureRoot = (Join-Path $PSScriptRoot '..\runtime-captures'),
    [switch]$DisableDriverThreading,
    [switch]$WithoutValidation,
    [switch]$PrepareOnly
)

# Preserve the installed DLL and its code generation. Change only D3D11 creation
# flags at the exported x86 API boundary, using the debugger. No plugin deployment.
$ErrorActionPreference = 'Stop'
$cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe'
foreach ($required in @($cdb, $GameExe, $UserDir)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing: $required" }
}
$GameExe = (Resolve-Path -LiteralPath $GameExe).Path
$UserDir = (Resolve-Path -LiteralPath $UserDir).Path.TrimEnd('\')
$captureName = 'amd-crash-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$capture = (New-Item -ItemType Directory -Path (Join-Path $CaptureRoot $captureName)).FullName
$commandsPath = Join-Path $capture 'capture.cdb'
$debuggerLog = Join-Path $capture 'debugger.log'
$dumpPath = Join-Path $capture 'exception.dmp'
if ($dumpPath.Contains('"')) { throw 'Capture path must not contain a double quote.' }

$flags = 0
if (-not $WithoutValidation) { $flags = $flags -bor 2 }
if ($DisableDriverThreading) { $flags = $flags -bor 8 }
$commands = @(
    '.symopt+0x400',
    'sxe av',
    'sxe -c ".echo AMD capture: SCD3D11 loaded; lm m SCD3D11; g" ld:SCD3D11.dll'
)
if ($flags -ne 0) {
    # stdcall x86: Flags is argument four, at ESP+0x10, for both entry points.
    # The capability probe also creates a device, so instrument both APIs.
    # Use processor breakpoints, not bu: ReShade's dxgi.dll proxy patches a
    # 5-byte jmp over these same prologues, and the debugger restoring its saved
    # int3 byte on top of that jmp leaves garbage code (AV at the export entry).
    $arm = foreach ($api in @('D3D11CreateDevice', 'D3D11CreateDeviceAndSwapChain')) {
        'ba e1 d3d11!{0} \".echo AMD capture: {0} flags before; dd @esp+0x10 L1; ed @esp+0x10 (poi(@esp+0x10)|0x{1:x}); .echo flags after; dd @esp+0x10 L1; g\"' -f $api, $flags
    }
    $commands += ('sxe -c "{0}; .echo AMD capture: d3d11 flag breakpoints armed; g" ld:d3d11.dll' -f ($arm -join '; '))
}
$commands += @(
    'g',
    '.echo === STOPPED: inspect exception before attributing it to AMD ===',
    '.exr -1',
    '.ecxr',
    'r',
    '~* kv',
    'lmv',
    ('.dump /ma "{0}"' -f $dumpPath),
    '.echo === Capture finished. Debugger remains stopped. Use q to quit or g to continue. ==='
)
$commands | Set-Content -LiteralPath $commandsPath -Encoding ASCII
$launchArguments = $GameArguments
[pscustomobject]@{
    Created = (Get-Date).ToString('o')
    GameExe = $GameExe
    GameArguments = $launchArguments
    PluginAndLogDirectory = $UserDir
    RequestedAdditionalDeviceFlags = ('0x{0:x}' -f $flags)
    Debugger = $cdb
    Note = 'First debugger stop after startup is captured; inspect its exception code. Full dump includes process memory.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $capture 'run.json') -Encoding UTF8

# Hash the currently installed renderer copies for build identification, without
# assuming that any local PDB matches them. The dump records what actually loaded.
$pluginRoots = @((Join-Path $UserDir 'Plugins'), (Join-Path (Split-Path (Split-Path $GameExe)) 'Plugins'))
$pluginRoots | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -Filter 'SCD3D11.dll' -Recurse -File
} | ForEach-Object { Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 } |
    Select-Object Path, Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $capture 'renderer-hashes.json') -Encoding UTF8

Write-Output "Capture directory: $capture"
Write-Output "Game arguments: $($launchArguments -join ' ')"
Write-Output 'Validation messages go directly to debugger.log. Preserve this directory after a crash.'
if ($PrepareOnly) { return }
Push-Location (Split-Path -Parent $GameExe)
try {
    # Ignore the process-exit stop so an ordinary exit does not produce a crash dump.
    & $cdb '-G' '-logo' $debuggerLog '-cf' $commandsPath $GameExe @launchArguments
} finally {
    Pop-Location
    # LogRedirect may move the renderer log into Logs. Preserve either location.
    foreach ($relative in @('SC4D3D11.log', 'Logs\SC4D3D11.log')) {
        $sourceLog = Join-Path $UserDir $relative
        if (Test-Path -LiteralPath $sourceLog) {
            Copy-Item -LiteralPath $sourceLog -Destination (Join-Path $capture ($relative.Replace('\', '-')))
        }
    }
}
