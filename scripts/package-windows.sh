#!/usr/bin/env bash
# The desktop app for Windows as a folder to zip (in MSYS2's UCRT64 shell): Puddle.exe and puddled.exe, Qt's DLLs, plugins
# and QML (windeployqt), and every other DLL they need from MSYS2 (OpenSSL, Opus, libdatachannel, the C++ runtime, ...).
#   scripts/package-windows.sh <build dir> <out dir>
set -euo pipefail
build=$1 out=$2
cd "$(dirname "$0")/.."
rm -rf "$out"
mkdir -p "$out"
cp "$build/Puddle.exe" "$build/puddled.exe" "$out/"
wdq=$(command -v windeployqt6 || command -v windeployqt-qt6 || ls /ucrt64/share/qt6/bin/windeployqt.exe 2>/dev/null | head -1)
"$wdq" --release --qmldir src/gui/qml --no-translations "$out/Puddle.exe"
# the DLLs windeployqt leaves out: whatever any file here needs from MSYS2, until nothing new is missing
prefix=$(cygpath -m "${MINGW_PREFIX:-/ucrt64}")
while :; do
  added=0
  while read -r dll; do
    [ -e "$out/$(basename "$dll")" ] && continue
    cp "$dll" "$out/" && added=1
  done < <(find "$out" -iname '*.exe' -o -iname '*.dll' | while read -r f; do ntldd "$f" 2>/dev/null; done |
           awk '{print $3}' | tr '\\' '/' | grep -i "^${prefix}/bin/" | sort -u)
  [ $added = 1 ] || break
done
ls "$out" | head -50
