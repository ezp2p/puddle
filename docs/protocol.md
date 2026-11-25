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
| 05 ponds, member side | built | `src/federation/ponds.cpp`, `docs/ponds.md` |
| 05 receipts from Ponds | built: checked for a Pond with `receipts`, whose certificate is then never pinned | `src/federation/ponds.cpp`, `docs/ponds.md` "Proving who answers" |
| 05 pictures in Ponds | built: our profile's pictures sent to a Pond with `pictures`, its icon and members' pictures fetched from it | `src/federation/ponds.cpp`, `docs/ponds.md` "Pictures" |
| 08 calls | built: one to one, Puddle to Puddle (roadmap 8.1) | `src/federation/calls.cpp`, `docs/calls.md` |
| 06 Levee | built: joining, the inbox, everything out through it, custody receipts and what happens when the Levee gives up (`undelivered`, `dropped`), files left there, calls through its relay | `src/service/levee.cpp`, `src/core/levee.cpp`, `docs/levee.md` |

## What Puddle has to do next for the protocol

- **More vectors** for what the protocol does not cover yet: key chains, address records.
