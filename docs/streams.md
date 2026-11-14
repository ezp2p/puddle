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
   it; then the record is refiled under that id (state, note and addresses carry over),
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
- **So does every answer.** Once we hold a peer's key, nothing counts as delivered to them unless their Puddle signs
  for it, and a message is not even sent until the Puddle at the address has proved it is them (see "Proving who
  answers"). Whoever holds their old address, or sits between the two, gets nothing that matters and is reported.
- **An address change is proven the same way** (see "Telling friends about a new address"): a signed record from a
  key we already hold, so a friend who moves is still the same friend, wherever they write from.

## Closing, blocking

- **Delete:** `POST /api/v1/stream/delete` closes locally and tells the peer with
  `POST /stream/v1/close` (retried if they are offline).
- **Block:** by user id, local only, nothing is sent. Closes any stream with that id; their requests
  and invites are silently dropped; group invites that list them are declined with no reason.

## Telling friends about a new address

A Puddle's address can change. It publishes an **address record**
`{"type":"address","id":..,"addresses":[..],"seq":N,"ts":..}`, signed by its current key, to every
active stream with `POST /stream/v1/address`. The highest `seq` wins. It notices a change at start (the address in its
config, compared with what it last announced, kept in `state/address.json`), and what
each peer is known to hold is kept on its stream record (`addr_sent`), so one who was away is told when it returns.
Peers try all known addresses, newest first, and a peer that takes a record sends whatever it was holding for the
sender straight away rather than at its next scheduled try. If both
sides move while offline they cannot find each other; that is the accepted limit for now (no middleman servers).

The TLS certificate is not part of this. It can change whenever its owner likes (see "Proving who answers"), so it is
neither announced nor remembered. Older Puddles put a `tls_fp` in the record; it is ignored.

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

## Proving who answers

A request signature proves the sender to the recipient. This is the other direction: the sender learning that what
answered really is the Puddle it meant, by its keys, before it hands anything over and before it believes anything was
received.

**TLS only encrypts.** Every Puddle makes its own certificate, nobody checks it against anything, and it may be
replaced at any time. It is not remembered between connections and never needs anyone's approval. What a certificate
does still give is a fingerprint of *this* connection, and that is what ties the signatures below to the connection
they arrived on.

**Receipts.** A Puddle answering a request it has authenticated (from step 4 of the checks above, so `clock_skew`,
`replayed` and refusals included) from someone it already holds a record of adds

    X-Puddle-Receipt: v1 id=<responder id>; kid=<key id>; fp=<certificate fingerprint>; sig=<base64>

where `fp` is the hex SHA-256 of its own TLS certificate, and `sig` an Ed25519 signature by its current key over

    puddle-receipt-v1
    <the responder's user id>
    <the request's nonce>
    <the HTTP status>
    <hex SHA-256 of the answer's body>
    <hex SHA-256 of the responder's own TLS certificate>

A request that did not authenticate gets the usual `404` with no receipt, so a stranger still learns nothing, not even
whose Puddle it reached. Neither does a stranger who *did* authenticate, as anyone can on the routes proven by a chain
in the body: an answer is signed only for someone we already hold a record of, in any state (blocked or not, so a
block is not given away; for an answer to our own request, that is decided after it has been filed). `access` and
`access/status`, which must answer everyone the same whatever they decide, are never signed at all.

An answer sent straight from a file on disk (a download, any part of it asked for with `Range`) is signed for over
exactly the bytes that go, hashed as they are read. Ponds don't sign receipts (yet): a Puddle still pins a Pond's certificate
(`docs/ponds.md`).

**Checking a receipt.** When the sender holds a key for the peer, it accepts an answer only if the receipt is there,
names the peer's id and the key it holds for them, carries this request's nonce, the status and body it got, and the
fingerprint of the certificate *it* saw on this connection. That last part is what catches someone in the middle: they
can pass the request on to the real Puddle and its signed answer back, but the real Puddle signs its own certificate,
and the sender saw theirs.

**Handshake first.** A receipt arrives after the request has gone. So before sending anything to a peer it holds a key
for on a route that needs a stream (every route but the four below: `events`, `close`, `address`, `profile`, `media`,
`group/invite` and the `files/...` routes), the sender calls
`GET /stream/v1/hello`, which carries nothing, and checks its receipt. That proves the certificate on that connection
belongs to the peer. The real request then goes out on a connection only if it shows that same certificate; one that
shows another gets a fresh handshake first. A proof is kept in memory for ten minutes, and each good receipt renews it,
so a steady conversation costs one handshake, not one per message.

Requests that set up or repair a stream (`access`, `access/result`, `access/status`, `identity`) are checked by the receipt
alone (or, for `access` and `access/status`, not at all, as below): the other
side may not have our current key, so it could not answer a handshake without answering a stranger. What they carry is
our public chain, a profile and a one-time ticket bound to that recipient, so nothing a squatter could use.

**First contact** (a request to an address, with no key for whoever lives there) can't be checked at all; see "Proving
who you are". Nor can any request for a stream (`access`, and `access/status`), since those answers are never signed.
The answer to it, `access/result`, brings their chain, and everything from then on is checked.

**What the sender makes of it.** Only a signed answer means anything. Everything else means nothing was delivered: the
item stays queued, is retried on the outbox's schedule, and carries the reason, which the app shows on the message:

| what happened | outcome | what the owner is told |
|---|---|---|
| signed `2xx` | delivered | |
| signed refusal (`4xx`/`5xx`) | failed, kept with the reason | they refused it, and why (`no_stream`: they closed the stream) |
| nothing answered | waiting | nothing answered at the address |
| something answered that isn't a Puddle (no TLS, no HTTP) | waiting | something at the address answered, but it isn't a Puddle |
| an answer with no receipt (such as the plain `404` a stranger gets) | waiting | the Puddle at the address didn't recognise us: it isn't them, or they no longer have a stream with us |
| a receipt from another key, or one that doesn't verify | waiting | the Puddle at the address couldn't prove it's them |
| a good signature for a different certificate | waiting | the connection to the address was intercepted |

A peer that rolled its keys and hasn't reached us yet "couldn't prove it's them" until its new chain arrives (its
outbox is retrying it); taking the new chain, like taking a new address, sends whatever was waiting for them at once,
and an attempt that crossed paths with it is simply made again with the new key. Every address we hold for the peer is tried, newest first,
before giving up on a round.


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

A Pond is not a stream peer, as was first drafted here. Its members' Puddles sign their requests the same way, but a Puddle
joins a Pond with a `pond://` invite and then calls it directly, holding a request for its events open; the Pond never
calls a Puddle, so none of the stream flow (asking, answers, the outbox) is used. See docs/ponds.md.

## Not decided

Retry and backoff numbers; where the nonce cache lives across restarts; how the owner is notified
of requests while the app is closed; limits on the request note and on how many pending requests
a Puddle will hold.
