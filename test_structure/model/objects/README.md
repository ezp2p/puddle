# Objects

What the owner API returns, worked out from the C code in `src/core/`: the structs that hold the data, and the
handlers that write it as JSON today. Each object file has:

- **`x-source`**: where it comes from in the C code (struct, writer, where it is stored), and how today's JSON
  differs from the model (`today`)
- **`x-internal`**: fields the struct holds that the owner API deliberately does not show, with why, so nothing
  is lost from sight while designing
- **`x-methods`**: what can be done to it, and the route each does it with

## The objects

| Object | In the C code | Stored in | Notes |
|---|---|---|---|
| **User** | nowhere as one thing | records `people` + `streams` + `blocks` | The main new object. Today the same person comes back five different ways, and the profile lives inside the stream record. |
| PartialUser | – | – | User's short form, embedded wherever a person appears |
| SelfUser | `get_me` | identity + `profiles.yaml` | As today |
| **Stream** | `puddle_stream` | `state/streams/<id>.json` | `peer` becomes `user`. Eleven protocol fields stay internal. |
| **Conversation** | `conversation` in `messages.c` | records `conversations` (today: built per request) | Gains an `id`, a `name` for DMs, and the whole Group for groups |
| **Message** | `puddle_view_msg` + outbox item | `chats/<chat>/<day>.jsonl` | `sender` becomes `author`; group notices become messages with a `type` |
| MessagePreview | `conversation.last_*` | the conversation record's `last` | What a conversation shows of its latest message. This is what the code sends; the old spec said a full Message. |
| **Group** | `puddle_group` | group record + its events | Ids become PartialUsers |
| Member | `puddle_gmember` | inside the group | Gains `since`, which the struct already has |
| OwnProfile | `puddle_profile` | `profiles.yaml` | As today |
| Identity | `puddle_identity` | `identity/` | As today |
| Invite | `puddle_invite` | `state/invites.tsv` | As today |
| Block | `puddle_block` | `state/blocks.tsv` | `user` becomes a PartialUser |
| AccessPolicy | `puddle_access` | `state/access-policy` | As today |
| Error | `puddle_fail` | – | Written as the code sends it |

Lists are a `Page` (`../api/page.yaml`), not an object of their own.

## Things the code has that are not objects

- **Read markers** (`puddle_streams_read_marker`): when the owner last read each chat. They move into the
  conversation record (`read_at`) and appear only as `Conversation.unread`.
- **Outbox items** (`puddle_ob_item`): what this Puddle still owes other Puddles. The owner sees only the part
  that concerns a message (`Message.delivery`). The rest is protocol traffic (answers to requests, key updates,
  profile pushes). If the owner ever needs to see it, it would be an object of its own.
- **Profile assignments** (`puddle_assignments`): which profile each peer sees. Shown as `Stream.profile`.
- **Chunks** (`puddle_chunk`): day summaries for comparing logs between Puddles. Federation only.

## Where the old spec and the code disagreed

Writing these from the code turned up places where `api/openapi.yaml` describes something the code doesn't do.
The model follows the code:

- **Paging:** the code pages every list with `limit` + `offset` and answers with `next` (an offset). The spec
  said messages page with `before`.
- **Lists:** they come back wrapped (`{"streams": [...], "next": ...}`), not as bare arrays.
- **A conversation's latest message:** sent as a short `last` (with a preview), not a full Message.
- **Conversations:** a group's carries `members` (a count) and `sort_ts`, and a DM's carries `stream`. None of
  these was in the spec.
- **Errors:** `{"error": "<code>"}`, plus `message` from some handlers. The spec described a `code` field that
  the code never sends.
