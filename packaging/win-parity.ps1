# Runs two staged builds of one commit over the same machines and checks that they ran
# them the same: every frame, every sound sample, the RAM and the CPU (--bench-hash).
# Made for win-arm64, run beside the win-x64 archive (under Windows' x64 emulation) on
# the same ARM64 machine. Also prints each build's speed: the same work on both.
#
#   .\win-parity.ps1 -A <win-x64 folder> -B <win-arm64 folder> [-Card <Next card>]
#
# Every built-in machine boots its own ROM; with -Card the Next boots NextZXOS from it
# and runs the programs pgo-train.ps1 trains on, the ones the card has (without one,
# the Next runs its boot ROM only). Sound is mixed
# as in real time (--bench-full), so the filters run too.

param(
	[Parameter(Mandatory)][string]$A,
	[Parameter(Mandatory)][string]$B,
	[string]$Card,
	[int]$Frames = 400
)

$ErrorActionPreference = 'Stop'

$cases = [System.Collections.Generic.List[object]]::new()
foreach ($m in 'zx48', 'zx128', 'zxplus3', 'pent', 'pent1024', 'scorp', 'profi', 'atm2', 'phoenix', 'alf', 'evo-baseconf', 'evo-tsconf') {
	$cases.Add(@{ name = $m; args = @('-m', $m, '--bench-skip', '300') })
}
if ($Card) {
	if (-not (Test-Path $Card)) { throw "Not found: $Card" }
	$cases.Add(@{ name = 'zxnext'; args = @('-m', 'zxnext', '--bench-skip', '1500'); next = $null })
	foreach ($p in 'demos\scrollnutter\ScrollNutter.nex', 'demos\Spectron 2084\Spectron2084.nex',
			'demos\tilemap\tm.nex', 'demos\WidescreenImageDemo\Daybreak.nex', 'games\Next\TX-1696\main.nex',
			'games\Next\Warhawk\Warhawk.nex', 'games\Next\Nextoid\nextoid.nex', 'games\Next\AngryBloaters\Bloaters.nex') {
		if (Test-Path (Join-Path $Card $p)) {
			$cases.Add(@{ name = Split-Path $p -Leaf; args = @('-m', 'zxnext', '--bench-skip', '1500'); next = $p })
		}
	}
} else {
	# no card: its boot ROM alone, which stops at the card it cannot find
	$cases.Add(@{ name = 'zxnext'; args = @('-m', 'zxnext', '--bench-skip', '300') })
}

$work = Join-Path ([IO.Path]::GetTempPath()) "xpeccy-parity-$PID"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null
if ($Card) {
	# copied once, with one date for all as tests/regress.py does: the FAT served for a
	# folder carries the files' dates, and a fresh copy's would differ from run to run
	$master = Join-Path $work 'card-master'
	Copy-Item -Recurse $Card $master
	$pinned = [datetime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
	foreach ($i in @(Get-ChildItem $master -Recurse -Force) + (Get-Item $master)) { $i.LastWriteTimeUtc = $pinned }
}

# one run: a fresh copy of the build's config, and the card as it was (a run may write to it)
function Invoke-Case($app, $case) {
	$cfg = Join-Path $work 'config'
	if (Test-Path $cfg) { Remove-Item $cfg -Recurse -Force }
	Copy-Item -Recurse (Join-Path $app 'config') $cfg
	$cl = @('--confdir', $cfg, '--no-update-check') + $case.args
	if ($case.ContainsKey('next')) {
		$c = Join-Path $work 'card'
		# only what the last run changed is copied back, the dates with it
		robocopy $master $c /MIR /COPY:DAT /DCOPY:T /R:0 /NFL /NDL /NJH /NJS /NP | Out-Null
		if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
		$cl += @('--sdcard', $c)
		if ($case.next) { $cl += Join-Path $c $case.next }
	}
	$cl += @('--bench', $Frames, '--bench-full', '--bench-hash')
	$out = & (Join-Path $app 'xpeccy-plus.exe') @cl 2>$null | Out-String
	$code = $LASTEXITCODE
	$hash = [regex]::Match($out, '(?m)^hash: .*$').Value.Trim()
	$speed = [regex]::Match($out, 'x([\d.]+) real time').Groups[1].Value
	if ($code -ne 0 -or -not $hash) { $hash = "failed (exit $code)" }
	@{ hash = $hash; speed = $speed }
}

$env:SDL_AUDIODRIVER = 'dummy'	# no sound card on a runner, and nothing to hear
$rows = @('| case | | x64 build | ARM64 build |', '|---|---|---|---|')
$bad = 0
foreach ($c in $cases) {
	$ra = Invoke-Case $A $c
	$rb = Invoke-Case $B $c
	$same = ($ra.hash -eq $rb.hash) -and -not $ra.hash.StartsWith('failed')
	if (-not $same) {
		$bad++
		Write-Host "$($c.name): DIFFERS" -ForegroundColor Red
		Write-Host "  A: $($ra.hash)"
		Write-Host "  B: $($rb.hash)"
	} else {
		Write-Host "$($c.name): same, x$($ra.speed) / x$($rb.speed)"
	}
	$rows += "| $($c.name) | $(if ($same) { 'same' } else { '**differs**' }) | x$($ra.speed) | x$($rb.speed) |"
}

if ($env:GITHUB_STEP_SUMMARY) {
	(@('**Parity**: the same hashes from both builds, speed in real time (hashing included)', '') + $rows) -join "`n" >> $env:GITHUB_STEP_SUMMARY
}
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($bad) { throw "$bad of $($cases.Count) cases ran differently" }
Write-Host "all $($cases.Count) cases ran the same"
