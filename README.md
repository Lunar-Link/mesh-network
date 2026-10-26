# mesh-network

Node software for the Resilient Lunar Surface Communications Network capstone: a portable C library that makes a set of identical radio nodes discover each other, route around failures, and carry messages through outages until they can be delivered and confirmed. It is written to the brief's requirements and is independent of the radio and the microcontroller.

**Status (26 October 2026):** specification, implementation plan, and a compiling skeleton. The protocol logic is not implemented yet; the skeleton builds with warnings as errors, and the smoke tests pass. This is a starting point to be improved.

| Document | Contents |
|---|---|
| [SPEC.md](SPEC.md) | Requirements, system model, architecture, wire formats, state machines, port interface, parameters, traceability. |
| [PLAN.md](PLAN.md) | Ground rules, phases with exit criteria, test strategy, simulator scenarios, target build, open items. |

## Layout

```
mesh-network/
  include/lmesh/      public headers, included by the platform and the application
  src/                library modules, one per protocol concern
  sim/                host simulator: virtual clock and medium, scenario runner
  tests/unit/         host unit tests
  CMakeLists.txt      host build: library, simulator, tests
```

`src/` and `include/` follow the PlatformIO library layout so the folder can later be a dependency of a board firmware project without rearranging it. The simulator and tests sit outside `src/` and are never compiled into firmware.

### Public headers

| File | Contents |
|---|---|
| `lmesh.h` | The API: init, tick, next wake time, radio receive and transmit-done entry points, send and cancel, queries for the interface, the line protocol. |
| `lmesh_config.h` | Compile-time parameters with defaults, each overridable by a `-D` flag. |
| `lmesh_types.h` | Node identifiers, error codes, priorities, message types and states, events, query rows, counters, identity. |
| `lmesh_port.h` | The platform port: clock, random, radio send, channel busy, airtime estimate, battery, health, storage. The application callbacks: events and command execution. |
| `lmesh_state.h` | The complete node state as fixed-size tables, so the application can allocate it statically. |

### Library modules

| File | Layer | Contents |
|---|---|---|
| `frame.c` | link | Frame header codec, CRC-32 and AES-CMAC integrity tag, network-id filter. |
| `aes.c` | link | AES-128 forward cipher used by the CMAC tag. |
| `link.c` | link | Transmit queue with due times and priority bands, one frame in flight, hop acknowledgments with status codes, retries with airtime-scaled backoff, repeat detection, link failure counting. |
| `flood.c` | link | Controlled flood for status and route requests: duplicate cache keyed by origin and sequence, hop limit, random rebroadcast delay. |
| `neighbor.c` | routing | Neighbor table: liveness from missed HELLOs and failed unicasts, HELLO loss bitmap, bidirectional ETX. |
| `hello.c` | routing | HELLO encode and decode, periodic and triggered sends, pagination of the route list. |
| `route.c` | routing | Route table: feasibility condition, selection with hysteresis, retraction, expiry, sequence-number requests. |
| `store.c` | message | Fixed message store: checksummed records, lifetime countdown, eviction order, message-id counter. |
| `message.c` | message | Submit, forward, deliver; custody transfer; receipts; origin re-send; message states and events; weighted round-robin scheduler. |
| `dedup.c` | message | Per-origin sliding windows at the destination; recent-forward cache at relays. |
| `status.c` | message | Status frame encode and decode, node table, station identifier. |
| `host.c` | interface | Line protocol: command parser, JSON formatting of events and table dumps. |
| `node.c` | glue | Initialization, tick, dispatch of received frames to modules, event delivery, queries. |
| `internal.h` | all | Byte-order helpers, wrap-safe time comparison, shared helpers. |

### Simulator and tests

| File | Contents |
|---|---|
| `sim/sim_net.c` | Virtual medium: per-link up/down and loss, half-duplex receivers, node kill and revive, event-driven clock stepping; the port implementation for simulated nodes. |
| `sim/sim_main.c` | Scenario runner, `lmesh_sim <scenario> [seed]`; scenarios are added here as phases complete. |
| `tests/unit/test_smoke.c` | Initialization contract, argument validation, tick and wake bound. `test_<module>.c` holds each module's tests. |

## Techniques

- **Layered, externally clocked library.** Link, routing, and message layers behind a thin platform port. A single `lmesh_tick()` runs every timer; `lmesh_next_wake_ms()` lets the platform sleep. No heap, no blocking, no floating point.
- **Authenticated frames.** An 8-byte header and a 4-byte tag, CRC-32 for the bench or truncated AES-128-CMAC under a network key. Authentication without encryption keeps the design inside amateur-radio rules.
- **Hop-acknowledged unicast with custody.** Each hop acknowledges a data frame only after storing the message, so custody passes hop by hop. Timeouts and backoffs scale from the radio's airtime estimate rather than fixed constants.
- **Proactive distance-vector routing.** Periodic HELLOs carry each node's route vector; link cost is bidirectional ETX. Loop avoidance follows the Babel routing protocol: destination sequence numbers, a feasibility condition on accepted updates, explicit retractions, triggered updates, and sequence-number requests to unblock after failures.
- **Store-carry-forward messaging.** Messages wait in a fixed store while there is no route and go out when one appears. Lifetimes count down on each node's own clock, so no shared time is needed. The origin keeps a shadow copy and re-sends end to end until a receipt arrives.
- **Exactly-once delivery.** Repeat detection at the hop, a recent-forward cache at relays, and per-origin sliding windows at the destination; commands run once and their result is replayed in later receipts.
- **Priority without starvation.** Weighted round-robin across urgent, normal, and routine classes with weights 4:2:1, bounding a routine message's wait to seven handovers.
- **Status floods and a node table.** Each node floods a small health frame about once a minute; every node holds a table of all nodes and their links, so the interface can be plugged into any node.
- **Line protocol for the interface.** Text commands in, one JSON object per line out. The node never waits on it.
- **Host simulation first.** Deterministic, seeded scenarios with built-in invariant checks gate each phase before hardware.

## Build and test

Requires CMake 3.16 or newer and a C11 compiler.

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/lmesh_sim smoke 1
```

Warnings are errors on every target. Override a parameter with, for example, `-DCMAKE_C_FLAGS=-DLMESH_STORE_SIZE=32`.

## Conventions

C11, no dynamic allocation, no radio names in `src/`, American English, and minimal comments: one line at the top of a file saying what it is, and a line before a large section when its purpose is not evident.
