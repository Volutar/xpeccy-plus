# Builds Xpeccy+ and stages a ready-to-run folder (binary + Qt/SDL DLLs + config + docs).
#
#   .\make-dist.ps1                     # qt5-x64, incremental
#   .\make-dist.ps1 -Preset qt6-x64     # Qt6 build in its own folder
#   .\make-dist.ps1 -Clean -Zip         # from scratch + archive
#   .\make-dist.ps1 -Manual             # with the user manual (docs/manual/README.md)
#   .\make-dist.ps1 -SysNext            # with System/Next beside the program, for tests
#
# What goes into the folder is defined by the install() rules in CMakeLists.txt and
# cmake/windeploy.cmake.in; this script only picks a toolchain and drives cmake.
# Build artifacts and the staged folders stay under the gitignored build/ dir.
#
# SDL2 and curl's source come from build\deps - run packaging\fetch-deps.ps1 once to get
# them; libcurl is built from it here, once per preset.

param(
	[ValidateSet('qt5-x64', 'qt6-x64', 'qt5-x86')]
	[string]$Preset = 'qt5-x64',
	[switch]$Clean,
	[switch]$Zip,
	[switch]$Release,
	[switch]$Full,		# keep everything windeployqt copied, skip the trim step
	[switch]$Manual,	# build the user manual (tools/manual.py) and put it in the folder
	[switch]$SysNext,	# put System/Next beside the program: the Next's card with none made
	[int]$Jobs = 16,
	# a build agent installs Qt and MinGW wherever it likes, and has cmake in
	# PATH already; locally the table below is right and none of this is needed
	[string]$QtDir,
	[string]$MinGW
)

$ErrorActionPreference = 'Stop'

$srcRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $srcRoot 'build'
$cmakeBin = 'C:\Qt\Tools\CMake_64\bin'	# the Qt installer's; ignored where there is none

# same manifest fetch-deps.ps1 works from, so the two can't drift apart
$sdlVer = (Get-Content (Join-Path $PSScriptRoot 'deps.json') -Raw | ConvertFrom-Json).sdl2.version
$depsSDL = Join-Path $buildRoot "deps\SDL2-$sdlVer-mingw"

# toolchain table: everything machine-specific lives here
$toolchains = @{
	'qt5-x64' = @{
		QtVer = 5
		QtDir = 'C:\Qt\5.15.2\mingw81_64'
		MinGW = 'C:\Qt\Tools\mingw810_64'
		Triple = 'x86_64-w64-mingw32'
		Arch = 'x64'
		Tag = ''
	}
	'qt5-x86' = @{
		QtVer = 5
		QtDir = 'C:\Qt\5.15.2\mingw81_32'
		MinGW = 'C:\Qt\Tools\mingw810_32'
		Triple = 'i686-w64-mingw32'
		Arch = 'x86'
		Tag = ''
	}
	'qt6-x64' = @{
		QtVer = 6
		QtDir = 'C:\Qt\6.6.2\mingw_64'
		MinGW = 'C:\Qt\Tools\mingw1120_64'
		Triple = 'x86_64-w64-mingw32'
		Arch = 'x64'
		Tag = '-qt6'
	}
}

$tc = $toolchains[$Preset]
if ($QtDir) { $tc.QtDir = $QtDir }
if ($MinGW) { $tc.MinGW = $MinGW }
$sdlPrefix = Join-Path $depsSDL $tc.Triple

if (-not (Test-Path $sdlPrefix)) {
	throw "SDL2 $sdlVer is missing, run packaging\fetch-deps.ps1 first"
}
foreach ($p in @($tc.QtDir, $tc.MinGW)) {
	if (-not (Test-Path $p)) { throw "Not found: $p" }
}

# version string, mirroring cmake/genversion.cmake
$baseVer = (Get-Content (Join-Path $srcRoot 'VERSION') -TotalCount 1).Trim()
$version = if ($Release) { $baseVer } else { "$baseVer-dev+$(Get-Date -Format 'yyyyMMdd')" }

