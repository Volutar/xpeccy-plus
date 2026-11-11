# The training runs of a profile-guided build (make-dist.ps1 -Pgo): an instrumented
# program is run, headless, over what users run, and writes how often each branch and
# function ran (gcc: beside its objects, clang: where make-dist told it); the final build
# is laid out by those counts.
#
#   .\pgo-train.ps1 -App <staged folder> [-SysNext <card>] [-Extra <folder>,...]
#
# Every built-in machine boots its own ROM; the Next, with System/Next (or a cut-down
# copy of it) as its card, boots NextZXOS and runs programs that come with it. -Extra
# adds every .nex found under the folders given (programs of one's own, run on the same
# card with the folder of each put onto it, so the files beside a program are where it
# looks). The card is copied first: a run may write to it. The runs go one after
# another: the counts of each are added to the same files at its exit.

param(
	[Parameter(Mandatory)] [string]$App,
	[string]$SysNext,
	[string[]]$Extra = @(),
	[int]$Frames = 400
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $App 'xpeccy-plus.exe'
if (-not (Test-Path $exe)) { throw "Not found: $exe" }
$work = Join-Path ([IO.Path]::GetTempPath()) "xpeccy-pgo-$PID"
$cfg = Join-Path $work 'config'
New-Item -ItemType Directory -Force $cfg | Out-Null

try {
	# own: a program of -Extra, named nowhere (a CI log is public)
	$cases = [System.Collections.Generic.List[object]]::new()
	foreach ($m in 'zx48', 'zx128', 'zxplus3', 'pent', 'scorp', 'atm2', 'evo-tsconf', 'evo-baseconf', 'profi') {
		$cases.Add(@{ a = "-m $m --bench-skip 300"; own = $false })
	}
	if ($SysNext) {
		if (-not (Test-Path $SysNext)) { throw "Not found: $SysNext" }
		$card = Join-Path $work 'card'
		Copy-Item -Recurse $SysNext $card
		# NextZXOS boots for some 1200 frames before a program starts
		$next = "-m zxnext --sdcard `"$card`" --bench-skip 1500"
		$cases.Add(@{ a = $next; own = $false })
		foreach ($p in 'demos\scrollnutter\ScrollNutter.nex', 'demos\Spectron 2084\Spectron2084.nex',
				'demos\tilemap\tm.nex', 'demos\WidescreenImageDemo\Daybreak.nex', 'games\Next\TX-1696\main.nex',
				'games\Next\Warhawk\Warhawk.nex', 'games\Next\Nextoid\nextoid.nex', 'games\Next\AngryBloaters\Bloaters.nex') {
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
		$cl = "--confdir `"$cfg`" --no-update-check $($c.a) --bench $Frames --bench-full"
		$p = Start-Process $exe -ArgumentList $cl -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
		$line = Get-Content $out | Select-String '^bench' | Select-Object -Last 1
		if (($p.ExitCode -eq 0) -and $line) {
			Write-Host ("pgo {0,2}/{1}: {2}" -f $n, $cases.Count, ($line.Line -replace '^bench: ', ''))
		} elseif ($c.own) {
			# one that fails costs its own counts only
			Write-Host "pgo $n/$($cases.Count): failed (exit $($p.ExitCode)), a program of -Extra" -ForegroundColor Yellow
		} else {
			Get-Content "$out.err" -ErrorAction SilentlyContinue | Select-Object -Last 20 | Write-Host
			throw "pgo: run $n failed (exit $($p.ExitCode)): $($c.a)"
		}
	}
} finally {
	Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
