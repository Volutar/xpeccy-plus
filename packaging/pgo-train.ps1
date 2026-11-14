# The training runs of a profile-guided build (make-dist.ps1 -Pgo): an instrumented
# program is run, headless, over what users run, and writes how often each branch and
# function ran (gcc: beside its objects, clang: where make-dist told it); the final build
# is laid out by those counts.
#
#   pgo-train.ps1 -App <staged folder or the program> [-SysNext <card>] [-Extra <folder>,...]
#                 [-TsConf <folder>,...]
#
# Every built-in machine boots its own ROM; the Next, with System/Next (or a cut-down
# copy of it) as its card, boots NextZXOS and runs programs that come with it. -Extra
# adds every .nex found under the folders given (programs of one's own, run on the same
# card with the folder of each put onto it, so the files beside a program are where it
# looks). The card is copied first: a run may write to it. -TsConf: every .spg and .trd
# under the folders, on the TSConf, each in a fresh config. The runs go one after
# another: the counts of each are added to the same files at its exit.

param(
	[Parameter(Mandatory)] [string]$App,
	[string]$SysNext,
	[string[]]$Extra = @(),
	[string[]]$TsConf = @(),
	[int]$Frames = 400
)

$ErrorActionPreference = 'Stop'

# the program itself, or the folder it was staged in
$exe = $App
if (Test-Path $App -PathType Container) {
	$exe = @('xpeccy-plus.exe', 'xpeccy-plus', 'usr/bin/xpeccy-plus', 'xpeccy-plus.app/Contents/MacOS/xpeccy-plus') |
		ForEach-Object { Join-Path $App $_ } | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $exe -or -not (Test-Path $exe -PathType Leaf)) { throw "Not found: the program in $App" }
$work = Join-Path ([IO.Path]::GetTempPath()) "xpeccy-pgo-$PID"
$cfg = Join-Path $work 'config'
New-Item -ItemType Directory -Force $cfg | Out-Null

try {
	# own: a program of -Extra or -TsConf, named nowhere (a CI log is public);
	# cfg: a config of its own, made fresh
	$cases = [System.Collections.Generic.List[object]]::new()
	foreach ($m in 'zx48', 'zx128', 'zxplus3', 'pent', 'scorp', 'atm2', 'evo-tsconf', 'evo-baseconf', 'profi') {
		$cases.Add(@{ a = "-m $m --bench-skip 300"; own = $false })
	}
	foreach ($d in $TsConf) {
		if (-not (Test-Path $d)) { throw "Not found: $d" }
		foreach ($f in (Get-ChildItem $d -Recurse -File -Include '*.spg', '*.trd' | Sort-Object FullName)) {
			# a disk loads for some 950 frames before its program starts
			$skip = if ($f.Extension -eq '.trd') { 1500 } else { 300 }
			$cases.Add(@{ a = "-m evo-tsconf `"$($f.FullName)`" --bench-skip $skip"; own = $true; cfg = $true })
		}
	}
	if ($SysNext) {
		if (-not (Test-Path $SysNext)) { throw "Not found: $SysNext" }
		$card = Join-Path $work 'card'
		Copy-Item -Recurse $SysNext $card
		# NextZXOS boots for some 1200 frames before a program starts
		$next = "-m zxnext --sdcard `"$card`" --bench-skip 1500"
		$cases.Add(@{ a = $next; own = $false })
		foreach ($p in 'demos/scrollnutter/ScrollNutter.nex', 'demos/Spectron 2084/Spectron2084.nex',
				'demos/tilemap/tm.nex', 'demos/WidescreenImageDemo/Daybreak.nex', 'games/Next/TX-1696/main.nex',
				'games/Next/Warhawk/Warhawk.nex', 'games/Next/Nextoid/nextoid.nex', 'games/Next/AngryBloaters/Bloaters.nex') {
			$f = Join-Path $card $p
			if (Test-Path $f) { $cases.Add(@{ a = "$next `"$f`""; own = $false }) }
		}
		foreach ($d in $Extra) {
			if (-not (Test-Path $d)) { throw "Not found: $d" }
			foreach ($f in (Get-ChildItem $d -Recurse -File -Include '*.nex' | ForEach-Object FullName)) {
				$cases.Add(@{ a = "$next `"$f`""; own = $true })
			}
		}
	} elseif ($Extra) {
		Write-Host 'pgo: -Extra needs -SysNext (the card its programs run on)' -ForegroundColor Yellow
	}

	# with sound mixed, as a user runs it
	$out = Join-Path $work 'run.out'
	$n = 0
	foreach ($c in $cases) {
		$n++
		$dir = $cfg
		if ($c.cfg) {
			$dir = Join-Path $work "config-$n"
			New-Item -ItemType Directory -Force $dir | Out-Null
		}
		$cl = "--confdir `"$dir`" --no-update-check $($c.a) --bench $Frames --bench-full"
		$p = Start-Process $exe -ArgumentList $cl -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
		$line = Get-Content $out | Select-String '^bench' | Select-Object -Last 1
		if (($p.ExitCode -eq 0) -and $line) {
			Write-Host ("pgo {0,2}/{1}: {2}" -f $n, $cases.Count, ($line.Line -replace '^bench: ', ''))
		} elseif ($c.own) {
			# one that fails costs its own counts only
			Write-Host "pgo $n/$($cases.Count): failed (exit $($p.ExitCode)), a program of -Extra or -TsConf" -ForegroundColor Yellow
		} else {
			Get-Content "$out.err" -ErrorAction SilentlyContinue | Select-Object -Last 20 | Write-Host
			throw "pgo: run $n failed (exit $($p.ExitCode)): $($c.a)"
		}
	}
} finally {
	Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