$buildDir = Join-Path $buildRoot "out\$Preset"
$distName = "xpeccy-plus-$version-win-$($tc.Arch)$($tc.Tag)"
$distDir = Join-Path $buildRoot "dist\$distName"

$env:PATH = "$($tc.MinGW)\bin;$($tc.QtDir)\bin;$env:PATH"
if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
$env:SDL2DIR = $sdlPrefix

Write-Host "=== $Preset -> $distName ===" -ForegroundColor Cyan

if ($Clean -and (Test-Path $buildDir)) { Remove-Item $buildDir -Recurse -Force }

# cmake eats backslashes in -D values, so every path passed below must be forward-slashed
$mingwFwd = $tc.MinGW -replace '\\', '/'
$qtFwd = $tc.QtDir -replace '\\', '/'

# --- libcurl, for the program's downloads (src/xcore/download.cpp) ---
# Built once per preset from the source fetch-deps.ps1 checked: static, Schannel (the
# system's TLS and certificates), HTTP and HTTPS only - nothing to ship beside the exe.

$curlVer = (Get-Content (Join-Path $PSScriptRoot 'deps.json') -Raw | ConvertFrom-Json).curl.version
$curlTgz = Join-Path $buildRoot "deps\curl-$curlVer.tar.gz"
$curlSrc = Join-Path $buildRoot "deps\curl-$curlVer"
$curlDir = Join-Path $buildRoot "deps\curl-$curlVer-$Preset"
if (-not (Test-Path "$curlDir\lib\libcurl.a")) {
	if (-not (Test-Path $curlTgz)) { throw "curl $curlVer is missing, run packaging\fetch-deps.ps1 first" }
	if (-not (Test-Path $curlSrc)) {
		& "$env:SystemRoot\System32\tar.exe" -xzf $curlTgz -C (Join-Path $buildRoot 'deps')
		if ($LASTEXITCODE -ne 0) { throw 'curl: unpacking failed' }
	}
	$curlBuild = Join-Path $buildRoot "out\curl-$curlVer-$Preset"
	if (Test-Path $curlBuild) { Remove-Item $curlBuild -Recurse -Force }
	$curlArgs = @(
		'-G', 'MinGW Makefiles'
		'-DCMAKE_BUILD_TYPE=Release'
		"-DCMAKE_INSTALL_PREFIX=$($curlDir -replace '\\', '/')"
		'-DCURL_TARGET_WINDOWS_VERSION=0x0600'
		'-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON'
		'-DBUILD_CURL_EXE=OFF', '-DBUILD_TESTING=OFF', '-DBUILD_EXAMPLES=OFF'
		'-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF', '-DENABLE_CURL_MANUAL=OFF'
		'-DCURL_USE_SCHANNEL=ON', '-DCURL_USE_OPENSSL=OFF', '-DCURL_USE_PKGCONFIG=OFF'
		'-DCURL_USE_LIBSSH2=OFF', '-DCURL_USE_LIBPSL=OFF', '-DUSE_NGHTTP2=OFF', '-DUSE_LIBIDN2=OFF'
		'-DCURL_ZLIB=OFF', '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF'
		'-S', $curlSrc
		'-B', $curlBuild
	)
	foreach ($proto in 'DICT', 'FILE', 'FTP', 'GOPHER', 'IMAP', 'IPFS', 'LDAP', 'LDAPS', 'MQTT', 'POP3', 'RTSP', 'SMTP', 'TELNET', 'TFTP', 'WEBSOCKETS') {
		$curlArgs += "-DCURL_DISABLE_$proto=ON"
	}
	Write-Host "--- curl $curlVer for $Preset ---" -ForegroundColor Cyan
	& cmake @curlArgs
	if ($LASTEXITCODE -ne 0) { throw 'curl: configure failed' }
	& cmake --build $curlBuild -j $Jobs
	if ($LASTEXITCODE -ne 0) { throw 'curl: build failed' }
	& cmake --install $curlBuild
	if ($LASTEXITCODE -ne 0) { throw 'curl: install failed' }
	Remove-Item $curlBuild -Recurse -Force
}

