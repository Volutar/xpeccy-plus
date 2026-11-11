# An x64 build on an ARM64 machine (under Windows' emulation) taking the native win-arm64
# one's place, as a user's would - for the ARM64 runner, beside win-parity.ps1:
#
#   .\win-switch.ps1 -App <win-x64 folder> -Zip <win-arm64 zip> [-Self <version@commit>]
#
# 1. -Self given (the nightly's own version and commit, as XPECCY_UPDATE_SELF takes it):
#    a start's check of a copy of the x64 build must find this an ARM64 machine and offer
#    the native file of that nightly. Without -Self only the machine is checked (the log
#    line before anything is asked).
# 2. The copy updates itself from the zip (--update-from), Restart Now: the program that
#    starts after is the ARM64 one, in the same folder, which must then pass win-smoke.ps1.

param(
	[Parameter(Mandatory)][string]$App,
	[Parameter(Mandatory)][string]$Zip,
	[string]$Self,
	[int]$Port = 30100
)

$ErrorActionPreference = 'Stop'

$work = Join-Path ([IO.Path]::GetTempPath()) "xpeccy-switch-$PID"
$inst = Join-Path $work (Split-Path $App -Leaf)
$cfg = Join-Path $work 'config'
$logs = Join-Path $work 'log'
New-Item -ItemType Directory -Force $work, $cfg | Out-Null
Copy-Item -Recurse $App $inst
$exe = Join-Path $inst 'xpeccy-plus.exe'

# one command, its answer; the program answers a connection with a prompt first
function Send-Command($cmd, $waitMs = 1000) {
	$c = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
	try {
		$s = $c.GetStream()
		$buf = [byte[]]::new(65536)
		Start-Sleep -Milliseconds 200
		while ($s.DataAvailable) { $null = $s.Read($buf, 0, $buf.Length) }
		$b = [Text.Encoding]::UTF8.GetBytes("$cmd`n")
		$s.Write($b, 0, $b.Length)
		Start-Sleep -Milliseconds $waitMs
		$ans = ''
		while ($s.DataAvailable) { $n = $s.Read($buf, 0, $buf.Length); $ans += [Text.Encoding]::UTF8.GetString($buf, 0, $n) }
		($ans -replace '>\s*$', '').Trim()
	} finally { $c.Close() }
}

# the PID listening on the port, once one does
function Wait-Port([int]$seconds) {
	for ($i = 0; $i -lt $seconds * 2; $i++) {
		$l = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
		if ($l) { return $l.OwningProcess }
		Start-Sleep -Milliseconds 500
	}
	throw "nothing listens on $Port after $seconds s"
}

function Log-Lines($pattern) {
	Get-ChildItem $logs -Recurse -File -Filter '*.log' -ErrorAction SilentlyContinue |
		ForEach-Object { Select-String -Path $_.FullName -Pattern $pattern } | ForEach-Object { $_.Line -replace '^.*?(NET|APP)\s+', '' }
}

function Stop-Mine($procId) {
	try { Send-Command 'quit' 500 | Out-Null } catch {}
	try { Wait-Process -Id $procId -Timeout 15 -ErrorAction Stop } catch { Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue }
}

