# Proposed layout (mock)

A sketch of the code base split into layers, in C++. Nothing here builds. The files that show how the layers fit
together have real-looking code; the rest hold one comment saying what would go there.

```
test_structure/
  model/        the design, in YAML: what everything else follows (model/README.md)
    wire/         shapes both APIs share: ids, Profile, Color, MediaRef
    objects/      what the owner API returns: User, Message, Conversation, Group, Stream, ...
    events/       what Puddles sign and send each other: message, edit, redact, chat.*, member.*
    api/          the owner API, /api/v1 (openapi.yaml)
    federation/   the stream API, /stream/v1 (federation.yaml)

  platform/     everything that differs per OS: running as a service, paths, sockets, files, locks
    posix/  macos/  linux/  windows/

  storage/      where data is kept, without saying how: logs, records, blobs, secrets, behind one interface
    files/        the default driver: plain files in the data directory (today's layout)
    memory/       for tests
  core/         the engine: chats, blocks, streams, groups, outbox, identity; each deposits its data with storage
  wire/         the shared shapes as JSON, one function each
  events/       signed events: build, sign, verify, parse, and the rules that fold them into a chat or a group
  objects/      User, Message, Conversation, Group, Stream, ... with methods; built from events plus local state

  http/         TLS, the server, the router, the client: plumbing both APIs use
  api/          owner API: routes/ call objects, json/ writes them, updates.cpp streams them
  federation/   stream API: routes take and give events; sender.cpp delivers the outbox

  service/      puddled: main() only
  client/       the library apps use: the same objects, over HTTPS, one instance per id
  app/          the desktop app (Qt): the window, and starting puddled when it isn't running

  tests/        one folder per layer
```

Each layer is one folder with each `.hpp` beside its `.cpp`. Includes name the folder: `#include "objects/user.hpp"`.

Where to start reading:
1. `model/README.md`, then `model/objects/user.yaml` and `model/events/message.yaml`. These are the same message
   seen two ways.
2. `objects/user.hpp` and `objects/user.cpp`, then `api/routes/users.cpp`: an object, and a route that uses it.
3. `events/chat.hpp` and `events/chat_view.hpp`, then `federation/events.cpp`: events, and a route that takes them.
4. `storage/storage.hpp`, then `core/blocks.hpp`: the storage interface, and one part of core depositing with it.
5. `service/main.cpp`: how it all starts.

## Who may use whom

Each layer uses only what is below it. Two things hold the line: each layer links only the targets listed in
`CMakeLists.txt`, and `tests/layers/` fails the build if a file includes a folder its layer may not use. (Linking
alone wouldn't catch a header-only include.)

```
                   ┌── api ──────── objects ──┐
service ──► http ◄─┤                          ├── events ── wire ── core ── storage ── platform
                   └── federation ────────────┘

app ──► client ──(HTTPS)──► a running puddled
 └────► platform              (to find and start puddled)
```

- **`api` and `federation` never include each other.** The owner API changes as the apps need. The stream API
  must keep working with older Puddles, so it changes rarely and only by adding.
- **`api` speaks objects; `federation` speaks events.** An object is one Puddle's view for its owner (unread,
  blocked, delivery). An event is a signed fact both sides must agree on. Each Puddle builds its own objects from
  the events it holds. Shapes that really are the same on both sides (a profile, an id) live in `wire/`, and both
  APIs use them.
- **Folding rules live in `events/`, not `objects/`.** Which edit wins and who is in a group must come out the same
  on every Puddle. `objects/` only adds local state on top.
- **Only `storage/` knows how data is kept.** `core` turns its data into bytes and deposits it ("put this
  record", "append this entry"). `objects` and everything above only talk to `core`. Changing how data is kept
  means writing a driver, and nothing else changes.
- **`objects` never sees HTTP or JSON.** A `User` can't tell whether the window or a route asked for it.
- **`core` and `objects` never `#ifdef` on the OS.** Anything that differs goes behind a `platform/` header.
- **`client` and `app` never link the server layers.** They know a Puddle only through `model/api/`.

## How a message travels

```
Alice's app ── POST /api/v1/conversations/{bob}/messages/send ──► api/routes/conversations.cpp
  objects::Conversation::send   builds an events::MessageEvent, signs it, appends it to core::Chats
                                (storage keeps it), and queues it in core::Outbox
  federation/sender.cpp         sends it to Bob's Puddle as a Delivery
Bob's Puddle ── POST /stream/v1/events ──► federation/events.cpp
  checks the signature and the rules in events/, then appends the exact bytes to core::Chats
  api/updates.cpp               sees the new event, builds an objects::Message, sends it to Bob's app
```

## Two `User`s

`puddle::User` (objects/) and `puddle::client::User` (client/) have the same shape and the same verbs. The first
reads the Puddle's own storage and acts on it. The second wraps the JSON the API returns, and its methods are
HTTPS calls. An app developer only ever sees the second.
