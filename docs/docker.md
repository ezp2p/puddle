# Running a Puddle in Docker

`puddled`, the headless Puddle, is published as a container image for amd64 and arm64:

```
ghcr.io/ezp2p/puddle:latest          the newest release
ghcr.io/ezp2p/puddle:0.1.0           one release
ghcr.io/ezp2p/puddle:0.1             the newest 0.1.x
ghcr.io/ezp2p/puddle:dev             the newest build of the dev branch, replaced on every push to it: for trying
ghcr.io/ezp2p/puddle:dev-<commit>    the build of that commit of dev
```

Prereleases (tags with a `-`, such as `0.2.0-rc.1`) get only their own tag, never `latest`. A pull request builds the
image for both and publishes nothing (`.github/workflows/docker.yml`).

## Quick start

`compose.yaml` and `.env.example` in the repository are all it takes, without the rest of the checkout:

```sh
mkdir puddle && cd puddle
curl -fsSLO https://raw.githubusercontent.com/ezp2p/puddle/main/compose.yaml
curl -fsSL https://raw.githubusercontent.com/ezp2p/puddle/main/.env.example -o .env
$EDITOR .env                                  # PUDDLE_ADVERTISE at least; PUDDLE_IMAGE=…:dev for the newest build
docker compose up -d
docker exec -it puddle puddled --init         # once: your identity and recovery phrase; then it starts
```

Then connect your phone (step 4 below). The steps below say the same with `docker run`, and why.

## From scratch

What you need: a machine that is always on with Docker, and an address other people's Puddles can reach it at (a
domain name or a fixed public IP, with the port forwarded to that machine if it is behind a router).

**1. Start it**, with the address from above:

```sh
docker run -d --name puddle --restart unless-stopped \
  -p 8443:8443 -p 8443:8443/udp -v puddle:/data \
  -e PUDDLE_ADVERTISE=home.example.org:8443 \
  ghcr.io/ezp2p/puddle
```

`PUDDLE_ADVERTISE` is the address other people's Puddles reach you at (your link), so it is the address and port on
the outside of your router or host, not the container's. On a fresh volume the Puddle has no identity yet, so it
waits (`docker logs puddle` says so) and serves nothing.

**2. Set it up**, once, from a terminal:

```sh
docker exec -it puddle puddled --init
```

This makes the identity and shows the recovery phrase in that terminal only, never in the logs. Write it down: it is
the only way to get your identity back if the volume is lost. Press Enter, and the Puddle starts by itself. From then on
it starts straight away, restarts and updates included.

**3. Set your name.** Nothing is shown about you until you choose a profile ([profiles.md](profiles.md)). Start the
container with `-e PUDDLE_NAME="Your name"` to have a first one, `main`, made for you, or add one with the owner API
(`POST /api/v1/profiles`) or from the phone app later. A profile is one file each in `profiles/` in the volume, beside `puddle.yaml`.

**4. Connect your phone, or the desktop app.** A Puddle in Docker has no window, so you use it from the phone app, or from the
desktop app: on its first run, choose **Connect to my Puddle**. Then, on the server:

```sh
docker exec -it puddle puddled --pair
```

It shows a pairing link (it works once, for ten minutes): paste it into the app, name the device, and note the six digits the
app shows. `puddled --pair` waits for it, shows the same six digits, and asks you to accept it; say yes only if they match.
(Without a terminal, `--pair` only prints the link; accept with `docker exec puddle puddled --accept <digits>`.) With Kubernetes,
the same with `kubectl exec -it deploy/<name> -- puddled --pair`.

The link carries the address in `advertise`: set `PUDDLE_ADVERTISE` (or `advertise:` in `puddle.yaml`) to where the app
reaches the Puddle, not the container's own address.

**5. Make friends.** Give people your link (`home.example.org:8443`) and send them requests from the phone.

## Testing with two Puddles

