param([string]$Mode = 'off', [string]$City = 'Fulham', [string]$Region = 'London')
$ErrorActionPreference = 'Continue'
$gameDir = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps'
$userDir = 'C:\Users\caspe\Documents\SimCity 4'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo "runtime-captures\cull-$Mode"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

function Cmd([string]$c, [int]$t = 15000) {
    try {
        $x = New-Object System.Net.Sockets.TcpClient
        $x.Connect('127.0.0.1', 50020); $x.ReceiveTimeout = $t; $x.SendTimeout = $t
        $s = $x.GetStream()
        $b = [Text.Encoding]::ASCII.GetBytes($c); $s.Write($b, 0, $b.Length)
        $sb = New-Object Text.StringBuilder; $buf = New-Object byte[] 8192
        try { while (($n = $s.Read($buf, 0, $buf.Length)) -gt 0) { [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $n)) } } catch {}
        $x.Close(); return $sb.ToString().Trim()
    } catch { return "ERR" }
}
function WaitState([string]$want, [int]$sec) {
    $dl = [DateTime]::UtcNow.AddSeconds($sec)
    while ([DateTime]::UtcNow -lt $dl) {
        if ((Get-Process -Id $p.Id -EA SilentlyContinue).HasExited -ne $false) { return $false }
        if ((Cmd 'GetAppState') -match "^\s*$want") { return $true }
        Start-Sleep 2
    }
    return $false
}

Get-Process 'SimCity 4' -EA SilentlyContinue | Stop-Process -Force; Start-Sleep 1
Copy-Item (Join-Path $repo 'build\review\SCD3D11.dll') (Join-Path $userDir 'Plugins\SCD3D11.dll') -Force
Remove-Item (Join-Path $userDir 'SC4D3D11.log') -EA SilentlyContinue
$before = @(Get-ChildItem "$userDir\Exception Reports\*.txt" -EA SilentlyContinue | Select -Expand Name)
$p = Start-Process (Join-Path $gameDir 'SimCity 4.exe') -WorkingDirectory $gameDir -PassThru -ArgumentList @(
    '-UserDir:"' + $userDir + '\\"', '-CustomResolution:enabled', '-r1600x900x32', '-w', '-NetCommandGenerator:enabled',
    "-ParallelCull:$Mode")
"pid=$($p.Id) mode=$Mode $(Get-Date -f HH:mm:ss)"

if (WaitState '1|2' 120) {
    if ((Cmd 'GetAppState') -match '^\s*1') {
        Cmd "LoadCity `"$City`" any full" | Out-Null
        WaitState '2' 90 | Out-Null
    }
    "state=$(Cmd 'GetAppState')  city=$(Cmd 'GetCityName')"
    Cmd 'GamePause true Animation' | Out-Null
    Cmd 'GamePause true SimulationClock' | Out-Null
    Cmd 'GamePause true 24HourClock' | Out-Null
    Start-Sleep 2

    $steps = @('SetViewTarget cell 40 40', 'SetViewTarget cell 12 12', 'RotateCW',
        'SetViewTarget cell 58 50', 'ZoomIn', 'RotateCW', 'SetViewTarget cell 24 40', 'ZoomOut', 'SetViewTarget cell 40 40')
    $i = 0; $log = @()
    foreach ($s in $steps) {
        Cmd $s | Out-Null; Start-Sleep -Milliseconds 700
        Cmd ("TakeSnapshot `"{0}\{1:d2}.png`"" -f $out, $i) | Out-Null
        Start-Sleep -Milliseconds 250
        $log += ("{0:d2} {1,-26} fps={2}" -f $i, $s, ((Cmd 'GetFrameRate') -split '\s')[0])
        $i++
    }
    $log | Tee-Object (Join-Path $out 'frames.txt')
    Cmd 'QuitGame false' | Out-Null
    Start-Sleep 3
}
else { "never reached region/city view" }

if (Get-Process -Id $p.Id -EA SilentlyContinue) { $p.Kill(); "killed" } else { "exited" }
Copy-Item (Join-Path $userDir 'SC4D3D11.log') $out -EA SilentlyContinue
$new = @(Get-ChildItem "$userDir\Exception Reports\*.txt" -EA SilentlyContinue | Select -Expand Name) | Where-Object { $_ -notin $before }
if ($new) {
    "CRASH:"
    $new | ForEach-Object { Copy-Item "$userDir\Exception Reports\$_" $out; (Get-Content "$userDir\Exception Reports\$_" -TotalCount 22) -join "`n" }
} else { "no crash" }
Get-Content (Join-Path $out 'SC4D3D11.log') -EA SilentlyContinue | Select-String -Pattern 'cull|ERROR|assert'
"out=$out"