$env:SDL_AUDIODRIVER = 'dummy'
$base = @('--confdir', "`"$cfg`"", '--log', '--log-dir', "`"$logs`"", '-m', 'zx48')
$ok = $true
$mine = [System.Collections.Generic.List[int]]::new()
try {
	# 1. a start's check, due (the last one long ago), on this machine as it is
	"[GENERAL]`nport = $Port`nupdate.last = 1`n" | Set-Content (Join-Path $cfg 'config.conf')
	if ($Self) { $env:XPECCY_UPDATE_SELF = $Self }
	$p = Start-Process $exe -ArgumentList $base -PassThru
	$null = Wait-Port 30
	$mine.Add($p.Id)
	# the first line says what is asked, the next what came of it
	for ($i = 0; ($i -lt 60) -and -not (@(Log-Lines 'update check') | Where-Object { $_ -notmatch ' channel' }); $i++) { Start-Sleep -Milliseconds 500 }
	Stop-Mine $p.Id
	Remove-Item Env:XPECCY_UPDATE_SELF -ErrorAction SilentlyContinue
	$lines = @(Log-Lines 'update check')
	$lines | ForEach-Object { Write-Host "  $_" }
	if (-not ($lines -match 'ARM64 machine: win-arm64 wanted')) {
		Write-Host '::error::the x64 build did not find an ARM64 machine'
		$ok = $false
	}
	if ($Self -and -not ($lines -match 'offered as the native build')) {
		# GitHub's answer is the network's: a limit hit on a shared runner is no failure of ours
		if ($lines -match 'rate limit|could not|no answer') {
			Write-Host '::warning::the check got no answer from GitHub, the offer was not seen'
		} else {
			Write-Host '::error::the native build was not offered'
			$ok = $false
		}
	}

	# 2. the switch: the zip put in place, Restart Now
	"[GENERAL]`nport = $Port`n" | Set-Content (Join-Path $cfg 'config.conf')
	Remove-Item $logs -Recurse -Force -ErrorAction SilentlyContinue
	$p = Start-Process $exe -ArgumentList ($base + @('--no-update-check', '--update-from', "`"$Zip`"")) -PassThru
	$mine.Add($p.Id)
	$null = Wait-Port 30
	$ready = $false
	for ($i = 0; ($i -lt 60) -and -not $ready; $i++) {
		Start-Sleep -Milliseconds 1000
		$ready = (Send-Command 'widget-rect updRestart' 300) -match '^\d+ \d+ \d+ \d+$'
	}
	if (-not $ready) { throw 'the update was not ready after 60 s' }
	Send-Command 'click-widget updRestart 10 10' 300 | Out-Null
	try { Wait-Process -Id $p.Id -Timeout 60 -ErrorAction Stop } catch { throw 'the x64 build did not quit for the update' }
	$next = Wait-Port 60
	$mine.Add($next)
	$path = (Get-Process -Id $next).Path
	Write-Host "after Restart Now: $path"
	for ($i = 0; ($i -lt 30) -and -not (Log-Lines 'update: now'); $i++) { Start-Sleep -Milliseconds 500 }
	Start-Sleep -Seconds 16	# the files the old one still held: removed 15 s after the start
	Stop-Mine $next
	Log-Lines 'update' | ForEach-Object { Write-Host "  $_" }
	if ($path -ne $exe) { Write-Host "::error::not the program of the folder that started: $path"; $ok = $false }
	if (-not (Log-Lines 'update: now')) { Write-Host '::error::the program after Restart Now did not report the update'; $ok = $false }

	# the program is ARM64 now (PE header: machine 0xAA64), the files the zip's
	$fs = [IO.File]::OpenRead($exe)
	try {
		$h = [byte[]]::new(4096); $null = $fs.Read($h, 0, $h.Length)
		$pe = [BitConverter]::ToInt32($h, 0x3C)
		$machine = [BitConverter]::ToUInt16($h, $pe + 4)
	} finally { $fs.Close() }
	Write-Host ('program machine: 0x{0:X4}' -f $machine)
	if ($machine -ne 0xAA64) { Write-Host '::error::the program is not ARM64'; $ok = $false }
	$man = Get-Content (Join-Path $inst 'manifest.sha256') | ForEach-Object { $_.Substring(66) }
	$have = Get-ChildItem $inst -Recurse -File | ForEach-Object { $_.FullName.Substring($inst.Length + 1) -replace '\\', '/' }
	$missing = @($man | Where-Object { $have -notcontains $_ })
	$extra = @($have | Where-Object { ($man -notcontains $_) -and ($_ -ne 'manifest.sha256') })
	Write-Host "files: $($have.Count), the zip's: $($man.Count), missing $($missing.Count), others $($extra.Count)"
	$extra | Select-Object -First 20 | ForEach-Object { Write-Host "  other: $_" }
	if ($missing) { $missing | ForEach-Object { Write-Host "  missing: $_" }; $ok = $false }
	& (Join-Path $PSScriptRoot 'win-smoke.ps1') -Dist $inst
	if (-not $ok) { throw 'the switch failed' }
	Write-Host 'switch: x64 -> ARM64 done' -ForegroundColor Green
} finally {
	foreach ($id in $mine) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
	Start-Sleep -Milliseconds 500
	Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
