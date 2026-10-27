#!/bin/sh
# Builds Xpeccy+ and packs it as an AppImage.
#
#   ./make-appimage.sh              # incremental
#   CLEAN=1 ./make-appimage.sh      # from scratch
#   RELEASE=1 ./make-appimage.sh    # release version string
#   MANUAL=1 ./make-appimage.sh     # with the user manual (docs/manual/README.md)
#
# What goes into the AppDir is defined by the install() rules in CMakeLists.txt;
# this script only drives cmake and linuxdeploy. Run packaging/linux-setup.sh
# once first.
#
# BUILD_DIR must be on a real Linux filesystem. Under WSL the sources may well
# live on /mnt/c, but linuxdeploy makes symlinks and sets permissions in the
# AppDir, and drvfs cannot do either - hence the default under $HOME.

set -e

SRC_DIR="${SRC_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-$HOME/build/xpeccy-plus}"
TOOLS_DIR="${TOOLS_DIR:-$HOME/.cache/xpeccy-plus-tools}"
JOBS="${JOBS:-$(nproc)}"

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

cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DXRELEASE="$([ -n "$RELEASE" ] && echo ON || echo OFF)" \
	-DQTVERSION=5 \
	-DUSEOPENGL=1 \
	-DUSEQTNETWORK=1 \
	-DSDL1BUILD=0 \
	-DXPACKAGE=linux-x86_64 \
	-DCMAKE_INSTALL_PREFIX=/usr

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
