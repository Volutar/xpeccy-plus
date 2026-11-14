#!/bin/sh
# Installs everything needed to build Xpeccy+ and pack it as an AppImage.
#
#   ./linux-setup.sh
#
# Written for Debian/Ubuntu (tested on Ubuntu 22.04, including WSL2). Safe to
# run again: apt skips what is already there and the AppImage tools are only
# downloaded when missing.

set -e

TOOLS_DIR="${TOOLS_DIR:-$HOME/.cache/xpeccy-plus-tools}"

PACKAGES="build-essential gcc-12 g++-12 cmake pkg-config file ca-certificates wget
	qtbase5-dev qtbase5-dev-tools libqt5opengl5-dev
	libsdl2-dev zlib1g-dev libgl1-mesa-dev libcurl4-openssl-dev
	desktop-file-utils libfuse2"

# gcc-12: what make-appimage.sh builds with; PGO_SET also needs pwsh, not installed here.
# libfuse2: Ubuntu 22.04 ships fuse3 only, and an AppImage mounts itself
# through fuse2. Needed to run one, not to build one.

echo "=== packages ==="
sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
# shellcheck disable=SC2086
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PACKAGES

echo "=== AppImage tools -> $TOOLS_DIR ==="
mkdir -p "$TOOLS_DIR"
BASE="https://github.com/linuxdeploy/linuxdeploy"
for tool in \
	"linuxdeploy-x86_64.AppImage $BASE/releases/download/continuous" \
	"linuxdeploy-plugin-qt-x86_64.AppImage ${BASE}-plugin-qt/releases/download/continuous"
do
	name=${tool%% *}
	url=${tool#* }
	if [ -x "$TOOLS_DIR/$name" ]; then
		echo "have $name"
	else
		echo "fetching $name"
		wget -q -O "$TOOLS_DIR/$name" "$url/$name"
		chmod +x "$TOOLS_DIR/$name"
	fi
done

echo
echo "done. Build with: packaging/make-appimage.sh"
