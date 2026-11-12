# Hardening

How a Puddle (and a Pond) stands up to someone sending it junk on purpose: what it already does, and the gaps still open.
Notes for now, to be worked through later. Line references were true on 2026-10-29.

## What a request costs before it is refused

A Puddle refuses everything it can't authenticate with a `404`, the same as an unknown path (`CLAUDE.md`, "Conventions").
How cheap each refusal is:

- **Header.** At most 8 KiB (`HEADER_MAX`, `src/http/serve.cpp`), read within a 15 s deadline (`REQUEST_DEADLINE_SEC`), or
  the connection is dropped. A repeated `Authorization`, `X-Puddle-Signature` or `Content-Length`, any `Transfer-Encoding`
  (no chunked bodies), and a malformed `Expect` are all `400` before anything else (`src/http/http.cpp`).
- **Owner token** (`Authorization: Bearer …`). A prefix check, a length check, then a constant-time compare against the token
  in memory (`puddle_auth_check_bearer`, `src/core/auth.cpp`). No disk, no lookup.
- **Peer signature** (`X-Puddle-Signature`). Parsed, then the sender's key is found in the in-memory streams table
  (`puddle_streams_peer_key`). An unknown sender stops there, before any cryptography. A known sender's id with a bad
  signature costs one Ed25519 verify. No disk.
- **Routes open to strangers** (`/stream/v1/access` and the other `PUDDLE_AUTH_CHAIN*` routes). These parse and verify a key
  history sent in the body, so they share one limit of 50/s with bursts of 100 for all strangers together (`anon_bucket`,
  `src/service/server.cpp`).

## Large bodies

The size of a body is decided from `Content-Length` before a byte of it is read (`src/http/serve.cpp`):

- **Unknown path, or an owner route without the owner token:** the limit is 0. Anything with a body is `404` and the body is
  never read. A "200 GB upload" costs the header and nothing more, and the connection closes.
- **A route that takes a body:** its own limit (`body_max` in each route table), never more than `PUDDLE_BODY_HARD_MAX`
  (4 MiB). Over it: `404` to a stranger, `413` to the owner, and nothing read.
- **Peer routes read the body before the signature is checked**, because the signature covers its hash. So a stranger can
  make a Puddle receive up to that route's limit and then be refused: 256 KiB on `/stream/v1/events`
  (`PUDDLE_DELIVERY_MAX_BYTES`), 96 KiB on the access routes, 64 KiB or less on the rest. The buffer is `malloc`ed for the
  declared length up front.
- **More bytes than `Content-Length` says** are never read as body: exactly the declared length is read (`read_body`), and
  a Puddle serves one request per connection (`Connection: close`). Afterwards `puddle_sock_finish`
  (`src/platform/net_posix.cpp`) drains at most 20 reads of 1 KiB, about a second at most, so the answer isn't lost to a
  reset, and then closes the connection. The rest of a "1 MB declared, 1 GB sent" body is dropped by the kernel.
- **Peers never push files.** A file moves by the receiver fetching chunks (`docs/files.md`), so there is no route a peer can
  stream a large file into. The owner's own uploads come through owner routes.
- **What we fetch** from other Puddles and from Ponds has a cap per call too (for example `EVENTS_MAX_BODY`, 16 MiB, in
  `src/federation/ponds.cpp`).

The Pond (Drogon) refuses any body over 2 MiB (`setClientMaxBodySize`, `pond/src/pondd.cpp`), and each route then holds
its own smaller limit: 64 KiB for a message, 256 KiB to begin a file, 1 MiB for a chunk.

## Open gaps

1. **No per-IP connection limit.** A Puddle serves at most 64 connections at once (`MAX_CONNECTIONS`,
   `src/service/server.cpp`), one thread each, and turns the rest away. One sender holding 64 slow connections (each may
   last up to the 15 s deadline, and be reopened at once) shuts out real peers. Wanted: a cap per remote address, well
   below 64.
2. **The TLS handshake comes first.** Every connection does a full handshake before its header is read, and that costs far
   more CPU than any check above. A per-IP rate on new connections (gap 1) is the defence. Nothing cheaper can happen
   before TLS.
3. **Every refused request writes one log line** (`handle_request`, `src/service/server.cpp`) to stderr. journald
   rate-limits; launchd, or anything else sending stderr to a file, lets the file grow without limit. That is the only way
   a flood of junk reaches disk. Wanted: collapse repeats ("…and 4,812 more from 203.0.113.9 in the last minute").
4. **Bandwidth spent before the signature check.** A stranger who names a real path can make a Puddle read up to 256 KiB
   per request before the `404` (see "Large bodies"). Bounded by gap 1, but still bandwidth a Puddle on a home connection
   pays for. Could be cut by checking the parts of the signature that don't depend on the body (a known sender, a fresh
   timestamp) before reading it.
