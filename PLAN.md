# Lunar Mesh Network: Implementation Plan

*Revision 1, 26 October 2026. Companion to [SPEC.md](SPEC.md). This plan and the skeleton it describes are a starting point to be improved as the work proceeds; revise it in place and date the revisions.*

---

## Contents

1. [Intent](#1-intent)
2. [Ground rules](#2-ground-rules)
3. [Phases](#3-phases)
4. [Test strategy](#4-test-strategy)
5. [Simulator scenarios](#5-simulator-scenarios)
6. [Target build](#6-target-build)
7. [Open items](#7-open-items)
8. [Risks](#8-risks)

---

## 1. Intent

Build the node software in [SPEC.md](SPEC.md) as a portable C library, prove each layer on the host in a deterministic simulator before any radio is involved, and only then port it to the development boards. The host simulator is the authority on protocol behavior; hardware tests confirm that the radio and platform do what the port contract says.

The order of work is bottom-up through the layers, with the two hard requirements, decentralized rerouting (R4) and store-and-forward with receipts (M2, M3, M5), each getting a phase of its own and a gate of randomized simulator sweeps.

---

## 2. Ground rules

1. **C11, no heap.** Every table is a fixed array sized by `lmesh_config.h`. No `malloc`, no recursion without a fixed bound, no floating point in the protocol (ETX is fixed-point in 1/8 units).
2. **Externally clocked, never blocking.** The library runs only inside `lmesh_tick()`, `lmesh_radio_rx()`, `lmesh_radio_tx_done()`, and the API calls. It reads the clock through the port and never spins or sleeps.
3. **Radio-agnostic.** The library knows the radio through `lmesh_port` only. Nothing in `src/` names a radio, a band, or a modulation.
4. **Host first.** Every module gets unit tests on the host, and every protocol behavior gets a simulator scenario, before it runs on a board.
5. **Warnings are errors.** `-Wall -Wextra -Werror -Wshadow -Wconversion` on GCC and Clang, `/W4 /WX` on MSVC.
6. **Minimal comments.** One line at the top of each file saying what it is, and a line before a large section when its purpose is not evident. Nothing describing how a line works.
7. **One image for every node.** Identity is provisioned data passed to `lmesh_init()`; there are no node-specific builds.
8. **Deterministic tests.** Simulator runs take a seed; a failing seed is a reproducible bug. Sweeps run many seeds and report the worst case.
9. **American English** in identifiers, documents, and output.

---

## 3. Phases

Sizes are relative: S is a day or two, M up to a week, L more than a week, for one developer. Requirements in the last column are the ones the phase closes in the simulator.

| # | Phase | Contents | Exit criterion | Size | Closes |
|---|---|---|---|---|---|
| **P0** | Skeleton | Folder layout, public headers, config, port and callback structures, module headers, stub sources, host build, simulator harness with a virtual clock and medium, smoke test. *This pass.* | Builds with warnings as errors; smoke test initializes N nodes and ticks them. | S | — |
| **P1** | Frame and integrity | `frame` encode/decode with bounds checks, CRC-32 tag, AES-128-CMAC tag, network-id filter, counters. | Unit tests: round trip of every type, every malformed input rejected, tag vectors from RFC 4493 pass. | S | — |
| **P2** | Link layer | `link` transmit queue with due times and bands, one frame in flight, `HOP_ACK` with status codes, retries with airtime-scaled backoff, repeat detection, link failure counting, channel-busy deferral; `flood` with duplicate cache and hop limit. Simulator medium gains per-link loss and airtime occupancy. | Unit tests on the queue and acknowledgment state machine. Simulator: unicast over a lossy link delivers exactly once to the layer above; three failures mark the neighbor down; a flood reaches every node once with the hop limit honored. | M | — |
| **P3** | Neighbors and routing | `neighbor` table with HELLO loss bitmap and bidirectional ETX; `hello` encode/decode, periodic and triggered sends, pagination; `route` table with feasibility, selection with hysteresis, retraction, expiry, sequence-number requests; events. | Scenarios S1–S6. Randomized sweep: 200 seeds of random topologies up to 16 nodes, random node kills with an alternate path; every destination with a path converges, zero forwarding loops detected by hop count, recovery time distribution recorded. | L | R1, R2, R4 |
| **P4** | Messaging | `store` records with checksum, lifetimes, eviction; `message` submit, forward, hold, custody transfer, receipts, origin shadow and end-to-end re-send, states and events, weighted round-robin; `dedup` windows and forward cache; command execution callback. | Scenarios S7–S12. Randomized sweep: partition and heal with 30 % loss and random relay kills; every unexpired message delivers exactly once; exactly one Delivered event per message; routine wait bounded at seven handovers under urgent saturation. | L | M1–M7, U4 |
| **P5** | Status and interface | `status` frames and node table, station identifier; `host` line protocol: command parser, JSON formatter for events and dumps; `lmesh_next_wake_ms()`. | Scenario S13. Unit tests on parser and formatter. A scripted session against the simulator exercises every command and sees every event type. | M | N3, U1–U3, U5 |
| **P6** | Target build | PlatformIO library manifest and example application for the development boards (STM32 or ESP32 as the team settles), port implementation for the dev radio, provisioning of identity, serial for the line protocol. Radio-specific; see Section 6. | Two boards exchange frames; four boards pass a message over two hops with a receipt. | M | N1, N2, R3 |
| **P7** | Bench and campaign | Failure injection on the bench, soak run, the characterization campaign (C1–C6), demo script D1–D5 three times in a row. | Data, plot, and method for each of C1–C6; demo rehearsal clean. | M–L | C1–C6, D1–D5 |

P1 to P5 need no hardware. P6 can start as soon as boards arrive, in parallel with P3, by porting the P0 skeleton and proving the frame exchange. Deferred features in SPEC Section 16 come after P7 or when a phase has slack.

---

## 4. Test strategy

**Unit tests** (`tests/unit/`): one executable per module, plain C with a tiny assertion macro, no framework. They test encoders, decoders, tables, and state machines in isolation with fabricated inputs and a fake clock. They run under CTest on the host.

**Simulator scenarios** (`sim/`): N library instances in one process share a virtual medium. The medium delivers each transmitted frame to every node whose link from the transmitter is up, after the frame's airtime, dropping it with the link's loss probability. It supports killing and reviving nodes, cutting and restoring links, changing loss, and advancing the virtual clock in steps. Scenarios are deterministic given a seed. Each scenario checks an invariant (Section 5) and records metrics (delivery ratio, latency, recovery time, duplicates, store depth) to a CSV so that the characterization figures can be previewed before hardware exists.

**Randomized sweeps**: scenarios S3, S6, S9, S11 run over many seeds as the gate for P3 and P4. A failure prints its seed; the seed is added to a regression list.

**Hardware tests** (P6, P7): the same scenarios re-expressed as bench procedures with power switches instead of simulator kills, using the `stats` and `msgs` dumps and the event stream with laptop timestamps as the instrument.

**Invariant checks built into the simulator**:

- No message is delivered to an application more than once (M6).
- Exactly one terminal state per originated message (U4).
- No frame's hop count exceeds the number of nodes (loop detector).
- A stored message disappears only by custody transfer, expiry, or counted eviction.
- Every node's route table is loop-free after convergence (follow next hops from every node to every destination).

---

## 5. Simulator scenarios

| # | Scenario | Layer | Checks | Maps to |
|---|---|---|---|---|
| S1 | Two nodes, perfect link | routing | Both discover each other within two HELLO periods; ETX 1.0. | R1 |
| S2 | Line of four, A–B–C–D | routing | A has a route to D through B within three periods; metric equals the sum of link ETX. | R2, D2 |
| S3 | Random connected topology, 4–16 nodes | routing | Every pair converges; route graph loop-free; record convergence time. Sweep. | R2 |
| S4 | Diamond A–B–D, A–C–D; kill B | routing | A's route to D moves to C; record time from kill to new route; no loop. | R4, D3 |
| S5 | Diamond; cut link A–B only | routing | Same as S4 by link failure rather than node death. | R4 |
| S6 | Random topology, random single kill with surviving path | routing | Every surviving pair reconverges; sequence-number requests observed when needed. Sweep. | R4, C4 |
| S7 | Line of four, text A to D | message | Queued, Transmitted, Delivered at A; receipt path logged; latency recorded. | M1, M5, D2 |
| S8 | Line of four, command A to D | message | Executed once at D; result code in receipt; second copy not re-executed. | M1, M6 |
| S9 | Line of four, 30 % loss on every link, 100 messages | message | Each delivered once; duplicate counter zero at the application; delivery ratio and latency recorded. Sweep. | M6, C2, C3 |
| S10 | Diamond; kill both relays; send mixed priorities; one 30 s lifetime; revive one relay | message | Messages hold at Queued; the 30 s message goes Expired; on revival the rest deliver urgent-first with no external action. | M2, M3, M7, D4, D5 |
| S11 | Saturate urgent at A toward D; one routine message | message | Routine handed to the link layer within seven handovers. Sweep over load. | M7, C5 |
| S12 | Relay killed while holding the only copy | message | Origin shadow re-sends; delivered exactly once; one Delivered event. | M5, M6 |
| S13 | Six nodes, status for ten minutes | status | Every node's table shows every other node and its links; a killed node goes stale within 2.5 periods. | N3, U2, U3 |
| S14 | Store filled during an outage | message | Eviction order as specified; counters match; origin records marked Failed. | C5 |

---

## 6. Target build

The team's working assumption is **PlatformIO** for the development boards, STM32 or ESP32, as the simplest shared toolchain. Nothing is started yet. When P6 begins:

- Add a `library.json` at the folder root so the library is a PlatformIO dependency: `src/` and `include/` are already laid out as PlatformIO expects, and `sim/` and `tests/` sit outside `src/` so they are not compiled into firmware.
- Put the board application in a separate PlatformIO project (for example `firmware/` beside this folder) that depends on this library by path, implements `lmesh_port` for the chosen board and radio, provisions identity from flash or a build flag during bring-up, and wires the line protocol to the USB serial.
- The PlatformIO `native` environment can run the same unit tests with `pio test` if the team prefers one tool; the CMake host build stays as the simulator runner.
- Radio-specific decisions (frame size the radio supports, airtime model, channel sensing) are made in the port and must not leak into `src/`.

---

## 7. Open items

Each has a working assumption so that work proceeds.

| # | Item | Working assumption |
|---|---|---|
| 1 | Frame size on the final radio | 120 bytes compiled; the port may be given a smaller size later (SPEC Section 16). |
| 2 | Receipt priority | Inherits the original's priority. Revisit if receipts crowd urgent traffic in S11. |
| 3 | HELLO period versus airtime on the development radio | 10 s. Measure airtime in P6 and lengthen if HELLOs exceed 10 % of the channel. |
| 4 | Whether relays should bump lifetimes on `REFUSED_FULL` waits | No; the countdown is honest. |
| 5 | Persisting store records | Deferred (SPEC Section 16); the record layout is ready for it. |
| 6 | Station identifier placement | In STATUS only. Confirm it satisfies the identification rule for the mesh as operated, with the licensed team member as control operator. |
| 7 | Simulator collision model | Not in P2; add before P7 if load tests on the bench show collisions dominate. |
| 8 | Interface page | Out of this folder's scope. The line protocol is its contract. |

---

## 8. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Routing fails to converge in the sweeps | P3 slips | The feasibility condition and requests are the published Babel rules; compare behavior against RFC 8966 before inventing fixes. Keep the hop limit as the safety net. |
| Store and dedup interplay produces duplicates or lost messages | P4 slips | The simulator's invariant checks run in every scenario; add a scenario for each bug found. |
| Timers tuned for the simulator are wrong on air | Bench surprises | Every timer scales from `airtime_ms()`; measure airtime first in P6 and feed it to the simulator. |
| Channel contention without slotting | Delivery ratio under load | Jitter on all timers, busy check where the radio offers it, small HELLOs; measure on the bench; collision model if needed. |
| Memory on the final microcontroller | Store size | Budget in SPEC Section 14; `LMESH_STORE_SIZE` is the knob. |
| Scope creep from the deferred list | P3–P5 slip | Deferred items wait for P7 or slack; the baseline is the brief. |
