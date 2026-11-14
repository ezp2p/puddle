# Puddle and the Puddle Protocol

The wire every ezp2p program speaks is specified in **`ezp2p/protocol`** (a checkout beside this one: `../protocol`).
This file says which parts Puddle implements and where. Where this repository's other documents and the protocol
disagree about the wire, the protocol wins, and the difference is a bug in one of them.

Puddle is the **reference implementation**: the protocol's test vectors were made by Puddle's code, and the
`protocol_vectors` test checks Puddle against them on every build (skipped when there is no `../protocol` checkout; set
`PUDDLE_PROTOCOL_DIR` to point elsewhere). Only when the protocol changes are they made again, with
`PUDDLE_WRITE_VECTORS=../protocol/vectors ./build/protocol_vectors_test`.

| Protocol part | Puddle | Where |
|---|---|---|
| 01 identity, key chains | built | `src/core/identity.cpp`, `docs/identity.md` |
| 01 device ids, check codes | built | `src/core/devices.cpp` |
| 02 request signatures | built | `src/http/reqsig.cpp`, `docs/streams.md` "Request signatures" |
| 02 answer receipts, handshake | built (roadmap phase 6) | `src/http/reqsig.cpp`, `src/http/router.cpp`, `src/federation/sender.cpp` |
| 03 streams | built | `src/federation/`, `api/federation.yaml`, `docs/streams.md` |
| 04 devices (phone clients) | built (roadmap phase 7) | `src/api/devices.cpp`, `src/gui/app/apiclient.cpp`, `docs/identity.md` "Devices" |
| 05 ponds, member side | built; pins a Pond's certificate until Ponds sign receipts | `src/federation/ponds.cpp`, `docs/ponds.md` |
| 05 receipts from Ponds | not yet (proposed in the protocol) | |
| 06 Levee | not yet: the address record's `levee` field and custody receipts are specified, not built | `docs/levee.md` (draft) |

## What Puddle has to do next for the protocol

- **Ponds that sign receipts** (spec 05): check them when a Pond lists the feature `receipts`, and stop pinning that
  Pond's certificate.
- **Levee** (spec 06): sign receipts over the Levee's certificate for answers that came through it; read the `levee`
  field of address records and accept that Levee's custody receipts as "waiting at their Levee".
- **More vectors** for what the protocol does not cover yet: key chains, address records.
