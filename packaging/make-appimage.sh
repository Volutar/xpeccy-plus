#!/bin/sh
# Builds Xpeccy+ and packs it as an AppImage.
#
#   ./make-appimage.sh              # incremental
#   CLEAN=1 ./make-appimage.sh      # from scratch
#   RELEASE=1 ./make-appimage.sh    # release version string
#   MANUAL=1 ./make-appimage.sh     # with the user manual (docs/manual/README.md)
#   PGO_SET=<dir> ./make-appimage.sh    # profile-guided, trained on that checkout
#                                       # of the training set (dotkoval/xpeccy-pgo)
#
# What goes into the AppDir is defined by the install() rules in CMakeLists.txt;
# this script only drives cmake and linuxdeploy. Run packaging/linux-setup.sh
# once first.
#
# gcc 12 (22.04's own, same glibc and libstdc++ floor) is faster than 11; CC/CXX pick another.
# PGO_SET: built instrumented, trained by pgo-train.ps1 (pwsh, xvfb-run), built again.
#
# BUILD_DIR must be on a real Linux filesystem. Under WSL the sources may well
# live on /mnt/c, but linuxdeploy makes symlinks and sets permissions in the
# AppDir, and drvfs cannot do either - hence the default under $HOME.

set -e

SRC_DIR="${SRC_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-$HOME/build/xpeccy-plus}"
TOOLS_DIR="${TOOLS_DIR:-$HOME/.cache/xpeccy-plus-tools}"
JOBS="${JOBS:-$(nproc)}"
CC="${CC:-gcc-12}"
CXX="${CXX:-g++-12}"
PWSH="${PWSH:-pwsh}"

APPDIR="$BUILD_DIR/AppDir"
NAME=xpeccy-plus

# version string, mirroring cmake/genversion.cmake
BASE_VER=$(head -n 1 "$SRC_DIR/VERSION" | tr -d ' \r\n')
if [ -n "$RELEASE" ]; then
	VERSION="$BASE_VER"
else
	VERSION="$BASE_VER-dev+$(date +%Y%m%d)"
fi

OUT="$BUILD_DIR/$NAME-$VERSION-linux-x86_64.AppImage"

for tool in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
	[ -x "$TOOLS_DIR/$tool" ] || { echo "missing $TOOLS_DIR/$tool, run linux-setup.sh"; exit 1; }
done
for tool in "$CC" "$CXX"; do
	command -v "$tool" > /dev/null || { echo "missing $tool, run linux-setup.sh"; exit 1; }
done
if [ -n "$PGO_SET" ]; then
	[ -d "$PGO_SET/sysnext" ] || { echo "no card at $PGO_SET/sysnext"; exit 1; }
	command -v "$PWSH" > /dev/null || { echo "missing pwsh, needed for PGO_SET"; exit 1; }
	[ -n "$DISPLAY" ] || command -v xvfb-run > /dev/null \
		|| { echo "missing xvfb-run, needed for PGO_SET without a display"; exit 1; }
fi

echo "=== $NAME $VERSION ==="
echo "src   $SRC_DIR"
echo "build $BUILD_DIR"

[ -n "$CLEAN" ] && rm -rf "$BUILD_DIR"

# always said, so a build without MANUAL does not install one a previous run built
MANUAL_DIR=
if [ -n "$MANUAL" ]; then
	python3 "$SRC_DIR/tools/manual.py" html
	MANUAL_DIR="$SRC_DIR/build/manual"
fi

# the flags always said, so a plain build after a profile-guided one is plain
configure() {
	cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
		-DCMAKE_BUILD_TYPE=Release \
		-DXRELEASE="$([ -n "$RELEASE" ] && echo ON || echo OFF)" \
		-DQTVERSION=5 \
		-DXPACKAGE=linux-x86_64 \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_C_COMPILER="$CC" \
		-DCMAKE_CXX_COMPILER="$CXX" \
		-DCMAKE_C_FLAGS="$1" \
		-DCMAKE_CXX_FLAGS="$1"
}

FLAGS=
if [ -n "$PGO_SET" ]; then
	echo "--- pgo: instrumented build ---"
	find "$BUILD_DIR" -name '*.gcda' -delete 2> /dev/null || true
	configure -fprofile-generate
	cmake --build "$BUILD_DIR" -j "$JOBS"
	TRAIN="$BUILD_DIR/pgo-train"
	rm -rf "$TRAIN"
	DESTDIR="$TRAIN" cmake --install "$BUILD_DIR"
	echo "--- pgo: training ---"
	RUN=
	[ -z "$DISPLAY" ] && RUN="xvfb-run -a"
	set -- -App "$TRAIN/usr/bin/$NAME" -SysNext "$PGO_SET/sysnext"
	[ -d "$PGO_SET/next" ] && set -- "$@" -Extra "$PGO_SET/next"
	[ -d "$PGO_SET/tsconf" ] && set -- "$@" -TsConf "$PGO_SET/tsconf"
	# shellcheck disable=SC2086
	$RUN "$PWSH" -NoProfile -File "$SRC_DIR/packaging/pgo-train.ps1" "$@"
	rm -rf "$TRAIN"
	# a training that wrote nothing must not give a build called profile-guided
	[ -n "$(find "$BUILD_DIR" -name '*.gcda' | head -n 1)" ] \
		|| { echo "pgo: the training wrote no counts"; exit 1; }
	echo "--- pgo: the build by the profile ---"
	# code the training never ran is optimized as usual, not for size
	FLAGS="-fprofile-use -fprofile-correction -Wno-missing-profile -fprofile-partial-training"
fi

configure "$FLAGS"
cmake --build "$BUILD_DIR" -j "$JOBS"

# cmake --install only adds files, so start from an empty AppDir
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

rm -f "$OUT" "$BUILD_DIR/$NAME"*.AppImage

# APPIMAGE_EXTRACT_AND_RUN: the tools are AppImages themselves and would
# otherwise need fuse2 just to unpack; QMAKE: Ubuntu keeps it out of PATH
cd "$BUILD_DIR"
APPIMAGE_EXTRACT_AND_RUN=1 \
QMAKE="${QMAKE:-/usr/lib/qt5/bin/qmake}" \
"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
	--appdir "$APPDIR" \
	--plugin qt \
	--desktop-file "$APPDIR/usr/share/applications/$NAME.desktop" \
	--icon-file "$SRC_DIR/images/$NAME.png" \
	--output appimage

# linuxdeploy names it after the desktop entry; give it the version, the way
# the Windows archives carry one
mv "$BUILD_DIR"/*x86_64.AppImage "$OUT"

echo
ls -lh "$OUT"
