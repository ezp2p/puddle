# Direct messages through a Pond: the Puddle's side (design, decided, not built)

**Status:** the owner has answered the open questions; nothing is built. How a letter is sealed and relayed, and the
decisions, are in the Pond repository's `docs/dms.md`. This page is what a Puddle and the app do.

Two members of a Pond write to each other without being friends: no stream, no address exchanged. The Pond relays the
messages ("letters"), sealed end to end from one Puddle to the other with the encryption keys already in their key
chains, and signed by their authors, so the Pond can neither read nor forge them. **The Pond stores nothing**: while
the other side is away, the sender's Puddle holds the letter and sends it again when they are back. Each conversation is
in the direct messages, shown as **"Ann @ Lily Pad"**, so it can't be confused with a conversation with a friend.

## What the Puddle does

Like everything else to do with a Pond, the Puddle does it and the app only asks its own Puddle (`docs/ponds.md`, "Who
talks to the Pond").

- **Sending.** `POST /api/v1/ponds/{pond}/dms/{member}/send {body, files?}`. The Puddle fetches the member's chain from
  the Pond (`/pond/v1/members/{member}/chain`) the first time, checks it is that member's (the chain's root hashes to the
  id), keeps it, signs the letter with the owner's key, seals it to the member's encryption key, and puts it in an
  outbox, as a DM to a friend goes in the outbox today. The letter shows in the conversation at once, as *sending*.
- **Waiting.** If the Pond says the member's Puddle isn't there (`relayed: false`), the letter waits in the outbox. The
  Pond's `dm_back` event, or a retry every 15 minutes at first and less often later, sends it again. It is *delivered*
  when the other Puddle's receipt comes back; after 30 days without one it says it wasn't delivered.
- **Files.** Each file is encrypted with its own key, named in the letter, and streamed through the Pond once the letter
  has arrived (`/pond/v1/dms/files/...`); the Pond passes the bytes on and keeps none. The file stays in the sender's
  Puddle until it has been fetched, as the outbox keeps a letter. The receiving Puddle keeps the files as it keeps a
  DM's files (`docs/files.md`).
- **Receiving.** The Pond's follower already holds a request open on the Pond's events; a `dm` event (sent to us alone,
  in memory) brings a sealed letter. The follower opens it, checks the author's signature against their chain and that
  it was meant for us in this Pond, drops a repeat, keeps it, and sends back a `delivered` receipt, also a sealed letter.
- **Keeping them.** Each conversation is a chat of its own in the chat logs, `pdm:<pond id>:<member id>`, kept like a
  DM's: all of it, not just the newest 200 as for a Pond's channels, since only the two Puddles have it.
- **Taking DMs in a Pond.** On by default; the owner can turn them off per Pond, which the Puddle tells the Pond
  (`/pond/v1/dms/settings`). The Pond's admin can turn them off for everyone there.
- **Leaving.** When either leaves the Pond, or is removed, the conversation stays, read-only, and anything waiting to go
  to them is dropped.

## Blocks

Blocks stay in the Puddle, silent, as they are today (`docs/streams.md`): the Pond never hears of them, and someone
blocked is never told. There are two:

- **Block direct messages** (from the conversation, the card, the menu on someone): their letters, in every Pond, are
  dropped unread. Their messages in channels still show. Kept in `state/dm_blocks.tsv`, a user id and a time per line.
- **Block** (the Puddle's block, `/api/v1/users/{user}/block`, `state/blocks.tsv`): as today, no stream with them, and
  now also their letters dropped **and their messages in every Pond hidden**: not shown in channels, in threads or in
  replies (a reply to them says "Blocked message"), and their voice not played. Members lists still list them, marked
  blocked, so an owner can unblock them.

Either way the Puddle still answers a blocked person's letter with `delivered`, so to them it looks delivered, as a
blocked friend's DM does today. Hiding is the app's: the Puddle keeps a Pond's messages as they came and marks the
author blocked, so unblocking shows them again.

## Becoming friends

From the conversation, **Add friend** sends a letter carrying the owner's `puddle://` link; the other side's **Accept**
sends a stream request to it, which the owner's Puddle takes without asking again, since it made the offer. Only that
member learns the address.

## Owner routes, all new

`GET /api/v1/ponds/{pond}/dms` (the conversations), `GET .../dms/{member}/messages`, `POST .../dms/{member}/send`,
`POST .../dms/{member}/read`, `POST .../dms/{member}/befriend`, `POST .../dms/settings {accept}`,
`GET /api/v1/dm-blocks`, `POST /api/v1/users/{user}/dm-block`, `POST /api/v1/users/{user}/dm-unblock`.
`GET /api/v1/conversations` lists these beside the others, with the Pond's id and name.

## In the app

- **Message** in the menu on someone and on their card (`components/PersonMenu.qml`, `components/ProfileCard.qml`)
  opens the conversation: for a friend, the DM with them as now (a friend is always written to over the stream); for
  another member who takes DMs, "Ann @ Lily Pad".
- The conversation is in **Direct messages** in the sidebar, beside friends and groups, with the Pond's icon on the
  person's picture and "@ Lily Pad" after their name, in the list and in its header. Its notifications are a DM's.
- A banner at the top says what is true of it: "Only you and Ann can read these. Lily Pad passes them on and sees that
  you write to each other, not what you write. It keeps none of it." With **Add friend**, **Block messages** and **Block**.
- A message waiting for the other side says so ("Ann's Puddle is away; it will be sent when it's back"), and a file
  says it is waiting to be fetched.
- In the Pond's menu, **Direct messages…**: take direct messages here, on or off.
- Settings, **Blocked**: both lists, each with Unblock.

## Still open

- Where the letter format goes in the protocol (part 05 or a part of its own); see the Pond's `docs/dms.md`, decision 9.
