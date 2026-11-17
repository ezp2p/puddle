#!/usr/bin/env bash
# Build the Puddle app and run it in its own sandbox, so it never touches your real Puddle.
#   scripts/dev.sh              run "me"
#   scripts/dev.sh alice        run a second, separate Puddle called alice (run bob too, and they can be friends:
#                               each one's link, from "copy my link", works on this machine)
#   scripts/dev.sh alice --reset  forget alice and start from first-run setup
# Run it again to pick up a new build: it quits the old copy of that name first.
set -euo pipefail
cd "$(dirname "$0")/.."
name=${1:-me}
reset=${2:-}
root="$PWD/build/dev/$name"
port=$((8600 + $(printf %s "$name" | cksum | cut -d' ' -f1) % 300))

cmake --build build --target puddle --parallel >/dev/null || cmake --build build --target puddle --parallel

# quit the old copy first (before a reset removes the file that says which it is)
if [ -f "$root/pid" ] && kill -0 "$(cat "$root/pid")" 2>/dev/null; then
  kill "$(cat "$root/pid")"; sleep 1
fi

[ "$reset" = --reset ] && rm -rf "$root"
mkdir -p "$root/.config/puddle"
if [ ! -f "$root/.config/puddle/puddle.yaml" ]; then
  printf 'address: "127.0.0.1"\nport: %s\n' "$port" > "$root/.config/puddle/puddle.yaml"
fi


bin=build/Puddle.app/Contents/MacOS/Puddle
[ -x "$bin" ] || bin=build/Puddle
HOME="$root" "$bin" >"$root/log" 2>&1 &
echo $! > "$root/pid"
echo "Puddle \"$name\" running (pid $(cat "$root/pid")), port $port. Data: $root. Log: $root/log"
