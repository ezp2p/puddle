# Roadmap: keys, then streams

Step through this one item at a time: I propose, you say go, I build it with tests, report, and we
tick it off. Nothing is committed unless you ask. Each phase ends at a good place to commit.

Items marked **needs you** are decisions I should not make alone.

## Where we are

Built and tested (phases 1 to 4): the identity (keys from a recovery phrase, the signed key chain, rolling), the owner token and
the uniform `404`, the HTTPS listener and request signing, streams between Puddles (request, accept, close, block, invites,
certificate pinning), the retrying outbox, direct messages with edit and delete, key and address announcements, profiles and
pictures over a stream, and the Qt app (Friends, conversations, chat, delivery state). 32 test suites, including two Puddles
talking over TLS and the window driven by real clicks and keys.

Group chats (phase 5) are built. Not built: Ponds, the phone client, encryption at rest. See "Later" below and `discuss-later.md`.

## Order, and why

Keys first, because every stream record stores the peer's key chain. Changing the chain format after
streams exist would mean migrating saved data. Then the server plumbing, then streams, then messages.

---

## Phase 1: Keys from a recovery phrase

Each generation of keys comes from one phrase, written down as eight groups of four characters
(`7QK4-M2XB-9TDW-HC5R-ZP3E-A8VN-4YJF-X2K7`, the last group a checksum). **The phrase rolls with the
keys:** every roll makes a *new* phrase, which is written down again, and the old one stops mattering.

A generation has two kinds of key, both derived from its phrase:

- **daily keys** (signing and encryption): kept on the machine, and given to phones
- a **roll key**: never stored. It exists only while the phrase is typed in, and its one job is to
  authorize the next roll. The chain commits to its hash in advance.

So a thief with the daily keys (a stolen phone, a copied file) can't roll, because they lack the roll
key. A leaked *old* phrase is harmless: its daily keys are retired and its roll key was spent. The
*current* phrase is the master secret.

- [x] **1.1 The phrase.** `src/core/phrase.cpp`, tests in `tests/phrase_test.c`. 28 random characters from
  an alphabet without look-alikes (140 bits), plus a 20-bit CRC group. Typing is forgiving (case,
  spaces, hyphens; O is 0, I and L are 1). Every single wrong character and every swap of neighbours
  is caught, checked exhaustively over 200 phrases; any other slip is missed one time in a million.
  The seed is HKDF-SHA256 of the data characters, pinned by a known-answer test cross-checked against
  an independent implementation.
- [x] **1.2 Keys from a phrase.** `src/core/identity.cpp`. The seed gives three keys through HKDF with
  fixed labels (`signing`, `encryption`, `roll`); the labels are pinned by a test against values worked
  out independently. Creating an identity makes the phrase, derives the keys, and writes a first record
  that commits to the hash of the roll key. `keys` holds the two daily keys only (mode 0600); tests prove
  the phrase, its seed and the roll key appear in no file. Creation is all-or-nothing (built in a private
  directory, moved into place in one rename; two processes racing, one wins). **`puddled`** prints the
  phrase once when attached to a terminal, waits for Enter, and carries on; with no terminal it refuses
  and says to run `puddled --init`. An identity from before phrases is refused with instructions, never
  replaced. What changed under it: the old pre-generated "next keys" and the roll built on them are gone,
  so **chains of more than one record are refused until 1.3**, and the roll tests (including a retired
  key in the request-signing test) come back with 1.3.
- [x] **1.2b First-run screen in the app.** `src/gui/qml/setup/Setup.qml` and the Backend's start-up state machine.
  Welcome step, then the phrase as eight numbered groups (four across when wide, two on a phone-width
  window; light and dark), a copy button, and a "written down" tick before Continue enables. The Puddle
  does not start until Continue. Checked by rendering the screen in both palettes and by `tests/backend_test.cpp`.
  One addition the roadmap didn't have: quitting while the phrase is on screen would leave an identity
  nobody wrote down, discovered only much later. So a new identity is **unconfirmed** (a marker inside the
  identity folder, created atomically with it) until the owner confirms; the server refuses to open an
  unconfirmed identity; and an unfinished one is safe to throw away and redo, because nothing could have
  used it. A confirmed identity can never be discarded. `puddled` follows the same rule.

- [x] **1.3 Roll.** `puddle_identity_roll_prepare` and `puddle_identity_roll_commit` in `src/core/identity.cpp`,
  and `puddled --roll`. The owner types the *current* phrase (not echoed); the Puddle checks it is the one
  the chain committed to, makes a **new phrase** and the next keys from it, and only after "the new phrase
  is written down" commits. Phase 1 changes nothing on disk, so stopping there costs nothing and the old
  phrase still works. The new record carries three signatures over its exact bytes: the roll key
  (`sig_roll`), the new signing key (`sig`), and the old signing key (`sig_prev`). It reveals the roll key
  (`roll`), names the previous record (`prev`), and commits to the new phrase's roll key (`next`).
  `puddle_chain_verify` now accepts rolled chains: each record must follow the last, reveal a roll key that
  hashes to what the last one committed to, and have all three signatures. The old private key is gone from
  the machine afterwards (tests scan every file for the old key, the old roll key, and both phrases).
  **Crash safety:** the signed record and the new keys file are first written to one journal
  (`roll.pending`) in a single atomic step, and `puddle_identity_open` finishes any journal it finds, so a
  death at any point either changes nothing or completes. Tested at each stage, and a journal that doesn't
  belong to the chain, has keys that don't match its record, or is damaged is refused without touching
  anything. Tested forgeries: someone with only the daily keys (including passing the stolen signing key off
  as the roll key), a leaked old phrase after a later roll, each of the three signatures wrong or missing,
  wrong `prev`, wrong `seq`, a replayed rotation, and a rotation moved onto another identity's chain.
  `puddled --roll` needs a terminal and takes the data directory's lock, so it refuses while the Puddle is
  running (which would keep using the old keys). The HTTP endpoints (2.4) do the same in two calls while the Puddle runs.
