# Free-roam bench on yuzu: 0 vs N local friends (roam_probe near career spawn).
#   powershell -File runtime\switch\yuzu-roam-bench.ps1 -Friends 0 -Tag roam0
#   powershell -File runtime\switch\yuzu-roam-bench.ps1 -Friends 1 -Tag roam1
param(
  [string]$Nro = "out\switch\nfsu2.nro",
  [string]$Tag = "roam",
  [int]$Friends = 0,
  [int]$Seconds = 220,
  [int]$HttpPort = 8765
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Sd = Join-Path $env:APPDATA "yuzu\sdmc\switch\nfsu2"
# your machine: YUZU_EXE (path to yuzu.exe; yuzu-run.ps1 reads it too) and, with several WSL distros, NFSU2_WSL_DISTRO
$Distro = if ($env:NFSU2_WSL_DISTRO) { @('-d', $env:NFSU2_WSL_DISTRO) } else { @() }
$RootWsl = ((& wsl.exe @Distro -- wslpath -a ($Root -replace '\\', '/')) -join '').Trim()
$NroPath = if ([IO.Path]::IsPathRooted($Nro)) { $Nro } else { Join-Path $Root $Nro }
if (!(Test-Path $NroPath)) { throw "NRO not found: $NroPath" }
if (!(Test-Path $Sd)) { throw "yuzu SD missing: $Sd" }

function Wsl([string]$Cmd, [switch]$OkAny) {
  & wsl.exe @Distro -- bash -lc $Cmd
  if (-not $OkAny -and $LASTEXITCODE -ne 0) { throw "wsl failed ($LASTEXITCODE): $Cmd" }
}

New-Item -ItemType File -Path (Join-Path $Sd "online") -Force | Out-Null
New-Item -ItemType File -Path (Join-Path $Sd "tbuf") -Force | Out-Null
Set-Content -Path (Join-Path $Sd "server.txt") -Value "ws://127.0.0.1:$HttpPort/lan" -NoNewline
Set-Content -Path (Join-Path $Sd "cars.txt") -Value "4" -NoNewline
Copy-Item (Join-Path $Root "tests\scripts\nx_career_drive.txt") (Join-Path $Sd "show.txt") -Force
if ($Friends -le 0) {
  New-Item -ItemType File -Path (Join-Path $Sd "noroam") -Force | Out-Null
  Write-Host "markers: online + noroam (0 remote cars)"
} else {
  Remove-Item (Join-Path $Sd "noroam") -ErrorAction SilentlyContinue
  Write-Host "markers: online, friends=$Friends via local hub"
}

try {
  if ($Friends -gt 0) {
    Wsl "bash $RootWsl/runtime/switch/start-local-roam.sh $Friends $HttpPort"
  } else {
    # Still need a listening hub so the client does not spin reconnect; 0 probes.
    Wsl "bash $RootWsl/runtime/switch/start-local-roam.sh 0 $HttpPort"
  }

  & (Join-Path $PSScriptRoot "yuzu-run.ps1") -Seconds $Seconds -Nro $NroPath
  if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { Write-Host "yuzu-run exit $LASTEXITCODE" }

  $log = Join-Path $Sd "log.txt"
  if (!(Test-Path $log)) { throw "no log.txt" }
  $lines = Get-Content $log
  $gcpus = @()
  foreach ($l in $lines) {
    $m = [regex]::Match($l, '^\[status\].*gcpu=([0-9.]+)ms/f flow=(\d+)')
    if ($m.Success -and [int]$m.Groups[2].Value -ge 5) {
      $gcpus += [double]$m.Groups[1].Value
    }
  }
  # Skip first few loading samples when we have enough.
  $use = if ($gcpus.Count -gt 8) { $gcpus | Select-Object -Skip 4 } else { $gcpus }
  $avg = if ($use.Count) { ($use | Measure-Object -Average).Average } else { 0 }
  $fps = if ($avg -gt 0) { 1000.0 / $avg } else { 0 }
  $build = (Get-Content (Join-Path $Root "runtime\switch\BUILD") | Select-Object -First 1)
  Write-Host ("b{0} {1}: friends={2}, samples {3}, gcpu {4:N2} ms/f (~{5:N1} fps)" -f $build, $Tag, $Friends, $use.Count, $avg, $fps)
  $lines | Where-Object { $_ -match '\[roam\]|\[net\]|free roam:|graphics:' } | Select-Object -Last 25 | ForEach-Object { Write-Host $_ }
  $csv = Join-Path $Root "out\yuzu-bench.csv"
  $ts = Get-Date -Format "yyyy-MM-ddTHH:mm:ss"
  Add-Content $csv ("{0},b{1},{2},{3:N2},{4:N1},{5}" -f $ts, $build, $Tag, $avg, $fps, $Friends)
}
finally {
  Wsl -OkAny "pkill -f 'nfsu2-server --data' 2>/dev/null; pkill -x roam_probe 2>/dev/null; pkill -f nfsu2-roamload 2>/dev/null; exit 0"
}
