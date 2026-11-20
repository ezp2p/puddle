# Direct messages through a Pond: the Puddle's side (proposal, not built)

**Status:** a design for the owner to decide on; nothing is built. The design itself, how a letter is sealed, carried and
stored, and the open questions with recommendations, are in the Pond repository's `docs/dms.md`. This page is what a
Puddle and the app would do.

Two members of a Pond write to each other without being friends: no stream, no address exchanged. The Pond relays and
holds the messages ("letters"), sealed end to end from one Puddle to the other with the encryption keys already in their
key chains, and signed by their authors, so the Pond can neither read nor forge them. Each conversation is shown as
**"Ann @ Lily Pad"**, so it can't be confused with a conversation with a friend.

## What the Puddle does

Like everything else to do with a Pond, the Puddle does it and the app only asks its own Puddle (`docs/ponds.md`, "Who
talks to the Pond").

- **Sending.** `POST /api/v1/ponds/{pond}/dms/{member}/send {body}`. The Puddle fetches the member's chain from the Pond
  (`/pond/v1/members/{member}/chain`) the first time, checks it is that member's (the chain's root hashes to the id), keeps
  it, signs the letter with the owner's key, seals it to the member's encryption key, and sends it to the Pond. The letter
  goes in the conversation at once, as a channel message does.
- **Receiving.** The Pond's follower already holds a request open on the Pond's events; a `dm` event (sent to us alone)
  brings a sealed letter. The follower opens it, checks the author's signature against their chain and that it was
  meant for us in this Pond, keeps it, and tells the Pond it has it (`/pond/v1/dms/received`). After a reset, or a week
  away, it asks for what waits (`GET /pond/v1/dms`).
- **Keeping them.** Each conversation is a chat of its own in the chat logs, `pdm:<pond id>:<member id>`, kept like a
  DM's: all of it, not just the newest 200 as for a Pond's channels, since only the two Puddles have it.
- **Settings and blocks.** Who may write to the owner in each Pond (`everyone`, `roles`, `nobody`), and blocking
  someone's letters there, are sent to that Pond. Blocking someone across the app (`/api/v1/users/{user}/block`) also
  blocks their letters in every Pond the owner shares with them.
- **Becoming friends.** From the conversation, "Add friend" sends a letter carrying the owner's `puddle://` link; the
  other side's "Accept" sends a stream request to it, which the owner's Puddle takes without asking again, since it made
  the offer. Only that member learns the address.
- **Leaving.** When either leaves the Pond, or is removed, the conversation stays, read-only.

Owner routes, all new: `GET /api/v1/ponds/{pond}/dms` (the conversations), `GET .../dms/{member}/messages`,
`POST .../dms/{member}/send`, `POST .../dms/{member}/read`, `POST .../dms/{member}/befriend`, `POST .../dms/block`,
`/unblock`, `POST .../dms/settings {accept}`.

## In the app

- **Message** in the menu on someone and on their card (`components/PersonMenu.qml`, `components/ProfileCard.qml`, from
  the branch `claude/profile-card`) opens the conversation: today these show only for a friend; with letters, also for any member of the Pond who takes
  them, opening "Ann @ Lily Pad".
- The conversation looks like a DM, with the Pond's icon on the person's picture and "@ Lily Pad" after their name in its
  header and in the list. It is in the one list of conversations, beside friends and groups (where exactly is an open
  question), and its notifications are the same.
- A banner at the top says what is true of it: "Only you and Ann can read these. Lily Pad carries them and sees that you
  write to each other, not what you write." With **Add friend** and **Block**.
- In the Pond's menu, **Direct messages…**: who may write to you here (everyone, operators and administrators, nobody).

## Open questions

They are listed, with recommendations, at the end of the Pond's `docs/dms.md`: relaying versus introducing, friend
offers, what a blocked sender is told, the default for who may write, whether the Pond sees who writes to whom, files,
how long the Pond holds a letter, where the conversations show in the app, and the letter format's place in the
protocol.
