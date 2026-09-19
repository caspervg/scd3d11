# Phase 1 shadow capture: build the driver, run SimCity 4 with -NativeShadowDiag,
# drive the camera across zooms and rotations so every LOD is asked for, then
# collect SC4ShadowDiag.log into runtime-captures/.
#
# The hooks are read-only, so this run renders exactly what an unhooked run does.
param(
    [string]$Region = 'London',
    [string]$City = 'Fulham',
    [string]$Budget = '20000',
    [string[]]$Cells = @('64 64'),
    [int]$ShadowQuality = 5,
    [string[]]$ExtraArgs = @(),
    [switch]$Screenshot,
    [int]$Width = 1600,
    [int]$Height = 900,
    [switch]$SkipBuild,
    [switch]$KeepRunning,
    [string]$GameExe = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe',
    [string]$UserDir = 'C:\Users\caspe\Documents\SimCity 4'
)

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'cmake-build-release'
$builtDll = Join-Path $buildDir 'SCD3D11.dll'
$gameDir = Split-Path -Parent $GameExe
$pluginDll = Join-Path $UserDir 'Plugins\SCD3D11.dll'
$traceLog = Join-Path $UserDir 'SC4ShadowDiag.log'
$driverLog = Join-Path $UserDir 'SC4D3D11.log'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$captureDir = Join-Path $repo "runtime-captures\shadow-diag-$stamp"

# Raw TCP with a newline terminator. Measured against a live 1.1.641 listener:
#
#   bare command          5030 ms   the server waits out its own read timeout
#   command + newline     251 ms   replies immediately
#   Shutdown(Send)         251 ms   but the reply is lost, the socket dies first
#   HTTP POST              fails    this build answers with a bare body, not an
#                                   HTTP status line, so the client rejects it
#
# The reference client (0xC0000054/sc4-network-game-command) sends a bare
# command and reads to EOF, which is why it needs 30-second timeouts. Without
# the terminator every command costs five seconds and stalls the game with it.
function Cmd([string]$c, [int]$t = 8000) {
    try {
        $x = New-Object System.Net.Sockets.TcpClient
        $x.Connect('127.0.0.1', 50020); $x.ReceiveTimeout = $t; $x.SendTimeout = $t
        $s = $x.GetStream()
        $b = [Text.Encoding]::ASCII.GetBytes($c + [char]10)
        $s.Write($b, 0, $b.Length); $s.Flush()
        $sb = New-Object Text.StringBuilder; $buf = New-Object byte[] 8192
        try { while (($n = $s.Read($buf, 0, $buf.Length)) -gt 0) { [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $n)) } } catch {}
        $x.Close(); return $sb.ToString().Trim()
    } catch { return 'ERR' }
}

function WaitState([string]$want, [int]$sec) {
    $deadline = [DateTime]::UtcNow.AddSeconds($sec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:game.HasExited) { return $false }
        if ((Cmd 'GetAppState') -match "^\s*($want)\b") { return $true }
        Start-Sleep 2
    }
    return $false
}

if (-not $SkipBuild) {
    $vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat'
    $cmake = 'C:\Users\caspe\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmake)) { $cmake = 'cmake' }
    $build = 'call "{0}" x86 >nul && "{1}" --build "{2}" --target SCD3D11' -f $vcvars, $cmake, $buildDir
    & $env:ComSpec /d /s /c $build
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}
if (-not (Test-Path -LiteralPath $builtDll)) { throw "Built DLL not found: $builtDll" }

New-Item -ItemType Directory -Path $captureDir -Force | Out-Null
Get-Process 'SimCity 4' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
Copy-Item -LiteralPath $builtDll -Destination $pluginDll -Force
Remove-Item -LiteralPath $traceLog, $driverLog -Force -ErrorAction SilentlyContinue

