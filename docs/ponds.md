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

## Versions

A Pond and a Puddle are upgraded apart; what ties them is the protocol, `/pond/v1` (the Pond's design, "Versions").
Within v1 things are only added, so the Puddle ignores fields and event kinds it does not know. Every `/info`, and
joining, says the Pond's `protocol` (which additions it has) and `features` (which optional parts). The Puddle keeps
both in `pond.json` and shows them on the owner's Pond, and checks them before using anything added after protocol 1,
rather than guessing from a `404`. Files are the first such part: a Pond that does not say what files it takes is sent
none.

## Files

Messages in a Pond carry files as messages between Puddles do: up to 10 each, of any kind, with an optional caption.
The Pond keeps each file once and serves it to its members (the Pond's design, "Files"). A Pond file has the same id,
chunks and hashes as a file between Puddles (`docs/files.md`, "The shape of a file"), so the Puddle keeps both in the
same file store (`storage/files`), and the app shows both with the same pieces.

**What the Pond takes** is its admin's choice. `/info` and the `pond` event say it, `pond.files.max_size` (0 if the
admin turned files off), and the Puddle keeps it in `pond.json` and shows it on the owner's Pond as `files.max_size`.

**Sending.** The owner's send takes `files` as a conversation's send does: each is uploaded to (or taken by) the
Puddle first, so it is here whole. The Puddle then, before posting the message:

1. checks each against what the Pond takes (`files_off`, `too_large`);
2. begins it on the Pond with its size and chunk hashes (`POST /pond/v1/files/{id}`), and sends each chunk the Pond
   says it is missing. A file the Pond has already, from anyone, costs nothing to send again;
3. posts the message with the files, under a nonce as always.

The owner's call answers once the message is posted. A send that fails part way leaves what was sent on the Pond (the
Pond throws away files no message carries after a day), and trying again picks up where it stopped.

**Keeping a cache.** So that moving between channels and scrolling never waits on a download, each Pond has a
*fetcher* beside its follower: a second thread, so a large file never holds up the events. Whenever events bring
messages (and after a sync, and every minute) it:

1. lists the files of every message kept (the newest 200 of each channel), newest message first;
2. records each in the file store as carried by its message, under the chat `pond:<pond id>/<channel>`, so the store
   keeps the file for as long as the message is kept here. Files whose messages are no longer kept (pushed out by
   newer ones, or their channel gone) are let go: the store throws a file away when nothing carries it any more;
3. fetches whatever is not here yet, up to the owner's `files.auto_download` each (100 MiB by default), from the Pond:
   the chunk hashes, then each chunk, checked as it arrives. A download that stops picks up where it was.

What it holds is listed in `<data_dir>/ponds/<pond id>/files.txt` (a line per file and message), so it knows what to
let go after a restart. Leaving a Pond lets go of all of it.

**Asking for one.** A file over `files.auto_download`, or one in a message older than what is kept (scrolled back to),
waits until the owner asks: `POST /api/v1/ponds/{pond}/channels/{channel}/messages/{message}/files/{file}/download`.
The fetcher fetches those first, and keeps them for a week past their message. For a message not kept, it asks the
Pond for that message to learn the file's size.

**Showing.** Every message the owner reads (from what is kept, or a page asked of the Pond) has its files written as
`File` objects, with their `state` (`available`, `downloading`, `waiting`, `too_large`) and `received`, from what is in
the store. When a file arrives the Pond's `version` changes, so the app looks again. The app fetches the bytes from
`/api/v1/files/{file}/content` into its own cache, as for any file.

**Bandwidth.** The owner's limits apply as between Puddles: nothing over `files.max_size` is fetched, and nothing over
`files.auto_download` without being asked. A busy Pond can still mean many small files a day; this is the cost of
not waiting on downloads, and why only the kept messages' files are fetched by themselves.

## Roles

