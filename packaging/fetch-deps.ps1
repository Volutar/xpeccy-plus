# Downloads the build dependencies that aren't expected to be installed system-wide:
# SDL2, curl's source (make-dist.ps1 builds it per preset), and on request the
# System/Next distribution the ZX Spectrum Next's tests boot from. Qt, MinGW and zlib
# come from a Qt installation.
#
#   .\fetch-deps.ps1            # get what deps.json pins, skip what is already there
#   .\fetch-deps.ps1 -SysNext   # System/Next too, for make-dist.ps1 -SysNext
#   .\fetch-deps.ps1 -Force     # re-download and re-extract
#   .\fetch-deps.ps1 -Update    # move the pins to the newest upstream releases
#
# Versions are pinned on purpose: rebuilding a given tag half a year later must
# produce the same binary. -Update rewrites deps.json so a version bump lands as
# its own reviewable commit instead of happening silently on every build.

param(
	[switch]$Update,
	[switch]$Force,
	[switch]$SysNext
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'	# the progress bar makes downloads crawl

$srcRoot = Split-Path $PSScriptRoot -Parent
$depsDir = Join-Path $srcRoot 'build\deps'
$manifest = Join-Path $PSScriptRoot 'deps.json'

New-Item -ItemType Directory -Path $depsDir -Force | Out-Null

function Get-Pin {
	param([string]$Name = 'sdl2')
	(Get-Content $manifest -Raw | ConvertFrom-Json).$Name
}

function Set-Pin {
	param([string]$Version, [string]$Sha256, [string]$Name = 'sdl2', [string]$SourceSha256)
	# patch the values of that one entry in place so the file keeps its formatting
	$text = Get-Content $manifest -Raw
	$pairs = @(@('version', $Version), @('sha256', $Sha256))
	if ($SourceSha256) { $pairs += ,@('source_sha256', $SourceSha256) }
	foreach ($kv in $pairs) {
		$text = $text -replace "(`"$Name`"\s*:\s*\{[^}]*?`"$($kv[0])`"\s*:\s*`")[^`"]*(`")", "`${1}$($kv[1])`${2}"
	}
	Set-Content $manifest $text -NoNewline
}

function Get-Zip {
	param([string]$Version)
	$url = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/SDL2-devel-$Version-mingw.zip"
	$zip = Join-Path $depsDir "SDL2-devel-$Version-mingw.zip"
	if (-not (Test-Path $zip) -or $Force) {
		Write-Host "downloading $url"
		Invoke-WebRequest $url -OutFile $zip -TimeoutSec 600
	}
	return $zip
}

if ($Update) {
	# NOTE: libsdl-org/SDL hosts SDL3 as well, and its "latest" release is an SDL3
	# one, so the newest release-2.* tag is what has to be looked for here.
	$releases = Invoke-RestMethod 'https://api.github.com/repos/libsdl-org/SDL/releases?per_page=100' -TimeoutSec 60
	$newest = $releases | Where-Object { $_.tag_name -like 'release-2.*' } | Select-Object -First 1
	if (-not $newest) { throw 'no SDL2 release found' }
	$newVer = $newest.tag_name -replace '^release-', ''

	if ($newVer -eq (Get-Pin).version) {
		Write-Host "SDL2 pin is current ($newVer)"
	} else {
		$old = (Get-Pin).version
		$zip = Get-Zip $newVer
		# the source tarball is what the macOS build compiles (macos.yml)
		$tgz = Join-Path $depsDir "SDL2-$newVer.tar.gz"
		Invoke-WebRequest "https://github.com/libsdl-org/SDL/releases/download/release-$newVer/SDL2-$newVer.tar.gz" -OutFile $tgz -TimeoutSec 600
		Set-Pin $newVer (Get-FileHash $zip -Algorithm SHA256).Hash -SourceSha256 (Get-FileHash $tgz -Algorithm SHA256).Hash
		Write-Host "SDL2 pin: $old -> $newVer (commit packaging\deps.json)" -ForegroundColor Yellow
	}
}

