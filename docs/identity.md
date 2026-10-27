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

Phones authenticate to their home Puddle (see openapi.yaml). The home Puddle **transfers the identity
keys to the phone** after the owner approves it.

Pairing:

1. On the Puddle: "Add a device" shows a QR code with the Puddle's address, its TLS certificate
   fingerprint and a one-time secret. Scanning it pins the Puddle's self-signed certificate on the
   phone, so the phone knows it is talking to the right server.
2. The phone sends a pairing request with its name and a fresh public key.
3. The Puddle prompts the owner: "New device *Alex's iPhone* wants to log in. Accept?" Both screens
   show the same short code so the owner knows which request they are approving.
4. On accept, the Puddle sends the identity keys encrypted to the phone's public key, plus a session
   token for the API.

Consequence: any paired device can sign and decrypt as the user, and a lost phone cannot be revoked
on its own. The answer to a lost or stolen phone is **rotating the identity keys** (below), so
rotation must be quick. The phone holds the keys, so it verifies and decrypts events itself; the home
Puddle is its always-on replica.

*Alternative:* give each device its own key, signed by the identity key, so one device can be revoked
without rotating. More machinery (device certificates, revocation lists); worth it if lost phones
turn out to be common.

### The owner token

Until pairing is built, the owner API has one credential: a random 64-hex **owner token** made on first
run in `<data_dir>/state/owner-token` (mode 0600), presented as `Authorization: Bearer <token>`. The
Puddle's own app and the owner's tools read it from that file. Paired phones will get tokens of
their own. Without a valid token every route, including `/health`, answers `404` as if it did not exist.
(`src/core/auth.cpp`.)

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

TLS stays self-signed and only provides confidentiality on the wire. Servers authenticate each other
by signatures, not by certificate. A contact may also pin the certificate fingerprint they last saw.

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