`compose.test.yaml` in the repository is for testing only: two Puddles, Alice and Bob, built from the checkout, on a
network of their own, so streams can be tried without two computers. Their links are `alice:8443` and `bob:8443`;
from the host they are on `127.0.0.1:18443` and `127.0.0.1:28443`.

```sh
docker compose -f compose.test.yaml up -d --build
docker compose -f compose.test.yaml exec alice puddled --init
docker compose -f compose.test.yaml exec bob puddled --init
```

Then, for example, Alice asks Bob and he accepts:

```sh
TA=$(docker compose -f compose.test.yaml exec -T alice cat /data/data/state/owner-token)
TB=$(docker compose -f compose.test.yaml exec -T bob cat /data/data/state/owner-token)
curl -sk -H "Authorization: Bearer $TA" -d '{"target":"bob:8443","message":"hi","profile":"main"}' https://127.0.0.1:18443/api/v1/streams/request
curl -sk -H "Authorization: Bearer $TB" https://127.0.0.1:28443/api/v1/streams            # Alice's id is in "id"
curl -sk -H "Authorization: Bearer $TB" -d '{"profile":"main"}' https://127.0.0.1:28443/api/v1/streams/<Alice'"'"'s id>/accept
```

To connect the desktop app to one of them (Connect to my Puddle), change the address in the pairing link to the host's
(`puddle-pair://127.0.0.1:18443?id=...`): the phone goes by the Puddle's id and keys, not its address.

`docker compose -f compose.test.yaml down -v` deletes both, keys and all.

## What is in /data

Everything the Puddle keeps lives in the `/data` volume:

| Path              | What                                                            |
|-------------------|-----------------------------------------------------------------|
| `puddle.yaml`     | the configuration                                               |
| `profiles/`       | your profiles, one file each ([profiles.md](profiles.md))       |
| `cert.pem`, `key.pem` | the TLS certificate (it only encrypts; your keys prove who you are) |
| `data/identity/`  | your keys                                                       |
| `data/chats/`, `data/state/` | conversations, friends, phones, the owner token      |

Back it up: `docker exec puddle puddled --backup /data/backup.puddle` writes all of it to one file that only your recovery
phrase opens, and `puddled --restore --from FILE` brings it back ([backup.md](backup.md)). The recovery phrase alone gets your
identity back, but not your chats.

**Moving a Puddle here from the desktop app:** start this container without `--init`, then in the app use Settings, This computer,
**Move to a server…**, and restore the file it writes with `docker exec -it puddle puddled --restore --from /data/<file>`
([backup.md](backup.md), "Moving to a server").

On its first start the container writes `puddle.yaml` (and, with `PUDDLE_NAME`, a first profile), listening on every interface on port
8443. After that they are yours to edit; restart the container to pick up changes. These variables are read only when
the files are made:

| Variable           | Default  | Meaning                                                   |
|--------------------|----------|-----------------------------------------------------------|
| `PUDDLE_ADVERTISE` | (none)   | your address, `host:port`; also fills it in later if `puddle.yaml` still has only the placeholder |
| `PUDDLE_PORT`      | `8443`   | the port inside the container                             |
| `PUDDLE_NAME`      | (none)   | if set, a first profile, `main`, with this display name   |

The container runs as user 10001. To use a folder on the host instead of a volume, make it owned by that user
(`sudo chown 10001:10001 /srv/puddle`) and use `-v /srv/puddle:/data`.

## Everyday

- **Logs:** `docker logs -f puddle`
- **Owner token**, for the API (`api/openapi.yaml`): `docker exec puddle cat /data/data/state/owner-token`
- **Change your keys:** stop the container, then `docker run -it --rm -v puddle:/data ghcr.io/ezp2p/puddle --roll`
- **Update:** `docker pull ghcr.io/ezp2p/puddle`, then remove the container and start it again with the same
  command. The volume carries everything over.

## Building it yourself

```sh
docker build -t puddle .
```

The image is published by `.github/workflows/docker.yml` when a release tag (`0.1.0`) is pushed.
