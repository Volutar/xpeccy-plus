# Pictures of a staged build's main window and its Options dialog, taken by the program
# itself over its control port (winshot), with no style and with the bundled ones named.
# For win-arm64, whose Qt is not win-x64's: the two sets side by side show whether it looks
# the same.
#
#   .\win-shots.ps1 -App <staged folder> -Out <folder> [-Styles '', 'Light.qss']

param(
	[Parameter(Mandatory)][string]$App,
	[Parameter(Mandatory)][string]$Out,
	[string[]]$Styles = @('', 'Light.qss', 'Dark.qss'),
	[int]$Port = 30000
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# one command, its answer; the program answers a connection with a prompt first
function Send-Command($cmd, $waitMs = 1500) {
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
		$ans.Trim()
	} finally { $c.Close() }
}

$env:SDL_AUDIODRIVER = 'dummy'
foreach ($style in $Styles) {
	$tag = if ($style) { [IO.Path]::GetFileNameWithoutExtension($style) -replace ' ', '' } else { 'System' }
	$work = Join-Path ([IO.Path]::GetTempPath()) "xpeccy-shots-$PID"
	if (Test-Path $work) { Remove-Item $work -Recurse -Force }
	$cfg = Join-Path $work 'config'
	Copy-Item -Recurse (Join-Path $App 'config') $cfg
	"[GENERAL]`nport = $Port`nstyle = $style`n" | Set-Content (Join-Path $cfg 'config.conf')
	$p = Start-Process (Join-Path $App 'xpeccy-plus.exe') -ArgumentList '--confdir', "`"$cfg`"", '--no-update-check', '-m', 'zx48' -PassThru
	try {
		$up = $false
		for ($i = 0; $i -lt 60 -and -not $up; $i++) {
			Start-Sleep -Milliseconds 500
			if ($p.HasExited) { throw "exited with $($p.ExitCode) before its port was up" }
			$up = [bool](Get-NetTCPConnection -LocalPort $Port -State Listen -OwningProcess $p.Id -ErrorAction SilentlyContinue)
		}
		if (-not $up) { throw "no control port after 30 s" }
		Start-Sleep -Seconds 2	# the first frames
		Write-Host "$tag main: $(Send-Command "winshot $Out\$tag-main.png")"
		Write-Host "$tag options: $(Send-Command 'run-action key.options' 2500) $(Send-Command "winshot $Out\$tag-options.png")"
	} finally {
		Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
		$p.WaitForExit(10000) | Out-Null
		Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
	}
}
