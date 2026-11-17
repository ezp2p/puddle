#!/bin/sh
# Start puddled in the container. On the first start, write a puddle.yaml that listens on every interface (inside a
# container, 127.0.0.1 is unreachable from outside) and keeps everything under /data. After that the files are yours to
# edit; these variables are only read when they are made (PUDDLE_ADVERTISE also fills in a placeholder left without it):
#   PUDDLE_ADVERTISE  the address other people use to reach you, "home.example.org:8443" (needed for your link)
#   PUDDLE_PORT       the port puddled listens on inside the container (default 8443)
#   PUDDLE_NAME       if set, a first profile, "main", with this display name (docs/profiles.md); none is made otherwise
# With no arguments puddled starts with --wait: on a fresh volume it waits until someone runs
#   docker exec -it <container> puddled --init
# (which shows the recovery phrase in that terminal, never in the logs), then starts by itself. Arguments replace
# --wait and go to puddled, e.g. --roll.
set -eu

dir=/data
config="$dir/puddle.yaml"

if [ ! -e "$config" ]; then
    port="${PUDDLE_PORT:-8443}"
    {
        echo "# Puddle configuration (written by the container on its first start; see docs/docker.md)"
        echo
        echo "# Every interface inside the container; publish the port with docker run -p."
        echo "address: 0.0.0.0"
        echo "port: $port"
        echo
        echo "# The address other people use to reach you: \"203.0.113.7:8443\" or \"home.example.org:8443\"."
        if [ -n "${PUDDLE_ADVERTISE:-}" ]; then
            echo "advertise: \"$PUDDLE_ADVERTISE\""
        else
            echo "# advertise: home.example.org:8443"
        fi
        echo
        echo "# Everything else (keys, chats, your profiles: one file each in data/profiles) is kept here."
        echo "data_dir: data"
    } > "$config"
elif [ -n "${PUDDLE_ADVERTISE:-}" ] && grep -q '^# advertise: home.example.org:8443$' "$config"; then
    # made on an earlier start without it, and not set by hand since
    sed -i "s|^# advertise: home.example.org:8443\$|advertise: \"$PUDDLE_ADVERTISE\"|" "$config"
fi
if ! grep -q '^advertise:' "$config" && ! grep -q '^address: *127\.' "$config"; then
    echo "docker-entrypoint: $config has no advertise address, so your link won't work; set PUDDLE_ADVERTISE or edit it" >&2
fi

if [ -n "${PUDDLE_NAME:-}" ] && [ ! -d "$dir/data/profiles" ]; then
    name=$(printf '%s' "$PUDDLE_NAME" | sed 's/\\/\\\\/g; s/"/\\"/g')
    mkdir -p "$dir/data/profiles"
    cat > "$dir/data/profiles/main.yaml" <<EOF
# A Puddle profile (docs/profiles.md). Choose who sees it in the app, or with the owner API.
display_name: "$name"
EOF
fi

if [ $# -eq 0 ]; then
    if [ ! -d "$dir/data/identity" ]; then
        echo "docker-entrypoint: this Puddle isn't set up yet. To set it up: docker exec -it <this container> puddled --init" >&2
    fi
    set -- --wait
fi
exec /usr/local/bin/puddled -c "$config" "$@"
