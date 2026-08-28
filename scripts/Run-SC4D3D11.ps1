param(
    [ValidateSet('Debug', 'Release')]
    [string]$BuildType = 'Debug',
    [int]$Width = 1920,
    [int]$Height = 1080,
	[ValidateSet('Windowed', 'Fullscreen', 'Borderless')]
	[string]$PresentationMode = 'Windowed',
    [int]$ScreenshotDelaySeconds = 20,
    [switch]$WaitForExit,
    [string]$GameExe = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe',
    [string]$UserDir = 'D:\OneDrive - Maplix\SimCity 4\',
    [string]$PluginDll = 'D:\OneDrive - Maplix\SimCity 4\Plugins\SCGL.dll'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# cmake-build-debug is CLion's x64 tree; SimCity 4 is x86, so use the x86 build trees.
if ($BuildType -eq 'Debug') { $buildDir = Join-Path $repo 'build\review' }
else { $buildDir = Join-Path $repo 'build\final-minrelease' }
$builtDll = Join-Path $buildDir 'SCGL.dll'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat'
$cmake = 'C:\Users\caspe\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe'
$gameDir = Split-Path -Parent $GameExe
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$captureDir = Join-Path $repo "runtime-captures\$stamp-$($Width)x$Height-$BuildType"
New-Item -ItemType Directory -Path $captureDir -Force | Out-Null

if (-not (Test-Path -LiteralPath $GameExe)) { throw "SC4 executable not found: $GameExe" }
if (-not (Test-Path -LiteralPath $vcvars)) { throw "MSVC environment script not found: $vcvars" }

# --no-tests=ignore keeps the release tree, which is configured with BUILD_TESTING=OFF,
# from failing the run just because it has no tests to execute.
$buildCommand = 'call "{0}" x86 && "{1}" --build "{2}" --config {3} && ctest --test-dir "{2}" -C {3} --no-tests=ignore --output-on-failure' -f $vcvars, $cmake, $buildDir, $BuildType
# vcvarsall writes harmless warnings to stderr, which -ErrorActionPreference Stop would
# turn into a terminating error; judge the build by its exit code instead.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & $env:ComSpec /d /s /c $buildCommand } finally { $ErrorActionPreference = $previousPreference }
if ($LASTEXITCODE -ne 0) { throw "Build or tests failed with exit code $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $builtDll)) { throw "Built DLL not found: $builtDll" }

if (Test-Path -LiteralPath $PluginDll) {
    Copy-Item -LiteralPath $PluginDll -Destination (Join-Path $captureDir 'SCGL.before.dll')
}
New-Item -ItemType Directory -Path (Split-Path -Parent $PluginDll) -Force | Out-Null
Copy-Item -LiteralPath $builtDll -Destination $PluginDll -Force
$pluginRoot = Split-Path -Parent $PluginDll
Get-ChildItem -LiteralPath $pluginRoot -Recurse -Filter '*.dll' | Sort-Object FullName | ForEach-Object {
    $pluginHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
    "$pluginHash  $($_.FullName.Substring($pluginRoot.Length).TrimStart('\'))"
} | Set-Content -LiteralPath (Join-Path $captureDir 'plugin-dlls.txt')

$logs = @('SC4D3D11.log', 'SC4D3D11-states.log')
foreach ($name in $logs) {
    $path = Join-Path $gameDir $name
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination (Join-Path $captureDir "$name.before")
        Remove-Item -LiteralPath $path -Force
    }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $PluginDll).Hash
# -UserDir selects the plugin/user folder SC4 loads SCGL.dll from; without it the game
# falls back to the Documents folder and never loads this driver.
# The value must stay quoted: Start-Process passes the list through verbatim, and an
# unquoted path with spaces makes SC4 parse only "D:\OneDrive", load the wrong plugin
# folder, and silently fall back to its DirectX driver instead of SCGL.
$arguments = @("-UserDir:`"$UserDir`"", '-CPUCount:1', '-CustomResolution:enabled', "-r${Width}x${Height}x32")
switch ($PresentationMode) {
    'Windowed' { $arguments += '-w' }
    'Fullscreen' { $arguments += '-f' }
    'Borderless' { $arguments += @('-f', '-Borderless') }
}
@(
    "started=$(Get-Date -Format o)"
    "dll=$PluginDll"
    "dll_sha256=$hash"
    "exe=$GameExe"
    "arguments=$($arguments -join ' ')"
) | Set-Content -LiteralPath (Join-Path $captureDir 'run.txt')

$oldRecording = $env:SC4D3D11_RECORD_STATES
$env:SC4D3D11_RECORD_STATES = '1'
try {
    $process = Start-Process -FilePath $GameExe -ArgumentList $arguments -WorkingDirectory $gameDir -PassThru
} finally {
    $env:SC4D3D11_RECORD_STATES = $oldRecording
}
Add-Content -LiteralPath (Join-Path $captureDir 'run.txt') -Value "pid=$($process.Id)"

$deadline = [DateTime]::UtcNow.AddSeconds(60)
do {
    Start-Sleep -Milliseconds 500
    $process.Refresh()
} while (-not $process.HasExited -and $process.MainWindowHandle -eq 0 -and [DateTime]::UtcNow -lt $deadline)

if (-not $process.HasExited -and $process.MainWindowHandle -ne 0) {
    Add-Type -AssemblyName System.Drawing
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SC4WindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
    # Without this the client rect comes back in logical pixels while CopyFromScreen works in
    # physical ones, so the capture is a cropped top-left corner on a scaled display.
    [SC4WindowCapture]::SetProcessDPIAware() | Out-Null
    $window = New-Object SC4WindowCapture+RECT
    $client = New-Object SC4WindowCapture+RECT
    $origin = New-Object SC4WindowCapture+POINT
    if ([SC4WindowCapture]::GetWindowRect($process.MainWindowHandle, [ref]$window) -and
        [SC4WindowCapture]::ClientToScreen($process.MainWindowHandle, [ref]$origin)) {
        [SC4WindowCapture]::SetWindowPos($process.MainWindowHandle, [IntPtr]::Zero,
            -($origin.X - $window.Left), -($origin.Y - $window.Top), 0, 0, 0x15) | Out-Null
    }
    Start-Sleep -Seconds $ScreenshotDelaySeconds
    $origin = New-Object SC4WindowCapture+POINT
    if ([SC4WindowCapture]::GetClientRect($process.MainWindowHandle, [ref]$client) -and
        [SC4WindowCapture]::ClientToScreen($process.MainWindowHandle, [ref]$origin)) {
        $bitmap = New-Object System.Drawing.Bitmap ($client.Right - $client.Left), ($client.Bottom - $client.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
            $bitmap.Save((Join-Path $captureDir 'startup.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}

if ($WaitForExit -and -not $process.HasExited) { $process.WaitForExit() }
if ($process.HasExited) {
    Add-Content -LiteralPath (Join-Path $captureDir 'run.txt') -Value "exit_code=$($process.ExitCode)"
}
foreach ($name in $logs) {
    $path = Join-Path $gameDir $name
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination (Join-Path $captureDir $name) -Force
    }
}

Write-Output "Capture: $captureDir"
Write-Output "SC4 PID: $($process.Id)"