# System/Next for the tests (make-dist.ps1 -SysNext), passed on whole as its license
# asks: a clone of the pinned tag, checked against its commit, without the git metadata
# and with no line endings changed. The program itself never ships it.
function Get-SysNext {
	$sn = (Get-Content $manifest -Raw | ConvertFrom-Json).sysnext
	$dir = Join-Path $depsDir "sysnext-$($sn.release)"
	if ((Test-Path $dir) -and -not $Force) {
		Write-Host "System/Next $($sn.release): already in build\deps"
		return
	}
	$tmp = "$dir.tmp"
	foreach ($d in @($dir, $tmp)) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
	Write-Host "cloning $($sn.source) $($sn.tag)"
	& git -c core.autocrlf=false -c core.eol=lf -c advice.detachedHead=false clone --quiet --depth 1 --branch $sn.tag $sn.source $tmp
	if ($LASTEXITCODE -ne 0) { throw 'System/Next: clone failed' }
	$head = (& git -C $tmp rev-parse HEAD).Trim()
	if ($head -ne $sn.commit) {
		Remove-Item $tmp -Recurse -Force
		throw "System/Next $($sn.release): the tag has moved`n  expected $($sn.commit)`n  got      $head"
	}
	Remove-Item (Join-Path $tmp '.git') -Recurse -Force
	Rename-Item $tmp (Split-Path $dir -Leaf)
	Write-Host "System/Next $($sn.release): ready in build\deps" -ForegroundColor Green
}

if ($SysNext) { Get-SysNext }

# curl's source, checked against its pin; make-dist.ps1 builds it with each preset's
# MinGW into build\deps\curl-<version>-<preset>
function Get-CurlSource {
	param([string]$Version)
	$tgz = Join-Path $depsDir "curl-$Version.tar.gz"
	if (-not (Test-Path $tgz) -or $Force) {
		$url = "https://curl.se/download/curl-$Version.tar.gz"
		Write-Host "downloading $url"
		Invoke-WebRequest $url -OutFile $tgz -TimeoutSec 600
	}
	return $tgz
}

if ($Update) {
	# curl.se/info names the newest release
	$info = (Invoke-WebRequest 'https://curl.se/info' -UseBasicParsing -TimeoutSec 60).Content
	if ($info -notmatch '(?m)^Version:\s*(\S+)') { throw 'curl.se/info: no version' }
	$newVer = $Matches[1]
	$old = (Get-Pin curl).version
	if ($newVer -eq $old) {
		Write-Host "curl pin is current ($newVer)"
	} else {
		$tgz = Get-CurlSource $newVer
		Set-Pin $newVer (Get-FileHash $tgz -Algorithm SHA256).Hash curl
		Write-Host "curl pin: $old -> $newVer (commit packaging\deps.json)" -ForegroundColor Yellow
	}
}

$curl = Get-Pin curl
$curlTgz = Join-Path $depsDir "curl-$($curl.version).tar.gz"
if ((Test-Path $curlTgz) -and -not $Force) {
	Write-Host "curl $($curl.version): source already in build\deps"
} else {
	$curlTgz = Get-CurlSource $curl.version
	$hash = (Get-FileHash $curlTgz -Algorithm SHA256).Hash
	if ($hash -ne $curl.sha256) {
		Remove-Item $curlTgz -Force
		throw "curl $($curl.version) checksum mismatch`n  expected $($curl.sha256)`n  got      $hash"
	}
	Write-Host "curl $($curl.version): source ready in build\deps" -ForegroundColor Green
}

$pin = Get-Pin
$target = Join-Path $depsDir "SDL2-$($pin.version)-mingw"

if ((Test-Path $target) -and -not $Force) {
	Write-Host "SDL2 $($pin.version): already in build\deps"
	return
}

$zip = Get-Zip $pin.version

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($hash -ne $pin.sha256) {
	Remove-Item $zip -Force
	throw "SDL2 $($pin.version) checksum mismatch`n  expected $($pin.sha256)`n  got      $hash"
}

if (Test-Path $target) { Remove-Item $target -Recurse -Force }
$unpacked = Join-Path $depsDir "SDL2-$($pin.version)"
if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }

Expand-Archive -Path $zip -DestinationPath $depsDir -Force
Rename-Item $unpacked (Split-Path $target -Leaf)

Write-Host "SDL2 $($pin.version): ready in build\deps" -ForegroundColor Green
