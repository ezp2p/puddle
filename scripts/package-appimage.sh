#!/usr/bin/env bash
# The desktop app as one AppImage (Linux): Puddle with its Qt, QML and libraries beside it, run as a single file.
#   scripts/package-appimage.sh <build dir> <out file>
# Needs QMAKE (the Qt it was built with) and downloads linuxdeploy and its Qt plugin into the build dir.
set -euo pipefail
build=$1 out=$2
cd "$(dirname "$0")/.."
root=$PWD
tools=$build/appimage-tools
mkdir -p "$tools"
for t in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
  [ -x "$tools/$t" ] || { curl -fsSL -o "$tools/$t" "https://github.com/linuxdeploy/$(echo "$t" | sed 's/-x86_64.AppImage//')/releases/download/continuous/$t"; chmod +x "$tools/$t"; }
done
export APPIMAGE_EXTRACT_AND_RUN=1          # no FUSE on the runners
export QML_SOURCES_PATHS=$root/src/gui/qml # which QML modules to bring
export EXTRA_QT_MODULES="svg;multimedia"   # the icons; the camera and video (loaded at run time, so not seen in the binary)
appdir=$build/AppDir
rm -rf "$appdir"
icon=$build/puddle.png
cp src/gui/assets/puddle.png "$icon"
"$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" -e "$build/Puddle" -d dist/puddle.desktop -i "$icon" --plugin qt
# puddled too, for running it without a window (systemd --user, as in the server download)
install -m755 "$build/puddled" "$appdir/usr/bin/puddled"
OUTPUT=$out "$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" --output appimage
