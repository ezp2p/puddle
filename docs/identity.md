# Identity, trust and encryption (draft)

Decisions so far: **one person per Puddle**, so one identity per server. Phones are additional
devices of that one person. No middleman servers in v1.

## Identity

The identity is made of:

- an **Ed25519** signing key
- an **X25519** encryption key
- an Ed25519 **roll key**, used only to authorize changing the keys (below)

All three are derived from one **recovery phrase**: eight groups of four characters, shown to the
owner once when the identity is made and never stored or logged (`phrase.c`). The signing and
encryption keys are the *daily keys*. They live on this machine in `<data_dir>/identity/keys`
(mode 0600), and are what a phone is given. The roll key exists only while the phrase is typed in.

**User id = `base32(SHA-256(first signing public key ‖ SHA-256(first roll public key)))`**, taken from the very
first generation. The id never changes, even when keys roll (below). It is self-certifying: given an id and
the identity's key chain, anyone can check they belong together, with no registry or server. It does not
depend on an IP address, a hostname or a TLS certificate, so it survives the user moving.

The roll key's hash is part of the id on purpose. Someone who steals only the daily keys can sign a *different*
first record (with a roll key of their own), but its id comes out different, so a chain from the daily keys alone cannot
be passed off as the same identity. With the id covering only the signing key, that forgery would have been a valid chain for the victim's id.

## Devices (phones)

**A phone is a thin client of its own Puddle** (decided). It holds none of the identity's keys and stores no chats:
it asks its Puddle, over the owner API, for what the desktop app shows, and the Puddle does all the signing,
encrypting and decrypting. Chats are kept on the Puddle, readable by it, as they already are; giving a phone the keys
would only make sense once they are stored encrypted (`discuss-later.md`).

