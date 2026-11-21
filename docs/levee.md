# Levee

A **Levee** is the public side of one Puddle: a small daemon (`ezp2p/levee`) its owner runs on a server with a public
address, usually behind a reverse proxy. The Puddle connects **out** to it and needs no open ports. The wire is the
protocol's part 06 (`third_party/protocol/spec/06-levee.md`); this page is Puddle's side of it.

Built 2026-11-21. Decided with the owner:

| Question | Decision |
|---|---|
| Who runs it | One Levee per Puddle, run by its owner, like an extension of the Puddle in a DMZ. It may know the owner's friends (their public keys), never their messages. |
| Joining | `leveectl join` on the Levee makes a `levee://` link, good once; the owner pastes it in Settings, "Levee". |
| Live or mailbox | Both: requests pass straight through while the Puddle is attached; the one-way ones are spooled while it is not, with a custody receipt to the sender. |
| What goes out | Everything, through the Levee: nobody the Puddle talks to learns its home address. |
| Strangers | The Levee knows the Puddle's friends and phones (`peers`, sealed to it) and refuses anyone else, except first contacts (a stream request, a phone pairing), which are rate-limited. |
| Files | Left at the Levee, sealed under a key only the message carries, until each friend has fetched them; then deleted. Asking again is answered by putting it there again. |
| Calls | Through a TURN relay on the Levee, in the first build. Its UDP ports are published directly, beside the proxy. |
| Pond voice | Through the relay too, by default, so Ponds don't learn the Puddle's address; the owner may send it straight to the Pond instead (shorter, but the Pond sees the address). |
| Storage | Plain files in the Levee's data folder; no database. |

## In the app

Settings, "Levee": paste the link and Join. The section then says where friends reach the Puddle and whether it is
connected now, and offers to leave. A Puddle with no window (in Docker, say): `puddled --join-levee 'levee://…'`,
`puddled --levee` and `puddled --leave-levee`, which call the same API (`service/main.cpp`). The owner API: `GET /api/v1/levee`, `POST /api/v1/levee/join {link}`,
`POST /api/v1/levee/leave` (`api/openapi.yaml`, "Levee").

## What joining changes

- **The address.** `puddle_my_address` says the Levee's (`core/levee.h` keeps the join in `state/levee.json`): links,
  phone pairing links, the self sent with stream requests and answers, and the address record, which now also names the
  Levee (`levee: {id, key}`) so friends believe its custody receipts. Friends are told at once (`puddle_sync_resume`).
- **Coming in.** `service/levee.cpp` keeps the Puddle attached: it attaches, sends `peers` whenever they change, holds a
  request on the Levee's `inbox`, and runs each item through the router as if it had come directly, posting the answer
  back. A spooled request's clock is checked against when the Levee took it (`puddle_request.received_ms`).
- **Going out.** `http/client.cpp` sends every request to the Levee's `send` while joined, signed for the Levee on the
  outside (`X-Levee-Signature`); the receipt inside is checked as always. The Puddle's own requests to its Levee go
  direct (`opts.direct`).
- **Custody.** Sending to a friend behind a Levee: a `202` with that Levee's custody receipt (`X-Puddle-Custody`) counts as
  delivered. The friend's Levee is kept on their stream record (`levee_id`, `levee_key`), from their self or address record.
- **Files.** Each file in a message sent from behind a Levee gets a key (`files[].key`, in the signed event); the worker
  seals its hashes and chunks under it and leaves them at the Levee for the chat's members. A download asks the sender's
  Levee first when the file has a key (`puddle_levee_fetch`), and says `done` there when it is whole.
- **Calls.** With a relay, ICE uses only it (`iceTransportPolicy: relay`), so nothing shows the Puddle's own address, and
  offers and answers say `udp: 0`; a friend then adds no candidates of their own making.

## Pond voice

A Pond voice channel is a connection to the Pond, which forwards everyone's voice (the Pond's docs/voice.md). Behind a
Levee it goes through the relay like a call (`pc_config(cs, true)` in `federation/calls.cpp`): the Puddle offers only the
relay's address, and its join says `udp: 0`, since the request reaches the Pond from the Levee's address and the Pond would
otherwise add that as a candidate.

Settings, "Levee", has a switch for **Pond voice straight to the Pond** (`POST /api/v1/levee/settings
{pond_voice_direct}`, kept in `state/levee.json`): the connection then uses the calls port as without a Levee. It is a
shorter path and doesn't load the Levee, which carries everything the channel sends the owner, but the Pond sees the
Puddle's own address. It is off unless the owner turns it on, applies from the next channel joined, and leaving the Levee
forgets it. Only the owner changes it: the worker's saves keep it as it is.

Leaving: the Levee forgets the Puddle and everything it kept for it, and friends are told the Puddle's own address again
(or none, if it has no `advertise` and listens everywhere).

## Tests

The Levee repository's `tests/e2e_test.cpp` runs a real `puddled` behind a real `leveed` and befriends another: joining,
talking both ways, a call through the relay, a message taken while the Puddle is away and handed over when it is back, a
file fetched from the Levee while its sender is off, and leaving. Puddle's `ui_test` checks the Settings section.

## Not yet

- **Holding what goes out** for a friend who is away and has no Levee: the Puddle's outbox keeps trying, which needs it on.
- **Several Levees**, or a Levee and a direct address at once.
- **Voice over HTTPS** for networks that block UDP.
- **Phones through the Levee** work like friends (their requests are signed and sealed to the Puddle), but a phone's own
  calls leg still offers its host candidates.
