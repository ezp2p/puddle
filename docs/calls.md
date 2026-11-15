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
- **Group calls are a mesh:** a connection between each pair of the people in the call, over the connections the group
  already has. No Puddle carries anyone else's call, so it suits small groups (up to 8 in a call).

## The path

    window ──(in process)── Puddle A ══ WebRTC: DTLS-SRTP over UDP ══ Puddle B ──(in process)── window
                                                                          └══ WebRTC, its own leg ══ phone

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

### Echo

On speakers, the friend's voice comes back into the microphone, and without help the friend hears themself a moment
later. So the microphone's sound passes through an echo canceller (`EchoCanceller` in `callaudio.*`) before it is
encoded: WebRTC's own (the webrtc-audio-processing library, as packaged by Linux distributions for PipeWire and
PulseAudio), with its noise suppression and high-pass filter; or, where that library isn't packaged (Homebrew), speexdsp's
echo canceller and preprocessor. CMake picks whichever it finds (`-DPUDDLE_ECHO=auto|webrtc|speex|off`); with neither,
calls still work, without cancelling. It runs only when there is both a speaker and a microphone, and keeps running while
muted so it stays in step with the room.

An echo canceller needs, beside each 10 ms from the microphone, what the speaker was playing then or a little before. The
speaker's side hands it every sample it gives the speaker. A speaker takes its sound in gulps just before it runs out (170
ms at a time on PipeWire, a few ms elsewhere), so what it was handed rises in stairs whose foot is where it is playing,
and the reference runs along that foot: 10 ms a chunk, never past what was handed, and moved up if it has stayed more than
20 ms below for 3 s (the microphone's and speaker's clocks drift apart; 0.15% on the machine this was tried on). Qt's own
measure of the speaker's queue isn't used: it moves in the same steps and says nothing in between.

`callaudio_test` plays a voice-like sound, puts its echo in the microphone 30 ms later with two reflections, through a
speaker that gulps 170 ms and one that keeps 80 ms queued: WebRTC's canceller takes about 46 dB off the echo, speexdsp's 17
to 22, and both keep this end's own voice when it talks over the friend.

## The owner's side

| route | |
|---|---|
| `GET /api/v1/calls` | `{version, items}`: the current call, then group calls going on that the owner is not in, then ended ones, newest first; `?user=` a friend's only, `?group=` a group's only, `?limit=` how many (50) |
| `POST /api/v1/calls/start` | `{user}`: ring a friend; `{group}`: start a call in a group, or join the one going on there |
| `POST /api/v1/calls/{call}/accept` | answer, or join a group call |
| `POST /api/v1/calls/{call}/decline` | (a group call stops ringing, and can still be joined) |
| `POST /api/v1/calls/{call}/end` | hang up, or give up ringing; leave a group call |
| `POST /api/v1/calls/{call}/media` | a phone's leg: `{sdp}` its offer, answered `{sdp, udp}` ("On a phone") |

Each call says which of the owner's devices has it (`device`): `null` for the window, or the phone (`{id, name}`) that
started or answered it. A friend's call rings on all of them; the first to answer has it, and the others show where it is
and can hang it up, but have no sound of it.

The window hands frames to its Puddle and gets them back through a C interface (`federation/calls.h`), since it runs
the Puddle in its own process. It asks `GET /api/v1/calls?limit=1` every 0.7 s and reads further only when `version`
has moved.

### In the window

- A friend's conversation has a call button in its header (one call at a time, so it is off during one); so does a group's,
  which starts a call there, or says "Join (2)" while one goes on that the owner is not in.
- A bar floats over the top of whatever page is open while a call goes on: a friend ringing (Answer, Decline), ours
  ringing at theirs, connecting, or the call with how long it has run, mute and hang up. A friend ringing is also a
  notification.
- Each call is a note in the chat among the messages ("You called Bob · 3:12", "Missed call from Bob", "Alice started a
  call · 12:40"), shown once the messages around it are loaded.
- A group call's bar shows the group, and how many others are in it; hanging up leaves it to them.

## On a phone

A phone is a thin client (`identity.md`, "Devices"), so its Puddle carries its calls as it does the window's, and the
phone has a WebRTC leg of its own to its Puddle for the sound:

1. The phone starts a call or answers one, signed by its device key like everything it asks. The Puddle notes that phone
   as the call's `device`.
2. It makes an offer (one Opus audio track, `sendrecv`, complete with its candidates) and sends it in
   `POST /api/v1/calls/{call}/media`. Only that phone may, and only while the call is going on.
3. The Puddle answers with its own description and its calls port: `{sdp, udp}`. The leg uses the same UDP port as every
   other call, so nothing more needs to be open. The phone adds the Puddle's address, the one it reaches the Puddle at, as
   a candidate at that port.
4. Once the call is active, the Puddle passes Opus frames between the friend's connection and the phone's leg as they
   come. Each is re-stamped with that connection's own RTP clock, keeping any gap the frames had, so the jitter buffer at
   the far end still sees a lost frame as lost.

Both descriptions name their DTLS fingerprints and travel in a request the phone signed and an answer the Puddle signed
(with its receipt), so the leg is bound to the pairing just as a friend's call is bound to the stream. No STUN and no TURN
here either: a phone that reaches its Puddle's HTTPS port reaches its calls port beside it.

Asking again replaces the leg, for a phone that moved to another network. The call ends (`failed`, for the friend too) if
the leg doesn't connect within 20 seconds, if it drops for 10, or if the phone stops asking how the call stands (the app
asks every 0.7 s while it is open) for 10 seconds or is removed. That last check notices a phone that went away much
sooner than the leg's own connection would.

