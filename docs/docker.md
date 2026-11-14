# Running a Puddle in Docker

`puddled`, the headless Puddle, is published as a container image for amd64 (arm64 to come) with each release:

```
ghcr.io/ezp2p/puddle:latest      the newest release
ghcr.io/ezp2p/puddle:0.1.0       one release
ghcr.io/ezp2p/puddle:0.1         the newest 0.1.x
```

Prereleases (tags with a `-`, such as `0.2.0-rc.1`) get only their own tag, never `latest`.

## From scratch

What you need: a machine that is always on with Docker, and an address other people's Puddles can reach it at (a
domain name or a fixed public IP, with the port forwarded to that machine if it is behind a router).

**1. Start it**, with the address from above:

```sh
docker run -d --name puddle --restart unless-stopped \
  -p 8443:8443 -v puddle:/data \
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
(`POST /api/v1/profiles`) or from the phone app later. A profile is one file each in `data/profiles/` in the volume.

**4. Connect your phone.** A Puddle in Docker has no window, so you use it from the phone app. Ask the Puddle for a
pairing link with its owner token (it works once, for ten minutes):

```sh
TOKEN=$(docker exec puddle cat /data/data/state/owner-token)
curl -sk -X POST -H "Authorization: Bearer $TOKEN" -d '{}' https://127.0.0.1:8443/api/v1/devices/pair
```

Paste the `link` from the answer into the phone app, name the phone, and note the six digits it shows. Then let it in,
checking the same six digits are in the list:

```sh
curl -sk -H "Authorization: Bearer $TOKEN" https://127.0.0.1:8443/api/v1/devices
curl -sk -X POST -H "Authorization: Bearer $TOKEN" -d '{}' https://127.0.0.1:8443/api/v1/devices/<id from the list>/accept
```

**5. Make friends.** Give people your link (`home.example.org:8443`) and send them requests from the phone.

## Testing with two Puddles

`compose.yaml` in the repository is for testing only: two Puddles, Alice and Bob, built from the checkout, on a
network of their own, so streams can be tried without two computers. Their links are `alice:8443` and `bob:8443`;
from the host they are on `127.0.0.1:18443` and `127.0.0.1:28443`.

```sh
docker compose up -d --build
docker compose exec alice puddled --init
docker compose exec bob puddled --init
```

Then, for example, Alice asks Bob and he accepts:

```sh
TA=$(docker compose exec -T alice cat /data/data/state/owner-token)
TB=$(docker compose exec -T bob cat /data/data/state/owner-token)
curl -sk -H "Authorization: Bearer $TA" -d '{"target":"bob:8443","message":"hi","profile":"main"}' https://127.0.0.1:18443/api/v1/streams/request
curl -sk -H "Authorization: Bearer $TB" https://127.0.0.1:28443/api/v1/streams            # Alice's id is in "id"
curl -sk -H "Authorization: Bearer $TB" -d '{"profile":"main"}' https://127.0.0.1:28443/api/v1/streams/<Alice'"'"'s id>/accept
```

To pair the desktop app in phone mode with one of them, change the address in the pairing link to the host's
(`puddle-pair://127.0.0.1:18443?id=...`): the phone goes by the Puddle's id and keys, not its address.

`docker compose down -v` deletes both, keys and all.

## What is in /data

Everything the Puddle keeps lives in the `/data` volume:

| Path              | What                                                            |
|-------------------|-----------------------------------------------------------------|
| `puddle.yaml`     | the configuration                                               |
| `data/profiles/`  | your profiles, one file each ([profiles.md](profiles.md))       |
| `cert.pem`, `key.pem` | the TLS certificate (it only encrypts; your keys prove who you are) |
| `data/identity/`  | your keys                                                       |
| `data/chats/`, `data/state/` | conversations, friends, phones, the owner token      |

Back the volume up: it holds your keys. The recovery phrase gets your identity back without it, but not your chats.

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
