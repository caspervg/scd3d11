# A/B the parallel render cull: run the same scripted camera tour per mode,
# collect framerates + screenshots, then diff screenshots against the baseline.
param([string]$Modes = 'off,serial,parallel', [string]$City = 'Fulham', [string]$Region = 'London')
$modeList = @($Modes -split '[,\s]+' | Where-Object { $_ })
$ErrorActionPreference = 'Continue'
$gameDir = 'C:\Program Files (x86)\SimCity 4 Deluxe Edition\Apps'
$userDir = 'C:\Users\caspe\Documents\SimCity 4'
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo ("runtime-captures\cull-ab-{0:yyyyMMdd-HHmmss}" -f (Get-Date))
New-Item -ItemType Directory -Force $root | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing,System.Windows.Forms -TypeDefinition @'
using System; using System.Drawing; using System.Runtime.InteropServices;
public static class Cap {
  [StructLayout(LayoutKind.Sequential)] public struct RC { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RC r);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref PT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  public static void Shot(IntPtr hwnd, string path) {
    RC rc; GetClientRect(hwnd, out rc);
    PT o = new PT(); ClientToScreen(hwnd, ref o);
    int w = rc.R - rc.L, h = rc.B - rc.T;
    if (w <= 0 || h <= 0) return;
    using (var bmp = new Bitmap(w, h))
    using (var g = Graphics.FromImage(bmp)) {
      g.CopyFromScreen(o.X, o.Y, 0, 0, new Size(w, h));
      bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
  }
  // returns {changedPixels, totalPixels, maxChannelDelta}
  public static long[] Diff(string pa, string pb) {
    using (var a = new Bitmap(pa)) using (var b = new Bitmap(pb)) {
      int w = Math.Min(a.Width, b.Width), h = Math.Min(a.Height, b.Height);
      var ra = a.LockBits(new Rectangle(0,0,w,h), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
      var rb = b.LockBits(new Rectangle(0,0,w,h), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
      byte[] ba = new byte[ra.Stride*h], bb = new byte[rb.Stride*h];
      System.Runtime.InteropServices.Marshal.Copy(ra.Scan0, ba, 0, ba.Length);
      System.Runtime.InteropServices.Marshal.Copy(rb.Scan0, bb, 0, bb.Length);
      a.UnlockBits(ra); b.UnlockBits(rb);
      long changed = 0, max = 0, total = (long)w * h;
      for (int i = 0; i + 3 < ba.Length; i += 4) {
        int d = Math.Abs(ba[i]-bb[i]) + Math.Abs(ba[i+1]-bb[i+1]) + Math.Abs(ba[i+2]-bb[i+2]);
        if (d > 12) changed++;
        if (d > max) max = d;
      }
      return new long[] { changed, total, max };
    }
  }
}
'@
[Cap]::SetProcessDPIAware() | Out-Null

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

$steps = @('SetViewTarget cell 40 40', 'SetViewTarget cell 12 12', 'RotateCW', 'SetViewTarget cell 58 50',
    'ZoomIn', 'RotateCW', 'SetViewTarget cell 24 40', 'ZoomOut', 'SetViewTarget cell 40 40')

foreach ($mode in $modeList) {
    $md = Join-Path $root $mode
    New-Item -ItemType Directory -Force $md | Out-Null
    Get-Process 'SimCity 4' -EA SilentlyContinue | Stop-Process -Force; Start-Sleep 1
    Copy-Item (Join-Path $repo 'build\review\SCD3D11.dll') (Join-Path $userDir 'Plugins\SCD3D11.dll') -Force
    Remove-Item (Join-Path $userDir 'SC4D3D11.log') -EA SilentlyContinue
    $before = @(Get-ChildItem "$userDir\Exception Reports\*.txt" -EA SilentlyContinue | Select -Expand Name)
    $p = Start-Process (Join-Path $gameDir 'SimCity 4.exe') -WorkingDirectory $gameDir -PassThru -ArgumentList @(
        '-UserDir:"' + $userDir + '\\"', '-CustomResolution:enabled', '-r1600x900x32', '-w',
        '-NetCommandGenerator:enabled', "-ParallelCull:$mode")
    Write-Host "[$mode] pid=$($p.Id)"

    $ok = $false; $loaded = $false; $dl = [DateTime]::UtcNow.AddSeconds(150)
    while ([DateTime]::UtcNow -lt $dl -and -not $p.HasExited) {
        $s = Cmd 'GetAppState'
        if ($s -match '^\s*2') { $ok = $true; break }
        if ($s -match '^\s*1' -and -not $loaded) {
            Cmd "LoadRegion `"$Region`"" | Out-Null; Start-Sleep 4
            Cmd "LoadCity `"$City`" any full" | Out-Null; $loaded = $true; Start-Sleep 6
        }
        else { Start-Sleep 3 }
    }
    if (-not $ok) { Write-Host "[$mode] never reached city view"; if (-not $p.HasExited) { $p.Kill() }; continue }

    Cmd 'GamePause true Animation' | Out-Null
    Cmd 'GamePause true SimulationClock' | Out-Null
    Cmd 'GamePause true 24HourClock' | Out-Null
    Start-Sleep 2
    $p.Refresh(); $hwnd = $p.MainWindowHandle
    [Cap]::ShowWindow($hwnd, 9) | Out-Null; [Cap]::SetForegroundWindow($hwnd) | Out-Null; Start-Sleep 1
    $frames = @(); $i = 0
    foreach ($st in $steps) {
        Cmd $st | Out-Null
        # wait for the camera to stop moving so screenshots are comparable
        $prev = ''; $stable = 0
        for ($t = 0; $t -lt 30 -and $stable -lt 3; $t++) {
            Start-Sleep -Milliseconds 250
            $vt = Cmd 'GetViewTarget'
            if ($vt -eq $prev) { $stable++ } else { $stable = 0; $prev = $vt }
        }
        Start-Sleep -Milliseconds 400
        [Cap]::SetForegroundWindow($hwnd) | Out-Null
        try { [Cap]::Shot($hwnd, ("{0}\{1:d2}.png" -f $md, $i)) } catch { Write-Host "shot err: $_" }
        $frames += ("{0:d2} {1,-24} fps={2}" -f $i, $st, ((Cmd 'GetFrameRate') -split '\s')[0])
        $i++
    }
    $frames | Tee-Object (Join-Path $md 'frames.txt')
    Cmd 'QuitGame false' | Out-Null; Start-Sleep 3
    if (-not $p.HasExited) { $p.Kill() }
    Copy-Item (Join-Path $userDir 'SC4D3D11.log') $md -EA SilentlyContinue
    $new = @(Get-ChildItem "$userDir\Exception Reports\*.txt" -EA SilentlyContinue | Select -Expand Name) | ? { $_ -notin $before }
    if ($new) { Write-Host "[$mode] CRASH: $new"; $new | % { Copy-Item "$userDir\Exception Reports\$_" $md } }
}

# diff every mode pair
$pairs = @()
for ($a = 0; $a -lt $modeList.Count; $a++) { for ($b = $a + 1; $b -lt $modeList.Count; $b++) { $pairs += , @($modeList[$a], $modeList[$b]) } }
foreach ($pr in $pairs) {
    $base = $pr[0]; $m = $pr[1]
    Write-Host "`n===== $base vs $m ====="
    Get-ChildItem (Join-Path $root "$base\*.png") -EA SilentlyContinue | ForEach-Object {
        $bImg = Join-Path $root "$m\$($_.Name)"
        $b = $bImg
        if (-not (Test-Path $b)) { Write-Host "$m/$($_.Name): missing"; return }
        if ((Get-FileHash $_.FullName).Hash -eq (Get-FileHash $b).Hash) { Write-Host "$($_.Name): IDENTICAL"; return }
        $r = [Cap]::Diff($_.FullName, $b)
        Write-Host ("{0}: changed={1:P2} maxDelta={2}" -f $_.Name, ($r[0] / [double]$r[1]), $r[2])
    }
}
Get-ChildItem "$root\*\frames.txt" | ForEach-Object { Write-Host "`n--- $($_.Directory.Name) ---"; Get-Content $_ }
Write-Host "`nout: $root"
