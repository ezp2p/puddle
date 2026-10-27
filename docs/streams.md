# Streams (draft)

A **stream** is a trusted connection between two Puddles (later also a Puddle and a Pond). Nothing
is sent between two Puddles without one, apart from asking for one. This describes the logic; the
endpoints are in `api/openapi.yaml` (what the owner's app calls) and `api/federation.yaml` (what
Puddles call on each other).

Built so far: the identity, its recovery phrase and rolling (`src/core/identity.cpp`), and request
signing and checking (`src/http/reqsig.cpp`: Alice's side signs, Bob's side runs the checks below in
order, with a bounded nonce memory), and the route table that applies them (`src/http/router.cpp`), the stream store (`src/api/streams.cpp`: peer
records, blocks, invite tokens, the access setting, and the key lookup the signature check uses) and the link parser
(`src/core/link.cpp`). The requests and endpoints that use them are described below.

## What each side keeps

One small file per peer under `<data_dir>/state/streams/`, replaced atomically:

- the peer's user id and their **key chain** (verified; see identity.md)
- their address(es), as a signed address record with a sequence number
- the profile they showed us, and which of our profiles we show them
- the state (below), the note sent with the request, and timestamps

Plus `state/blocks.tsv` (blocked user ids) and `state/invites.tsv` (invite tokens). A block is not
a stream state: it is a separate list that overrides everything.

## States

| state | meaning |
|-------|---------|
| `pending_out` | we asked, they have not answered |
| `pending_in`  | they asked, the owner has not answered |
| `active`      | trusted both ways; messages may flow |
| `closed`      | dried up (deleted, or declined). Either side may ask again. |

Transitions: a request makes `pending_out` / `pending_in`; accepting makes `active` on both sides;
declining makes `closed`; `stream/delete` on either side makes `closed` on both. Chat history is
never removed when a stream closes.

## Asking for a stream

There are **no public endpoints**. A Puddle never tells a stranger anything: not its identity, its
name, its policy, or whether it is even a Puddle. The one request it must accept from someone it
doesn't know is a request for a stream, and its answer to that is always the same.

1. The owner gets an **address**: `203.0.113.7`, `home.example.org:8443` or `[2001:db8::1]` (the port defaults to
   443), optionally written `puddle://…` and optionally with `?invite=<token>`. There is no user id in it. Nobody
   types or compares one: who lives at the address is learned from their answer (below).
