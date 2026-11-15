# Calls

Live voice (and later video) between friends. This is the design the code follows; the steps are roadmap phase 8.

Decided with the owner:

- **Puddle to Puddle.** The two Puddles carry the call, since they are the parts other Puddles can already reach. Each
  passes the call to and from its owner's device: the desktop window (in the same process) or a phone (over a leg of
  its own, step 8.3). A phone stays a thin client that only ever connects to its own Puddle, so it needs no NAT
  traversal; the cost is one more hop for a phone, and the Puddle's upload carrying the call.
- **WebRTC, through libdatachannel.** Standard ICE, DTLS-SRTP and RTP, Opus for voice. A small C++ library (MPL-2.0)
  that builds on Linux, macOS, Windows and Android.
- **Own addresses only.** No STUN or TURN server run by anyone else: a call uses the addresses the two Puddles already
  advertise, so nobody outside learns who calls whom. A Puddle that can't be reached waits for the Levee relay
  (`levee.md`, "Voice and video").
- **First version:** one to one calls in the window, then on the phone, then group calls, then video.

## The path

    window ──(in process)── Puddle A ══ WebRTC: DTLS-SRTP over UDP ══ Puddle B ──(in process)── window
                                                                          └──(its own leg, 8.3)── phone

A Puddle doesn't decode anything: it forwards Opus frames between its owner's device and the friend's Puddle. Only the
devices record and play.

## Setting a call up

Over the stream, as signed requests to the friend's Puddle (a `PEER` route: only someone with an active stream gets
past the `404`), each answered with a receipt (`streams.md`, "Proving who answers"):

| route | body | answer |
|---|---|---|
| `POST /stream/v1/call/offer` | `{call, sdp, udp}` | `200 {"state": "ringing"}`; `409 busy` (already in a call); `409 no_calls` (built without calls) |
| `POST /stream/v1/call/answer` | `{call, sdp, udp}` | `204`; `404 no_call` |
| `POST /stream/v1/call/end` | `{call, reason}` | `204`; `404 no_call` |

- `call` is 26 characters of base32 (16 random bytes), chosen by the caller.
- `sdp` is the WebRTC offer or answer, with its candidates (nothing trickles: our candidates are known at once). It
  names the DTLS certificate's fingerprint, and it arrives in a request signed by the friend's identity, so the media
  is bound to who they are: someone in the middle can't put their own certificate in, since they can't sign for the
  friend.
- `udp` is the port the sender's calls arrive on (`calls.port`), so the other side can add the sender's advertised
  addresses as candidates at that port.

None of this goes through the outbox. A call is now or never: one that can't be set up is missed, not retried.

## Finding a path

Every call of a Puddle shares one UDP port, `calls.port` in `puddle.yaml` (the same number as the HTTPS `port` unless
set), on the address it listens on. libdatachannel offers the addresses of the machine's own interfaces; each side also
adds the other's advertised addresses (its address record, the one its friends already use) at the `udp` port the other
gave. So two Puddles that can reach each other's HTTPS port can call if the UDP port is open beside it (forwarded on a
home router, published in Docker). No STUN, no TURN.

## A call's life

| state | meaning |
|---|---|
| `calling` | ours: the offer reached them and they are being rung |
| `ringing` | theirs: an offer arrived, waiting for the owner |
| `connecting` | accepted: ICE and DTLS are finding their way |
| `active` | connected, media flows |
| `ended` | over; `reason` says how |

`reason`: `hung_up` (either side ended an active call), `declined`, `cancelled` (the caller gave up before an
answer), `no_answer` (nobody answered in 45 seconds; the callee sees it as `missed`), `busy`, `failed` (no path found
within 20 seconds of accepting, or the connection dropped), `unreachable` (the offer never reached them).

One call at a time for now: an offer while in a call is answered `409 busy`. Every call that ends is noted in
`<data_dir>/state/calls.jsonl` (one line each: id, friend, direction, reason, and when it began, connected and ended; the
newest 500 are kept), which is how a chat shows its calls.

## Media

Opus at 48 kHz, mono, 20 ms frames, around 32 kbit/s, with in-band FEC. Each Puddle sends its owner's frames with
libdatachannel's Opus packetizer and an RTCP sender report, and hands the frames that arrive to its owner's device as they
come; the device keeps the jitter buffer and decodes.

In the window (`src/gui/app/callaudio.*`): the microphone is read with Qt Multimedia, as 48 kHz mono 16-bit when the
device gives that, otherwise in its own format, mixed down and resampled here; every 20 ms is encoded and handed to the
Puddle. What arrives goes into a jitter buffer that starts playing once it holds 60 ms, covers a missing frame with
Opus's own guess, drops what comes after its moment, skips ahead when it falls more than half a second behind, and
carries on from what came after a gap of more than 100 ms rather than guessing at all of it. The speaker reads from it
and gets silence when there is nothing (a sink that reads nothing stops). Muting stops sending. Without libopus the
window still builds and calls connect, in silence. Tests set `PUDDLE_TEST_NO_AUDIO` so that no test opens the
computer's own microphone or speaker.

## The owner's side

| route | |
|---|---|
| `GET /api/v1/calls` | `{version, items}`: the current call, then ended ones, newest first; `?user=` a friend's only, `?limit=` how many (50) |
| `POST /api/v1/calls/start` | `{user}`: ring a friend |
| `POST /api/v1/calls/{call}/accept` | |
| `POST /api/v1/calls/{call}/decline` | |
| `POST /api/v1/calls/{call}/end` | hang up, or give up ringing |

The window hands frames to its Puddle and gets them back through a C interface (`federation/calls.h`), since it runs
the Puddle in its own process. It asks `GET /api/v1/calls?limit=1` every 0.7 s and reads further only when `version`
has moved.

### In the window

- A friend's conversation has a call button in its header (one call at a time, so it is off during one).
- A bar floats over the top of whatever page is open while a call goes on: a friend ringing (Answer, Decline), ours
  ringing at theirs, connecting, or the call with how long it has run, mute and hang up. A friend ringing is also a
  notification.
- Each call is a note in the chat among the messages ("You called Bob · 3:12", "Missed call from Bob"), shown once the
  messages around it are loaded.

## Later steps

- **Phones (8.3):** the phone's Puddle carries the call, and the phone sets up a WebRTC leg of its own with its Puddle,
  signalled over the device API and bound to the device key.
- **Group calls (8.4):** a group call is a call per pair of members (a mesh), over the group's connections, so it suits
  small groups.
- **Video (8.5):** a video track beside the audio, from the window's camera (and the phone's).
