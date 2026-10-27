# Objects

Draft, now largely built (see docs/cpp-port.md). How the owner API (`api/openapi.yaml`) is reshaped around a small set of objects, each with one
definition, one place in the C core that builds it, and one class in the client that wraps it.

The model is Discord's: the API documents a handful of objects (User, Channel, Message, Guild, Member), every
route returns one of them or a list of them, and client libraries turn each into a class with properties and
methods (`message.edit()`, `user.send()`). The wire carries data only; the methods live in the client library.

## Why

The same person shows up five different ways in the owner API today:

| Where                 | What you get                                  |
|-----------------------|-----------------------------------------------|
| `Stream.peer`         | `Person`: `id` + `profile`                     |
| `GroupMember`         | `id` only ("names come from `stream`")        |
| `Message.sender`      | a bare `UserId`                               |
| `Conversation.name`   | a string, already resolved                    |
| `Block.user`          | a bare `UserId`                               |

So every client stitches people back together itself. `Chats` in the desktop app keeps `names_`, `avatars_`
and `colors_` hashes, refreshes streams to fill them, and looks names up by id when it draws a message or a
member. Each phone client would have to repeat that.

On the server, each handler file writes its own JSON (`profile_json` in `routes.c`, others in `messages.c`,
`groupapi.c`, `owner_streams.c`), so the same thing can drift between routes. The `Error` schema has already
drifted: the spec says `error` is a message and `code` the stable code, while `puddle_fail` writes the code
into `error`.

## Where the objects live

Outside the routes. An object is the shared vocabulary; a route is one way to get it. Events are another way,
and the client cache a third. All three need the same `User`, so it can't belong to any one of them.

```
api/objects.yaml          every object, defined once (data only)
   │  $ref
   ├── api/openapi.yaml   owner routes: inputs, and which object each returns
   └── api/federation.yaml  stream routes reuse the shared primitives (UserId, Profile, Color, MediaRef)

src/core/objects/         C: build each object from the store/streams/groups, write it as JSON
   ▲
src/api/api/             C: route handlers by resource; parse input, act, return an object

src/client/               C++/Qt: one class per object, with properties and methods, cached by id
   ▲
src/gui/                  the window binds to client objects
```

Three layers, each depending only on the one below:

1. **Wire objects** (`api/objects.yaml`). JSON shapes and nothing else. `openapi.yaml` and `federation.yaml`
   already define `UserId`, `EventId`, `Hex`, `Color`, `MediaRef` and `Profile` twice; they move here and both
   specs `$ref` them.
2. **Core objects** (`src/core/objects/`). For each object, a C struct, a function that fills it from what the
   Puddle holds, and a writer. Handlers never write an object's fields themselves.
3. **Client objects** (`src/client/`). A library the desktop app links, holding one live instance per id. The
   methods are here.

The federation API keeps its own messages (signed events, access requests, chains). Those are protocol and are
not owner-facing objects. It only shares the primitives.

## The objects

Fields keep their current names where they already exist. "Partial" means a short form that is embedded in
other objects; the full form comes from the object's own route.

### User

A person as this Puddle knows them: anyone we have a stream with, are asking, are in a group with, or blocked.

```yaml
User:
  required: [id]
  properties:
    id:       UserId
    profile:  Profile        # what they show us; absent until they have
    stream:   StreamState    # absent when there is no stream (e.g. only blocked, or a group member we never met)
    blocked:  boolean        # present, true, when blocked

PartialUser:                 # embedded in Message, Member, Conversation
  required: [id]
  properties:
    id:           UserId
    display_name: string
    avatar:       MediaRef
    color:        Color
```

