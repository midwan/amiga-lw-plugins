param(
  [Parameter(Mandatory=$true)][string]$Config,
  [Parameter(Mandatory=$true)][string]$Log,
  [string]$Marker = "DONE",
  [int]$TimeoutSec = 300,
  [string]$ShotDir = "",
  [int]$ShotEverySec = 5,
  [int]$AfterMarkerSec = 0
)
$ErrorActionPreference = "Stop"
if (Get-Process -Name Amiberry -ErrorAction SilentlyContinue) { throw "Another Amiberry instance is running" }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}
"@
function Shot($p, $name) {
  $p.Refresh(); $h = $p.MainWindowHandle
  if ($h -eq [IntPtr]::Zero -or -not $ShotDir) { return }
  $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r)
  $w = $r.R - $r.L; $hh = $r.B - $r.T; if ($w -le 0 -or $hh -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
  [void][W]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
  $bmp.Save((Join-Path $ShotDir $name), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
if ($ShotDir) { New-Item -ItemType Directory -Force -Path $ShotDir | Out-Null }
if (Test-Path $Log) { Remove-Item $Log }
$exe = Join-Path $env:LOCALAPPDATA "Programs\amiberry\Amiberry.exe"
$p = Start-Process -FilePath $exe -ArgumentList @("-f", "`"$Config`"", "-G") -WorkingDirectory (Split-Path $exe) -PassThru
$start = Get-Date; $i = 0; $found = $null
try {
  while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Seconds $ShotEverySec; $i++
    if ($p.HasExited) { "Amiberry exited with $($p.ExitCode)"; break }
    Shot $p ("shot{0:D3}.png" -f $i)
    if (-not $found -and (Test-Path $Log) -and (Select-String -Path $Log -SimpleMatch $Marker -Quiet)) {
      $found = Get-Date; "marker after $([int](($found - $start).TotalSeconds))s"
    }
    if ($found -and ((Get-Date) - $found).TotalSeconds -ge $AfterMarkerSec) { break }
  }
  if (-not $found -and (Test-Path $Log) -and (Select-String -Path $Log -SimpleMatch $Marker -Quiet)) {
    $found = Get-Date; "marker found at end of run"
  }
  if (-not $found) { "TIMEOUT without marker" }
} finally {
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
if (-not $found) { exit 1 }
