# model/

Where Puddle's data and APIs are designed. Each file is YAML (JSON Schema 2020-12, or OpenAPI 3.1 for the two
APIs). The code in the matching top-level folder follows it.

```
model/
  wire/         shapes both APIs send, defined once            -> wire/
  objects/      what the owner API returns: one file per object -> objects/, api/json/
  records/      what core keeps in storage: one file per collection -> core/
  events/       what Puddles sign and send each other           -> events/
  api/          the owner API, /api/v1                           -> api/
  federation/   the stream API, /stream/v1                       -> federation/
```

## Which folder a shape belongs in

- **wire/**: the same on both sides of every connection. The profile someone shows you is the profile your Puddle
  stores and your app displays. Ids, Profile, Color, MediaRef.
- **objects/**: one Puddle's view, for its owner. It can hold things nobody else should see (blocked, unread,
  delivery state, which profile a friend is shown). Never sent to another Puddle.
- **events/**: signed facts that every Puddle must agree on. Stored and sent as their exact bytes. Each file
  says who may send the event and the rules a receiver applies (`x-sent-by`, `x-rules`).
- **api/** and **federation/**: routes. Each refers to the shapes above. Messages that exist only to run the
  protocol (asking for a stream, delivering a batch of events) are defined inside `federation.yaml`, because
  nothing else uses them.

If one shape seems to fit two of these folders, it is usually two shapes. `events/message.yaml` is what Alice
signed. `objects/message.yaml` is what Bob's app shows: the same message, with its edits applied, plus `unread`,
`delivery` and the author's name.

## How files refer to each other

```yaml
$ref: "../wire/ids.yaml#/$defs/UserId"
$ref: "../objects/partial_user.yaml"
```

The direction mirrors the code: `objects` and `events` may use `wire`, `api` uses `objects` and `wire`, and
`federation` uses `events` and `wire`. `api` never refers to `events`, and `federation` never refers to `objects`.

## Two kinds of "event"

**Events** are the signed facts in `events/`. The owner API's live stream sends **updates** (`api/openapi.yaml`,
`/api/v1/updates`): "this object changed, here it is". Today the path is `/api/v1/events`. The model renames it
so the word means one thing.
