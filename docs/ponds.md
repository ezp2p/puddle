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

- the Pond, its key (`pond.key`, from protocol 3: refused unless its SHA-256 is the Pond's id) and the certificate it
  showed (first contact trusts whoever answers, as with a Puddle);
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

Who answers is proven as described in "Proving who answers" below; when it can't be, nothing is sent. Stopping the Puddle cuts
any request that is waiting (`puddle_net_opts.on_socket` and `puddle_sock_cut`), so shutting down never waits on a
Pond.

What the owner changes is carried by the same thread, which is woken to say it:

- a different profile for the Pond (`POST /pond/v1/profile`, without pictures, which a Pond has no way to fetch);
- new keys after a roll (`POST /pond/v1/keys`).

## Proving who answers

A Pond that lists `receipts` in `features` (protocol 3, the Puddle Protocol's spec/05) signs every answer to a member
with its key, over its own TLS certificate: the same `X-Puddle-Receipt` a Puddle sends (`docs/streams.md`, "Proving who
answers"). For such a Pond (`signs` in `pond.json`) every call passes the Pond's key to the client, which believes an
answer only with a good receipt, and sends nothing over a certificate the Pond hasn't proven. Shown a new one (a
renewed certificate, say), the Puddle asks `GET /pond/v1/info` over it, which changes nothing; if the receipt names that
certificate, it is kept as the Pond's and the call is made again (`federation/ponds.cpp`, `member_call`). If not, the
status is `not_proven` and the call fails with `not_proven`, and the follower tries again later.

The one answer a Pond leaves unsigned to a member's Puddle is the plain `404` for someone it doesn't know. Over a
certificate the Pond has proven, that still means it no longer has us (`removed`).

A Pond without `receipts` is pinned to the certificate it showed at join, as before: another one is
`certificate_changed`, and nothing is sent. A Pond that gains receipts tells us its key in an `/info` over the pinned
certificate; from then on it is held to its receipts, and it is never pinned again, whatever a later answer says.

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
what the Pond says: `204` when done, `400` with a `message` to show, `404 no_channel`, `409 outranked` or `not_yourself`. `unreachable`, `not_a_pond`, `certificate_changed`, `not_proven` and `failed` are `502`.

## Voice channels

A Pond's voice channel (its `docs/voice.md`) is a place to drop into and talk, as on Discord. It is not a call: nothing rings,
nothing alerts anyone, nothing is noted in a chat, and it isn't in `/api/v1/calls`. The Pond forwards: each member's Puddle
makes one WebRTC connection to it, sends its owner's voice up an Opus track, and gets every other voice back over a data
channel (`frames`), each frame marked with its sender's slot in the channel (2 bytes), its RTP timestamp (4) and the Opus
frame.

- **Who carries it.** The Puddle, beside its calls (`federation/pondvoice.cpp`): the same UDP port (`calls.port`), the same
  device at the end of it (the window, for now; a phone's leg later), and one at a time with calls. In a voice channel, a
  friend's ring is answered `busy`, and their window says "*Ann* is already in a call"; starting or answering a call is
  `busy` too, and so is joining a voice channel while in a call. The owner leaves one to start the other. Joining another
  voice channel leaves this one, keeping muted and deafened.
- **The path.** The Puddle offers, signed like every request to a Pond (`POST /pond/v1/channels/{c}/voice/join` with
  `{sdp, udp}`), and the Pond answers with its own description, its voice UDP port and our slot. The Puddle adds the Pond's
  address at that port as a candidate: the Pond is the one with a public address, so this works from behind NAT, with no
  STUN or TURN. The Pond adds the address the request came from.
- **Whose voice.** Who has which slot comes from the Pond's `voice` and `voice_left` events, and `voice` in `/info`, kept in
  memory by `core/ponds` (`puddle_ponds_voice_member`). A frame from a slot not heard of yet goes to the device as
  `slot:<n>` until the event comes.
- **Muted, deafened.** `POST /pond/v1/voice/{session}/state` tells the Pond, which tells everyone. Muted, the Puddle sends
  nothing and the Pond forwards nothing; deafened, the Pond sends nothing and the Puddle drops anything that comes.
- **Going away.** A connection that doesn't come up within 20 seconds, or drops and isn't back within 10, has ended
  (`failed`); one the Pond no longer has (`404 no_session`) has ended (`removed`). An ended one stays, with why, until the
  owner leaves it or joins again, so the window can say so. Leaving tells the Pond; so does stopping the Puddle.

The owner's routes: `POST /api/v1/ponds/{pond}/channels/{channel}/voice/join`, `GET /api/v1/voice`,
`POST /api/v1/voice/state {muted?, deafened?}` and `POST /api/v1/voice/leave`. A Pond's channels say their `kind`, and a
voice channel who is in it (`voice`).

In the window (`PondSidebar.qml`), a voice channel has a speaker instead of `#`, and who is in it underneath, ringed while
they talk. Clicking it shows its messages, as for any channel (a voice channel takes text too); the phone beside it joins
its voice, with nothing to answer, and while in it becomes a red one to leave. While in one, a panel slides up at the foot of the
left-hand list, above your own row (`VoicePanel.qml`, as Discord has it, and the same for a call: `docs/calls.md`). It has the channel and Pond (a click on the name goes to the channel), whether it is connected, and
mute, deafen, camera, screen, pop out and leave. Your row under it keeps Settings in reach. Who is talking comes from the frames arriving (the Mixer
in `callaudio.*`: anyone heard from in the last 300 ms), and this end from its own microphone. How the microphone goes is
the owner's choice in Settings (`TalkGate`): voice activation (the default: sent while louder than a threshold, held 300 ms
after so word endings aren't cut off; the Settings show how loud you are against it), or push to talk (only while a key of
the owner's choosing is held, while the window is in front). Between, nothing is sent at all, which is how the others see
who is talking. A call sends the microphone always, unless muted.

**Cameras and screens.** The connection to the Pond also carries the owner's camera (`video`) and shared screen (`screen`),
sent while on (`/voice/state {camera, screen}`, which the Pond tells everyone), and 16 tracks that only come in, `v1` to
`v16`. The Puddle watches everyone else's camera and screen by itself (`POST /pond/v1/voice/{session}/watch`): each new one
takes the first free track and keeps it until it goes, so nobody's picture moves, up to 16 at once. They reach the device
as from the member's id, or `<id>/screen`. In the window the video pane (as in calls: `docs/calls.md`, "The video pane")
sits above the voice channel's messages while you are in it and someone's camera or screen is on; full size and pop out
work the same, from the buttons on the pane or the panel.

Tested in `pondvoice_test` (a real Puddle and its window models against a stand-in Pond that speaks the voice routes with
libdatachannel: also cameras and screens both ways, and watching), `callaudio_test` (the gate, and who is talking), and the
pond repository's `e2e_test` (two real Puddles in a real Pond's voice channel).

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
- In a voice channel: a phone (roadmap 9.3), and admins disconnecting or muting someone (9.5). Push to talk works only
  while the window is in front. More than 16 cameras and screens at once are not all watched.
- A Pond changing its certificate: today the member must leave and join again. See the Pond's design, "Not yet".
- Whether a Pond sees the same key as everyone else, or one derived for it (`discuss-later.md`).