What a phone has instead is **a key of its own**, made on the phone at pairing (Ed25519). Its id is
`base32(SHA-256("puddle-device-v1\n" ‖ public key))`, the same shape as a user id but never one. With it the
connection is proven both ways, exactly as between two Puddles (`streams.md`, "Request signatures" and "Proving who
answers"), and nothing that crosses the wire can be replayed or reused:

- **every request is signed by the device key** (`X-Puddle-Signature`, its device id where a sender id goes, the
  Puddle's user id as the recipient); there is no token to steal
- **every answer carries the Puddle's receipt**, signed by its identity key over the connection's certificate, so the
  phone knows it reached its own Puddle and nobody in between, and the Puddle's certificate may change at any time

The owner token stays for the desktop app and the owner's tools on the same machine, which read it from a file.

**Pairing:**

1. In the desktop app, "Add a device" asks the Puddle for a **pairing link** (`POST /api/v1/devices/pair`), shown as a
   QR code and as text: `puddle-pair://<address>?id=<Puddle's user id>&secret=<one-time secret>`. The secret is
   128 random bits, good for one use within ten minutes; asking again replaces it.
2. The phone makes its key and sends `POST /device/v1/pair` with a name for itself ("Alex's phone"), its public key
   and the secret, signed by that key. A wrong or spent secret is a `404` like any unknown path.
3. The answer carries the Puddle's key chain and a receipt. The phone checks the chain belongs to the id in the link
   (ids are self-certifying), then the receipt against the chain's current key: from here on it knows its Puddle by
   its keys, as a friend would. The device is now **pending**.
4. The desktop app shows "*Alex's phone* wants to use your Puddle" with a short **check code** (six digits from the
   device id), which the phone shows too, and the owner accepts or declines. The phone learns which from
   `GET /device/v1/status`, signed like everything else.

**What a device may do:** the whole owner API, as the desktop app does, except managing devices: making pairing links,
accepting, declining and revoking stay with the owner token, so a stolen phone cannot let anything else in.

**A lost phone** is revoked on its own (`POST /api/v1/devices/{device}/revoke`): its next request is a `404`. Nothing about the
identity changes, nobody else needs telling, and no key roll is needed, since the phone never held the identity's keys.

**Reaching home:** a phone reaches its Puddle at the same advertised address friends use, so away from home that needs
the same port forward or public address.

**Ponds** come along for free: the app reaches Ponds only through its own Puddle (`docs/ponds.md`), so a phone does too.

Built in `src/core/devices.cpp` (the device list, pairing, the key) with the routes in `src/api/devices.cpp`; the router
accepts a device's signature wherever the owner token is accepted. The app is a phone when built for Android, or with
`PUDDLE_PHONE=1` (or `--phone`) to try it on a desktop. Roadmap phase 7.

### The owner token

The owner API takes two credentials: a device's signature (above), or a random 64-hex **owner token** made on first
run in `<data_dir>/state/owner-token` (mode 0600), presented as `Authorization: Bearer <token>`. The
Puddle's own app and the owner's tools read it from that file. Without either, every route, including `/health`,
answers `404` as if it did not exist. (`src/core/auth.cpp`.)

## Rotation (in v1, optional, owner-triggered)

Keys can be replaced without changing the user id. **The phrase rolls with the keys:** every roll makes
a new phrase. Authority to roll comes from a **roll key**, derived from the phrase and never stored:

- Every record in the chain commits to the hash of the *next* roll key. The first record's commitment is
  to the roll key of the first phrase (`next` in `chain.jsonl`).
- To roll, the owner types the current phrase, which gives that generation's roll key. The Puddle makes a
  new phrase, derives the next daily keys and roll key from it, and writes a record that reveals the old
  roll key, names the new keys, commits to the new roll key, and increments `seq`.
- The record carries three signatures: from the roll key (authority), from the new signing key
  (possession), and from the *old* signing key (the old key announcing its successor).
- Peers keep the whole chain and verify it back to the first record, whose signing key hashes to the
  user id. A record is valid only if the revealed roll key hashes to what the previous record committed to.
- Someone holding only the daily keys (a stolen phone: devices are given daily keys, never the roll
  key) cannot roll. A leaked *old* phrase cannot roll either: its roll key was spent. Only the current
  phrase is the master secret.
- The roll is two-phase: the new phrase is shown first, and the roll commits only once the owner confirms it
  is written down. After it commits, the old phrase recovers nothing.
- Events carry the id of the key that signed them (`kid`), so history signed before a roll still verifies.

Built in `src/core/identity.cpp`; run it with `puddled --roll`, or over the owner API while the Puddle runs (`POST /api/v1/identity/roll`, then `/confirm`). If it is interrupted, the next start finishes it:
the signed record and the new keys are written together to a journal before either real file changes.

## The chain file

`identity/chain.jsonl` is what other Puddles and Ponds are given to check an identity (as `chain` in requests). It is
one record per line, each line ending in `\n`. This is the exact format; a separate implementation (the Pond's
`src/identity.cpp`) checks chains from this description.

Encodings: keys and signatures are standard base64 with padding (32 and 64 bytes); hashes are 64 lowercase hex
characters of SHA-256; the id is RFC 4648 base32, lowercase, without padding (52 characters).

The first line, the **inception**:

    {"record":{"type":"identity.inception","seq":0,"signing":"<b64>","encryption":"<b64>","next":"<hex>","ts":<ms>},"sig":"<b64>"}

- `signing` is the first Ed25519 signing key, `encryption` the X25519 key, `next` the SHA-256 of the first roll key.
- `sig` is by `signing`, over the exact bytes of the `record` value as they appear in the line, from its `{` to its
  `}`. Signatures always cover those bytes, never a re-serialisation, so a checker must take the record's text from
  the line rather than rebuild it.
- The id is `base32(SHA-256(signing key || the 32 bytes that next encodes))`.

Each later line, a **rotation**:

    {"record":{"type":"identity.rotate","seq":N,"prev":"<hex>","signing":"<b64>","encryption":"<b64>","roll":"<b64>","next":"<hex>","ts":<ms>},"sig":"<b64>","sig_roll":"<b64>","sig_prev":"<b64>"}

- `seq` is one more than the record before, and `prev` is the SHA-256 of that record's bytes.
- `roll` reveals the roll key whose SHA-256 the record before gave as `next`; `next` commits to the following one.
- `sig` is by the new `signing` key, `sig_roll` by `roll`, and `sig_prev` by the signing key of the record before,
  all over this record's bytes.

A chain is valid when every line checks. The current keys are those of the last record, and the key id a request
names (`kid`) is the hex SHA-256 of the current signing key. Puddle writes the fields in the order shown, but a checker
should find them by name. Blank lines are skipped. A chain is at most 4 MiB and 10,000 records.

## Restoring on a new computer

Built in `src/core/identity.cpp` (`puddle_identity_restore`) and run with `puddled --restore --chain FILE`.
It needs two things:

- the owner's **latest** recovery phrase, the one written down at the most recent roll (or at the start, if
  they never rolled). It re-derives that generation's daily keys and roll key.
- a copy of the **chain**. It is not secret: it is `identity/chain.jsonl`, and it is worth keeping a copy
  somewhere. If none exists, a friend's Puddle holds one (step 3.6 will fetch it).

Everything is checked before anything is made: the chain must be valid, and the phrase's keys must be exactly the
ones its newest record announces and commits to. An **older** phrase is refused and recognised as such (its keys
were retired by a later roll). A chain that is out of date, or belongs to someone else, is refused too. The
restored files are byte-for-byte what they were, and no new phrase is made. If the old computer might be in
someone else's hands, roll the keys straight afterwards.

## Transport

TLS stays self-signed and only provides confidentiality on the wire. Puddles authenticate each other
by signatures, not by certificate, in both directions: requests are signed by the sender, and answers carry a
receipt signed by the responder over the certificate it used for that connection, so nothing is pinned and a
certificate may change at any time (`streams.md`, "Proving who answers").

## Signing

Every event carries `sender` (a user id), `kid` and a `sig` over the exact event bytes. To verify, a
server finds that key in the sender's chain, then checks the signature. Unsigned or badly signed
events are dropped.

## Becoming contacts (the handshake)

1. A gets B's **address** (`home.example.org:8443`; the port defaults to 443), pasted or in a link. No id is
   part of it: the id is something the answer tells A (streams.md, "Asking for a stream").
2. A sends B a signed **contact request** carrying A's id, key chain, address, display name and a random ticket.
3. B's user accepts. B replies signed, with B's key chain, id and A's ticket.
4. Each stores the other's chain and address. From here the id is a label for a chain, never typed by anyone;
   what proves a message is from them is the key in that chain. First contact trusts whoever answers at the
   address (an owner's choice, recorded in the roadmap); friend codes and a handshake come later.

## Address changes

A user's address (IP) can change. A server publishes a signed **address record**:
`{id, addresses[], seq, ts, sig}`. The highest `seq` wins.

- On a change, push the new record to every contact at their last known address.
- Contacts also hold each other's records and can hand them on when asked (signed, so they can't be
  forged). That helps if the user was unreachable when the change was pushed.
- Servers try every address they have for a contact, newest first.

**Known limitation (accepted for v1):** if two friends both move while offline, neither can find the
other, and they have to exchange invites again. There are no middleman servers in v1. A rendezvous
or relay role can come later with self-hosted servers.

## Encryption (decided for now: one key per Puddle)

Each Puddle has a single encryption key pair (the X25519 key above). DMs and groups all use it:
a message body is encrypted to each recipient Puddle's public key, and each Puddle decrypts with its
one private key. There are no per-chat or per-group keys, and no key rotation on membership changes.

This is deliberately simple and a little weaker. A member who wants to leak a conversation can
always forward what they can read, so per-chat keys would not stop that; they would only limit the
damage if a single key were stolen. Per-chat keys, key rotation and forward secrecy are possible
later without changing the API, which never exposes keys.

- Only `body` is encrypted. Envelope fields (`type`, `chat`, `sender`, `ts`, `ref`) stay readable so
  Puddles can route, bucket and dedupe.
- Use a standard construction rather than a custom one: HPKE (RFC 9180, needs OpenSSL 3.2+, our
  minimum is 3.0) or libsodium sealed boxes. This is a decision on dependencies, not on design.
- For a first demo, bodies may travel unencrypted over TLS until this is built.

## Threat model

Group membership is protected by rules every Puddle applies itself (docs/groups.md): only a member can invite, a join counts only from someone
invited, and a connection for a group is made only for someone invited to it, with their own signed join as proof. A modified app cannot make
others accept a member they never heard of. It cannot stop a member from sharing what they can read, and every member of a group learns every
other member's address.

## Open questions

- Recovery phrase: word list and format, and what the UI says when the owner declines to write it down.
- Pre-rotation is a known but intricate design (compare KERI). It needs review before we ship it.
- Backdating: someone holding a *retired* key could sign events with an old `ts`. Do contacts treat
  events signed by a retired key as invalid after a rotation they have seen, or only before it?
- Removing a member from a group (leaving is each member's own; nobody can remove another, for now).
- Per-device keys instead of key transfer (see Devices).