# -UserDir needs the trailing backslash, and the value must stay quoted or SC4
# parses only the first path segment and silently loads the wrong plugin folder.
$arguments = @(
    ('-UserDir:"{0}\"' -f $UserDir.TrimEnd('\')),
    '-CustomResolution:enabled', "-r${Width}x${Height}x32", '-w',
    '-NetCommandGenerator:enabled', '-Intro:off',
    "-NativeShadowDiag:$Budget"
) + $ExtraArgs
$script:game = Start-Process -FilePath $GameExe -WorkingDirectory $gameDir -PassThru -ArgumentList $arguments
@(
    "started=$(Get-Date -Format o)"
    "pid=$($script:game.Id)"
    "dll_sha256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $pluginDll).Hash)"
    "arguments=$($arguments -join ' ')"
    "region=$Region city=$City"
) | Set-Content -LiteralPath (Join-Path $captureDir 'run.txt')
Write-Host "pid=$($script:game.Id) region=$Region city=$City budget=$Budget"

if (-not (WaitState '1|2' 180)) { throw 'SimCity 4 never reached the region or city view' }
if ((Cmd 'GetAppState') -match '^\s*1') {
    Cmd "LoadRegion `"$Region`"" | Out-Null
    Start-Sleep 4
    Cmd "LoadCity `"$City`" any full" | Out-Null
    if (-not (WaitState '2' 180)) { throw "City '$City' never finished loading" }
}
Start-Sleep 5
Write-Host "city=$(Cmd 'GetCityName') state=$(Cmd 'GetAppState')"

# Freeze the sim so the trace reflects the drawn scene rather than construction
# churn, then sweep the camera: every zoom and every rotation asks the shadow
# code for a different LOD and orientation.
Cmd 'GamePause true SimulationClock' | Out-Null
# Props and prebuilt-network pieces both need ShadowQuality > 2, and the default
# is 2. The cheat box and the socket share one command system, so 'rp' should
# reach the same render property; the trace's quality= field confirms it did.
Write-Host "rp shadowquality $ShadowQuality -> $(Cmd ""rp shadowquality $ShadowQuality"")"
foreach ($cell in $Cells) {
    Cmd "SetViewTarget cell $cell" | Out-Null
    Start-Sleep 1
    Cmd 'RotateCW' | Out-Null
    Start-Sleep 1
}
Start-Sleep 2

if ($Screenshot) {
    Add-Type -AssemblyName System.Drawing
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ShotHelper {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
    [ShotHelper]::SetProcessDPIAware() | Out-Null
    $script:game.Refresh()
    [ShotHelper]::SetForegroundWindow($script:game.MainWindowHandle) | Out-Null
    Start-Sleep 2
    $client = New-Object ShotHelper+RECT
    $origin = New-Object ShotHelper+POINT
    if ([ShotHelper]::GetClientRect($script:game.MainWindowHandle, [ref]$client) -and
        [ShotHelper]::ClientToScreen($script:game.MainWindowHandle, [ref]$origin)) {
        $bitmap = New-Object System.Drawing.Bitmap ($client.Right - $client.Left), ($client.Bottom - $client.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
            $bitmap.Save((Join-Path $captureDir 'view.png'), [System.Drawing.Imaging.ImageFormat]::Png)
            Write-Host "screenshot: $(Join-Path $captureDir 'view.png')"
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
}

if (-not $KeepRunning) {
    # From city view QuitGame takes two booleans, <show confirmation dialog>
    # and <save city>; the one-argument form never quits. Both false means no
    # dialog and, importantly, no write to the player's save. Quitting cleanly
    # is what lets PreAppShutdown restore the patched bytes and log the
    # modules' summary counters.
    Cmd 'QuitGame false false' | Out-Null
    if (-not $script:game.WaitForExit(60000)) {
        Write-Host 'QuitGame did not return; stopping the process so the trace is flushed'
        $script:game.Kill() | Out-Null
        Start-Sleep 2
    }
}

foreach ($log in @($traceLog, $driverLog)) {
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination $captureDir -Force }
}
$captured = Join-Path $captureDir 'SC4ShadowDiag.log'
if (Test-Path -LiteralPath $captured) {
    Write-Host "trace: $captured ($((Get-Item -LiteralPath $captured).Length) bytes)"
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        & $python.Source (Join-Path $PSScriptRoot 'summarize-shadow-diag.py') $captured |
            Tee-Object -FilePath (Join-Path $captureDir 'summary.txt')
    }
} else {
    Write-Host "no trace was written; check $driverLog for the byte-guard result"
}
