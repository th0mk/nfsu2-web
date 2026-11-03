# yuzu-run.ps1 -- runs the console build in yuzu (the yuzu.exe in YUZU_EXE) with the game package already in yuzu's virtual SD, and prints what the app logged.
#   powershell -File runtime\switch\yuzu-run.ps1 [-Seconds 60] [-Nro <path>]   (default: release\nro\switch\nfsu2\nfsu2.nro)
# The virtual SD is %APPDATA%\yuzu\sdmc\switch\nfsu2 (nfsu2.dat is there once; the app's log.txt, report.txt and save/ live there too).
# yuzu runs the same arm64 code, so it shows what works (menus, files, touch/mouse, network, crashes) but NOT the console's speed.
param([int]$Seconds = 60, [string]$Nro = "$PSScriptRoot\..\..\release\nro\switch\nfsu2\nfsu2.nro")
$yuzu = $env:YUZU_EXE; if (-not $yuzu -or -not (Test-Path $yuzu)) { throw 'set the environment variable YUZU_EXE to the path of yuzu.exe' }
$sd = "$env:APPDATA\yuzu\sdmc\switch\nfsu2"
Get-Process yuzu -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
Remove-Item "$sd\log*.txt", "$sd\report*.txt" -ErrorAction SilentlyContinue
Start-Process -FilePath $yuzu -ArgumentList '-g', (Resolve-Path $Nro).Path -WorkingDirectory (Split-Path $yuzu)
$t0 = Get-Date; while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) { Start-Sleep -Seconds 2; $lg = Join-Path $sd "log.txt"; if ((Test-Path $lg) -and (Select-String -Path $lg -Pattern "unsupported instructions" -Quiet)) { Start-Sleep 2; break } }
Get-Content "$sd\log.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\[nx\]|^\[net\]|CANNOT|crash|error' } | Select-Object -First 60
Get-Process yuzu -ErrorAction SilentlyContinue | Stop-Process -Force