# --- configure + build ---

$cmakeArgs = @(
	'-G', 'MinGW Makefiles'
	"-DQTVERSION=$($tc.QtVer)"
	'-DCMAKE_BUILD_TYPE=Release'
	"-DXRELEASE=$(if ($Release) { 'ON' } else { 'OFF' })"
	'-DUSEOPENGL=1'
	'-DUSEQTNETWORK=1'
	'-DSDL1BUILD=0'
	"-DTRIMDEPLOY=$(if ($Full) { 'OFF' } else { 'ON' })"
	"-DZLIB_LIBRARY=$mingwFwd/$($tc.Triple)/lib/libz.a"
	"-DZLIB_INCLUDE_DIR=$mingwFwd/$($tc.Triple)/include"
	"-DCURL_DIR=$($curlDir -replace '\\', '/')/lib/cmake/CURL"
	# the asset this is, for the update check: the zip's name less its version
	"-DXPACKAGE=win-$($tc.Arch)$($tc.Tag)"
	'-S', $srcRoot
	'-B', $buildDir
)
if ($tc.QtVer -eq 5) {
	$cmakeArgs += "-DQt5_DIR=$qtFwd/lib/cmake/Qt5"
} else {
	$cmakeArgs += "-DCMAKE_PREFIX_PATH=$qtFwd"
}

# always said, so a build without -Manual does not install one a previous run built
$manualDir = ''
if ($Manual) {
	& python (Join-Path $srcRoot 'tools\manual.py') html
	if ($LASTEXITCODE -ne 0) { throw 'manual: html failed' }
	$manualDir = (Join-Path $buildRoot 'manual') -replace '\\', '/'
}
$cmakeArgs += "-DMANUAL_DIR=$manualDir"

# System/Next is not shipped: the Next makes its card from the official release
# (src/xgui/nextcard.cpp). -SysNext puts the copy fetch-deps.ps1 got beside the
# program, which the Next takes when no card was made - what the tests start from.
# A folder staged before keeps one until -Clean.
$sn = (Get-Content (Join-Path $PSScriptRoot 'deps.json') -Raw | ConvertFrom-Json).sysnext
$sysnextDir = Join-Path $buildRoot "deps\sysnext-$($sn.release)"
if ($SysNext -and (Test-Path $sysnextDir)) {
	$cmakeArgs += "-DSYSNEXT_DIR=$($sysnextDir -replace '\\', '/')"
} else {
	if ($SysNext) { Write-Host "System/Next $($sn.release) is missing (packaging\fetch-deps.ps1 -SysNext)" -ForegroundColor Yellow }
	$cmakeArgs += '-DSYSNEXT_DIR='
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }

& cmake --build $buildDir --target all -j $Jobs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# --- stage ---

# The whole layout - binary, config, docs, Qt/SDL runtime, plugin trimming -
# lives in CMakeLists.txt + cmake/windeploy.cmake.in, so a CI agent gets it
# from git and this script stays a toolchain wrapper.
#
# Always wipe first: cmake --install only adds files, it never removes ones
# left behind by an older version.
if (Test-Path $distDir) { Remove-Item $distDir -Recurse -Force }

& cmake --install $buildDir --prefix $distDir
if ($LASTEXITCODE -ne 0) { throw 'install failed' }

if ($Zip) {
	$zipPath = Join-Path $buildRoot "dist\$distName.zip"
	if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
	Compress-Archive -Path $distDir -DestinationPath $zipPath
	Write-Host "zip: $zipPath"
}

$size = (Get-ChildItem $distDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("done: {0} ({1:N1} MB)" -f $distDir, $size) -ForegroundColor Green
