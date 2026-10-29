# Group chats

A group is a chat with several people. It follows the rule of everything else here: Puddles talk to each other directly, there is no server in the
middle, and nothing is public.

## What a group is

- **Its id** is made from who made it, when, and a random nonce: `base32(SHA-256("group\n" creator "\n" ts "\n" nonce))`, written into its first
  event, `chat.create`. Anyone given that event can check that the id is the one its parts make. (The first event cannot contain its own hash, which
  is why the id is not simply the event's hash.)
- **Its log** is kept on every member's Puddle, like a direct message's: signed events, each stored as it was signed. `message`, `edit` and `redact`
  are exactly the direct-message events, with the group's id as the chat; what a reader sees is worked out from them the same way.
- **Its members** are worked out from five more events:

| event | by | meaning |
|---|---|---|
| `chat.create` | the creator | the group exists; `name`, `nonce` |
| `chat.rename` | any member | `body` is the new name |
| `member.invite` | any member | `member` is the invitee |
| `member.join` | the invitee | yes |
| `member.leave` | a member or an invitee, for themselves | out (or a no to an invitation) |

There is no one in charge. The rules (`puddle_group_apply`) are the same on every Puddle, so the same events give the same members:
only a member can invite or rename; a join counts only from someone invited and only if it is newer than the invitation; a leave newer than
your last change takes you out; a later invitation can bring a leaver back; the newest name wins (the same moment is broken by the name's own
order, so everyone picks alike); never more than 50 members and invited together. **A member can invite anyone they are friends with. Nobody can
remove someone else.** Leaving is up to the one who leaves.

## Invitations

An invitation travels over a friendship (`POST /stream/v1/group/invite`, from a friend): the group's `chat.create`, the inviter's signed
`member.invite` naming us, and who is in the group with where to reach them. We can check the id and the inviter's word. We take the roster
on the inviter's word, because we have nothing else to take it on yet. The invited owner sees it as an invitation (Pending, next to friend
requests). Accepting shows **a notice that the other members will be able to see your address**.

Saying yes sends `member.join` to every member we can reach, and, for those we cannot reach yet, asks for a group-only connection (below).
Saying no sends a `member.leave` to whoever invited us and nobody else.

## Group-only connections ("scoped streams")

Often the people in a group do not know each other. So two members who share no stream get one that is only for groups:

1. The one who joins asks each member they hold no stream with (`POST /stream/v1/access`, with `scope` set), and the request carries their own
   signed `member.join` as proof that someone in the group invited them. The address to ask at comes from the invitation.
2. The member checks that they are in that group **or invited to it and not yet answered** (whoever says yes first asks the others, and the others may not have got to the invitation yet; refusing would leave the first one cut off for good), that the requester is invited (or already in), that the join is the requester's own and signed by
   the key in the chain they sent, and that the requester is not blocked. Then the join is applied, the stream is recorded in the state `scoped`,
   and the answer says yes (`access/result` with `scoped:true`, which can only answer a request made for a group). If we then turn the invitation down, the connection that was only for that group is let go like any other.
   A request that is never answered is asked again, after three minutes and then every quiet minute the Puddle looks, within the receiver's limit of five requests an hour per person.
3. Both sides then tell each other that they have joined, once, since a member met this way has no other way to learn it.

What a scoped stream allows: group events (for groups both are in), key and address updates, and the pictures in each other's profile. It does
not allow direct messages, friend requests or anything else a friend can do. If one of them later asks the other to be friends, that is an
ordinary request, and the group connection carries on while it waits. When the last group two people share is left, a connection that was only
for groups is closed (and the other side is told). A block closes it at once and always wins.

**What it costs: every member learns every other member's address**, which is usually an IP. This is the same as exchanging phone numbers in a group
chat, and so it is treated: the person is told before they join, and there is no mode that hides it. (A relay through the creator would hide
addresses but lose everything else: the group would stop whenever the creator was offline.)

## Messages

A message is signed by its author and sent by the author directly to every member they have an open stream with (friend or group-only). There is
no relaying, so nobody can drop or reorder what someone else wrote. Delivery is the same outbox as direct messages: offline members are retried,
and a message that cannot be delivered says why. What the owner sees about a sent message is the worst of what became of it across the members.

A member who joins later does not get the messages from before: the invitation carries the members and the name, not the history.

## Blocks

A blocked person is rejected at the door like anywhere else: they cannot connect to us, and we do not send to them. In a group you share this
means neither of you receives the other's messages; the rest of the group is unaffected, and you stay in it. Accepting an invitation is refused
while someone in the group is blocked.

## Limits and caveats

- 50 members and invited together; 50 invitations waiting for the owner.
- A member hears only from members they have a connection with. If a connection cannot be made (the member's address changed while both were
  away), their messages do not arrive until it is.
- The roster in an invitation is the inviter's word. A forged roster only affects the one who trusts that inviter.
- Nobody can remove another member; neither can the creator. Decided for now; see `discuss-later.md`.