2. A's Puddle makes a random **ticket**, files the request as `pending_out` under a made-up id (a hash of the
   ticket; the record is marked `provisional`), and sends `POST /stream/v1/access` straight to that address:
   its own key chain, id, address, a profile (the one the owner chose), a short note (up to 500 characters), the
   ticket, and the invite token if there is one, signed with its key. The request is signed *for no recipient*
   (`*` where the recipient's id goes), because nobody has said who lives there; only this route accepts such a
   request, so everywhere else a signature for the wrong recipient is still a `404`. Nothing is fetched first.
3. B's Puddle first checks the request is really from who it says: well formed, the chain is the requester's
   and the signature is its key's. A request that fails any of that is a `404`, the same as a path that does not
   exist, so a stranger can't use this route to find out whether it is talking to a Puddle at all. One that
   passes is answered `202` with no body **whatever B decides**. Then, privately, it drops the request
   silently if the sender is blocked, if its policy is `closed`, if the sender is over its rate limit
   (five requests an hour), or if B is full (200 waiting, 5000 on record). Otherwise:
   - **valid invite token:** approve at once (tokens expire and have a use count)
   - **policy `open`:** approve at once
   - **policy `manual`** (the default): record `pending_in` and tell the owner
4. When approved (immediately, or when the owner accepts with `POST /api/v1/streams/{id}/accept`), B becomes
   `active` and its outbox calls A's `POST /stream/v1/access/result` with its own chain and profile, retrying
   on the schedule in `roadmap.md` (3.4) if A is away; what B still owes is kept on the stream (`owed`), so a
   restart loses nothing. B puts the requester's ticket in the answer. A accepts it only if the ticket is one
   it made and has not used, the chain belongs to the id the answer claims and the result is signed by a key in
   it; then the record is refiled under that id (state, note, addresses and the certificate it saw carry over),
   the ticket is spent, and it becomes `active`. This is the first time A learns who B is and sees B's keys. A
   second answer with the same ticket, from anyone, is ignored. If A already has a closed stream with that id the
   request starts it afresh, if A has blocked that id the request is dropped, and any other existing record stands.
   Like the request, the call is answered `202` once it authenticates, whatever A makes of it.
5. Declining sends `approved: false` the same way, with B's chain and the ticket, and nothing about why.
   If A withdrew the request meanwhile (the record stays, closed) and B says yes anyway, A tells B the stream is closed.
7. If B has itself asked A's address meanwhile, A's request tells B who is there: B refiles its request under A's
   id and treats the two as one (both asked, so yes).
6. A requester whose Puddle missed all this can ask `POST /stream/v1/access/status`, chain-proven like the
   request (a POST, because B may hold nothing about A: a request it ignored leaves no trace). B says
   `approved` only to a requester it has an active stream with, `declined` to one whose stream it closed, and
   `pending` to everyone else: blocked, ignored, never heard of. So no answer ever reveals a block, a closed
   door, or that the request was seen. (Nothing yet calls this; the retrying outbox makes it rarely needed.)

`closed` therefore means "ignore strangers", not "refuse": the sender just waits. A request reflected
off a forged return address can make B send a small signed result to someone who didn't ask; results
are rate-limited too.

## Proving who you are

A request proves one thing: whoever sent it holds the key whose hash is the id they claim. It does not prove that
the Puddle at an address is the person you meant. That is the owner's choice, made on purpose:

- **First contact trusts whoever answers at the address.** Someone able to intercept the connection could answer
  as the "friend". Giving an address to a friend is, like giving a phone number, no proof of who picks up. Friend
  codes (a code you must be given in advance) and a handshake are planned for later. Nothing in the app asks the
  owner to compare ids or codes; Developer Mode shows them for anyone who wants to.
- **Names and notes are self-declared.** Anyone can type "Alice". What can't be faked later is her key.
- **Every later request proves it again.** Each one is signed with the sender's current key and checked against
  the chain stored at accept. Knowing an address, or having seen old traffic, is not enough to be them.
- **An address change is proven the same way** (see "Telling friends about a new address"): a signed record from a
  key we already hold, so a friend who moves is still the same friend, wherever they write from.

## Closing, blocking

- **Delete:** `POST /api/v1/stream/delete` closes locally and tells the peer with
  `POST /stream/v1/close` (retried if they are offline).
- **Block:** by user id, local only, nothing is sent. Closes any stream with that id; their requests
  and invites are silently dropped; group invites that list them are declined with no reason.

## Telling friends about a new address

A Puddle's address can change, and so can its certificate. It publishes an **address record**
`{"type":"address","id":..,"addresses":[..],"tls_fp":..,"seq":N,"ts":..}`, signed by its current key, to every
active stream with `POST /stream/v1/address`. The highest `seq` wins. It notices a change at start (the address in its
config, the certificate's fingerprint, compared with what it last announced, kept in `state/address.json`), and what
each peer is known to hold is kept on its stream record (`addr_sent`), so one who was away is told when it returns.
Peers try all known addresses, newest first. A `tls_fp` in a record is accepted without asking, because it is signed by
a key the peer already trusts, which is how a Puddle that regenerates its certificate stays on speaking terms. If both
sides move while offline they cannot find each other; that is the accepted limit for now (no middleman servers).

A Puddle is told only what the sender can prove: `address` is checked against the key we hold for them, and a
numeric address we would never connect to is refused.

## Request signatures

Every request carries

    X-Puddle-Signature: v1 id=<sender id>; kid=<key id>; ts=<ms>; nonce=<base64>; sig=<base64>

`kid` is the hex SHA-256 of the sender's signing public key, and `sig` is an Ed25519 signature over

    puddle-request-v1
    <METHOD>
    <request target: path and query, as sent>
    <hex SHA-256 of the body; of nothing for a GET>
    <ts>
    <nonce>
    <the recipient's user id>

The recipient checks, in this order, and **answers `404` to any failure up to and including the
signature**, the same as for a path that doesn't exist, so a stranger can't tell what is there:

1. the recipient id is its own (so a request can't be replayed against a different Puddle). Only `POST /stream/v1/access`
   also accepts `*`, for a first request to an address, which gives up that protection for that one message: a replay can
   only make a request from the sender appear at another Puddle
2. the sender is one it knows, and `kid` is that sender's *current* key (retired keys can't start new
   requests). The exceptions are `POST /stream/v1/access`, checked against the chain in its body, and
   `POST /stream/v1/identity`, checked by chain continuity (below)
3. the signature verifies

Only now is the sender authenticated, so only now can it be told specific things:

4. `ts` within five minutes of the recipient's clock, else `401 clock_skew` with its time so the
   owner can be told
5. the nonce is new in that window, else `401 replayed`
6. the sender has a stream in a state the endpoint allows, else `403 no_stream`

There is no "fetch the sender's keys" step. When a peer rolls its keys it pushes the new chain to
every active stream (`POST /stream/v1/identity`). That endpoint answers a stranger exactly as any
other: `404` unless the chain extends one the recipient already holds.

Events inside chats are signed separately, over their exact bytes, and name the key that signed them
(`kid`), so history signed before a key roll still verifies.

## Keys

Built in `src/core/identity.cpp` and `phrase.c`; the plan and status are in `roadmap.md` (phase 1).

**Creation.** On first run the Puddle makes a **recovery phrase** (eight groups of four characters),
shows it to the owner once, and derives three keys from it: an Ed25519 signing key, an X25519
encryption key, and an Ed25519 **roll key**. The user id is the base32 hash of the first signing key together
with the commitment to the first roll key.
Only the two daily keys are kept, in `<data_dir>/identity/keys` (mode 0600). The roll key and the phrase
are never stored or logged. The first record in `chain.jsonl` is signed by the signing key and commits
to the hash of the roll key.

**Rolling is optional** (built; `puddled --roll`). It never happens by itself. The owner can do it any
time, for example after losing a device that held the keys. The owner types the current phrase, which
re-derives the roll key; the Puddle makes a **new phrase** (written down again) and derives the next
keys from it. The new record is signed by the roll key, the new signing key and the old signing key,
and commits to the new phrase's roll key. Then:

1. the Puddle sends the chain to every peer that holds it (an active stream, or a request of ours still waiting)
   with `POST /stream/v1/identity`, queued **ahead** of anything else for that peer. Whatever was already queued
   is brought up to date: events signed with the old key are signed again, and a request or answer that carried the old
   chain is rewritten to carry the new one.
2. peers verify it continues the chain they hold and note the new keys; one that doesn't is ignored. A peer that was
   offline misses the push, so the sender keeps trying until it lands (the outbox's schedule), and remembers per peer
   which chain it is known to hold (`chain_sent`), so a restart, or a roll done with `puddled --roll` while the Puddle
   was stopped, is caught up at the next start.

The id does not change, so no one has to re-add anyone. Why it resists a stolen phone: a phone is given
the *daily* keys only, and rolling needs the roll key, which exists only while the current phrase is
typed in. A leaked *old* phrase is harmless, because its keys are retired and its roll key was spent.

**The phrase is the backup.** If the machine is lost, the latest phrase restores the identity (roadmap
1.4). After a roll, only the new phrase works.

## Group-only connections

A group's members who share no stream with each other get a connection limited to groups (`scoped`), set up when the invitee joins:
`POST /stream/v1/access` with `scope` set and the requester's own signed `member.join` as proof, approved by the target because it is in that
group and the requester is invited to it. The whole design is in `docs/groups.md`. The display name for it is still undecided.

## Ponds

A Pond is a peer with `kind: pond`. The same flows apply (link, invite token, `open` policy, signed
requests). What differs is what crosses the stream afterwards: channels and history held by the
Pond, rather than logs replicated between peers.

## Not decided

Retry and backoff numbers; where the nonce cache lives across restarts; how the owner is notified
of requests while the app is closed; limits on the request note and on how many pending requests
a Puddle will hold.
