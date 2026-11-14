# Builds Xpeccy+ and stages a ready-to-run folder (binary + Qt/SDL DLLs + config + docs).
#
#   .\make-dist.ps1                     # qt6-x64 (win-x64), incremental
#   .\make-dist.ps1 -Preset qt5-x86     # Qt5 32-bit (win-x86-legacy) in its own folder
#   .\make-dist.ps1 -Preset qt6-arm64   # ARM64 (win-arm64), on an ARM64 machine with MSYS2
#   .\make-dist.ps1 -Clean -Zip         # from scratch + archive
#   .\make-dist.ps1 -Manual             # with the user manual (docs/manual/README.md)
#   .\make-dist.ps1 -SysNext            # with System/Next beside the program, for tests
#   .\make-dist.ps1 -Pgo                # profile-guided: built, trained, built again
#   .\make-dist.ps1 -Pgo -PgoExtra D:\nex   # ...the .nex programs under D:\nex trained on too
#   .\make-dist.ps1 -Pgo -PgoCard D:\card   # ...on that card, not the System/Next of build\deps
#   .\make-dist.ps1 -Pgo -PgoTsConf D:\ts   # ...the .spg/.trd under D:\ts on the TSConf too
#
# What goes into the folder is defined by the install() rules in CMakeLists.txt and
# cmake/windeploy.cmake.in; this script only picks a toolchain and drives cmake.
# Build artifacts and the staged folders stay under the gitignored build/ dir.
#
# SDL2 and curl's source come from build\deps - run packaging\fetch-deps.ps1 once to get
# them; libcurl is built from it here, once per preset. qt6-arm64 takes clang, Qt, SDL2 and
# zlib from MSYS2's CLANGARM64 environment instead (installed by pacman, see windows.yml):
# qt.io has no MinGW Qt for ARM64, and SDL's own MinGW package no ARM64 build.

