#!/bin/sh
# Prints the "which file to download" part of a release page, in Markdown, with a
# direct link to each file - not everyone finds the Assets list on GitHub.
#
#   packaging/release-files.sh <tag> <name>    e.g. v2026.9 xpeccy-plus-2026.9
#
# <name> is the files' name up to the package: xpeccy-plus-<version>. The packages
# are named by packaging/make-dist.ps1, make-appimage.sh and macos.yml.
set -e
tag=$1
name=$2
repo=${GITHUB_REPOSITORY:-dotkoval/xpeccy-plus}
[ -n "$tag" ] && [ -n "$name" ] || { echo "usage: release-files.sh <tag> <name>" >&2; exit 2; }

# a link to the file, its name shown; "+" of a dev version is escaped in the URL
link() {
	echo "[\`$name$1\`](https://github.com/$repo/releases/download/$tag/$(echo "$name$1" | sed 's/+/%2B/g'))"
}

echo "**Which file to download**"
echo
echo "- Windows 10 and 11: $(link -win-x64.zip)"
echo "- Windows 11 on ARM: $(link -win-arm64.zip)"
echo "- Windows 7 and 8.1: $(link -win-x64-legacy.zip) (64-bit) or $(link -win-x86-legacy.zip) (32-bit)"
echo "- Linux: $(link -linux-x86_64.AppImage)"
echo "- macOS (Apple silicon): $(link -macos-arm64.dmg)"
