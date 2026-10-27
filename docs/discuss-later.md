# To discuss later

Things parked on purpose. Not blockers. Add to this list; don't relitigate it until asked.

## 1. Stopping huge profile pictures

Someone sends (or hosts) a 100 GB profile picture. Today the only protection is a placeholder
25 MiB cap when *importing your own* images (`PUDDLE_IMAGE_MAX_BYTES` in `profile.h`). Nothing
protects the *receiving* side yet.

Questions to settle:
- Limits: bytes, pixel dimensions, and animation frames, per image kind (avatar, banner, background)?
- Who enforces: the sender (resize on import) and the receiver (refuse on fetch), or both?
- Receiving: check the size before downloading, cap a download as it streams, and drop slow ones?
- Decompression bombs: a small file that expands to a huge bitmap when decoded.
- Formats: allow animated GIF / WebP? SVG is excluded for now.
- Resizing on import as a convenience, and which library does it (Qt in the app, or something in C).
- Total storage per peer, and cleaning up images no profile uses any more.

## 2. Reloading profiles while running

Profiles are files in the data folder, read once at startup and written by Puddle when the owner changes one through
the API. A file edited by hand while Puddle runs is not noticed (and the next change made through the API overwrites
it). Reloading while running (`POST /api/v1/profiles/reload`) needs decisions on: how a reload swaps profiles safely while
requests are in flight, what happens to the old images, whether a file watcher reloads
automatically, what a mistake in the new files does (keep the old ones, and how the owner is told),
and how changes are pushed to the streams that see them.

## 3. Encrypting chats at rest, and what it does to search

Nothing is encrypted on disk today: chat logs and the key file are plain files. Once messages are
encrypted between Puddles, the stored events keep their ciphertext bodies (they are stored exactly as
signed), which protects the logs, but the key to read them sits beside them in `identity/keys`.
Questions: lean on OS disk encryption (FileVault, BitLocker, LUKS) or encrypt ourselves; protect the
key with a passphrase or the OS keychain; and search. Search inside one chat is fine, since the chat
is decrypted in memory. Searching across all chats means decrypting them all or keeping an index, and
an on-disk index is plaintext unless it is encrypted too.

## 4. Forked histories after a leaked old phrase

A leaked *old* phrase can't take over the real chain, but it does hold that generation's daily keys and roll key,
so its holder could build an alternative chain that branches from the same earlier record. Peers who already
hold the genuine chain keep it and reject the fork; someone who has seen only the fork has no way to tell. A new
contact normally gets the chain straight from the owner's Puddle, which is what protects them. Stronger answers
(witnesses, or friends comparing the chains they hold) are for later.

## 5. A public profile for the address

A profile that goes with a Puddle's address (the `puddle://` link) and is shown to someone who is about to send a friend request, so
the requester can check they are asking the right person before anything is sent. Today nothing is shown until the other side
accepts, and the request itself says nothing about who it is going to. Questions: what is public and so visible to anyone who has the
link; how a Puddle answers a stranger's look at it without breaking "nothing is public" (the link would have to prove the asker
holds it, and the answer would have to be useless to anyone else); and whether it is a profile of its own or one of the existing ones.

## 9. More for the right-click menus and the conversation list

Right-click menus (`ContextTrigger`, `ConversationMenu`, `FieldMenu` in `src/gui/`) offer only what the owner API can already do. These
would be useful and each needs something new behind it:

- **Per conversation:** mark as unread, mute (for an hour, a day, for good), pin to the top, and hide a conversation without removing the
  friend. Needs a place to keep these per Puddle (so a phone sees the same), probably next to `read.tsv`, and the notification code to honour mute.
- **Per message:** forward, react with an emoji, "message info" (who it reached and when: the delivery state is already there), copy a link to
  a message, jump to the message a reply points at, select several to copy or delete.
- **Per person:** a nickname only you see; show their safety code to compare out loud (the short code exists); and "Add friend" on a group
  member we have no friendship with, which needs a request by user id (we already hold their address from the group connection). Removing someone
  from a group is the parked question under "Carried over".
- **Search:** inside a chat (decrypted in memory, so cheap) and across chats (see #3).
- **Text boxes:** spelling suggestions in the composer, which Qt Quick does not offer on its own.
- **Menus:** native menus on macOS and Windows (the styled menu looks the same everywhere but is not the system's). Qt 6.9 added a
  built-in `ContextMenu` attached type that does the right-click, Menu key and press-and-hold wiring; we use our own so the app still builds on
  Qt 6.2 to 6.8, and could switch when the minimum Qt is raised.

## 10. Who you are across places

A Puddle shows a different profile in each place (directly to a friend, in each group, later in each Pond), but it is
the same Puddle with the same user id everywhere. Someone who is your friend and also shares a group or Pond with you
keeps both profiles and can see both: being in a place together is the consent for that place's profile, and nobody is
told when two of your profiles meet. That is by design: profiles are about how you present yourself, not about hiding
who you are. Someone who wants two identities that cannot be linked runs two Puddles today. Later: one Puddle with
several identities, each with its own user id and address (multi-address Puddles), and what that means for keys,
streams and the app.

## Carried over from earlier

- Name for the group-only connection (a scoped stream). Options floated: Brook, Rill, Tributary.
- What the app says if the owner skips writing down the recovery phrase (the format is decided).
- The exact signature scheme for requests between Puddles.
- Who may remove someone from a group (leaving is unilateral; kicking is undecided).
- Whether a block also hides the blocked person's messages in groups you share, or makes you leave.
- A name for the phone client.
- Protecting the identity key file at rest (a passphrase or the OS keychain). It is a plain file with
  mode 0600 for now.
- Whether Pond identities are the same key everywhere, or derived per Pond.
