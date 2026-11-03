# yuzu-bench.ps1 -- runs tests/scripts/nx_quick_race.txt in yuzu and prints the race's average game-thread cost.
#   powershell -File runtime\switch\yuzu-bench.ps1 [-Nro out\switch\nfsu2.nro] [-Seconds 140] [-Tag name]
# Result: mean gcpu (ms/frame) of the [status] lines in flow 6 (the race) after the first 3 (loading noise), plus fps; appended to out/yuzu-bench.csv.
param([string]$Nro = "$PSScriptRoot\..\..\out\switch\nfsu2.nro", [int]$Seconds = 140, [string]$Tag = "")
$sd = "$env:APPDATA\yuzu\sdmc\switch\nfsu2"
Copy-Item "$PSScriptRoot\..\..\tests\scripts\nx_quick_race.txt" "$sd\show.txt" -Force
& "$PSScriptRoot\yuzu-run.ps1" -Seconds $Seconds -Nro $Nro | Out-Null
$ver = (Select-String -Path "$sd\log.txt" -Pattern 'NFSU2 (b\d+)' | Select-Object -First 1).Matches.Groups[1].Value
$r = Select-String -Path "$sd\log.txt" -Pattern '^\[status\] .*fps=([\d.]+) .*gcpu=([\d.]+)ms/f flow=6' | ForEach-Object { [pscustomobject]@{ fps=[double]$_.Matches.Groups[1].Value; g=[double]$_.Matches.Groups[2].Value } }
$r = @($r | Select-Object -Skip 3)
if ($r.Count -eq 0) { Write-Host "no race samples"; exit 1 }
$g = ($r | Measure-Object g -Average).Average; $f = ($r | Measure-Object fps -Average).Average
"{0} {1}: race samples {2}, gcpu {3:N2} ms/f, fps {4:N1}" -f $ver, $Tag, $r.Count, $g, $f
Add-Content "$PSScriptRoot\..\..\out\yuzu-bench.csv" ("{0},{1},{2},{3:N2},{4:N1}" -f (Get-Date -Format s), $ver, $Tag, $g, $f)