In the app (`src/gui/app/callleg.*`), the phone uses the window's microphone, speaker and jitter buffer (`callaudio.*`),
fed from the leg instead of from the Puddle in its own process.

## Group calls

A call in a group (`groups.md`) is a connection between each pair of the people in it, over the streams the group already
has: a friendship, or the group-only stream two members who never met were given. Each Puddle sends its owner's frames to
every other member in the call and passes on what each sends, so with five in a call each Puddle sends four streams and
gets four. Nobody carries the others' calls, nobody is in charge of it, and it stops when the last one leaves. Up to 8 in a
call (`PUDDLE_GROUP_CALL_MAX`): beyond that, a member's upload is the limit.

Over the stream, signed like a friend's call, from a member of the group to another (`403 not_member` otherwise):

| route | body | answer |
|---|---|---|
| `POST /stream/v1/call/group/join` | `{call, group, seq}` | `200 {in, call?}`: whether we are in it too; `call`, a smaller id we know of in the group |
| `POST /stream/v1/call/group/leave` | `{call, group, seq}` | `204` |
| `POST /stream/v1/call/group/offer` | `{call, group, sdp, udp}` | `204`, answered in a moment with `group/answer`; `404 no_call`, `409 ours_to_offer`, `409 full` |
| `POST /stream/v1/call/group/answer` | `{call, group, sdp, udp}` | `204`; `404 no_call` |

- **Starting and joining.** Whoever starts a call chooses its id and tells every member of the group (`join`); so does
  everyone who joins. A member who hears of a call they are not in is rung for 45 seconds, if they are free, and can join
  it for as long as it goes on (the group's chat says "Join (2)"). Turning it down only stops the ringing.
- **Connecting.** Each `join` is answered with whether the recipient is in the call too. For every two in it, the one
  with the smaller user id offers (`group/offer`), and the other answers (`group/answer`), so two who join at the same
  moment never both offer. Offers and answers name their DTLS fingerprints and travel signed, as a friend's call does.
- **Staying in step.** A member in the call says `join` again every 20 seconds; one not heard from in three of those, with
  no connection up, has gone. The same message mends a connection that dropped: the one to offer offers again. A `join`
  also means the sender is in no other call of the group.
- **Out of order.** Joins and leaves go out at once to everyone, on separate threads, so two of them can arrive the wrong
  way round (a "still in" just after a "left"). Each carries a `seq` that only goes up from that sender (its clock in
  microseconds, or one more than the last), and the older of two is ignored.
- **Two at once.** Two members who start a call at the same moment start two calls. The first to hear of the other moves
  to whichever has the smaller id (`join` answers with it), and so everyone ends up in one.
- **Leaving.** Leaving tells every member. The call goes on for the rest; it ends for one left alone after 45 seconds
  (`hung_up`, or `no_answer` if nobody ever came).

What the owner sees is a call with `group` set and its `members` (the others in it, each with whether it is connected):
`calling` while alone in it, `connecting`, `active` once connected with anyone; `ringing` or `open` for one going on that
the owner is not in. The history notes each call left (`hung_up`, `cancelled`, `no_answer`, `failed`), and each one rung
for and never joined (`missed`, or `declined`), under the group; `user` is who started it.

One call at a time still: in a group call, a friend's ring is answered `busy`, and the other way round.

`groupcall_test` has four Puddles, three of them in a group, two of whom have never met: starting, ringing, joining,
frames between every pair marked with whose they are (over the group-only stream too), leaving and coming back, busy, a
call nobody joins, two starting at once ending up in one, and that only a member can start one. `ui_test` does it from
the window's header and bar.

### Group calls on a phone

A phone in a group call has its leg to its Puddle as for a friend's call ("On a phone"). What it says goes on its track,
and its Puddle sends it to everyone connected. What the others say comes the other way over a data channel the phone's
offer also opens (`frames`: unordered, never sent again), one message per frame: the member's number in the call (`slot`
in `members`), the frame's RTP timestamp (4 bytes, big-endian), then the Opus frame. The app keeps a jitter buffer and
decoder for each (`Mixer` in `callaudio.*`), and plays them mixed, as the window does.

## Later steps

- **Video (8.5):** a video track beside the audio, from the window's camera (and the phone's).
