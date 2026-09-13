# Measure frame rate while the sim runs, with different -SimTickCap values.
param([string]$Caps = 'off,16', [string]$City = 'New City (5)', [string]$Region = 'London', [int]$SampleSeconds = 70)
$ErrorActionPreference = 'Continue'
$gameDir = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps'
$userDir = 'C:\Users\caspe\Documents\SimCity 4'
$repo = Split-Path -Parent $PSScriptRoot
$capList = @($Caps -split '[,\s]+' | Where-Object { $_ })

function Cmd([string]$c, [int]$t = 15000) {
    try {
        $x = New-Object System.Net.Sockets.TcpClient
        $x.Connect('127.0.0.1', 50020); $x.ReceiveTimeout = $t; $x.SendTimeout = $t
        $s = $x.GetStream(); $b = [Text.Encoding]::ASCII.GetBytes($c); $s.Write($b, 0, $b.Length)
        $sb = New-Object Text.StringBuilder; $buf = New-Object byte[] 8192
        try { while (($n = $s.Read($buf, 0, $buf.Length)) -gt 0) { [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $n)) } } catch {}
        $x.Close(); return $sb.ToString().Trim()
    } catch { return "ERR" }
}

foreach ($cap in $capList) {
    Get-Process 'SimCity 4' -EA SilentlyContinue | Stop-Process -Force; Start-Sleep 1
    Copy-Item (Join-Path $repo 'build\review\SCD3D11.dll') (Join-Path $userDir 'Plugins\SCD3D11.dll') -Force
    $args = @('-UserDir:"' + $userDir + '\\"', '-CustomResolution:enabled', '-r1600x900x32', '-w',
        '-NetCommandGenerator:enabled', '-ParallelCull:off')
    if ($cap -ne 'default') { $args += "-SimTickCap:$cap" }
    $p = Start-Process (Join-Path $gameDir 'SimCity 4.exe') -WorkingDirectory $gameDir -PassThru -ArgumentList $args
    Write-Host "[cap=$cap] pid=$($p.Id)"

    $ok = $false; $loaded = $false; $dl = [DateTime]::UtcNow.AddSeconds(150)
    while ([DateTime]::UtcNow -lt $dl -and -not $p.HasExited) {
        $s = Cmd 'GetAppState'
        if ($s -match '^\s*2') { $ok = $true; break }
        if ($s -match '^\s*1' -and -not $loaded) {
            Cmd "LoadRegion `"$Region`"" | Out-Null; Start-Sleep 4
            Cmd "LoadCity `"$City`" any full" | Out-Null; $loaded = $true; Start-Sleep 6
        } else { Start-Sleep 3 }
    }
    if (-not $ok) { Write-Host "[cap=$cap] no city"; if (-not $p.HasExited) { $p.Kill() }; continue }
    Start-Sleep 3

    # let the sim churn; sample fps + sim date drift
    Cmd 'SetViewTarget cell 64 64' | Out-Null
    Start-Sleep 5
    $t0 = [DateTime]::UtcNow
    $d0 = (Cmd 'GetSimulationDate "number"')
    $fps = @()
    while (([DateTime]::UtcNow - $t0).TotalSeconds -lt $SampleSeconds) {
        Start-Sleep -Milliseconds 800
        $f = (Cmd 'GetFrameRate') -split '\s'
        if ($f[0] -match '^\d') { $fps += [double]$f[0] }
    }
    $d1 = (Cmd 'GetSimulationDate "number"')
    $wall = ([DateTime]::UtcNow - $t0).TotalSeconds
    $simDays = 0; try { $simDays = [int]($d1 -replace '\D') - [int]($d0 -replace '\D') } catch {}
    $sorted = $fps | Sort-Object
    $below20 = ($fps | Where-Object { $_ -lt 20 }).Count
    Write-Host ("[cap=$cap] fps min={0:N1} p10={1:N1} median={2:N1} max={3:N1} <20fps={4}/{5}   simDays={6} over {7:N0}s wall" -f `
        $sorted[0], $sorted[[int]($sorted.Count * 0.1)], $sorted[[int]($sorted.Count / 2)], $sorted[-1], $below20, $sorted.Count, $simDays, $wall)
    Write-Host ("[cap=$cap] samples: " + (($fps | ForEach-Object { '{0:N0}' -f $_ }) -join ' '))
    Cmd 'QuitGame false' | Out-Null; Start-Sleep 3
    if (-not $p.HasExited) { $p.Kill() }
    (Get-Content (Join-Path $userDir 'SC4D3D11.log') -EA SilentlyContinue | Select-String 'sim tick budget')
}
