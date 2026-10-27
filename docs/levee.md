# Levee

*Design draft. Nothing here is built yet; the open questions at the end are still to be settled with the owner.*

A **Levee** is a relay a Puddle can sit behind. It runs on a server somewhere with a public address; the Puddle
connects out to it, and from then on the Puddle needs no open ports and makes no direct connections to anyone:
everything it sends goes out through the Levee, and everything sent to it arrives there and waits until the Puddle
collects it.

A Levee is **not a service for ordinary users**. It has no GUI and no app. It is a headless daemon, configured by a file,
for someone reasonably competent to run on a server (a VPS, a home server with a public address, a friend's box): the
same sort of person who would run a mail relay or a TURN server. The Puddle and its app only need to know a Levee's
address and the credentials to use it.

## What it is for

- **No open ports.** A Puddle behind NAT, CGNAT, a phone hotspot or a strict firewall can still be reached, because it
  only ever connects out.
- **Offline delivery.** What friends send while the Puddle is off waits at the Levee instead of waiting in each friend's
  outbox, and arrives as soon as the Puddle reconnects.
- **A stable address.** The Puddle's address is the Levee's, so it stays the same when the Puddle's own network changes.
  This also covers the case `docs/streams.md` accepts as a limit today: two Puddles that both move while offline can no
  longer find each other, but they can if either is reachable through a Levee.

## What it is not

- **It decrypts nothing and signs nothing.** Messages between Puddles are already end-to-end encrypted, and every request
  is already signed by the sending Puddle's key. The Levee stores and passes on what it is given, byte for byte. It cannot
  read messages, forge them, or change them without the change being caught.
- **It is not a Puddle and not a Pond.** It hosts no chats and has no members, profiles or history of its own. A Pond is
  a place people gather; a Levee is plumbing for one Puddle (or a few, see below).
- **It does not hide metadata.** It sees who sends to its Puddle, how much, and when, and which Puddles its Puddle
  sends to. Whoever runs a Levee should be someone the owner trusts with that.

## How it works

### Attaching

1. The owner tells their Puddle the Levee's address and a credential the operator gave them (see the open questions).
2. The Puddle connects out to the Levee and authenticates with its identity key: the Levee learns which user id it
   serves, and that this Puddle holds that id's current key.
3. The Puddle keeps that connection open. Whatever the Levee holds for it is handed over, and new arrivals follow as they
   come.

### Addresses

Adding a Levee is an **address change**, and Puddles already know how to announce one: the signed address record
(`POST /stream/v1/address`, docs/streams.md, "Telling friends about a new address").

- **Adding a Levee:** the Puddle publishes a new address record (a higher `seq`) whose address is the Levee's. It goes
  to every friend directly, or through the Levee if the Puddle is already behind one. A friend who is offline finds it
  waiting at their own Levee, or in the sender's outbox, as today.
- **Removing a Levee:** the Puddle publishes a record with its own address again (or a new Levee's), the same way.
- **Links:** a Puddle behind a Levee gives out links with the Levee's address, so new friends reach it there from the
  start.
- **Several Puddles behind one Levee:** they share its address, so requests carry the recipient's user id (they already
  do: request signatures name the recipient) and the Levee files them by it. A first request to someone known only by an
  address, which today may be signed for no recipient (`*`), needs the link to say which Puddle at that address is meant.

### Sending and receiving

- **Outgoing:** the Puddle hands each request to the Levee, which makes it on the Puddle's behalf and hands back the
  answer. The request is signed by the Puddle, so the receiver checks it exactly as now.
- **Incoming:** a request for the Puddle arrives at the Levee.
  - **While the Puddle is connected**, the Levee passes it straight through and returns the Puddle's answer: nothing in
    the stream protocol changes, including the calls that need an answer at once (fetching a picture, `access/status`,
    syncing chat history).
  - **While it is not**, the Levee keeps what is one-way (messages, group events, profile pushes, address records,
    requests for a stream) and answers `202`, and the Puddle collects them when it reconnects. A call that needs an
    answer fails as it would today against an offline Puddle, and the sender's outbox tries again later.

Because Puddles talk to the Levee's TLS certificate, not the Puddle's, a Puddle behind a Levee is recognised by its
signed requests and end-to-end encryption, not by pinning its certificate (see the open questions).

### Voice and video

Live calls need a relay too when neither side can be reached directly. That would be a Levee's job as well, much like a
TURN server: it forwards encrypted media between the two ends and sees only its size and timing. It is a separate piece,
built after messaging works through a Levee.

### Phones

A phone client reaching its own Puddle through the Levee (so the Puddle needs no open port for its owner either) is a
separate discussion: how the owner token is protected on the way, and whether the Levee may tell an owner's phone apart
from a peer.

## Running one

- A daemon, `levee` (proposed: its own code base, like Pond's; to confirm), with one config file (YAML, like Puddle's): the address to listen
  on, its certificate, the data folder, which Puddles it serves and what each may use.
- It keeps one folder per Puddle it serves: what waits for that Puddle, and what it is allowed.
- It logs to stdout or a file for the operator; there is no app and no web page.
- Limits per Puddle (how much it may hold, for how long, how fast strangers may send to it) are the operator's to set.

## Open questions

1. **Mailbox or live tunnel.** The draft above is both: a live pass-through while the Puddle is connected, a mailbox for
   one-way traffic while it is not. A pure mailbox would turn every call that needs an answer into two one-way messages,
   which changes the stream protocol a great deal. Confirm the hybrid.
2. **Certificate pinning.** Today a friend pins the Puddle's TLS certificate (`tls_fp`). Through a Levee they meet the
   Levee's certificate instead. Is it enough to rest who-you-are on the signed requests and the end-to-end encryption,
   and pin (or CA-check) the Levee's certificate only as a transport? What does "trust the new certificate" mean then?
3. **Strangers and spam.** Requests for a stream must reach the Puddle, so the Levee cannot accept only friends. Either
   the Puddle tells the Levee who its friends are (a privacy cost: the operator learns the friend list), or the Levee
   rate-limits unknown senders and holds their requests apart from friends' traffic.
4. **Who runs it.** One Levee per person, or one shared by a group of friends (or a community)? This decides how a
   Puddle authenticates to it, how the operator admits Puddles (an invite token, a list of user ids), and the limits.
5. **How long it holds things.** How much and for how long a Levee keeps what waits for an offline Puddle, and what the
   sender is told when it gives up.
6. **Several Levees.** May a Puddle use two at once (for redundancy), and how would its address record list them?
7. **Direct and Levee together.** When a Puddle behind a Levee can also be reached directly, should friends prefer the
   direct address (faster, less for the Levee to see) or always go through the Levee (one path, simpler)?