5. **Pond: nothing beyond Drogon's defaults** for connection counts per address. The same per-IP limit applies there.
6. **No rate limit on events from a known peer.** `POST /stream/v1/events` takes up to 100 events or 256 KiB a request
   (`PUDDLE_DELIVERY_MAX_*`, `src/events/message.h`), but nothing limits how many requests a peer sends. A stranger can't
   deliver any (a peer's signature is needed); a friend, or anyone whose keys were stolen, can fill the owner's disk and
   chat with as many as the connection limit lets through. Wanted: a bucket per sender (`puddle_keyed`, as
   `access_limit` already does for stream requests), and perhaps a cap per chat per day, with the owner told and the
   overflow refused.

## Reading files it shouldn't

Can a crafted request make a Puddle hand back its private keys (`identity/keys`), `puddle.yaml`, the owner token or the
profiles? Checked on 2026-10-29. What stands in the way today:

1. **No route serves a file by name.** A route is an exact path with typed parameters (`param_ok`, `src/http/router.cpp`):
   user ids, 64-hex event and file ids, profile ids, numbers, tokens of `[A-Za-z0-9_-]`. Query values allow the same set
   only (`puddle_query_parse`). Nothing is percent-decoded. So no `.` or `/` reaches a handler, and `../` can't be spelled.
2. **Storage checks again.** Before it builds a path, storage checks the id itself: `valid_chat_id` in
   `src/storage/store.cpp`, `puddle_file_id_valid` and `puddle_media_hash_valid` in `src/storage/`.
3. **What is served is addressed by hash and gated by who is asking.** Media only if it is in a profile shown to that peer;
   file chunks only if the file is on a message in a chat with them. The paths that are read are fixed
   (`state/address.json` and the like), never taken from a request.
4. **No owner route reads the keys or the recovery phrase back.** Changing keys needs the current phrase, and nothing
   returns it.

**There is no backstop.** All four are checks in our own code, in one process that holds everything: the private keys
are in plain text in `data_dir/identity/keys` (mode 0600), and the Puddle can read every file the user can. A route
written wrongly later, or a memory bug in the C++ that parses requests, would give everything away. Ways to add a hard
wall, roughly in order of what they buy:

- **Privilege separation for the keys** (as OpenSSH does): a small process holds the private keys and only signs and
  decrypts on request. The process that faces the network never has them. A compromise can misuse the signer while it
  runs, but can't take the keys away.
- **An OS sandbox** for the network-facing process, so the kernel refuses what our checks miss: Landlock and seccomp on
  Linux, the App Sandbox or `sandbox_init` on macOS, `unveil`/`pledge` on OpenBSD, AppContainer on Windows. Allow the data
  folder (without `identity/` once the keys live elsewhere) and nothing else.
- **A tripwire where files are opened to be sent:** resolve the path (`realpath`, `O_NOFOLLOW`) and refuse anything
  outside `files/` and `media/`, logging it loudly. That catches a mistake, not a memory bug.
- **Keys at rest:** in the OS keychain, or encrypted with a key kept there. Helps against a stolen disk or backup, not
  against a compromised running Puddle.
- **headless `puddled` as its own system user,** so even a full compromise can't read the rest of the machine.
- **Fuzzing** the request parser and router, and a test that no parameter type can ever match `.` or `/`.

## Ideas to look at

### Banning addresses, fail2ban-style

The Puddle keeps a count per remote address of the things that only an attacker or a broken client does, and when an
address crosses a line it is banned for a while: its connections are closed as soon as they are accepted, before TLS, with
nothing written back and nothing logged beyond one line when the ban starts.

Things that might count (each needs its own weight; a real peer whose clock or keys are out of date must not get banned):

- requests refused with a `404` for failed authentication: a wrong owner token, a signature from an unknown sender, a bad
  signature
- malformed requests: `400`s from the parser, a header over 8 KiB, a body declared over its limit
- TLS handshakes that fail or never finish, and connections that hit the 15 s deadline
- too many connections at once, or too many new ones per second (gaps 1 and 2)

To decide:

- the thresholds, the window they are counted over, and how long a ban lasts (doubling for each repeat?)
- whether the owner sees bans in the app, and can lift one or set a never-ban list
- that the owner's own phone client must not lock itself out with a stale token: maybe its failures count for less
- behind a Levee every connection comes from the Levee's address, so there the Levee has to do the banning, by the
  addresses it sees
- IPv6: a single host may hold a whole /64, so ban by prefix there
- memory: the table of counts needs a cap and to forget old entries, or it becomes the thing a flood fills

The Pond wants the same. It could lean on Drogon's hooks (a filter that runs before routing), or on a real fail2ban
reading its log, for admins who run one.

### UPnP

Review whether a Puddle should use UPnP IGD (or NAT-PMP / PCP) to open its own port on a home router, so that people
without a Levee don't have to forward it by hand. Weigh it against the risks: routers' UPnP is often buggy or turned off on
purpose, and it opens a port without the owner looking at the router.
