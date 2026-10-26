# Lunar Mesh Network: Specification

*Revision 1, 26 October 2026. A starting point; expect revisions as the simulator and bench results come in.*

This document specifies the node software for the Resilient Lunar Surface Communications Network: the protocol every node runs, the formats on the wire, the behavior the interface sees, and the interface between the protocol and the platform it runs on. It is written from the capstone brief (Section 2) and is independent of the radio and the microcontroller.

Radio specifics (band, modulation, spreading factor, power, driver) are out of scope. The protocol sees the radio only through the port interface in Section 12: it hands over whole frames and receives whole frames with a quality indication.

---

## Contents

1. [Scope and conventions](#1-scope-and-conventions)
2. [Requirements](#2-requirements)
3. [System model](#3-system-model)
4. [Architecture](#4-architecture)
5. [Frame format](#5-frame-format)
6. [Link layer](#6-link-layer)
7. [Neighbor discovery and link metric](#7-neighbor-discovery-and-link-metric)
8. [Routing](#8-routing)
9. [Message layer](#9-message-layer)
10. [Status broadcasts and node table](#10-status-broadcasts-and-node-table)
11. [Local interface protocol](#11-local-interface-protocol)
12. [Platform port](#12-platform-port)
13. [Parameters](#13-parameters)
14. [Memory budget](#14-memory-budget)
15. [Requirements traceability](#15-requirements-traceability)
16. [Deferred](#16-deferred)
17. [References](#17-references)

---

## 1. Scope and conventions

**In scope:** frame format and integrity, hop-acknowledged unicast, neighbor discovery and link quality, distance-vector routing, store-carry-forward messaging with receipts and priorities, status broadcasts, the line protocol for a local interface, and the platform port the protocol needs.

**Out of scope:** the radio driver and its settings, the microcontroller board support, power management, the browser or phone interface itself, the enclosure, and the lunar environmental hardening of the hardware. Section 16 lists protocol features that are defined in outline but deferred.

**Conventions.** Multi-byte fields are little-endian. "MUST" and "SHOULD" carry their usual meaning. Node identifiers are one byte: 1 to 254 are nodes, 0 means none, 255 means all neighbors. Times inside the protocol are measured on the local node's millisecond clock, which is monotonic and wraps; no two nodes share a clock. Parameter names in `LMESH_UPPER_CASE` are compile-time constants listed in Section 13.

---

## 2. Requirements

The brief, restated as numbered requirements. Every mechanism in this document names the requirements it serves, and Section 15 maps each requirement to its mechanism and its test.

| ID | Requirement |
|---|---|
| **N1** | At least four self-contained nodes, each with an embedded processor, a terrestrial radio, and portable power. |
| **N2** | Each node has a unique identifier. |
| **N3** | Each node reports basic health and battery status. |
| **R1** | Nodes discover their neighbors automatically. |
| **R2** | Nodes form a decentralized, multi-hop network. |
| **R3** | No internet, cellular, cloud, GPS, or existing network infrastructure. |
| **R4** | The network reroutes automatically when a node or link fails and an alternate path exists. |
| **M1** | The system exchanges text, commands, and low-rate telemetry. |
| **M2** | Undelivered messages are preserved during outages. |
| **M3** | Preserved messages are forwarded automatically when connectivity returns. |
| **M4** | Every message is identifiable. |
| **M5** | Delivery is confirmed to the sender. |
| **M6** | Duplicates are prevented. |
| **M7** | Transmission is priority-based, and routine traffic is never delayed indefinitely. |
| **U1** | A local interface sends messages. |
| **U2** | The interface shows nodes, connections, health, battery status, and message state. |
| **U3** | The interface shows network failures and recovery. |
| **U4** | The interface distinguishes queued, transmitted, delivered, expired, and failed messages. |
| **U5** | The network keeps operating without the interface. |
| **D1–D5** | Demo: formation (D1), multi-hop exchange (D2), rerouting after a failure (D3), storage through a complete outage (D4), automatic delivery on recovery (D5). |
| **C1–C6** | Characterize range (C1), delivery rate (C2), latency (C3), recovery time (C4), queue behavior (C5), power (C6). |

**Constraints from the project context**, which shape the design without being brief requirements:

| ID | Constraint | Design response |
|---|---|---|
| **X1** | Operation under amateur rules (47 CFR Part 97): no encryption, station identification at least every 10 minutes while transmitting, publicly documented format. | Frames are authenticated, never encrypted. Each node carries a station identifier in its status broadcast, whose period is bounded at 10 minutes. This document is the public format. |
| **X2** | Messages may have to wait out a lunar night (about 354 hours). | Lifetimes are 32-bit seconds. The store is designed so that it can later be backed by non-volatile memory. |
| **X3** | Radiation can flip bits in memory. | Stored records carry a checksum and are verified before use. Counters that must survive a reset are restored with a margin. |
| **X4** | Nodes spend most of their life idle on battery. | The protocol is externally clocked, never busy-waits, and reports when it next needs to run so the platform can sleep. |
| **X5** | No dynamic memory after start-up; one firmware image for every node. | All tables are fixed-size arrays sized by compile-time constants. Identity is provisioned data, not code. |

---

## 3. System model

- **Nodes** are identical in software. Each has a provisioned identity: node id, network id, a 16-byte network key for authentication, and an optional station identifier of up to 8 ASCII characters.
- **Network size.** The design targets up to `LMESH_MAX_NODES` (default 16) nodes. The demo build is four to six.
- **Medium.** Half-duplex broadcast. A frame transmitted by one node is heard by zero or more nodes; reception is probabilistic and may be asymmetric. A frame is at most `LMESH_MAX_FRAME` bytes (default 120). The platform reports an on-air time estimate per frame length so that timers scale with the radio's data rate.
- **Clocks.** Each node has a free-running millisecond counter. Nothing in the protocol depends on nodes agreeing about time.
- **Failures the protocol handles.** Nodes power off and on at any time, links come and go, the network partitions and heals, frames are lost or corrupted, acknowledgments are lost, stores fill. The protocol never crashes or deadlocks on any input; malformed frames are counted and dropped.
- **Failures the protocol does not handle.** A node whose memory is corrupted beyond the record checksums, two nodes provisioned with the same id, an attacker holding the network key.

---

## 4. Architecture

```
  +------------------------------------------------------------------+
  |  Local interface      line protocol: commands in, JSON events out |
  +------------------------------------------------------------------+
  |  Message layer        store, custody, lifetimes, receipts, dedup, |
  |                       priorities, message states, status, nodes   |
  +------------------------------------------------------------------+
  |  Routing layer        HELLO, neighbor table, ETX, route table,    |
  |                       sequence numbers, retractions, requests     |
  +------------------------------------------------------------------+
  |  Link layer           frame codec, integrity tag, transmit queue, |
  |                       hop acknowledgment, retries, controlled     |
  |                       flood                                       |
  +------------------------------------------------------------------+
  |  Platform port        clock, random, radio send/receive, airtime, |
  |                       battery, health, storage                    |
  +------------------------------------------------------------------+
```

Each layer talks only to the layers next to it. Every layer is externally clocked: a single `lmesh_tick()` from the platform's main loop runs every timer, and a `lmesh_next_wake_ms()` tells the platform how long it may sleep.

| Module | Layer | Responsibility |
|---|---|---|
| `frame` | link | Encode and decode the frame header and payload; compute and verify the integrity tag. |
| `link` | link | Transmit queue, one frame in flight, hop acknowledgments, retries with backoff, link failure counting, channel-busy deferral. |
| `flood` | link | Controlled flood for status and route requests: duplicate cache, hop limit, random rebroadcast delay. |
| `neighbor` | routing | Neighbor table: liveness, HELLO loss tracking, bidirectional ETX. |
| `hello` | routing | HELLO encode and decode, periodic and triggered transmission. |
| `route` | routing | Route table: feasibility condition, selection with hysteresis, retraction, sequence-number requests, expiry. |
| `store` | message | Fixed message store: records with checksum, lifetime countdown, eviction. |
| `message` | message | Submit, forward, deliver; receipts; message states; weighted round-robin scheduler. |
| `dedup` | message | Per-origin sliding windows at the destination; recent-forward cache at relays. |
| `status` | message | Status frames out and in; node table. |
| `host` | interface | Line protocol: parse commands, format events and queries as JSON lines. |
| `node` | all | Initialization, tick, dispatch of received frames to modules, event delivery. |

---

## 5. Frame format

Every transmission is one frame.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | Version and type | High nibble version (this revision: 1). Low nibble frame type. |
| 1 | 1 | Network id | Frames with another network id are dropped before any further work. |
| 2 | 1 | Transmitter | Node id of this hop's sender. |
| 3 | 1 | Receiver | Node id of this hop's intended receiver, or 255 for all neighbors. |
| 4 | 1 | Frame sequence | Per-transmitter counter, wraps. Used to match acknowledgments and to detect a repeated transmission. |
| 5 | 1 | Flags | Bit 0 `ACK_REQ`: receiver must reply with `HOP_ACK`. Bit 1 `RETRY`: this is a retransmission of the same frame sequence. Bits 2–7 reserved, zero. |
| 6 | 1 | Payload length | 0 to `LMESH_MAX_PAYLOAD`. |
| 7 | 1 | Reserved | Zero. Reserved for a future fragment field. |
| 8 | n | Payload | Type-specific, Sections 6 to 10. |
| 8 + n | 4 | Integrity tag | Section 5.2. |

`LMESH_MAX_PAYLOAD` = `LMESH_MAX_FRAME` − 12. With the default frame size of 120 bytes the payload is 108 bytes.

### 5.1 Frame types

| Type | Name | Addressing | Acknowledged | Section |
|---|---|---|---|---|
| 0 | `HELLO` | all neighbors | no | 7 |
| 1 | `DATA` | one neighbor | yes | 9 |
| 2 | `HOP_ACK` | one neighbor | no | 6 |
| 3 | `STATUS` | all neighbors, flooded | no | 10 |
| 4 | `ROUTE_REQ` | all neighbors, flooded | no | 8.6 |
| 5–15 | reserved | | | |

### 5.2 Integrity tag

The tag covers the header and payload. Two algorithms are selected at build time:

- **CRC-32** (IEEE polynomial): detects corruption only. For bench work before keys are provisioned.
- **AES-128-CMAC truncated to 32 bits** under the network key: detects corruption and rejects frames from nodes without the key. This is the deployment setting. It authenticates; it does not encrypt (X1).

A frame whose tag does not verify is counted and dropped. Relays that rebroadcast a flooded frame recompute the tag because the transmitter, frame sequence, and hop limit change.

---

## 6. Link layer

The link layer moves one frame at a time to a neighbor or to all neighbors and tells the layer above whether a unicast frame was accepted.

### 6.1 Transmit queue

- A fixed queue of `LMESH_TXQ_SIZE` frames (default 8), each with a due time, a priority band, and a completion callback. The link layer transmits at most one frame at a time and waits for the platform's transmit-complete signal before the next.
- Frames become eligible at their due time. Among eligible frames the queue serves priority bands by weighted round-robin (Section 9.8) so that no band starves. `HOP_ACK` frames bypass the queue and go out at once because the sender's timeout is counting.
- If the platform reports the channel busy, the transmission is deferred by a random interval of one to four slot times, where a slot is the airtime of a maximum-length frame. Platforms with no channel sensing report never busy.
- Broadcast frames (`HELLO`, `STATUS`, `ROUTE_REQ`) are transmitted once and reported complete when the radio finishes.

### 6.2 Hop acknowledgment

A `DATA` frame carries `ACK_REQ`. The receiver replies immediately with a `HOP_ACK` addressed to the transmitter:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Acknowledged frame sequence |
| 1 | 1 | Status: 0 `ACCEPTED`, 1 `DUPLICATE`, 2 `REFUSED_FULL`, 3 `REFUSED_INVALID` |

Sender behavior per status: `ACCEPTED` and `DUPLICATE` mean the receiver holds the message, so custody has passed. `REFUSED_FULL` means the receiver's store is full: the sender keeps custody, backs off `LMESH_REFUSE_BACKOFF_MS`, and does not count a link failure. `REFUSED_INVALID` means the receiver could not parse the message: the sender drops it, and if it is the origin, marks it Failed.

### 6.3 Retries and link failure

- The acknowledgment timeout is airtime(frame) + airtime(`HOP_ACK`) + `LMESH_ACK_GUARD_MS`.
- On timeout the frame is retransmitted with `RETRY` set and the same frame sequence, after a random backoff of 1 to 2^k slots for attempt k, up to `LMESH_HOP_RETRIES` (default 3) retransmissions.
- When the retries are exhausted the frame is reported failed to the layer above, which keeps the message (Section 9.4). The neighbor's consecutive-failure count is incremented; at `LMESH_LINK_FAIL_LIMIT` (default 3) the neighbor is declared down (Section 7.3). A successful acknowledgment resets the count.
- The receiver remembers the last accepted frame sequence from each neighbor. A `DATA` frame with `RETRY` set and that sequence is acknowledged again with `DUPLICATE` and not delivered upward. This stops a lost acknowledgment from creating a duplicate at the next hop.

### 6.4 Controlled flood

Used by `STATUS` and `ROUTE_REQ`. Every flooded payload begins with a three-byte flood header:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Origin node |
| 1 | 1 | Flood sequence, per origin |
| 2 | 1 | Hop limit remaining |

On receipt, a node looks up (origin, flood sequence) in a cache of the last `LMESH_FLOOD_CACHE` (default 32) entries. A new entry is delivered to the owning module and, if the hop limit is above one, scheduled for one rebroadcast with the hop limit decremented after a random delay of one to three slots. A repeat is dropped. Floods start with `LMESH_FLOOD_HOPS` (default 4).

---

## 7. Neighbor discovery and link metric

Serves R1, R4, U2, U3.

### 7.1 HELLO

Every node broadcasts a `HELLO` every `LMESH_HELLO_PERIOD_MS` (default 10 s) with ±20 % jitter, and additionally as a triggered update (Section 8.5).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | HELLO sequence, per node, wraps |
| 1 | 2 | Sender's own route sequence number (Section 8.2) |
| 3 | 1 | Flags: bit 0 `TRIGGERED`, bit 1 `MORE`: the route list continues in the next HELLO |
| 4 | 1 | Neighbor count n |
| 5 | 2n | n × (neighbor id, reception quality 0–255) |
| 5 + 2n | 1 | Route count m |
| 6 + 2n | 4m | m × (destination id, destination sequence number, metric) |

Reception quality is the fraction of that neighbor's recent HELLOs this node has heard, scaled to 255. Listing a neighbor tells it that the link is bidirectional and how well it is heard; this is the "I heard you" of Babel and the reverse-direction term of ETX. The metric encoding is Section 8.1.

When the full route list does not fit in one frame, the sender rotates through it across consecutive HELLOs and sets `MORE` on all but the last page. With 16 nodes the whole list fits in one default frame.

### 7.2 Neighbor table

One entry per neighbor heard, up to `LMESH_MAX_NEIGHBORS` (default 16):

- id, time last heard, last HELLO sequence, a bitmap of the last `LMESH_ETX_WINDOW` (default 16) expected HELLOs marking which were received;
- reception quality the neighbor reports for us, and when it reported it;
- last signal quality from the platform (RSSI, SNR), for the interface and for characterization;
- consecutive hop-acknowledgment failures;
- the neighbor's advertised route list (Section 8.3).

A neighbor is **usable** for routing only once it has listed us in a HELLO, so a one-way link is never used.

### 7.3 Liveness

A neighbor is declared **down** when either no HELLO arrives for `LMESH_HELLO_MISS_LIMIT` (default 3) periods plus the jitter margin, or `LMESH_LINK_FAIL_LIMIT` consecutive unicast attempts to it fail. Declaring a neighbor down retracts every route through it (Section 8.4), raises a `neighbor down` event, and schedules a triggered HELLO. A HELLO from a down neighbor brings it back **up** at once, with its ETX window reset to a pessimistic start, and raises `neighbor up`.

### 7.4 Link metric

The metric is ETX, the expected number of transmissions to get a frame and its acknowledgment across the link:

ETX = 1 / (d_forward × d_reverse)

where d_reverse is the fraction of the neighbor's HELLOs we received and d_forward is the fraction of our HELLOs the neighbor reports receiving. Until the neighbor reports, d_forward is taken as 0.5. ETX is stored and advertised in units of 1/8 (Section 8.1). A `link quality` event is raised when a neighbor's ETX crosses a band boundary (1.0–1.5, 1.5–3, above 3), which is what the interface draws.

---

## 8. Routing

Serves R2, R4. Proactive distance-vector routing in which every node is a destination. Loop avoidance follows Babel (RFC 8966): destination sequence numbers, a feasibility condition on accepted updates, and explicit retractions. DSDV (Perkins and Bhagwat 1994) is the ancestor; the Babel rules are used because DSDV's periodic sequence-number increments bias route choice toward the shortest hop count regardless of link quality.

### 8.1 Metric

Route metrics are cumulative ETX in units of 1/8, one byte: 8 means 1.0, 254 is the largest finite value, 255 is **infinity** and means the route is retracted or unknown. A node's cost to reach a destination through neighbor N is the metric N advertises plus the ETX of the link to N, saturating at 254.

### 8.2 Sequence numbers

Each node owns a 16-bit **route sequence number** for itself. It changes only when the node receives a `ROUTE_REQ` for a number higher than its own (Section 8.6), and on start-up, when it is restored from storage plus a margin of `LMESH_SEQNO_BOOT_MARGIN` (default 16), or chosen at random if nothing is stored. Comparison is modular. A node never increments its number periodically.

### 8.3 Route table and feasibility

For each destination the node keeps: destination id, the selected next hop, the selected route's sequence number and metric, the **feasibility distance** (the lowest (sequence number, metric) pair the node has ever advertised for that destination, compared lexicographically with newer sequence numbers counting as lower), and the time the selected route was last refreshed. For each neighbor the node keeps the vector that neighbor last advertised: for every destination, (sequence number, metric, time received).

An advertisement (s, m) from a neighbor for destination D is **feasible** if m is infinity, or s is newer than the feasibility distance's sequence number, or s equals it and m is strictly smaller than its metric. Only feasible advertisements can become the selected route. This is the condition that prevents loops: a node never routes through a neighbor whose knowledge of the destination is older or worse than what the node itself has already told the network.

### 8.4 Selection, hysteresis, retraction, expiry

- **Selection.** Among feasible advertisements from usable, up neighbors, select the one minimizing metric + link ETX. Ties favor the current next hop.
- **Hysteresis.** Switch away from a working route only if the candidate is better by at least `LMESH_ROUTE_HYSTERESIS` (default 4, that is 0.5 ETX) on two consecutive evaluations. Switch at once if the current route is retracted, its next hop is down, or it has expired.
- **Retraction.** When a destination becomes unreachable (its next hop went down, or the next hop advertised infinity, and no feasible alternative exists), the node advertises (current sequence number, infinity) for it in the next `LMESH_RETRACT_REPEATS` (default 3) HELLOs and raises `route lost`. Receiving infinity from the selected next hop retracts the route immediately.
- **Expiry.** An advertisement not refreshed within `LMESH_ROUTE_HOLD_MS` (default 35 s, 3.5 HELLO periods) is treated as infinity.
- **Own entry.** A node advertises itself as (own sequence number, 0). A node is always its own best route to itself and never accepts a route to itself.
- **Hop limit.** Independently of the above, a `DATA` message whose hop count reaches `LMESH_MAX_HOPS` (default 8) is dropped and counted. This bounds the damage of any transient loop.

### 8.5 Triggered updates

A route change, a retraction, or a neighbor going down or up schedules a HELLO with the `TRIGGERED` flag after a random delay of 0 to `LMESH_TRIGGER_DELAY_MS` (default 1 s), rate-limited to one triggered HELLO per `LMESH_TRIGGER_MIN_GAP_MS` (default 2 s). Triggered HELLOs carry the complete route list. This is what makes the rerouting target (Section 15, C4) a few seconds rather than a HELLO period.

### 8.6 Sequence-number requests

When a node loses its selected route to D and every advertisement it holds for D is infeasible (a neighbor still reaches D, but with an older or worse pair than the node has advertised), it floods a `ROUTE_REQ`:

| Offset | Size | Field |
|---|---|---|
| 0 | 3 | Flood header (Section 6.4) |
| 3 | 1 | Destination D |
| 4 | 2 | Requested sequence number: the feasibility distance's number plus one |

D, on receiving a request for a number newer than its own, adopts the requested number and sends a triggered HELLO. The new number propagates and makes the surviving routes feasible. Requests for the same destination are rate-limited to one per `LMESH_ROUTE_REQ_GAP_MS` (default 5 s), and a request for a number D has already passed is ignored. Nodes other than D forward the request only through the flood mechanism; no node answers for another.

Without this mechanism a node could remain stuck after a failure even though a path exists. It is small and it completes the loop-avoidance argument, so it is part of the baseline, not a stretch item.

---

## 9. Message layer

Serves M1 to M7, N3, U4. The model is Delay/Disruption-Tolerant Networking in the spirit of Bundle Protocol version 7 (RFC 9171): a message is a unit that nodes hold, carry, and forward, with a lifetime, and custody passes hop by hop. The wire format is this document's own, because bundle encodings do not fit the frame.

### 9.1 Message header

Carried as the `DATA` payload. 10 bytes, then content.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Origin node |
| 1 | 2 | Message id: per-origin counter |
| 3 | 1 | Destination node |
| 4 | 1 | Bits 0–3 type: 0 `TEXT`, 1 `COMMAND`, 2 `TELEMETRY`, 3 `RECEIPT`. Bits 4–5 priority: 0 `URGENT`, 1 `NORMAL`, 2 `ROUTINE`. Bit 6 `RESEND`: end-to-end re-send by the origin. Bit 7 reserved. |
| 5 | 4 | Remaining lifetime, seconds |
| 9 | 1 | Hop count |
| 10 | c | Content, at most `LMESH_MAX_CONTENT` = `LMESH_MAX_PAYLOAD` − 10 bytes (98 with the default frame) |

(origin, message id) identifies a message everywhere in the network (M4). The counter is persisted every `LMESH_MSGID_SAVE_EVERY` (default 16) messages and restored with a margin of 64 on start-up, so a rebooted node never reuses a live id (X3).

### 9.2 Message types (M1)

- **TEXT**: content is UTF-8 text, delivered to the application at the destination.
- **COMMAND**: content is application-defined. The destination executes it through the application callback exactly once and returns a result code and up to `LMESH_RESULT_MAX` bytes in the receipt.
- **TELEMETRY**: content is application-defined, delivered to the application. Low-rate structured data.
- **RECEIPT**: generated by the protocol at the destination for every delivered message, addressed back to the origin. Content: original message id (2), result code (1): 0 `DELIVERED`, 1 `EXECUTED`, 2 `REJECTED`, 3 `UNSUPPORTED`; then any result bytes. A receipt inherits the priority of the message it confirms, gets a fresh message id from the destination's counter, and is stored and forwarded like any other message. Receipts are not themselves receipted.

### 9.3 Store

A fixed array of `LMESH_STORE_SIZE` (default 64) records. Each record holds the header fields, the content, and bookkeeping: role (`ORIGIN_COPY`, `RELAY`, `ORIGIN_SHADOW`), state, attempt count, next attempt time, next hop in flight, time acquired on the local clock, and a 16-bit checksum over the record. Records are verified when read; a record that fails its checksum is discarded and counted (X3). The record layout is flat and self-describing so that a later revision can mirror it to non-volatile memory without changing the protocol (X2, Section 16).

### 9.4 Custody and forwarding (M2, M3)

- A message enters a node's store when the application submits it (role `ORIGIN_COPY`) or when a `DATA` frame carrying it is accepted and acknowledged (role `RELAY`). A node acknowledges `ACCEPTED` only after the record is stored. If the store cannot take it, the node answers `REFUSED_FULL` and the sender keeps custody.
- A stored message is **eligible** when the node has a selected route to its destination (or the destination is a usable neighbor), its next attempt time has passed, and no other message is in flight. The scheduler (Section 9.8) picks the next eligible message, hands it to the link layer addressed to the route's next hop, and waits.
- On `ACCEPTED` or `DUPLICATE` the relay record is released. On `REFUSED_FULL` the record waits `LMESH_REFUSE_BACKOFF_MS`. On link failure the record stays, its attempt count increments, and its next attempt is set by exponential backoff from `LMESH_FWD_BACKOFF_MIN_MS` (default 2 s) to `LMESH_FWD_BACKOFF_MAX_MS` (default 60 s). Route selection will usually have moved to another next hop by then.
- A message with no route is **held**. It does not consume attempts. A `route changed` event for its destination or a `neighbor up` event makes every held message eligible at once; this is what drains the store when connectivity returns (M3, D5).
- A message leaves a store in exactly three ways: the next hop took custody, its lifetime reached zero, or it was evicted (Section 9.7). Nothing else drops a stored message.

### 9.5 Lifetime (M2, X2)

Lifetime is a countdown in seconds. When a node forwards a message it subtracts the whole seconds the message has been in its store, measured on its own clock. A node checks lifetimes once a second; a message whose lifetime reaches zero is removed and counted, and if the node is the origin the message's state becomes Expired. No shared clock is needed; this is the bundle-age approach of RFC 9171 Section 4.4.2. Default lifetimes: text 600 s, command 120 s, telemetry 600 s, set by the submitter; the maximum is 2^32 − 1 s, enough for a lunar night.

### 9.6 Delivery, receipts, and duplicates (M5, M6)

At the destination:

- The `dedup` module keeps, per origin, the highest message id delivered and a 32-bit bitmap of the ids before it (the sliding window of RFC 4303 Appendix A). A message inside the window and already marked, or older than the window, is a **duplicate**: it is acknowledged at the hop, not delivered again, and its receipt is re-sent. A new id is marked and delivered. Windows are persisted with the message-id counter when storage is available.
- A `COMMAND` runs once; the result is captured in the receipt. If the same command arrives again, the stored result code is returned in a fresh receipt without re-execution.
- The receipt is submitted to the store addressed to the origin and follows Section 9.4.

At relays:

- The `dedup` module keeps a recent-forward cache of `LMESH_FWD_CACHE` (default 64) (origin, message id) pairs with the time forwarded. A `DATA` frame whose message is in the store or in the cache is acknowledged `DUPLICATE` and discarded. Entries age out after `LMESH_FWD_CACHE_HOLD_MS` (default 600 s).

At the origin:

- When the origin's copy is accepted by a next hop, the record becomes an `ORIGIN_SHADOW`: the content is kept, the state is Transmitted, and an end-to-end timer starts at `LMESH_E2E_RETRY_MIN_MS` (default 30 s), doubling to `LMESH_E2E_RETRY_MAX_MS` (default 300 s). If the timer fires before the receipt, the shadow is re-queued with `RESEND` set and forwarded again; relays and the destination dedup it. This covers a relay that loses power while holding the only copy.
- A matching receipt sets the state to Delivered, raises the event with the result code, and releases the shadow. Receipts for unknown or already-delivered messages are ignored.
- Receipts carry the `RESEND`-independent original id, so the first receipt to arrive wins whichever copy produced it.

### 9.7 Eviction

When a message must be stored and the store is full, the node evicts in this order: the `RELAY` record of `ROUTINE` priority nearest to expiry, then `NORMAL`, then `ORIGIN_SHADOW` records nearest expiry, then `ORIGIN_COPY` records of lower priority than the arrival. An `URGENT` record is never evicted for a non-urgent arrival. If nothing is evictable the arrival is refused. Evicting an origin record sets its state to Failed. Evictions are counted and reported in status.

### 9.8 Priority without starvation (M7)

Three classes: `URGENT`, `NORMAL`, `ROUTINE`. The scheduler keeps one queue per class ordered by submission time and serves them by weighted round-robin with weights `LMESH_WRR_WEIGHTS` = 4 : 2 : 1 (Katevenis, Sidiropoulos, and Courcoubetis 1991). A class that has an eligible message is never skipped more than six times in a row, so a routine message is handed to the link layer within seven transmissions of becoming eligible whatever the urgent load. The link layer applies the same weights across its priority bands. Within a class, the order is oldest first.

### 9.9 Message states (U4)

The origin tracks every message it created:

| State | Set when |
|---|---|
| **Queued** | Submitted; no other node has accepted it. Also after an end-to-end re-send is queued. |
| **Transmitted** | A next hop acknowledged `ACCEPTED` or `DUPLICATE`. |
| **Delivered** | A receipt arrived. Terminal. |
| **Expired** | Lifetime reached zero at the origin with no receipt. Terminal. |
| **Failed** | Evicted at the origin, refused as invalid, or the receipt reported `REJECTED` or `UNSUPPORTED`. Terminal. |

Every transition raises a `message state` event with the reason. Terminal records are kept for `LMESH_HISTORY_HOLD_MS` (default 600 s) in a small history ring so the interface can show them after the fact, then recycled.

---

## 10. Status broadcasts and node table

Serves N3, U2, U3, X1.

Every `LMESH_STATUS_PERIOD_MS` (default 60 s, ±10 s jitter) each node floods a `STATUS` frame. The period MUST NOT exceed 600 s so that the station identifier satisfies X1. Status is never stored or retried; a newer status replaces an older one.

| Offset | Size | Field |
|---|---|---|
| 0 | 3 | Flood header (Section 6.4) |
| 3 | 2 | Battery, millivolts (0 = unknown) |
| 5 | 1 | Health flags: bit 0 low battery, bit 1 store above 75 %, bit 2 radio fault, bit 3 reset since last status, bit 4 no usable neighbors, bits 5–7 application-defined |
| 6 | 4 | Uptime, seconds |
| 10 | 1 | Store depth, messages held |
| 11 | 1 | Messages expired or evicted since the last status, saturating |
| 12 | 1 | Station identifier length L, 0 to 8 |
| 13 | L | Station identifier, ASCII |
| 13 + L | 1 | Neighbor count n |
| 14 + L | 2n | n × (neighbor id, link ETX in 1/8 units) |

Every node keeps a **node table** of up to `LMESH_MAX_NODES` entries: the last status received from each node and when, plus the neighbor list it carried. The result is that any node can describe the whole reachable network: its nodes, their health and battery, and their links, which is what the interface draws (U2). A node whose status has not been heard for `LMESH_NODE_STALE_MS` (default 150 s, 2.5 periods) is marked stale and the interface greys it (U3).

---

## 11. Local interface protocol

Serves U1 to U5. The protocol is a line-oriented text protocol over whatever byte stream the platform offers (USB serial first). The node never waits on it: it is a viewer and a message source, and the mesh runs identically with nothing attached (U5). A terminal program is a complete fallback for a browser page.

### 11.1 Commands in

One command per line, space-separated fields, text to the end of the line.

| Command | Effect |
|---|---|
| `id` | Print this node's identity. |
| `send <dest> <prio> <lifetime_s> <text>` | Submit a `TEXT` message. `prio` is `u`, `n`, or `r`. |
| `cmd <dest> <lifetime_s> <text>` | Submit a `COMMAND` at `NORMAL` priority. |
| `tel <dest> <lifetime_s> <hex>` | Submit `TELEMETRY` with hex-encoded content. |
| `cancel <id>` | Drop a message this node originated, if not yet transmitted. |
| `nodes` | Dump the node table. |
| `links` | Dump the neighbor table with ETX, RSSI, SNR, state. |
| `routes` | Dump the route table. |
| `msgs` | Dump every message this node originated or holds, with state. |
| `stats` | Dump counters: frames sent and received by type, tag failures, acknowledgment timeouts, duplicates, evictions, expiries, airtime. |
| `status` | Send a status broadcast now. |

Every command is answered by one JSON line with `"ok":true` or `"ok":false` and a reason. Dumps are answered by one JSON line per row followed by `{"end":"<command>"}`.

### 11.2 Events out

One JSON object per line, written as events happen. The `ev` field names the event; `t` is the node's millisecond clock.

| `ev` | Fields | Source |
|---|---|---|
| `neighbor` | `id`, `state` (`up`, `down`), `etx` | Section 7.3 |
| `link` | `id`, `etx`, `rssi`, `snr` | Section 7.4 |
| `route` | `dest`, `next` (0 if lost), `metric`, `seq` | Section 8.4 |
| `msg` | `origin`, `id`, `dest`, `state`, `reason`, `type`, `prio` | Section 9.9 |
| `rx` | `origin`, `id`, `type`, `prio`, `hops`, `text` or `hex` | Section 9.6 |
| `status` | `node`, `batt_mv`, `flags`, `uptime_s`, `store`, `lost`, `neighbors` [[id, etx], ...], `stale` | Section 10 |
| `store` | `depth`, `evicted`, `expired` | Section 9.7 |
| `boot` | `id`, `net`, `reset` | start-up |

Numbers are decimal; ETX and metrics are printed as decimals (for example `1.25`). The interface page is specified elsewhere; this protocol is its only dependency on the node.

---

## 12. Platform port

The protocol is a library with no operating-system or hardware dependency. The platform provides the functions below through a structure of function pointers at initialization, and calls the library's entry points from its main loop and from its radio events.

**Provided by the platform** (`lmesh_port`):

| Function | Contract |
|---|---|
| `now_ms()` | Monotonic millisecond counter; wraps at 2^32. |
| `random()` | 32 random bits; quality matters only for jitter. |
| `radio_send(frame, len)` | Begin transmitting one frame. Returns 0 if accepted. The platform calls `lmesh_radio_tx_done()` when the radio is idle again. Only one send is outstanding at a time. |
| `radio_busy()` | Optional. True if the channel is sensed busy. Null means never busy. |
| `airtime_ms(len)` | Estimated on-air time of a frame of `len` bytes at the current radio settings. Every protocol timer scales from this. |
| `battery_mv()` | Optional. Battery voltage for status. |
| `health_flags()` | Optional. Application-defined bits for status. |
| `storage_read(slot, buf, len)`, `storage_write(slot, buf, len)` | Optional. Small non-volatile slots for counters and windows. Null means volatile only. |

**Called by the platform** (`lmesh.h`):

| Function | When |
|---|---|
| `lmesh_init(node, identity, port, callbacks)` | Once at start. |
| `lmesh_tick(node)` | From the main loop, as often as convenient. Runs every timer and state machine; never blocks. |
| `lmesh_next_wake_ms(node)` | After a tick, to learn how long the platform may sleep before the next tick is needed. |
| `lmesh_radio_rx(node, frame, len, rssi, snr)` | For every received frame. |
| `lmesh_radio_tx_done(node, ok)` | When a transmission finishes. |
| `lmesh_send(...)`, `lmesh_cancel(...)` | From the application or the interface. |
| `lmesh_neighbors()`, `lmesh_routes()`, `lmesh_messages()`, `lmesh_nodes()`, `lmesh_stats()` | Queries for the interface. |
| `lmesh_host_command(node, line, out, max)`, `lmesh_host_format_event(ev, out, max)` | The line protocol, if the platform uses it. |

**Provided by the application** (`lmesh_callbacks`):

| Callback | Contract |
|---|---|
| `on_event(ev)` | Every event in Section 11.2, as a structure. Must return promptly. |
| `on_command(from, data, len, result)` | Execute a command and fill in the result code and bytes. Must return promptly; long operations return `EXECUTED` and report later by telemetry. |

The radio port is deliberately thin: whole frames in, whole frames out, an airtime estimate, and an optional busy check. Spreading factor, bandwidth, frequency, power, and driver details live entirely in the platform.

---

## 13. Parameters

Compile-time constants in `lmesh_config.h`; each can be overridden with a `-D` flag.

| Constant | Default | Note |
|---|---|---|
| `LMESH_MAX_FRAME` | 120 | Bytes per frame including header and tag. |
| `LMESH_MAX_NODES` | 16 | Route table, node table, dedup windows. |
| `LMESH_MAX_NEIGHBORS` | 16 | Neighbor table. |
| `LMESH_TXQ_SIZE` | 8 | Frames. |
| `LMESH_HOP_RETRIES` | 3 | Retransmissions per unicast frame. |
| `LMESH_ACK_GUARD_MS` | 200 | Added to two airtimes for the acknowledgment timeout. |
| `LMESH_LINK_FAIL_LIMIT` | 3 | Consecutive failures before a neighbor is down. |
| `LMESH_FLOOD_HOPS` | 4 | Initial hop limit for floods. |
| `LMESH_FLOOD_CACHE` | 32 | (origin, sequence) pairs. |
| `LMESH_HELLO_PERIOD_MS` | 10000 | ±20 % jitter. |
| `LMESH_HELLO_MISS_LIMIT` | 3 | Missed periods before a neighbor is down. |
| `LMESH_ETX_WINDOW` | 16 | HELLOs in the loss estimate. |
| `LMESH_ROUTE_HOLD_MS` | 35000 | Advertisement expiry. |
| `LMESH_ROUTE_HYSTERESIS` | 4 | 0.5 ETX, in 1/8 units. |
| `LMESH_RETRACT_REPEATS` | 3 | HELLOs carrying a retraction. |
| `LMESH_TRIGGER_DELAY_MS` | 1000 | Maximum delay of a triggered HELLO. |
| `LMESH_TRIGGER_MIN_GAP_MS` | 2000 | Rate limit on triggered HELLOs. |
| `LMESH_ROUTE_REQ_GAP_MS` | 5000 | Rate limit on requests per destination. |
| `LMESH_SEQNO_BOOT_MARGIN` | 16 | Added to the restored sequence number. |
| `LMESH_MAX_HOPS` | 8 | Message hop limit. |
| `LMESH_STORE_SIZE` | 64 | Records. |
| `LMESH_MSGID_SAVE_EVERY` | 16 | Messages between counter saves; restore margin 64. |
| `LMESH_FWD_BACKOFF_MIN_MS` | 2000 | After a hop failure. |
| `LMESH_FWD_BACKOFF_MAX_MS` | 60000 | Cap. |
| `LMESH_REFUSE_BACKOFF_MS` | 5000 | After `REFUSED_FULL`. |
| `LMESH_E2E_RETRY_MIN_MS` | 30000 | Origin re-send, first interval. |
| `LMESH_E2E_RETRY_MAX_MS` | 300000 | Origin re-send, cap. |
| `LMESH_FWD_CACHE` | 64 | Relay recent-forward entries. |
| `LMESH_FWD_CACHE_HOLD_MS` | 600000 | Relay cache hold. |
| `LMESH_WRR_WEIGHTS` | 4, 2, 1 | Urgent, normal, routine. |
| `LMESH_HISTORY_HOLD_MS` | 600000 | Terminal records kept for the interface. |
| `LMESH_RESULT_MAX` | 32 | Command result bytes. |
| `LMESH_STATUS_PERIOD_MS` | 60000 | ±10 s jitter; must stay at or below 600000. |
| `LMESH_NODE_STALE_MS` | 150000 | Node table entry greyed. |
| `LMESH_STATION_ID_MAX` | 8 | Characters. |

Timers that depend on the radio's data rate (acknowledgment timeout, backoff slots, flood delays) are derived from `airtime_ms()` at run time, not from constants.

---

## 14. Memory budget

Estimate with the defaults, 16 nodes, 16 neighbors, 64 records, 120-byte frames.

| Structure | Size |
|---|---|
| Neighbor table, 16 × about 24 B | 0.4 kB |
| Advertised vectors, 16 neighbors × 16 destinations × 4 B | 1.0 kB |
| Route table, 16 × about 16 B | 0.3 kB |
| Message store, 64 × about 128 B | 8.2 kB |
| Dedup windows, 16 × 8 B, and forward cache, 64 × 8 B | 0.6 kB |
| Node table, 16 × about 48 B | 0.8 kB |
| Transmit queue, 8 × about 128 B | 1.0 kB |
| Flood cache, history ring, counters | 0.5 kB |
| **Total** | **about 13 kB** |

This fits any candidate microcontroller with 64 kB or more of RAM alongside a platform layer. The store dominates and scales linearly with `LMESH_STORE_SIZE`.

---

## 15. Requirements traceability

| Req. | Mechanism | Section | Simulator test | Hardware test | Demo |
|---|---|---|---|---|---|
| N1 | Platform port; library is hardware-independent | 12 | Builds for host and target | Four nodes run the image | D1 |
| N2 | Provisioned node id; station identifier | 3, 10 | Duplicate id detected in the node table | `id` command on each node | D1 |
| N3 | Status broadcast, node table | 10 | Every node's status reaches every node | Battery tracks a bench sweep | D1 |
| R1 | HELLO, neighbor table, bidirectional check | 7 | Nodes started in any order discover each other | Power-on order varied | D1 |
| R2 | Every node routes and relays; no special node | 8 | Random topologies converge; killing any single node leaves the rest routing | Two-hop message | D1, D2 |
| R3 | No infrastructure dependency in the port | 12 | — | No GPS fitted; laptop offline | D1 |
| R4 | Liveness, retraction, triggered HELLO, feasibility, requests | 7.3, 8.4–8.6 | Kill a relay with an alternate path: traffic resumes; no loops; recovery time logged | Relay powered off | D3 |
| M1 | Message types | 9.2 | Text, command, telemetry between arbitrary pairs | Over two hops | D2 |
| M2 | Store, hold with no route, lifetime | 9.3–9.5 | Partition: messages wait; one expires | Relays powered off | D4 |
| M3 | Held messages eligible on route or neighbor event | 9.4 | Heal: every unexpired message delivers unattended | Relay powered on | D5 |
| M4 | (origin, id), persisted counter | 9.1 | Ids unique across a reboot | `msgs` dump | D2 |
| M5 | Receipts, origin shadow, end-to-end re-send | 9.6 | Exactly one Delivered per message; relay killed mid-path still delivers | Receipt in the interface | D2 |
| M6 | Hop-level repeat check, relay cache, destination window | 6.3, 9.6 | 30 % loss with re-sends: each message delivered once | Duplicate counter zero in a soak | — |
| M7 | Weighted round-robin in scheduler and queue | 9.8, 6.1 | Saturate urgent: routine handed over within seven transmissions | Mixed burst order logged | D5 |
| U1 | `send`, `cmd`, `tel` | 11.1 | Parser tests | Checklist | D2 |
| U2 | `nodes`, `links`, `status` and `link` events | 10, 11 | Formatter tests | Checklist | D1 |
| U3 | `neighbor`, `route`, stale flag | 7.3, 8.4, 10 | — | Relay off: events appear | D3 |
| U4 | Message states and events | 9.9 | State machine driven through every transition | Checklist | D2–D5 |
| U5 | Node never blocks on the interface | 11 | — | Laptop unplugged mid-run | D5 |
| C1–C5 | Counters, per-frame RSSI/SNR, events with local timestamps | 11.2 | Scenario sweeps produce the metrics | Campaign | — |
| C6 | `lmesh_next_wake_ms()` lets the platform sleep | 12 | — | Current measurement per state | — |

---

## 16. Deferred

Defined in outline here so that the baseline leaves room for them; not part of the first implementation.

- **Fragmentation** of content longer than one frame (header byte 7 reserved for it).
- **Non-volatile store**: mirroring store records and dedup windows to flash or FRAM so messages survive a power loss (X2). The record checksum and flat layout are in the baseline for this reason.
- **Duty-cycled receive and sleep**: the protocol already reports its next wake time; the platform decides when to sleep. Preamble-sniffing receive without time synchronization is the candidate mechanism.
- **Replay protection** without a shared clock: a per-transmitter frame-sequence window at the link layer under the CMAC tag.
- **Broadcast text** to all nodes, as a flooded message type without receipts.
- **Time distribution** for timestamps in the interface, by carrying the sender's clock in HELLO and adopting the highest.
- **Link-layer collision model** in the simulator, to study C2 under load before hardware.
- **Multiple frame sizes per radio**: the port can report a smaller `LMESH_MAX_FRAME` than compiled; the baseline compiles one size.

---

## 17. References

- Perkins, C. and Bhagwat, P. (1994). "Highly Dynamic Destination-Sequenced Distance-Vector Routing (DSDV) for Mobile Computers." *SIGCOMM*. Destination sequence numbers.
- Chroboczek, J. and Schinazi, D. (2021). RFC 8966, "The Babel Routing Protocol." Feasibility condition (Section 3.5.1), retractions, sequence-number requests (Section 3.8.2), hello and "I heard you" link sensing.
- De Couto, D., Aguayo, D., Bicket, J., and Morris, R. (2003). "A High-Throughput Path Metric for Multi-Hop Wireless Routing." *MobiCom*. ETX.
- Fall, K. (2003). "A Delay-Tolerant Network Architecture for Challenged Internets." *SIGCOMM*. Store-carry-forward with custody.
- Burleigh, S., Fall, K., and Birrane, E. (2022). RFC 9171, "Bundle Protocol Version 7." Lifetime (Section 4.2.1) and bundle age without synchronized clocks (Section 4.4.2).
- Kent, S. (2005). RFC 4303, "IP Encapsulating Security Payload," Appendix A. Sliding-window duplicate detection.
- Katevenis, M., Sidiropoulos, S., and Courcoubetis, C. (1991). "Weighted Round-Robin Cell Multiplexing in a General-Purpose ATM Switch Chip." *IEEE JSAC*. Weighted round-robin with a starvation bound.
- Song, J. H., Poovendran, R., Lee, J., and Iwata, T. (2006). RFC 4493, "The AES-CMAC Algorithm." Frame authentication.
- 47 CFR Part 97, §97.113 (prohibited transmissions, including encryption) and §97.119 (station identification).
- NASA senior design brief, "Resilient Lunar Surface Communications Network," and the team's Milestone 1 report (October 2026), from which Section 2 is restated.