`GET /api/v1/users/{id}` returns a `User`. `GET /api/v1/users/me` returns a `SelfUser`: the same fields, with
`profile` as your default `OwnProfile`, plus `link`. (A user id is 52 base32 characters, so `me` can't collide.)
This replaces `/api/v1/me`.

### Stream

Keeps its name and most fields; `peer: Person` becomes `user: User`. `User.stream` repeats the state, so most
screens never need the `Stream` object. It is for the details: address, certificate change, the note, which of
your profiles they see.

### Conversation

One object for DMs and groups, told apart by `kind` (like a Discord channel's `type`).

```yaml
Conversation:
  required: [id, kind, name, unread]
  properties:
    id:           string       # the peer's UserId for a DM, the ChatId for a group
    kind:         [dm, group]
    name:         string
    unread:       integer
    last_message: Message
    recipient:    PartialUser  # dm only
    group:        Group        # group only
```

### Message

```yaml
Message:
  required: [id, conversation, type, author, ts, outgoing]
  properties:
    id:           EventId
    conversation: string        # a Conversation id (replaces `chat`)
    type:         [default, group_create, group_rename, group_invite, group_join, group_leave]
    author:       PartialUser   # replaces `sender`
    ts, body, outgoing, received_at, edited, edited_at, deleted, reply_to, delivery   # as today
    member:       PartialUser   # group_invite: who was invited
    name:         string        # group_create, group_rename
```

`GroupNotice` becomes a `Message` with a non-`default` type, so a conversation's history is one list in one
order. Today `Chats` keeps notices in a separate hash and merges them.

### Group and Member

```yaml
Group:
  required: [id, name, my, creator, members]
  properties:
    id, name, my, created_at, updated_at   # as today
    creator:    PartialUser
    invited_by: PartialUser
    members:    [Member]

Member:
  required: [user, state]
  properties:
    user:       PartialUser
    state:      [invited, member]
    invited_by: PartialUser
```

### The rest

`Profile`, `OwnProfile`, `Identity`, `Invite` and `AccessPolicy` stay as they are. `Block` becomes
`{ user: PartialUser, created_at }`. `Error` is fixed to match what is sent: `{ error: <code>, message? }`.

### Events

`/api/v1/events` sends each object as it changes: `{ type, data }`, where `data` is always one of the objects
above. `message.new`, `message.edited` and `message.deleted` carry a `Message`, `user.updated` a `User`
(replacing `profile.updated` and `peer.key_changed`), `stream.*` a `Stream`, `group.*` a `Group`, and
`conversation.updated` a `Conversation` (unread counts). Because events carry the same objects as the routes,
the client applies them to its cache with the same code.

## Routes

The conventions don't change: GET reads, POST acts, the verb goes in the path. What changes is that paths
are grouped by object, with the id in the path, so each route reads as "this object, this action".

| Today                                    | Becomes                                                     | Returns          |
|------------------------------------------|-------------------------------------------------------------|------------------|
| `GET me`                                 | `GET users/me`                                              | `SelfUser`       |
| (none)                                   | `GET users/{id}`                                            | `User`           |
| `GET stream`                             | `GET streams`                                               | `[Stream]`       |
| `POST stream/accept` `{stream}`          | `POST streams/{id}/accept`                                  | `Stream`         |
| `POST stream/decline`, `delete`, `trust`, `profile` | `POST streams/{id}/decline` (and `/delete`, `/trust`, `/profile`) | `Stream`  |
| `POST user/block` `{user}`               | `POST users/{id}/block`                                     | `User`           |
| `POST user/unblock`                      | `POST users/{id}/unblock`                                   | `User`           |
| `GET conversations`                      | `GET conversations`                                         | `[Conversation]` |
| (none)                                   | `GET conversations/{id}`                                    | `Conversation`   |
| `GET dm/messages`, `GET gm/messages`     | `GET conversations/{id}/messages`                           | `[Message]`      |
| `POST dm/send`, `POST gm/send`           | `POST conversations/{id}/messages/send`                     | `Message`        |
| `POST dm/edit`, `POST gm/edit`           | `POST conversations/{id}/messages/{mid}/edit`               | `Message`        |
| `POST dm/delete`, `POST gm/delete`       | `POST conversations/{id}/messages/{mid}/delete`             | `Message`        |
| `POST dm/retry`, `POST gm/retry`         | `POST conversations/{id}/retry`                             | `Conversation`   |
| `POST dm/read`                           | `POST conversations/{id}/read`                              | `Conversation`   |
| `GET gm`                                 | `GET groups`                                                | `[Group]`        |
| `POST gm/create`                         | `POST groups/create`                                        | `Group`          |
| `POST gm/invite`, `accept`, `decline`, `leave`, `rename` | `POST groups/{id}/invite` (and so on)       | `Group`          |

Paths are under `/api/v1/`. DMs and groups share the conversation routes; the Puddle tells them apart by the
id. Every POST returns the object it changed, so a client never has to re-fetch after an action.

Identity, profiles, invites, media and policy keep their paths; they already return one object each.

The router compares whole paths today (`find` in `router.c`). It needs `{id}` segments: match a pattern
segment by segment, check the id's form (UserId, ChatId, EventId) before the handler runs, and hand the
values to the handler in `puddle_call`. An id of the wrong form is a `404`, like an unknown path.

## The C side

```c
/* src/core/objects/user.h */
typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    const puddle_profile *profile;   /* NULL until they have shown one */
    int stream;                      /* a puddle_stream_state, or -1 for none */
    int blocked;
} puddle_user;

int  puddle_user_get(const puddle_ctx *ctx, const char *id, puddle_user *out); /* 0, or -1 if we know nothing of them */
void puddle_user_write(puddle_jw *w, const puddle_user *u);
void puddle_user_write_partial(puddle_jw *w, const puddle_user *u);
```

There is one header per object: `user`, `stream`, `conversation`, `message`, `group`, `profile`, `identity`.
The `_write` functions are the only code that writes an object's fields. A handler becomes:

```c
static int user_block(const puddle_call *c, puddle_jw *w)
{
    if (puddle_blocks_add(c->ctx->store, c->path_id) != 0)
        return puddle_fail(w, 500, "store_failed");
    puddle_user u;
    puddle_user_get(c->ctx, c->path_id, &u);
    puddle_user_write(w, &u);
    return 200;
}
```

The handler files move to `src/api/api/` by resource (`users.c`, `streams.c`, `conversations.c`,
`groups.c`, `identity.c`, `profiles.c`, `events.c`), replacing `messages.c`, `groupapi.c`, `profiles_api.c`,
`owner_streams.c` and the owner half of `routes.c`. The domain code (`store`, `streams`, `group`, `message`,
`outbox`) doesn't change.

## The client side

`src/client/` is a Qt C++ library with no QML in it. The desktop app links it, and so could a Qt phone client.

```
Client          the connection (ApiClient), the cache, the event stream
User            id, displayName, avatar, color, streamState, blocked
                  sendMessage(text), block(), unblock(), stream(), conversation()
Stream          state, address, certificateChanged, profile
                  accept(), decline(), remove(), trust(), setProfile(id)
Conversation    kind, name, unread, lastMessage, recipient (User*), group (Group*)
                  messages() (a list model), loadOlder(), send(text, replyTo), markRead(), retry()
Message         type, author (User*), body, ts, edited, deleted, replyTo (Message*), delivery
                  edit(text), remove(), reply(text)
Group           name, my, creator, members (a list model of Member)
                  invite(User*), accept(), decline(), leave(), rename(name)
Profile         displayName, avatar, blurb, about, pronouns, color
```

What makes it pleasant to use:

- **One instance per id.** `client->user(id)` always returns the same `User*`. A `Message` holds a `User*`, not
  an id, so when a friend changes their name every message, member row and sidebar entry that shows them
  updates through its QML binding. Nothing re-fetches and nothing looks names up.
- **Properties are `Q_PROPERTY`s with change signals**, so QML binds straight to them:
  `text: message.author.displayName`.
- **Events and replies feed the same cache.** Whether a `Message` arrives as a reply to `send()`, a page of
  history or a `message.edited` event, `Client` merges it into the instance with that id.
- **Methods are asynchronous.** Each returns a small pending handle with `done` and `failed` signals. The
  object updates itself from the reply, so most callers ignore the handle.

```cpp
User *bob = client->user(bobId);
bob->sendMessage("hi");

connect(conversation, &Conversation::messageAdded, [](Message *m) {
    if (m->author()->blocked())
        return;
    m->reply("got it");
});
```

`Chats` shrinks to view state only: which conversation is open, the sidebar's order, the attentive flag. Its
`currentName`, `currentAvatar`, `nameOf`, `person()` and the name/avatar/colour hashes go away, because the
window reads them from the objects.

## Order of work

Each step leaves everything building and passing, like `roadmap.md`.

1. `api/objects.yaml` with the shared primitives; both specs `$ref` it. No wire change.
2. `src/core/objects/` with writers for the objects as they are today; existing handlers call them. No wire
   change, but one place per object from here on.
3. Router path parameters. `GET users/{id}`, `GET users/me`, `GET conversations/{id}`.
4. The new shapes: `PartialUser` embedded in `Message`, `Member`, `Conversation`, `Block`; notices folded into
   `Message`; `Error` fixed. The desktop app is the only client, so this changes in place under `/api/v1`.
5. The resource routes from the table; the old ones are removed in the same step, with the app moved over.
6. `src/client/` with the cache; `Chats` moves onto it.
7. `/api/v1/events` sending objects; the client applies them and the 2-second polling goes.

## Open questions

- **Path parameters or ids in the body.** The table puts ids in the path (`users/{id}/block`). Keeping today's
  flat paths with ids in the body would leave the router as it is, but then a route no longer names its
  object. Recommended: path parameters.
- **What the phone client is built with.** If it is Qt, it links `src/client/`. If not, it gets the same wire
  objects and needs its own small wrapper.
- **Embedding versus sideloading.** A page of 50 messages repeats the author's `PartialUser` 50 times. The
  alternative is a `users` list beside the page that messages point into by id. Embedding is simpler to handle
  and the partial form is small. Recommended: embed, and revisit if pages get heavy.
