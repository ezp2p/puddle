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
  Pond. Nothing is shown where none was chosen. One YAML file each in the data folder (`docs/profiles.md`).
- **Pond**: a Discord-style community server (channels, many members). Runs as its own daemon on
  a different code base; it is not a Puddle. Puddles connect to Ponds. Not designed yet.
- **Levee**: a relay a Puddle can sit behind (`docs/levee.md`, a design draft). It runs on a server with a public
  address; the Puddle connects out to it, needs no open ports, and sends and receives everything through it. It holds
  what arrives while the Puddle is offline, and decrypts and signs nothing. Not a normal user service: a headless daemon
  with no GUI, for someone reasonably competent to run. Not built yet.
- **Stream**: a connection between two Puddles. (Older docs call this federation or
  server-to-server traffic.)

Puddles talk privately to one another (a peer-to-peer chat client); Ponds are public gathering places. In the window, the rail of servers is for Ponds, which
are not built yet.

When `api/*.yaml` or `docs/*.md` use these words, they mean the above. If a term is ambiguous
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
  finishing the layering is at the end of `docs/cpp-port.md`
- `src/service/`: `puddled`, the headless Puddle
- `src/gui/`: `Puddle`, the desktop app (Qt Quick): `app/` is the C++ behind the window, `qml/` the screens by topic, `qml/theme/Theme.qml` every colour and size; see `src/gui/README.md`
- `api/openapi.yaml`: API for phone clients and the desktop app. `api/federation.yaml`: streams.
- `docs/storage.md`, `docs/identity.md`, `docs/profiles.md`, `docs/streams.md`: design drafts the code follows
- `docs/levee.md`: the design draft for Levee (not built; open questions at its end)
- `docs/roadmap.md`: the step-by-step plan for keys and streams; work through it in order, one step at a time
- `docs/discuss-later.md`: open questions parked on purpose; don't raise them unprompted
- Build and test: `scripts/build.sh`