- [x] **1.4 Restore.** `puddle_identity_restore` and `puddled --restore --chain FILE`. From the *latest* phrase
  plus a copy of the chain (not secret; `identity/chain.jsonl`, or later a friend's copy) the identity is rebuilt on
  a new machine: the same id, the same keys file byte for byte, the chain exactly as given, nothing to confirm.
  Everything is checked first and nothing is made on any failure: the chain must be valid, and the phrase must be
  exactly the one the newest record announces and commits to. An **older** phrase is refused and recognised as an
  older one of this identity; a wrong phrase, a typo, a stale or foreign chain, a tampered chain, and a forged
  chain handed to the owner (right keys, thief's roll commitment) are each refused with a reason. Line endings and
  blank lines in a pasted chain are tidied without touching any signature. Restoring never overwrites an identity,
  and prints the advice to roll straight away if the old computer may be in other hands. Shares one atomic
  "build privately, move into place" step with creation. Run end to end through a real terminal against a
  simulated new computer. **Getting the chain from a friend** is still step 3.6.
  **A flaw found and fixed on the way:** the user id was the hash of the first signing key alone, so someone with
  just the daily keys could sign a different first record, with a roll key of their own, and have it be a valid
  chain for the same id. The id now also commits to the first roll key's hash, so any such forgery gets a
  different id. It is still derivable from the phrase alone.

*Commit point.*

## Phase 2: Server plumbing

- [x] **2.1 Read request bodies.** `src/http/serve.cpp`, with the header parsing in `http.c`. Serving one request is
  now its own module over an abstract connection (TLS in the server, a scripted fake in `tests/serve_test.c`), which is
  what makes the awkward cases testable. The rules: **the limit is decided before a body is read**, from the headers
  alone, and capped at 4 MiB whatever a route asks for, so memory is bounded by a number we choose and not by the
  sender. A body over the limit is **not read**: the owner gets `413`, anyone else gets exactly the `404` an unknown
  path gets, so a stranger can't learn which routes take bodies from the size at which they stop. The whole request has
  a deadline, so a body (or headers) trickled in slowly is cut off with `408`. `Expect: 100-continue` is honoured, but
  only for a body that will be accepted. **Anything that could make two parsers disagree about where a request ends is
  refused with `400`:** a repeated or non-numeric `Content-Length`, any `Transfer-Encoding` (chunked is not supported), a
  body on a GET or HEAD, an unknown `Expect`. One request per connection; anything after the first is ignored. Bodies
  are read exactly, binary-safe (NULs and a blank line inside a body are fine), and NUL-terminated for convenience.
  **No route takes a body yet**, so the limit function returns 0 until the route table (2.3) supplies real ones; the
  mechanism is exercised by the tests and by `curl` against the real server. Tests also now run with the allocator
  poisoning new and freed memory, which exposed one missed check (a body that wasn't NUL-terminated).
- [x] **2.2 JSON writer.** Escaping and building objects/arrays; handlers need it for every response.
  Built in `src/core/json.cpp` (`puddle_jw_*`): writes into the caller's buffer, no allocation, commas and
  quoting automatic. Misuse (value without key, unbalanced ends, depth over 64, full buffer) is remembered
  and reported once by `puddle_jw_finish`, which returns 0. Strings are always valid UTF-8 out: bad bytes
  become U+FFFD, control characters are escaped, so a peer's display name can't break a response.
- [x] **2.3 Route table and handler context.** Method + path → handler, each with access to the store,
  identity, profiles and (later) streams, and one of three auth kinds: owner token, signed peer, or
  chain-proven (stream requests and key-roll pushes). One replay memory per Puddle. `/health` moves onto it.
  *Done when:* an unauthenticated request to any route is the same `404`; tests for each auth kind.
  Built in `src/http/router.cpp/h` (the machinery) and `routes.c` (the Puddle's table; only `/health` so far).
  - A route declares its method, path, auth kind, body cap and handler. The router authenticates *before* the
    handler runs, so a handler only ever sees a request that passed for it. PEER looks the sender's current key
    up in the Puddle's streams (`ctx->peer_key`; none until phase 3, so every PEER route is `404` for now).
    CHAIN calls the route's own `chain_key` to find the key in the chain the body carries (written in 3.2).
  - Failure is the same `404` bytes for an unknown path, a missing token, a bad signature, a wrong
    recipient, an unknown sender and a key that isn't theirs. Only after the signature passes is a sender
    told anything: `401 clock_skew` (with our time), `401 replayed`, `503 busy` (nonce memory full).
  - The owner alone gets `405` + `Allow` for a known path with the wrong method; the body cap of a route a
    caller can't use is 0, so a stranger's body is refused unread, exactly as for an unknown path.
  - Handlers write JSON with the writer (2.2) and return a status; writing nothing gives an empty body
    (`202`); a handler that overflows its 28 KB buffer (a profile's longest fields can pass 10 KB once escaped) is a `500`, not a truncated answer.
  - HEAD is gone (GET and POST only, as the specs say); `/health` answers `{"status":"ok"}`.
- [x] **2.4 First real owner endpoints:** `GET /api/v1/identity`, `GET /api/v1/me`,
  `POST /api/v1/identity/roll`. Proves owner auth and JSON end to end.
  - `identity` and `me` answer as the schemas in `api/openapi.yaml`; `me` carries the default profile, with
    absent fields left out rather than `null`.
  - **Rolling is two steps, like `puddled --roll`:** `identity/roll` takes the current phrase and returns a new
    one with nothing changed; `identity/roll/confirm` commits; `identity/roll/cancel` drops it. One pending
    roll per Puddle, dropped after 10 minutes (a phrase shown that long ago may never have been written down).
    The phrase is never logged, never echoed, and the response buffers are wiped after sending.
  - **The link needs an address.** `advertise: host:port` in the config sets it; without it the link uses the
    listen address, and is `null` when that is `::` or `0.0.0.0` (the Puddle can't know its public address).
  - Not yet: telling active streams about the new keys (phase 4: they don't exist yet).

*Commit point.*

## Phase 3: Streams

- [x] **3.1 Stream store.** One file per peer (chain, address, state, note, profile shown), plus blocks,
  invite tokens and the access policy. The key lookup the signature check needs. A `puddle://` link parser.
  Built in `src/api/streams.cpp/h` and `link.c/h`; layout in `docs/storage.md`.
  - **A stored chain is a proven chain.** `puddle_streams_put` verifies the chain against the id the record is
    filed under, so nothing in the store is a key that hasn't been shown to belong to its id; files are
    verified again on load, and a damaged or edited one costs that one peer (skipped with a warning), not the
    Puddle. Ids are checked (`puddle_id_valid`: 52 base32 characters, last one `a` or `q`, so each id has exactly
    one spelling) before they go anywhere near a path.
  - **The key lookup** (`puddle_streams_peer_key`, wired into the router as `ctx->peer_key`) answers for any
    peer we hold a chain for, only for the key currently in that chain, and never for a blocked id. Which
    states may do what stays with the endpoints, which is where `403 no_stream` lives. The PEER routes are
    live now, though none exist yet.
  - **Blocks** are local, persisted before anything else happens, close any stream with that id, refuse their
    keys and refuse new records about them.
  - **Invites** are random 128-bit tokens with a lifetime (up to 30 days) and a use count; a spend is saved
    before it is honoured; at most 64 are live.
  - **Links** (4.6 dropped the id: now `[puddle://]host[:port][?invite=<token>]`): strict (no spaces, paths, fragments, other parameters,
    bare IPv6, malformed hosts or ports); the address is stored in one canonical spelling.
  - Not here: the request itself (3.2), rate limits (3.2), the TLS fingerprint of a peer (3.4), pushed chain
    updates after a roll (phase 4).
- [x] **3.2 Inbound request.** `POST /stream/v1/access`: verify the chain in the body, check blocks,
  policy, invite token and rate limit, record `pending_in`, always answer `202` with no body.
  Built in `src/federation/access.cpp` (the request) and `ratelimit.c`; the route is a CHAIN route.
  - **Order:** the router asks `puddle_access_chain_key` for the requester's key, found in the chain in the
    body (after a global limit on this unauthenticated work, 50 a second with a burst of 100); the
    signature is then checked and replays are caught as for any signed request; only then does the handler
    decide. So the `202` means "properly signed by this id", and everything else is the usual `404`.
    *This differs from the earlier wording ("202 whatever happens"): answering 202 to garbage would tell a
    stranger there is a Puddle here.* The decision itself is never visible: pending, approved, blocked,
    closed, rate-limited and full all produce the same bytes.
  - **Decision:** blocked, closed (unless we asked them first) and over-limit requests are dropped, as are
    group-only (`scope`) requests until groups exist. Someone we asked ourselves (`pending_out`) is approved
    at once (both want it). Otherwise approved at once if the policy is `open` or the invite token is good
    (spent only if it's what lets them in); otherwise `pending_in`. A closed stream may be asked for again;
    an active one is left alone; a request while waiting refreshes the note and address.
  - **What is kept** is rebuilt, not copied: the note (500 characters), and a profile of `display_name`,
    `blurb`, `about` and `pronouns` only, cleaned of control characters and of the direction overrides that
    let a name read backwards, with bad UTF-8 replaced. The address must parse as `host:port`.
  - **Limits:** five requests an hour per requester, 200 waiting for the owner, 5000 on record, 96 KiB per
    request (a chain in it up to 64 KiB, about 80 key changes).
  - An approval made here (open policy, invite, or mutual) is told to the requester with `access/result`: the
    record carries `owed` until the outbox delivers it (3.3, 3.5).
  - **Owed to 3.4:** the address in a request came from a stranger. When we connect to it, we must not
    treat it as trusted (no connecting to anything but a normal peer address, a short timeout).
- [x] **3.3 Owner stream endpoints.** List, accept, decline, delete, block, unblock, invite, policy.
  Built in `src/core/owner_streams.c` (all owner-token routes; nothing here talks to another Puddle).
  - `GET /api/v1/stream` (paged, filter by `state`; most recently changed first; a page stops early when
    its entries are large, and `next` says where to carry on), `accept`, `decline`, `delete`;
    `GET/POST /api/v1/stream/policy`; `POST /api/v1/stream/invite` (and, beyond the spec as first written,
    `GET` to list and `POST .../invite/revoke`, since a link that has leaked must be cancellable);
    `GET /api/v1/user/block`, `POST .../block`, `POST .../unblock`.
  - **What each may be applied to:** accept and decline only a `pending_in`; delete only an `active` stream
    or a `pending_out` one (withdrawing a request); anything else is `409`. Blocking yourself is `400`.
    Blocking an already-blocked id keeps the first time. A block drops the optional note the spec once had.
  - **Owed to the other side.** A decision has to reach the peer even if they are offline or we restart, so
    the stream record now carries `owed` (`approved`, `declined` or `closed`), set by accept, decline, delete
    and by approvals made on arrival (open policy, invite, mutual), and cleared by the outbox once delivered
    (3.4, 3.5). A block is silent and clears it.
  - An invite needs somewhere to point: with no `advertise:` address and a wildcard listen address,
    creating one is `409 no_address` with a message saying what to set.
  - `204` responses now carry no `Content-Length` or `Content-Type`, as HTTP requires.
  - Not here: `stream/request` (3.5), `stream/profile` (assigning a profile to a stream, phase 4).
- [x] **3.4 Outbound client.** A signing HTTPS client and a persistent outbox.
  - **TLS:** the connection uses each Puddle's own self-generated certificate and accepts any, with no
    CA and no hostname check; signatures do the authenticating. The certificate's fingerprint is recorded
    on first contact. It does not depend on the address, so an address change doesn't alter it; it
    changes only if the certificate is regenerated. A Puddle that regenerates announces the new
    fingerprint inside its signed address record, so legitimate changes are accepted silently. An
    unannounced change on a known peer holds sending and tells the owner.
  - **Retries:** a send that fails because the other Puddle is **offline** (connection refused, timed out,
    host or network unreachable) is retried. Every five minutes for the first 24 hours, then every thirty
    minutes. The check is a TCP+TLS connect per peer, not a resend of everything; when it succeeds the
    queued items for that peer go out. The schedule is saved, so it survives a restart.
  - **Any other failure** (TLS error, an HTTP error, `404`, a rejected signature) **fails once and is not
    retried.** The outbox keeps the reason so the app can later tell the owner "this failed because
    ..., try again or give up".
  Built in `src/http/client.cpp` (one signed HTTPS request) and `outbox.c` (the queue, on disk).
  - **Client:** TLS accepts any certificate and takes its SHA-256 fingerprint; `fingerprint_ok` is asked once
    the certificate is known and before anything is sent. Results are `OK` (any HTTP answer), `OFFLINE`
    (refused, timed out, unreachable, unknown host), `FAILED` (anything else: TLS, HTTP framing, an oversize
    or stalled answer) and `CERT_CHANGED`. Timeouts: 8 s to connect, 10 s per read or write, 30 s in all;
    answers over 64 KiB are refused.
  - **Addresses from strangers** are screened before dialling, as noted under 3.2: unspecified, multicast,
    broadcast, reserved and link-local (which includes the cloud metadata address) are never connected to,
    including IPv4-mapped IPv6 and every address a name resolves to. Loopback and private ranges are allowed:
    friends on one network are normal.
  - **Outbox:** one file per item in `state/outbox/`. Offline peers are retried on the schedule above, by sending
    their oldest item (that is the check; no separate probe); when one gets through the rest follow in order
    at once. Only the oldest item is tried each round, so a dead peer costs one attempt, not one per item. The
    day-long window starts at the first failure of a stretch and resets once the peer answers. Anything else
    fails once and is kept, marked failed, with its reason; items behind a failure still go. At most 1000 wait
    and 200 failures are kept (oldest forgotten first). Items go out in order per peer; one peer being away never
    delays another.
  - **Certificates:** the fingerprint of a peer is recorded in its stream record on first contact. A different
    one holds all that peer's items, marks the stream `certificate_changed`, and waits for
    `POST /api/v1/stream/trust`, which adopts the new one and releases the held items.
  - The server runs the outbox on its own thread. What happens when an item ends (an answer reaching a
    requester, `owed` cleared) is 3.5; for now the end is logged.
  - Not here: an address record announcing a new fingerprint (phase 4), and an endpoint to list the outbox's
    failures for the app (comes with the app's use of it). (Peers are delivered to in parallel; see 3.5.)

- [x] **3.5 Requesting and answering.** Owner `POST /api/v1/stream/request` sends the signed request;
  inbound `access/result`, `access/status` and `close`.
  Built in `src/federation/flow.cpp` (what we start and how it turned out) and `inbound.c` (what arrives).
  - **`POST /api/v1/stream/request`:** takes a `puddle://` link (an invite in it is sent along), a note and which
    profile to show. It refuses (`400`/`409`, with a code) yourself, a bad link, a note over 500 characters, an
    unknown profile, someone blocked, a stream already open or requested, or one they asked for (accept
    instead), and needs this Puddle's own public address (`advertise:`) because the answer has to come back to
    it. Otherwise it records `pending_out` and queues the signed request. If it cannot be queued the stream is
    closed with the reason, not left looking as if it were on its way.
  - **Answers that come back** (`access/result`, chain-proven): accepted only from an id we have a `pending_out`
    request to, whose chain belongs to that id, and with a plain `true` or `false`; then the stream becomes
    `active` (their chain, profile and address kept, the link's address first) or `closed`. Anything else, from
    anyone, changes nothing, and **an authenticated answer is always `202`**: a stranger can authenticate with a
    key of their own, so the reply must say nothing. A decline carries the responder's chain too (otherwise a
    forgery could not be told from it).
  - **`access/status`** is a `POST` with the chain in the body, not the `GET` first drafted, because the target
    may hold nothing about the requester (a request it ignored leaves no trace) and so could not otherwise tell
    who is asking. `approved` only to someone with an active stream, `declined` to someone whose stream we
    closed, `pending` to everyone else, blocked or ignored or unknown alike. Nothing in this Puddle calls it yet:
    the outbox's retries mean an answer is rarely missed, so a poller waits for a reason to exist.
  - **`close`** is signed with a key we hold, so the sender is known and can be told `403 no_stream` when there
    is nothing to close; from an active stream, a request they sent, or one we sent (a refusal), it closes.
  - **Telling the other side** is the `owed` mechanism from 3.3: accept, decline, delete and arrival-time
    approvals each queue the matching message, and the outbox's `done` hook clears what was owed once it lands
    (or records, in the stream's `error`, that it could not be told, and gives up: fail once). At start, anything
    still owed that the queue does not hold is queued again.
  - **Concurrency:** the outbox now delivers to different peers side by side (up to 8 threads, one per peer, so
    a peer's items stay in order), because one slow or dead peer must not delay the rest. The store gained
    `puddle_streams_update`, which reads, changes and writes a record as one step, since several threads now
    change records; the owner's accept/decline/delete and the arrival of a request use it.
- [x] **3.6 Two Puddles, end to end.** An automated test with two Puddles: request, accept, active, close;
  blocked and `closed` look like pending; an invite link auto-approves; a stranger gets `404`.
  `tests/e2e_test.c` starts five real Puddles (own data directory, own certificate, real TLS on loopback) and
  drives them through the owner API with real owner tokens: request, accept, active on both sides with chains,
  fingerprints and profiles recorded; decline and asking again; an invite let in with nobody clicking; a blocked
  requester and a Puddle closed to requests both see their request stay "pending" while the target holds nothing
  of them; delete, and being asked again; a restart while an answer is owed (not doubled, and re-queued from the
  record if the queue file is lost); a regenerated certificate holding sending until the owner trusts it; a
  refused request failing with its reason; answers nobody asked for changing nothing; and every `/stream/v1`
  route answering a stranger `404`. `inbound_test.c` and `flow_test.c` cover the details.
  *Not covered: the owner-recovery fetch from a friend (`GET /stream/v1/identity`), which is not built; it is
  its own step in phase 4.*

*Commit point. This is the first time it visibly works.*

## Phase 4: Messages and the rest of a stream

- [x] **4.1 Direct messages.** `POST /stream/v1/events` delivery with dedupe and sender checks, and the
  owner `dm/send|edit|delete` and message listing, on top of the chat store.
  - [x] **4.1b Edits and deletes.** In `message.c` (the events and the view) and `messages.c` (routes).
    - **Events:** `edit` (new body, `ref` = the message) and `redact` (`ref`, nothing else; a redaction carrying text is refused,
      so "deleting" can't smuggle content). Same signing, ids, limits and delivery as a message; a receiver judges them the
      same way, and `unsupported_type` now means any other type.
    - **The view:** edits and redactions are applied when a chat is read, not when it arrives, so the result is the same on
      every Puddle whatever the arrival order: only the author's count, only if later than the message, last edit wins, a
      redaction beats any edit. A message keeps its place and time, and shows `edited` / `edited_at` or `deleted` (no body).
    - **Owner:** `POST /api/v1/dm/edit` (answers with the message as it now reads) and `/dm/delete` (`204`; deleting twice
      sends nothing). Both need an open stream, and your own message (`404` otherwise; `409 deleted` to edit a deleted one).
      The spec gained `user` on both, because messages are filed by chat and an id alone does not say which. Their timestamps
      are forced past the message's, so a slow clock cannot make them invalid.
    - **Reading** (`dm/messages`) pages over *messages*, however many edits sit between them: it takes a window of the newest
      events and widens it until it holds enough messages.
    - Not here: an erase on this machine (storage.md's "optional local erase"); edit history; an edit window limit.
  - [x] **4.1a Send and receive text.** Built in `src/objects/message.cpp` (the event: chat id, signing, checks) and
    `messages.c` (routes).
    - **The event** is `{"type":"message","chat","sender","ts","body"[,"ref"]}`, stored and sent exactly as signed;
      its id is the SHA-256 of those bytes. The DM chat id is the one in `docs/storage.md`, and a DM has no
      `chat.create`. The signature is Ed25519 over `puddle-event-v1\n` + the bytes (so it can never be mistaken
      for a request or chain signature) and travels as `<key id>:<base64>` *outside* the bytes, which lets a Puddle
      that rolls keys sign the same event again without changing its id.
    - **A correction to the proposal:** I said a receiver would accept an event signed by an earlier key in the
      chain. It does not: a delivery must be signed by the sender's **current** key, because accepting retired
      ones would let someone holding a stolen phone's old keys keep writing as the owner after the owner rolls,
      which is exactly what rolling is for. Verifying old history with old keys is a different job (catching up on
      a chat), for later. What this leaves to 4.2: a message queued before a roll is signed with the old key,
      so after the roll the sender must sign queued ones again (possible now, since the key id is outside the
      bytes), and a receiver must learn the new key first (the push in 4.2); until then its deliveries are `404`.
    - **Receiving** (`POST /stream/v1/events`, peer-signed): needs an open stream (`403 no_stream`) and the
      DM chat between exactly the two of us (`400`); at most 100 events and 256 KiB, refused whole beyond that.
      Each event is then judged alone and answered `accepted`, `duplicate` or `rejected` with a reason
      (`bad_id`, `bad_signature`, `unknown_key`, `wrong_sender`, `wrong_chat`, `bad_time`, `bad_body`, ...).
    - **Sending** (`POST /api/v1/dm/send`): needs an open, unblocked stream; checks the body; stamps a `ts`
      that never repeats; signs; saves; queues for delivery (retried while they are away). If it is saved but
      cannot be queued, the answer is `500 not_queued` rather than a pretence.
    - **Reading** (`GET /api/v1/dm/messages?user=`, paged from the newest, a page holding what fits). The spec's
      `/dm/{user}/messages` became a query parameter, since routes here are exact paths.
    - **Trouble reaches the owner:** a delivery the other side refuses, or cannot be made, is noted as the
      stream's `error`.
    - Not here (4.1b and later): edits and deletes; a per-message sent/failed state; live updates; read markers;
      a per-sender rate limit (deliveries only come from people the owner has accepted); catching up on missed
      messages.
- [x] **4.2 Key roll and address pushes.** `POST /stream/v1/identity` and `/address`; peers verify and update.
  Built in `src/federation/keysync.cpp`.
  - **Keys:** after a roll (the API's `roll/confirm`, or at start if `puddled --roll` was used while stopped), every peer
    that holds our chain and has not been told (`chain_sent` on its record) gets it, queued **ahead** of everything else
    for that peer. The receiver takes it only as a continuation of the chain it holds (the stored text plus whole
    new records, each verifying); anything else, from anyone, is ignored. It is authenticated by the new chain in the body,
    which any stranger could do with their own, so it is always `202`.
  - **What was already waiting** is brought up to date, since it was made for the old keys: events are signed again by the
    new key (same bytes, same id), and a request or answer that carried the old chain gets the new. Without this a message
    queued before a roll would be refused after it, or a request answered `404`.
  - **Address and certificate:** at start the Puddle compares the address it advertises and its certificate's fingerprint
    with what it last announced (`state/address.json`); a difference makes the next signed address record (`seq`+1), pushed
    to every active stream. The record carries the new `tls_fp`, which the receiver accepts silently (signed by a key it
    trusts) and which releases anything held for that certificate. A certificate that changes *without* an announcement
    still holds sending until the owner trusts it (3.4): this is what tells the two apart.
  - **Who holds what** is on the stream record (`chain_sent`, `addr_sent`) and is set when an item is delivered, including
    the requests and answers that carry our chain and address, so nothing is sent twice and nothing is forgotten across a
    restart or an absence.
  - Addresses we would never connect to are refused wherever they arrive (a request, an address record).
  - New in the outbox: waiting items can be rewritten, "retry now" for a peer, and news of a key change goes first.
  - Not here: retrying a message refused as `unknown_key` for a roll we were not told of (the push goes first, so this only
    happens if it was lost); the owner-recovery fetch of a chain from a friend (`GET /stream/v1/identity`).
- [x] **4.3 Profiles over a stream.** Assignment, `profile` push, `media` fetch.
  Built in `profiles_api.c`, `wireprofile.c` and `media.c`, with larger changes underneath.
  - **Left out on purpose: reload.** The roadmap listed "reload endpoints", but reloading profiles while running is on
    `discuss-later.md`, so profiles are still read at start. Nothing else here depends on it.
  - **The wire profile** is now the whole thing, not only text: colour (one to five `#rrggbb`, an optional angle) and the
    three images as `{hash, mime, size}`. A stranger's is rebuilt field by field, each checked and cleaned, never stored
    as sent; one with anything wrong in it is refused whole (a request, an answer or a push alike). One function writes our own
    (`puddle_profile_write_fields`), used for the owner's API, requests, answers and pushes.
  - **Assigning** (`POST /api/v1/stream/profile`): sets which profile a stream sees (`null` = the default), tells the peer
    if the stream is open (retried while it is away), and keeps the choice for a stream that is not open yet.
    `GET /api/v1/profile` lists our profiles.
  - **Fetching.** `GET /stream/v1/media?hash=` serves a picture only if it is in the profile that peer is shown, and only over
    an open stream (anything else is `404`). Pictures we do not hold are fetched, over an open stream only, when a request is
    answered or accepted, or a profile arrives. The outbox learned to GET, with a cap on the answer (what the profile said the
    size was, plus a little); what comes back is kept only if its SHA-256, size and kind (judged from the file's first bytes) are
    what the profile said. Failures say so on the stream (`error`) and are not retried.
  - **The owner's app** reads any picture we hold (ours or fetched) from `GET /api/v1/media?hash=`.
  - **Underneath:** responses can now be a file bigger than the answer buffer (headers in the buffer, the file after them,
    freed by the server), and the client can take answers up to a size the caller names and reports the content type.
  - Still open, on purpose: what a receiver does about huge, slow or booby-trapped pictures. The 25 MiB cap is the placeholder
    it always was, and `discuss-later.md` #1 is untouched.
- [x] **4.4 The app shows it.** Discord-style Qt app for DMs and group chats (no servers yet): a narrow rail with a
  Friends icon on top (the server list comes later), a DM list with unread marks, a Friends page (All / Pending /
  Blocked, Add Friend with a `puddle://` link), and flat Discord-style messages (avatar, name, time, grouped, no bubbles).
  The app talks to its own Puddle over the owner API on HTTPS like a phone client, with the Puddle's own certificate pinned.
  - [x] **4.4a-core** `GET /api/v1/conversations` (last message, unread count) and `POST /api/v1/dm/read`
    (read marker in `state/read.tsv`, only moves forward); tests in `messages_test.c`.
  - [x] **4.4a-qt** `ApiClient` (only the Puddle's own certificate is trusted; the owner token goes with every request),
    `JsonListModel`, and `Chats` (people, conversations, open chat, actions, a 2 s refresh, signals for new requests and
    messages so the window can notify). `Backend` attaches it when the server starts. `chats_test.cpp` runs two real Puddles.
  - [x] **4.4b** shell and Friends page: rail with the Friends icon (and an unread badge), a DM list with unread marks, a user
    panel (your name, copy your link, activity log), the Friends page (All / Pending / Blocked, Add Friend with link, note
    and profile), and Pending rows that show the note, a short code (`K7QM-2XPD`, derived from the id) and Accept / Decline.
    Pictures are fetched through the owner API, checked against their hash, and cached. One pane at a time when narrow.
    Also: Cmd/Ctrl+N, tray notification (never the message text) and Dock badge, window size and place remembered.
  - [x] **4.4c** chat view and composer: flat Discord-style messages (picture and name on the first of a run, later lines tucked
    under it, date dividers, "(edited)", muted "Message deleted"), replies with a preview line, hover tools and a right-click
    menu (copy, reply, edit, delete with a confirmation), inline edit (Enter saves, Esc cancels, Up in an empty box edits
    your last message), the composer (Enter sends, Shift+Enter is a new line, 4000 characters, one unsent draft per chat),
    older history loads on scrolling up, a "Latest" button, a changed-certificate banner with Trust, and a composer that
    explains itself when the stream is not open. Messages in the open chat count as read only while the window is in view.
    `scripts/dev.sh [name]` runs the app in its own sandbox (run two names to be friends with yourself).
  - [x] **4.4d** polish: a banner when the window cannot reach its Puddle, dark-mode fixes (selected rows), the unread count in
    the window title, Alt+Up / Alt+Down between conversations, Cmd/Ctrl+W closes to the tray, and a clicked notification
    opens the chat or request it was about.

- [x] **4.5 Prove the app, and show delivery.**
  - [x] **4.5a The window under test.** `tests/ui_test.cpp` loads the real QML (`Shell`) over the app's real `Backend` and a
    second real Puddle, and drives it with real mouse and key events (offscreen): Add Friend (link, note, send), the other side
    accepts, open the chat, send with Enter, Shift+Enter makes a new line, receive, edit with Up, delete through the hover bin and
    its confirmation, a hidden window leaves messages unread until it is back, and the narrow one-pane layout. `testnode.h`
    holds the shared test Puddle. It found one real bug: absent `note`/`error` fields reached QML as `undefined`.
    The "attentive" binding moved from `Main.qml` into `Shell.qml` so the test covers it.
  - [x] **4.5b Delivery state.** `dm/messages` (and the `dm/send` answer) give each of your own undelivered messages a
    `delivery` object read from the outbox: `queued` (not tried yet), `retrying` (the other Puddle could not be reached; `next_at`),
    `failed` (with the reason; never retried by itself) or `held` (their certificate changed). Delivered messages carry none.
    A message is matched by its own delivery id, so an edit waiting in the outbox is not taken for it. `POST /api/v1/dm/retry`
    queues failed items again and tries waiting ones now (`puddle_outbox_requeue` is new). The chat shows "Sending…", "X can't be
    reached right now. Trying again at 14:05." with a Try now button, or "Not delivered: reason" with Retry. The window test takes
    the friend's Puddle offline, sends, sees the note, brings it back and presses Try now.

- [x] **4.6 Add by address.** The owner decided: nobody should handle a user id. Adding someone is typing their address
  (`203.0.113.7`, `home.example.org:8443`; the port defaults to **443**), and the id is exchanged over the stream.
  - **Parser:** `link.c` takes `[puddle://]host[:port][?invite=<token>]` and no id; `puddle_link_format(address, invite)` leaves
    `:443` off. `puddle_make_link` lost its `id` parameter, so "your address" in the app is just `host[:port]`.
  - **First request:** the requester files `pending_out` under a made-up id (hash of a random **ticket**; `provisional`) and sends
    `access` with its id, chain, address, note and the ticket, signed for recipient `*` (`PUDDLE_RECIPIENT_ANY`), which only the
    new `PUDDLE_AUTH_CHAIN_OPEN` route (`/stream/v1/access`) accepts. The outbox kind `open_access` signs that way.
  - **The answer** repeats the ticket. `puddle_flow_adopt` / `puddle_streams_adopt` refile the record under the responder's
    verified id (certificate pin and note carry over), spend the ticket, and carry on as before. A closed stream with that id
    starts afresh; any other existing record stands; a blocked id drops the request. A second answer with a spent ticket is ignored.
    A withdrawn request that is answered "yes" anyway tells them the stream is closed.
  - **Both asked:** a request from the address we have asked tells us who it is and the two are one (yes). A request from ourselves
    is dropped. A second request to an address that is already asked or a stream is `409`.
  - **Decision:** first contact trusts whoever answers at the address (nothing in the app asks anyone to compare an id or code).
    Friend codes and a handshake, an invite link that auto-adds like a Discord invite, and better address handling are for later.
  - **App:** Add Friend takes an address; "copy my address"; **Developer Mode** (Settings) shows user IDs and codes and "Copy user ID";
    outgoing requests are named by the address until they answer. Tests: `open_request_test.c` (new), `link_test`, `flow_test`,
    `e2e_test` and `ui_test` rewritten for addresses.

*Commit point.*

## Phase 5: Group chats

Design in `docs/groups.md`. Decided with the owner: groups appear in the same list as direct messages; members who are strangers to each other
are connected by a group-only connection (a `scoped` stream) rather than through the creator; the cost, that every member learns every other
member's address, is accepted and shown before joining (like phone numbers, nothing hides it); any member can invite a friend, rename or leave,
nobody can remove another member; a new member sees nothing from before they joined; 50 members at most; blocking someone means neither of you
gets the other's messages in a group, and you can still be in it.

- [x] **5.1 The group itself.** `group.c/h`: the id (`base32(SHA-256("group\n" creator "\n" ts "\n" nonce))`), the five membership events and their
  strict parse, the rules every Puddle applies the same way (`puddle_group_apply`), and the store of group records (`state/groups/`).
- [x] **5.2 Group messages on the wire.** `groupflow.c`, `groupapi.c`: fan-out of events to each member we have an open stream with (the
  existing outbox), `POST /stream/v1/events` for a group chat judged by the group's rules (the envelope and signature checks are shared with
  direct messages through a policy, `puddle_take_signed`), the invitation (`POST /stream/v1/group/invite`, from a friend), accept, decline, leave,
  rename. A decline is a `member.leave` to whoever invited.
- [x] **5.3 Group-only connections.** A new stream state `scoped`, and two flags on a stream: `group_ok` (group events may pass whatever else the
  stream is) and `scoped_req` (our request was only for a group). `POST /stream/v1/access` with `scope` and the requester's own signed
  `member.join` as proof; answered by `access/result` with `scoped:true`; both sides then tell each other they have joined, once. A friend request
  over a group connection leaves the group going. When the last shared group is left, a connection that was only for groups is closed.
- [x] **5.4 Owner API.** `GET /api/v1/gm`, `gm/create|invite|accept|decline|leave|rename|send|edit|delete|retry`, `GET gm/messages` (with `notices`
  for who made it, joined, left, renamed). `conversations` lists groups beside DMs (`kind`), `dm/read` takes `group`.
- [x] **5.5 The app.** Groups in the DM list as well as on their own **Groups** page under Friends (invitations, your groups, "Start new group"; a
  later "new message" window may start a DM or a group from one place). Group chat with the notices between the messages and a members box
  (rename, ask a friend, leave). An invitation says who invited you and nothing about how many are in the group: that would have to be asked
  again every time it was looked at. Joining shows a plain statement that your address will be visible.
- [x] **5.6 Tests.** `group_test`, `groupapi_test` (the receiving side and the scoped connections), `groupowner_test` (the owner's routes),
  `group_e2e_test` (three real Puddles over TLS where two are strangers, ending with a friend request over the group connection and leaving),
  `chats_test` and `ui_test` (the window: new group, accept with the address notice, decline, talk, members, rename, leave).

Found while doing it, and fixed: a made-up stream id was being sent key pushes (4.6); a join someone could not learn from anyone but its author
now travels directly (`told`); events about the group go to the invited too, or they cannot tell who else is coming.

*Commit point.*

## Later, not planned in detail

Removing a member, a display name for the group-only connection, Ponds, encrypting message
bodies, encrypting data at rest, the phone client and device pairing. Parked questions are in
`discuss-later.md`.

## Decisions

| # | Decision | Answer |
|---|----------|--------|
| 1 | Keys derived from the phrase; the old key counter-signs | Yes, and **the phrase rolls with the keys**: each roll makes a new phrase, authorized by a roll key from the old one. |
| 2 | Showing the phrase | `puddled` prints once on first run (terminal only, else `--init`). The app shows it once, in a small first-run screen. |
| 3 | TLS trust | Self-generated certificates, accept any, record the fingerprint, announce changes in the signed address record. |
| 4 | Retries | Offline: every 5 minutes for a day, then every 30. Anything else fails once; the reason is kept. |
