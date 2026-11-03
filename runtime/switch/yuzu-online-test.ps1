# yuzu-online-test.ps1 -- main menu -> Online -> RANKINGS in yuzu against a local hub, and the test CHECKS (exit 0 = all passed, 1 = a check failed).
#   powershell -File runtime\switch\yuzu-online-test.ps1                       an EXISTING profile copied from the console (tests/scripts/nx_online_rankings.txt: the user's timing)
#   powershell -File runtime\switch\yuzu-online-test.ps1 -Fresh -Package nro   FROM ZERO with release\nro copied to an empty SD, a NEW profile (nx_online_rankings_new.txt)
#   powershell -File runtime\switch\yuzu-online-test.ps1 -Fresh -Package nsp   FROM ZERO with release\nsp opened as a title, nothing on the SD but test flags
#   add -NegativeControl to kill the hub first: the test must then FAIL (it proves the checks can fail).   [-Nro path] [-MaxConnectMs 3000]
# -Fresh moves the yuzu SD folder switch\nfsu2 aside (nfsu2-keep) and puts it back at the end.
# Checks (each prints PASS/FAIL with the evidence):
#   hub       the local hub answers HTTP from Windows BEFORE the run (else the run would prove nothing)
#   yuzu      yuzu's network interface is not "None"/"Nenhum" (then every socket() of the guest fails)
#   save/install  the console profile is in the SD (default) / the package is installed the way a user would (-Fresh)
#   profile   the expected save folder was used and the main menu (GarageMain) was reached;  script  the script ran to its quit
#   nifm/net  console online, "on the virtual LAN as 10.77.x.y (N ms ...)" with N <= MaxConnectMs, no failed attempt before it
#   lobby     the Online lobby (UI_OLMAIN) was reached;  lobby-tcp  the TCP connect to the Online host 10.77.0.1:20920 went out, nothing said "no such host"
#   online-screen  an Online screen past the lobby was reached (RANKINGS, or the car selection that a profile without an Online car is sent to) and no UI_OL_Disconnect came up
#   data      (-Fresh) nfsu2.dat came from the right place: the SD card (nro) or the NSP's RomFS (nsp)
#   hub-log   the hub's OWN log shows exactly one new "joined" (an independent witness: the client's log alone could be wrong)
param([string]$Nro = "$PSScriptRoot\..\..\out\switch\nfsu2.nro", [int]$MaxConnectMs = 3000, [switch]$NegativeControl,
      [switch]$Fresh, [ValidateSet("nro", "nsp")][string]$Package = "nro")
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$sd = "$env:APPDATA\yuzu\sdmc\switch\nfsu2"; $sw = Split-Path $sd; $log = "$sd\log.txt"
# Your machine: set YUZU_EXE (path to yuzu.exe) and, if WSL has more than one distro, NFSU2_WSL_DISTRO (the default distro is used otherwise).
$Yuzu = $env:YUZU_EXE; if (-not $Yuzu -or -not (Test-Path $Yuzu)) { throw "set the environment variable YUZU_EXE to the path of yuzu.exe" }
$Distro = if ($env:NFSU2_WSL_DISTRO) { @('-d', $env:NFSU2_WSL_DISTRO) } else { @() }
$RootWsl = ((& wsl.exe @Distro -- wslpath -a ($Root -replace '\\', '/')) -join '').Trim()   # this repo as WSL sees it
# the local hub (start-local-roam.sh) serves the game's files: NFSU2_GAME_DIR = the folder with SPEED2.EXE, as WSL sees it (e.g. /mnt/d/Games/NFSU2)
if (-not $env:NFSU2_GAME_DIR) { throw "set NFSU2_GAME_DIR to the folder with SPEED2.EXE as WSL sees it (e.g. /mnt/d/Games/NFSU2)" }
$env:WSLENV = (@($env:WSLENV, 'NFSU2_GAME_DIR') | Where-Object { $_ }) -join ':'            # lets the variable cross into WSL
$script:fails = 0
function Check($name, [bool]$ok, $evidence) { if ($ok) { Write-Host ("PASS {0,-10} {1}" -f $name, $evidence) } else { Write-Host ("FAIL {0,-10} {1}" -f $name, $evidence) -ForegroundColor Red; $script:fails++ } }
function HubLog { (& wsl.exe @Distro -- cat /tmp/nfsu2-yuzu-roam-state/server.log 2>$null) -join "`n" }
function HubUp { try { $null = Invoke-WebRequest "http://127.0.0.1:8765/" -UseBasicParsing -TimeoutSec 3; $true } catch { [bool]$_.Exception.Response } }   # a 404 still means "the server answered"
function Restore {
  $keepLog = Join-Path $Root ("out\nsp\last-run-" + $(if ($Fresh) { $Package } else { "existing" }) + ".log")
  if (Test-Path $log) { New-Item -ItemType Directory (Split-Path $keepLog) -Force | Out-Null; Copy-Item $log $keepLog -Force }
  if ($Fresh -and (Test-Path "$sw\nfsu2-keep")) { Remove-Item $sd -Recurse -Force -ErrorAction SilentlyContinue; Rename-Item "$sw\nfsu2-keep" "nfsu2" } }