A Pond has operators and administrators who run it from the app (the Pond's design, "Roles"). The Puddle's part:

- **Being offered one.** The Pond's admin offers a role with `pondctl member offer`. It arrives as a `role_offer` event
  sent to us alone (and in `/info` as `offer`, for after a reset). The Puddle keeps it in `pond.json` and shows it on
  the owner's Pond as `offer`; the app shows a card under the Pond's name with **Accept** and **Decline**. The answer
  goes to the Pond (`POST /pond/v1/roles/answer`), and the role takes effect at once. The app's "Copy my id", in the
  Pond's menu, is what the owner gives the admin to name them.
- **Knowing roles.** Every member, and `me`, carries `role` (`operator`, `administrator` or `member`), kept with the
  members and shown on the owner's Pond as `role`. The app marks operators and administrators in the member list.
- **Using one.** The owner's admin routes (below) are relayed to the Pond's `/pond/v1/admin/...` as they are, signed by
  the Puddle, and the Pond's answer is passed back. Nothing is checked or kept here beyond that: the Pond decides who may
  do what, and its events bring the results (a new channel, a member gone) as for any change. A Pond that does not know
  the route for us (we hold no role) answers `404`, which the owner is told as `409 no_role`.

In the app, the Pond's name drops down its menu, as Discord's server menu does: **Server Settings** (for an operator
or administrator), **Appear as…**, **Copy my id**, and **Leave Pond**. Server Settings opens over the window, with
sections on the left (Overview, Channels, Members, Bans) and each section's settings in groups on the right
(`shell/PondSettings.qml`).

The Pond never calls the Puddle for any of this. A Puddle always opens the connection: the offer comes on the event
stream it already follows.

## What is kept

`core/ponds.cpp`, in `<data_dir>/ponds/<pond id>/`:

- `pond.json`: the Pond as it last told us (with its protocol and features), our cursor, its address and certificate.
- `members.jsonl`: one member per line, with the profile they chose for the Pond.
- `<channel id>.jsonl`: the newest messages of a channel, at most 200, with what each says of its files.
- `files.txt`: the files the fetcher holds in the file store for this Pond's messages (see "Files"). The files
  themselves are in `<data_dir>/files/`, with every other file.

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
| `POST /api/v1/ponds/{pond}/channels/{channel}/messages/send` | `{body, files}`. Each file is sent to the Pond first; the message is sent with a nonce, so a retry never posts twice. |
| `POST /api/v1/ponds/{pond}/channels/{channel}/messages/{message}/files/{file}/download` | Fetch a file that did not come by itself (`204`) |
| `POST /api/v1/ponds/{pond}/profile` | `{profile}`: id or null |
| `POST /api/v1/ponds/{pond}/leave` | Tells the Pond if it can be reached, and forgets it either way |
| `POST /api/v1/ponds/{pond}/offer/accept`, `/offer/decline` | Answer the role the Pond offers (`404 no_offer` if none): `{role}` |
| `POST /api/v1/ponds/{pond}/admin/pond` | `{name?, description?}` |
| `POST /api/v1/ponds/{pond}/admin/channels/create` | `{name, topic?}` |
| `POST /api/v1/ponds/{pond}/admin/channels/{channel}/edit`, `/delete` | `{name?, topic?}` for edit |
| `POST /api/v1/ponds/{pond}/admin/members/{member}/remove`, `/ban` | `{reason?}` for a ban |
| `GET /api/v1/ponds/{pond}/admin/bans` | Who is banned, as the Pond says |
| `POST /api/v1/ponds/{pond}/admin/bans/{member}/unban` | |

Errors: `no_pond`, `no_channel` and `no_profile` are `404`. `bad_text` is `400`. `slow_down` is `429`.
`invite_refused`, `removed`, `no_file`, `files_off`, `too_large` and `no_role` are `409`. The admin routes pass on
what the Pond says: `204` when done, `400` with a `message` to show, `404 no_channel`, `409 outranked` or `not_yourself`. `unreachable`, `not_a_pond`, `certificate_changed` and `failed` are `502`.

## In the app

The rail at the left of the window shows a round icon for each Pond (its initials), then a **+** to join one. Opening a
Pond replaces the conversation list with its channels. The name at the top has a menu to choose how you appear there,
or to leave. The page is the open channel. The right-hand panel lists the members. A Pond that can't be reached or has
removed us has a mark on its icon and a banner over its channel.

Files look and work as in a conversation: photos and videos in a grid, other files as cards, opened in the window or
saved. The message box has the paperclip, and takes dropped and pasted files. `FileActions.qml` (saving, opening,
viewing) and `AttachmentTray.qml` (the files waiting to go) are shared by both pages; `Composer.target` sends to the
open channel instead of the open chat.

## Not yet

- Unread counts and notifications for Pond messages.
- A cap on the room a Pond's files take here. Only the 200 newest messages per channel are kept, so it is bounded,
  but by the Pond's limits, not the owner's.
- A setting for which Ponds fetch files by themselves; today every Pond does, up to `files.auto_download`.
- Thumbnails, so a channel of large photos does not fetch each whole to show it.
- Asking again, refusals and the other notes between Puddles (`docs/files.md`) do not apply to a Pond: it serves
  any member.
- Mentions, replies, edits and reactions.
- A Pond changing its certificate: today the member must leave and join again. See the Pond's design, "Not yet".
- Whether a Pond sees the same key as everyone else, or one derived for it (`discuss-later.md`).