param(
	[ValidateSet('qt5-x64', 'qt6-x64', 'qt5-x86', 'qt6-arm64')]
	[string]$Preset = 'qt6-x64',
	[switch]$Clean,
	[switch]$Zip,
	[switch]$Release,
	[switch]$Full,		# keep everything windeployqt copied, skip the trim step
	[switch]$Manual,	# build the user manual (tools/manual.py) and put it in the folder
	[switch]$SysNext,	# put System/Next beside the program: the Next's card with none made
	[switch]$Pgo,		# profile-guided (pgo-train.ps1): needs a card, System/Next by default
	[string[]]$PgoExtra = @(),	# folders whose .nex programs the training runs too
	[string[]]$PgoTsConf = @(),	# folders whose .spg/.trd the training runs on the TSConf
	[string]$PgoCard,	# the card the training boots the Next from (default: System/Next of build\deps)
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

# toolchain table: everything machine-specific lives here. Package is the release asset's
# name less its version, and the build's XPACKAGE: the update check looks for exactly that
# file, so a rename strands every install of the old name (src/xcore/update.cpp)
$toolchains = @{
	'qt5-x64' = @{
		QtVer = 5
		QtDir = 'C:\Qt\5.15.2\mingw81_64'
		MinGW = 'C:\Qt\Tools\mingw810_64'
		Triple = 'x86_64-w64-mingw32'
		Package = 'win-x64-legacy'
	}
	'qt5-x86' = @{
		QtVer = 5
		QtDir = 'C:\Qt\5.15.2\mingw81_32'
		MinGW = 'C:\Qt\Tools\mingw810_32'
		Triple = 'i686-w64-mingw32'
		Package = 'win-x86-legacy'
	}
	'qt6-x64' = @{
		QtVer = 6
		QtDir = 'C:\Qt\6.6.2\mingw_64'
		MinGW = 'C:\Qt\Tools\mingw1120_64'
		Triple = 'x86_64-w64-mingw32'
		Package = 'win-x64'
	}
	# Qt is whatever version MSYS2 has (windows.yml prints it); one prefix holds it all
	'qt6-arm64' = @{
		QtVer = 6
		QtDir = 'C:\msys64\clangarm64'
		MinGW = 'C:\msys64\clangarm64'
		Triple = 'aarch64-w64-mingw32'
		Package = 'win-arm64'
		Msys = $true
	}
}

$tc = $toolchains[$Preset]
if ($QtDir) { $tc.QtDir = $QtDir }
if ($MinGW) { $tc.MinGW = $MinGW }
if ($tc.Msys) {
	# clang and ninja there, gcc and mingw32-make in the Qt installer's MinGW
	$sdlPrefix = $tc.MinGW
	$libPrefix = $tc.MinGW
	$generator = @('-G', 'Ninja', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++')
	if (-not (Test-Path "$($tc.MinGW)\bin\clang.exe")) { throw "No MSYS2 CLANGARM64 at $($tc.MinGW): its packages are listed in .github\workflows\windows.yml" }
} else {
	$sdlPrefix = Join-Path $depsSDL $tc.Triple
	$libPrefix = Join-Path $tc.MinGW $tc.Triple
	$generator = @('-G', 'MinGW Makefiles')
}

if (-not (Test-Path $sdlPrefix)) {
	throw "SDL2 $sdlVer is missing, run packaging\fetch-deps.ps1 first"
}
foreach ($p in @($tc.QtDir, $tc.MinGW)) {
	if (-not (Test-Path $p)) { throw "Not found: $p" }
}

# version string, mirroring cmake/genversion.cmake
$baseVer = (Get-Content (Join-Path $srcRoot 'VERSION') -TotalCount 1).Trim()
$version = if ($Release) { $baseVer } else { "$baseVer-dev+$(Get-Date -Format 'yyyyMMdd')" }

# a profile-guided build in a folder of its own: its flags would rebuild everything in
# the usual one, and back again on the next plain build
$buildDir = Join-Path $buildRoot "out\$Preset$(if ($Pgo) { '-pgo' })"
$distName = "xpeccy-plus-$version-$($tc.Package)"
$distDir = Join-Path $buildRoot "dist\$distName"

# the manual's python is the one found before MSYS2's own goes first in PATH
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
$env:PATH = "$($tc.MinGW)\bin;$($tc.QtDir)\bin;$env:PATH"
if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
$env:SDL2DIR = $sdlPrefix

Write-Host "=== $Preset -> $distName ===" -ForegroundColor Cyan

if ($Clean -and (Test-Path $buildDir)) { Remove-Item $buildDir -Recurse -Force }

# cmake eats backslashes in -D values, so every path passed below must be forward-slashed
$libFwd = $libPrefix -replace '\\', '/'
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
	$curlArgs = $generator + @(
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

$cmakeArgs = $generator + @(
	"-DQTVERSION=$($tc.QtVer)"
	'-DCMAKE_BUILD_TYPE=Release'
	"-DXRELEASE=$(if ($Release) { 'ON' } else { 'OFF' })"
	"-DTRIMDEPLOY=$(if ($Full) { 'OFF' } else { 'ON' })"
	"-DZLIB_LIBRARY=$libFwd/lib/libz.a"
	"-DZLIB_INCLUDE_DIR=$libFwd/include"
	"-DCURL_DIR=$($curlDir -replace '\\', '/')/lib/cmake/CURL"
	# the asset this is, for the update check: the zip's name less its version
	"-DXPACKAGE=$($tc.Package)"
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
	if (-not $python) { throw 'manual: no python' }
	& $python (Join-Path $srcRoot 'tools\manual.py') html
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

# --- profile-guided: an instrumented build trained first ---
# gcc writes its counts (.gcda) beside its objects in $buildDir; the build after reads
# them there. Code the training never ran is optimized as usual, not for size, where
# gcc can (10 and later): a machine left out of the training loses nothing.
# clang (qt6-arm64) writes raw counts into a folder of their own, every run of the
# program adding into one file at its exit; llvm-profdata then makes the profile of
# them that the build after reads.
if ($Pgo) {
	if (-not $PgoCard) { $PgoCard = $sysnextDir }
	if (-not (Test-Path $PgoCard)) { throw "pgo: no card at $PgoCard (packaging\fetch-deps.ps1 -SysNext)" }
	if ($tc.Msys) {
		$rawDir = Join-Path $buildDir 'profraw'
		$profData = Join-Path $buildDir 'xpeccy.profdata'
		$gen = "-fprofile-generate=$($rawDir -replace '\\', '/')"
		# no counts for a file, or counts of another source: said once, not per function
		$use = "-fprofile-use=$($profData -replace '\\', '/') -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
		if (Test-Path $rawDir) { Remove-Item $rawDir -Recurse -Force }
	} else {
		$gen = '-fprofile-generate'
		$use = '-fprofile-use -fprofile-correction -Wno-missing-profile'
		$gcc = (& (Join-Path $tc.MinGW 'bin\gcc.exe') -dumpversion).Trim()
		if ([int]$gcc.Split('.')[0] -ge 10) { $use += ' -fprofile-partial-training' }
		if (Test-Path $buildDir) { Get-ChildItem $buildDir -Recurse -File -Filter '*.gcda' | Remove-Item -Force }
	}
	Write-Host '--- pgo: instrumented build ---' -ForegroundColor Cyan
	& cmake @cmakeArgs "-DCMAKE_C_FLAGS=$gen" "-DCMAKE_CXX_FLAGS=$gen"
	if ($LASTEXITCODE -ne 0) { throw 'pgo: configure failed' }
	& cmake --build $buildDir --target all -j $Jobs
	if ($LASTEXITCODE -ne 0) { throw 'pgo: build failed' }
	$trainDir = Join-Path $buildRoot "out\$Preset-pgo-train"
	if (Test-Path $trainDir) { Remove-Item $trainDir -Recurse -Force }
	& cmake --install $buildDir --prefix $trainDir
	if ($LASTEXITCODE -ne 0) { throw 'pgo: install failed' }
	Write-Host '--- pgo: training ---' -ForegroundColor Cyan
	& (Join-Path $PSScriptRoot 'pgo-train.ps1') -App $trainDir -SysNext $PgoCard -Extra $PgoExtra -TsConf $PgoTsConf
	Remove-Item $trainDir -Recurse -Force
	if ($tc.Msys) {
		# a training that wrote nothing must not give a build called profile-guided
		$raw = @(Get-ChildItem $rawDir -File -Filter '*.profraw' -ErrorAction SilentlyContinue)
		if (-not $raw) { throw "pgo: the training wrote no counts into $rawDir" }
		& (Join-Path $tc.MinGW 'bin\llvm-profdata.exe') merge -o $profData @($raw.FullName)
		if ($LASTEXITCODE -ne 0) { throw 'pgo: llvm-profdata failed' }
	}
	Write-Host '--- pgo: the build by the profile ---' -ForegroundColor Cyan
	$cmakeArgs += "-DCMAKE_C_FLAGS=$use", "-DCMAKE_CXX_FLAGS=$use"
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

# The sha256 of every file shipped, as sha256sum prints them: an update tells a config
# file the user left as shipped from one they changed by it (src/xcore/update_apply.cpp)
$manifest = Get-ChildItem $distDir -Recurse -File | Sort-Object FullName | ForEach-Object {
	$rel = $_.FullName.Substring($distDir.Length + 1) -replace '\\', '/'
	'{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(), $rel
}
[IO.File]::WriteAllText((Join-Path $distDir 'manifest.sha256'), ($manifest -join "`n") + "`n")

if ($Zip) {
	$zipPath = Join-Path $buildRoot "dist\$distName.zip"
	if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
	Compress-Archive -Path $distDir -DestinationPath $zipPath
	Write-Host "zip: $zipPath"
}

$size = (Get-ChildItem $distDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("done: {0} ({1:N1} MB)" -f $distDir, $size) -ForegroundColor Green
