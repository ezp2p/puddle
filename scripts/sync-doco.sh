#!/usr/bin/env bash
# Copy what doco publishes from this repository into the doco repository beside it (../doco, or the folder given):
# the API specs (api/) and the design pages (docs/) other people need to talk to a Puddle. Run it whenever either
# changes, and commit the result in doco too. Files removed here are removed there.
#   scripts/sync-doco.sh            into ../doco/puddle
#   scripts/sync-doco.sh <doco>     into <doco>/puddle
set -euo pipefail
cd "$(dirname "$0")/.."
doco=${1:-../doco}
[ -d "$doco/.git" ] || { echo "no doco repository at $doco" >&2; exit 1; }
out="$doco/puddle"

# the designs that describe what a Puddle does on the wire or for its owner; working notes (roadmap, port logs,
# questions parked for later) stay here
pages=(identity streams groups profiles files storage objects levee)

rm -rf "$out/api" "$out/docs"
mkdir -p "$out/api" "$out/docs"
cp -R api/. "$out/api/"
for p in "${pages[@]}"; do cp "docs/$p.md" "$out/docs/"; done
echo "copied api/ and ${#pages[@]} pages into $out ($(git rev-parse --short HEAD))"
