# Ponds, from the Puddle's side

A Pond is a community server with text channels, like a Discord server. It is its own program, in the pond repository,
whose `docs/design.md` and `api/pond.yaml` describe the Pond itself. This page covers what a Puddle does to be a member
of one, and what the owner's app sees of it.

## Who talks to the Pond

The **Puddle** does, never the app. The Puddle holds the owner's key, so it is the one that signs requests to the Pond,
exactly as it signs requests to other Puddles. The desktop app and a phone client use the Puddle's owner API
(`/api/v1/ponds/...`) as they do for everything else. A Puddle behind NAT or a Levee works the same, because the
Puddle always opens the connection.

A Pond is not a stream. It does not take part in the stream flow (asking, `access/result` callbacks, the outbox). The
Puddle keeps a record of each Pond it has joined, and calls the Pond directly.

## Joining

The owner pastes a `pond://host[:port]?invite=<token>` link and chooses a profile for the place `pond:<pond id>`.
`POST /api/v1/ponds/join` takes `{link, profile}`. The Puddle sends `POST /pond/v1/join` to that address, signed for
`*` because it does not know the Pond's id yet. The body carries the invite, the Puddle's key chain, and the profile.

On success the Pond answers with its id, name, channels and the member as it sees them. The Puddle records:

- the Pond and the certificate fingerprint it showed (first contact trusts whoever answers, as with a Puddle);
- the profile choice for `pond:<id>`.

Then it starts following the Pond. An invite the Pond did not accept is `409 invite_refused`; the Pond does not say why.

A `puddle://` address and a `pond://` link are never mixed up. Each parser refuses the other kind with a message
saying which it is.

## Following

Each joined Pond has a thread (`federation/ponds.cpp`) that loops:

1. On start, and after a reset, it fetches `/pond/v1/info` and `/pond/v1/members`, then the newest 50 messages of
   each channel it holds nothing of yet.
2. It holds `GET /pond/v1/events?after=<cursor>` open. The Pond answers the moment anything happens, or empty after
   25 seconds.
3. It applies the events (messages, channels made, changed and deleted, members joining, leaving or changing their
   profile, the Pond renamed) and moves the cursor. A new channel's newest messages are fetched next.

If the Pond can't be reached, the status is `unreachable` and the thread backs off from 2 seconds to 60. A `404` that
is not about a channel means the Pond no longer knows us (we were removed). The status becomes `removed`, following
stops, and what was kept stays readable until the owner leaves.

If the Pond's certificate changed, the status is `certificate_changed` and nothing is sent. Stopping the Puddle cuts
any request that is waiting (`puddle_net_opts.on_socket` and `puddle_sock_cut`), so shutting down never waits on a
Pond.

What the owner changes is carried by the same thread, which is woken to say it:

- a different profile for the Pond (`POST /pond/v1/profile`, without pictures, which a Pond has no way to fetch);
- new keys after a roll (`POST /pond/v1/keys`).

## What is kept

`core/ponds.cpp`, in `<data_dir>/ponds/<pond id>/`:

- `pond.json`: the Pond as it last told us, our cursor, its address and certificate.
- `members.jsonl`: one member per line, with the profile they chose for the Pond.
- `<channel id>.jsonl`: the newest messages of a channel, at most 200.

Everything is cleaned before it is kept: text as text from strangers, channel names to their rules, and profiles with
`puddle_profile_wire_clean`. A Pond that sends something malformed changes nothing. Older history is not kept; it is
asked of the Pond when the owner scrolls back to it. A page that joins onto what is kept is added to it, up to 200.

Leaving deletes the folder.

## The owner API

| Route | What it does |
|---|---|
| `GET /api/v1/ponds` | `{version, items}`: each Pond with its status and channels. `version` changes whenever anything kept does, so an app polls it cheaply. |
| `POST /api/v1/ponds/join` | `{link, profile}` → the Pond (`201`) |
| `GET /api/v1/ponds/{pond}` | The Pond, with `me` and its `members` |
| `GET /api/v1/ponds/{pond}/channels/{channel}/messages?before=&limit=` | Oldest first, `{items, more}`. From what is kept when it can answer, else from the Pond. |
| `POST /api/v1/ponds/{pond}/channels/{channel}/messages/send` | `{body}`. Sent with a nonce, so a retry never posts twice. |
| `POST /api/v1/ponds/{pond}/profile` | `{profile}`: id or null |
| `POST /api/v1/ponds/{pond}/leave` | Tells the Pond if it can be reached, and forgets it either way |

Errors: `no_pond`, `no_channel` and `no_profile` are `404`. `bad_text` is `400`. `slow_down` is `429`.
`invite_refused` and `removed` are `409`. `unreachable`, `not_a_pond`, `certificate_changed` and `failed` are `502`.

## In the app

The rail at the left of the window shows a round icon for each Pond (its initials), then a **+** to join one. Opening a
Pond replaces the conversation list with its channels. The name at the top has a menu to choose how you appear there,
or to leave. The page is the open channel. The right-hand panel lists the members. A Pond that can't be reached or has
removed us has a mark on its icon and a banner over its channel.

## Not yet

- Unread counts and notifications for Pond messages.
- Attachments, and the cache of recent images that will come with them.
- Mentions, replies, edits and reactions.
- A Pond changing its certificate: today the member must leave and join again. See the Pond's design, "Not yet".
- Whether a Pond sees the same key as everyone else, or one derived for it (`discuss-later.md`).