Get-Process yuzu -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
& wsl.exe @Distro -- bash -lc 'pkill -f "nfsu2-server --data"; pkill -x roam_probe; pkill -f hub-keep.sh' 2>$null | Out-Null
if (-not $NegativeControl) {
  Start-Process wsl.exe -ArgumentList ($Distro + @('--', 'bash', "$RootWsl/runtime/switch/hub-keep.sh", '0', '8765', '900')) -WindowStyle Hidden
  $t0 = Get-Date; while (-not (HubUp) -and ((Get-Date) - $t0).TotalSeconds -lt 40) { Start-Sleep 1 }
}
$hubUp = HubUp
if ($NegativeControl) { Check "hub" (-not $hubUp) "negative control: the hub is down on purpose (up=$hubUp)" } else { Check "hub" $hubUp "http://127.0.0.1:8765/ answered from Windows" }
$joinedBefore = ([regex]::Matches((HubLog), '\[lan\] \S+ joined')).Count

$cfg = Get-Content "$env:APPDATA\yuzu\config\qt-config.ini" | Where-Object { $_ -match '^network_interface=' }
Check "yuzu" ($cfg -notmatch '=(Nenhum|None)\s*$') "$cfg"

if ($Fresh) {
  if (Test-Path "$sw\nfsu2-keep") { Remove-Item "$sw\nfsu2-keep" -Recurse -Force }
  if (Test-Path $sd) { Rename-Item $sd "nfsu2-keep" }
  New-Item -ItemType Directory $sd | Out-Null
  if ($Package -eq "nro") {
    Copy-Item "$Root\release\nro\switch\nfsu2\*" $sd -Recurse -Force
    Check "install" (Test-Path "$sd\nfsu2.dat") ("release\nro copied to the SD: " + ((Get-ChildItem $sd -Force -Name) -join ', '))
  } else {
    $nsps = @(Get-ChildItem "$Root\release\nsp\*.nsp")
    Check "install" ($nsps.Count -eq 1) ("NSP opened as a title: " + ($nsps | ForEach-Object { "$($_.Name) $([math]::Round($_.Length/1GB,2)) GB" }) + "; the SD has nothing of the game")
  }
} else {
  # an existing profile is a folder <name>\<name> under save\NFS Underground 2 (copy yours from the console's sd:/switch/nfsu2/save)
  $prof = Get-ChildItem "$sd\save\NFS Underground 2" -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName $_.Name) } | Select-Object -First 1
  Check "save" ([bool]$prof) $(if ($prof) { "existing profile '$($prof.Name)' found in the yuzu SD" } else { "no existing profile in $sd\save\NFS Underground 2 (copy one from the console, or use -Fresh)" })
}
Remove-Item "$sd\noroam", "$sd\log*.txt" -ErrorAction SilentlyContinue
New-Item -ItemType File "$sd\tbuf", "$sd\fnglog" -Force | Out-Null           # test flags only (tbuf: yuzu's shader limit)
if (-not $Fresh) { New-Item -ItemType File "$sd\online", "$sd\keepsave" -Force | Out-Null }
Set-Content "$sd\server.txt" 'ws://127.0.0.1:8765/lan' -NoNewline             # the local hub instead of nfsu2.xyz
$scriptName = if ($Fresh) { "nx_online_rankings_new.txt" } else { "nx_online_rankings.txt" }
Copy-Item "$Root\tests\scripts\$scriptName" "$sd\show.txt" -Force
$target = if ($Fresh -and $Package -eq "nsp") { $nsps[0].FullName } elseif ($Fresh) { "$sd\nfsu2.nro" } else { (Resolve-Path $Nro).Path }
Start-Process $Yuzu -ArgumentList '-g', "`"$target`"" -WorkingDirectory (Split-Path $Yuzu)
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 480) {
  Start-Sleep 3
  if (-not (Get-Process yuzu -ErrorAction SilentlyContinue)) { break }
  if ((Test-Path $log) -and (Select-String $log -Pattern 'quit at guest frame|ended: the game ended' -Quiet)) { Start-Sleep 4; break }
}
Get-Process yuzu -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not (Test-Path $log)) { Check "profile" $false "no log.txt"; Restore; exit 1 }
$L = Get-Content $log
if ($Fresh -and $Package -eq "nsp") { Check "no-sd" (-not (Test-Path "$sd\nfsu2.nro") -and -not (Test-Path "$sd\nfsu2.dat")) "the NSP made its own folder; no nro and no dat on the SD" }

$ver = ($L | Select-String 'NFSU2 (b\d+)' | Select-Object -First 1).Matches.Groups[1].Value
Write-Host "build $ver, run $([int]((Get-Date) - $t0).TotalSeconds) s"
$real = if ($Fresh) { $L | Select-String 'save_bench starts empty' } else { $L | Select-String 'script show.txt on the real save folder' }
$gm = $L | Select-String '\[fng\] f=\d+ screen GarageMain\.fng' | Select-Object -First 1
$quit = $L | Select-String 'quit at guest frame'
Check "profile" ([bool]$real -and [bool]$gm) $(if (-not $real) { "the run did not use the expected save folder" } elseif ($gm) { $gm.Line } else { "GarageMain.fng never reached (the profile did not load/create?)" })
Check "script" ([bool]$quit) $(if ($quit) { $quit[0].Line } else { "the script did not reach its quit" })

$online = $L | Select-String '\[net\] console online'
$failed = @($L | Select-String '\[net\] could not connect')
$lan = $L | Select-String '\[net\] on the virtual LAN as (10\.77\.\d+\.\d+) \((\d+) ms after the connect started\)' | Select-Object -First 1
Check "nifm" ([bool]$online) $(if ($online) { $online[0].Line } else { "no '[net] console online' line" })
if ($lan) {
  $ms = [int]$lan.Matches.Groups[2].Value
  $fb = @($failed | Where-Object { $_.LineNumber -lt $lan.LineNumber }).Count
  Check "net" ($ms -le $MaxConnectMs -and $fb -eq 0) "LAN $($lan.Matches.Groups[1].Value) in $ms ms (max $MaxConnectMs), failed attempts before it: $fb"
} else {
  Check "net" $false "never on the virtual LAN: $($failed.Count) failed attempts; last: $(($failed | Select-Object -Last 1).Line)"
}

$olmain = $L | Select-String '\[fng\] f=\d+ screen UI_OLMAIN\.fng' | Select-Object -First 1
# the existing profile (has a car) reaches RANKINGS; a brand-new profile is first sent to the Online car selection (UI_OLCarSelect), so -Fresh accepts either
$rank = $L | Select-String '\[fng\] f=\d+ screen UI_OL(Rankings|CarSelect)\.fng' | Select-Object -First 1
$disc = $L | Select-String 'screen UI_OL_Disconnect'
$nohost = $L | Select-String 'no such host on the switch'
$tcp = $L | Select-String 'tcp connect \S+ -> 10\.77\.0\.1:20920\s*$' | Select-Object -First 1
Check "lobby" ([bool]$olmain) $(if ($olmain) { $olmain.Line } else { "UI_OLMAIN.fng never reached" })
Check "lobby-tcp" ([bool]$tcp -and -not $nohost) $(if ($nohost) { $nohost[0].Line } elseif ($tcp) { $tcp.Line } else { "no TCP connect to the Online host 10.77.0.1:20920" })
Check "rankings" ([bool]$rank -and -not $disc) $(if ($disc) { "disconnected: " + $disc[0].Line } elseif ($rank) { $rank.Line } else { "no Online screen past the lobby (UI_OLRankings or UI_OLCarSelect) was reached" })

if ($Fresh) {
  $src = $L | Select-String '\[nx\] nfsu2\.dat \((.+)\):' | Select-Object -First 1
  $want = if ($Package -eq "nsp") { "inside the NSP's RomFS" } else { "SD card" }
  Check "data" ([bool]$src -and $src.Matches.Groups[1].Value -eq $want) $(if ($src) { $src.Line } else { "no '[nx] nfsu2.dat (...)' line" })
}
$joinedAfter = ([regex]::Matches((HubLog), '\[lan\] \S+ joined')).Count
Check "hub-log" (($joinedAfter - $joinedBefore) -eq 1) "hub 'joined' lines: $joinedBefore -> $joinedAfter (expected +1)"

Restore
if ($NegativeControl) {
  if ($script:fails -gt 0) { Write-Host "NEGATIVE CONTROL OK: the test failed $($script:fails) check(s) with the hub down, as it must"; exit 0 }
  Write-Host "NEGATIVE CONTROL BROKEN: nothing failed with the hub down -> the test proves nothing" -ForegroundColor Red; exit 1
}
if ($script:fails -gt 0) { Write-Host "RESULT: FAILED ($($script:fails) check(s))" -ForegroundColor Red; exit 1 }
Write-Host "RESULT: ALL CHECKS PASSED"; exit 0
