# Records

What core keeps in storage: one document per key, in a named collection (storage/records.hpp). These are not
sent anywhere. They are the files objects are built from.

Objects (`../objects/`) are what the owner sees. Records are what is kept. One object can draw on several
records (a User reads `people`, `streams` and `blocks`), and a record can hold things no object shows.

| Record | Collection | Key | Kept by | Objects built from it |
|---|---|---|---|---|
| person.yaml | people | user id | core/people.hpp | User, PartialUser |
| conversation.yaml | conversations | conversation id | core/conversations.hpp | Conversation, MessagePreview |

Still to model here, from what the C code stores today: streams (`puddle_stream`), groups (`puddle_group`),
outbox items (`puddle_ob_item`), blocks, invites, profile assignments.

Each file says which of its fields are **derived** (worked out from the chat logs, so they can be rebuilt) and
which are **the owner's own** (held nowhere else, so they must never be thrown away).
