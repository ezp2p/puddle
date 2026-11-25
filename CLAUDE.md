# Puddle

Self-hosted chat that runs on the user's own computer. C core, Qt 6 (QML) desktop app.

## Vocabulary

Use these terms the way the owner does, in code, docs and conversation.

- **Puddle**: the app stored and running on a user's computer. It is both the thing the user
  runs and the thing other people's Puddles talk to. (Older docs say "server" or "home server"
  for this; they mean a Puddle.)
- **Phone client**: the app on a user's phone. It connects back to that user's own Puddle and
  never hosts anything. No product name yet; "phone client" is a placeholder.
- **Profile**: how the owner appears to others (display name, picture, bios, name colour, links, ...). An
  owner has several (work, friends, family) and chooses which one each place sees: a DM, a group, later a
  Pond. Nothing is shown where none was chosen. One YAML file each in `profiles/` beside `puddle.yaml` (`docs/profiles.md`).
- **Pond**: a Discord-style community server (channels, many members). Runs as its own daemon on
  a different code base (`../pond`, MariaDB); it is not a Puddle. A Puddle joins one with a `pond://` invite and follows it
  (`docs/ponds.md`); the app talks only to its own Puddle. Text channels, with files, so far.
- **Levee**: a relay a Puddle can sit behind (`ezp2p/levee`; the Puddle's side is the wiki's `puddle/design/levee`). It runs on a server with a public
  address; the Puddle connects out to it, needs no open ports, and sends and receives everything through it. It holds
  what arrives while the Puddle is offline, and decrypts and signs nothing. Not a normal user service: a headless daemon
  with no GUI, for someone reasonably competent to run. Built (Levee 1).
- **Stream**: a connection between two Puddles. (Older docs call this federation or
  server-to-server traffic.)
- **Scoped stream**: a stream that is only for groups, between two members who are not friends (`docs/groups.md`). It carries
  group events, key and address updates and profile pictures, nothing else. Also called a group-only connection.

Puddles talk privately to one another (a peer-to-peer chat client); Ponds are public gathering places. In the window, the rail of servers is for Ponds.

When `api/*.yaml` or the wiki use these words, they mean the above. If a term is ambiguous
or looks like it conflicts with this list, ask rather than guess.

## Conventions

- Config and profile files are YAML (`puddle.yaml`, `profiles.yaml`), read with libyaml.
- `src/` is in layers, and a layer includes only itself and the ones below it. `api/` and `federation/` never include
  each other, and `objects/` includes neither. The owner's news reaches other Puddles through the courier in the context
  (`core/context.h`), never by the objects calling `federation/`. `tests/layers.cmake` (the `layers` test) checks this.
- Nothing is public. Every route is authenticated (owner token, or a signed request from a peer), and
  a failed check is answered `404` like an unknown path, never `401`/`403`, so strangers learn nothing.
- The HTTP APIs use only GET and POST. GET reads and never changes state; POST does everything
  else, with the verb in the path (`dm/delete`, `user/unblock`). No PUT, PATCH, DELETE or HEAD in
  the specs.

## Where things are

- The Puddle is C++20 (C-callable headers), in layers under `src/`; every include names its folder (`"core/identity.h"`).
  Lowest first: `platform/` (everything that differs per OS: files, sockets, signals, paths; each file ends in `_posix` or `_windows`, and the Windows ones are not yet tested), `core/` (config, keys, records: streams, groups, outbox,
  profiles), `storage/` (chat logs, media), `wire/` (shapes both APIs send), `events/` (signed events), `objects/` (User,
  Stream, Conversation, Message, Group), `http/` (TLS, server, router, client), `api/` (owner routes),
  `federation/` (stream routes), `service/` (the assembled Puddle, and `puddled`'s `main`). The plan for
  finishing the layering is at the end of the wiki's `puddle/design/cpp-port`
- `src/service/`: `puddled`, the headless Puddle
- `src/gui/`: `Puddle`, the desktop app (Qt Quick): `app/` is the C++ behind the window, `qml/` the screens by topic, `qml/theme/Theme.qml` every colour and size; see `src/gui/README.md`
- `api/openapi.yaml`: API for phone clients and the desktop app. `api/federation.yaml`: streams.
- **Design docs, decisions and plans are not in this repository.** They live on the website wiki
  (https://www.ezp2p.org/wiki/), whose source is `ezp2p/website` under `src/wiki/` (a checkout beside this one is
  `../website/src/wiki/`):
  - `puddle/design/`: the design the code follows: `identity`, `streams`, `storage`, `profiles`, `objects`, `groups`,
    `files`, `calls`, `ponds`, `pond-dms`, `levee`, `hardening`, `backup`, `theming`, `protocol`, `cpp-port`, and
    `history` (the old roadmap, phase by phase). `puddle/releases.md` and `puddle/docker.md` sit beside installation.
  - `decisions/`: every question raised and the direction taken. Add to it when something is decided.
  - `protocol/`: the Puddle Protocol's parts (the code and vectors stay in `ezp2p/protocol`, the submodule
    `third_party/protocol`). The protocol is what Pond, Levee and phone clients rely on: change the wire there first,
    then here.
  - A comment or spec that names `docs/<name>.md` means the wiki page `puddle/design/<name>` (`roadmap.md` is
    `history`, `discuss-later.md` is `decisions/open-questions`), or Pond's design for `design.md`, `dms.md` and `voice.md`.
  - A change to Puddle that changes its design updates the matching wiki page in a website pull request alongside.
- **Work still to do** is an issue on the project board (https://github.com/orgs/ezp2p/projects/1). Open questions
  are issues labelled `needs decision`; don't decide them alone. A feature that also needs Pond or protocol work gets
  its parent issue where the behaviour lives, with this repository's part as a sub-issue.
- Build and test: `scripts/build.sh` (after `git submodule update --init`)
- `third_party/protocol`: the protocol repository as a submodule; its `lib/` is libpuddle-proto (ids, chains, signatures, receipts, sealing), which `base/encoding.h`, `base/hash.h`, `base/seal.h` and `http/reqsig.h` put Puddle's names on
- `.github/workflows/build.yml`: every platform built and tested; a push to `dev` replaces the `dev-build` prerelease, a tag makes a release; `scripts/package-*.sh` package them. See the wiki's `puddle/releases`
- `Dockerfile`, `dist/docker-entrypoint.sh`: the `puddled` image, published to ghcr by `.github/workflows/docker.yml` (`:dev` on a push to dev, the version on a release tag); see the wiki's `puddle/docker`. `compose.yaml` (with `.env.example`) is the quick start for running one; `compose.test.yaml` is two Puddles for testing only
