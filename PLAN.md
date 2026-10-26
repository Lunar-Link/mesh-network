# Lunar Mesh Network: Implementation Plan

*Revision 2, 26 October 2026 (Revision 1 earlier the same day). Companion to [SPEC.md](SPEC.md). This plan is a starting point to be improved as the work proceeds; revise it in place and date the revisions. Revision 2 adds Section 9: where the implementation stands, what it measured, and where it deviates from the plan.*

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
9. [Revision 2: status, measurements, deviations](#9-revision-2-status-measurements-deviations)

---

## 1. Intent

Build the node software in [SPEC.md](SPEC.md) as a portable C library, prove each layer on the host in a deterministic simulator before any radio is involved, and only then port it to the development boards. The host simulator is the authority on protocol behavior; hardware tests confirm that the radio and platform do what the port contract says.

The order of work is bottom-up through the layers, with the two hard requirements, decentralized rerouting (R4) and store-and-forward with receipts (M2, M3, M5), each getting a phase of its own and a gate of randomized simulator sweeps.

---

## 2. Ground rules

1. **C11, no heap.** Every table is a fixed array sized by `lmesh_config.h`. No `malloc`, no recursion without a fixed bound, no floating point in the protocol (ETX is fixed-point in 1/8 units).
2. **Externally clocked, never blocking.** The library runs only inside `lmesh_tick()`, `lmesh_radio_rx()`, `lmesh_radio_tx_done()`, and the API calls. It reads the clock through the port and never spins or sleeps.
3. **Radio-agnostic.** The library knows the radio through `lmesh_port` only. Nothing in `lib/lmesh/` names a radio, a band, or a modulation; that belongs in the firmware `src/` behind the port.
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
| **P6** | Target build | PlatformIO project for the Nucleo-L476RG (STM32L476RG, STM32Cube HAL), port implementation for the dev radio, provisioning of identity, serial for the line protocol. Radio-specific; see Section 6. | Two boards exchange frames; four boards pass a message over two hops with a receipt. | M | N1, N2, R3 |
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

*Revised 26 October 2026 (Rev. 2).* The board is the **Nucleo-L476RG** (STM32L476RG, Cortex-M4F, 128 kB RAM, 1 MB flash) and the toolchain is **PlatformIO** with the STM32Cube HAL framework. The folder itself is the PlatformIO project:

- `platformio.ini` at the root defines the `nucleo_l476rg` environment (ST-LINK upload and debug, 115200 baud monitor). `src/main.c` is an empty application that initializes the HAL and services SysTick; it builds and links at 588 bytes of flash.
- The library lives in `lib/lmesh/` with its `library.json`, so PlatformIO compiles it automatically with `-Wall -Wextra -Werror -Wshadow -Wconversion` on the ARM compiler. The first board build caught three sign-conversion idioms that GCC 15 on the host accepted; the ARM toolchain PlatformIO ships is older and stricter, which is a feature.
- Next in P6: implement `lmesh_port` in `src/` for the chosen radio module (SPI driver, transmit-done interrupt, airtime estimate, optional channel activity detection), provision identity from a flash page or a build flag during bring-up, wire the line protocol to the ST-LINK virtual COM port, and call `lmesh_tick()` from the main loop with sleep until `lmesh_next_wake_ms()`.
- The PlatformIO `native` environment can run the same unit tests with `pio test` if the team prefers one tool; the CMake host build stays as the simulator runner.
- Radio-specific decisions (frame size the radio supports, airtime model, channel sensing) are made in the port and must not leak into `lib/lmesh/`.

**Pin map** (`include/board.h`, set up by `src/board.c`; the clock runs at 80 MHz from the PLL on the 4 MHz MSI):

| Function | Peripheral | Pins | Notes |
|---|---|---|---|
| Radio SPI | SPI1, 8-bit mode 0, 5 MHz | PA5 SCK (D13), PA6 MISO (D12), PA7 MOSI (D11) | The pins most LoRa shields use. PA5 is also the LD2 LED, so the LED is not used. |
| Radio select, reset, interrupt | GPIO, EXTI15_10 | PB6 NSS (D10), PC7 reset (D9), PA10 DIO0 (D2) | NSS and reset idle high; DIO0 rising edge with a pull-down. |
| Second SPI | SPI2, 8-bit mode 0, 5 MHz | PB13 SCK, PC2 MISO, PC3 MOSI, PB12 NSS | Morpho headers; reserved for storage or a second radio. |
| Sensor bus | I2C1, 100 kHz | PB8 SCL (D15), PB9 SDA (D14) | Arduino I2C position. |
| Line protocol | USART2, 115200 8N1 | PA2 TX, PA3 RX | The ST-LINK virtual COM port, so a laptop needs only the USB cable. |
| Battery sense | ADC1 channel 1, 12-bit | PC0 (A5) | Through a 2:1 divider set in `board.h`; `board_battery_mv()` returns millivolts, 0 when the read fails. |
| User button | GPIO input | PC13 | |

This replaces the CubeMX-generated proposal of 26 October, which had left SPI at 4-bit data, the UART at 209700 baud 7-bit on LPUART1 pins that do not reach the ST-LINK, no radio select or interrupt line, and an output on the 32 kHz crystal pad PC14.

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
| 9 | Route-request repeat backoff (Rev. 2) | Requests for one destination repeat every 5 s with no backoff while the sequence-number bump propagates; the S6 sweep saw a mean of 86 and a worst of 665 request frames after a kill. Working assumption: acceptable for 16 nodes; add doubling of the gap per repeat if the bench shows request floods crowding data. |
| 10 | Flood airtime at 16 nodes (Rev. 2) | With status floods reaching every node, the 16-node sweeps show 23 % mean and 52 % worst channel occupancy under the simulator's airtime model (15 ms plus 1.5 ms per byte), mostly HELLO and STATUS frames, with no channel sensing. The status and HELLO periods are the knobs; measure airtime on the development radio before tuning. |
| 11 | Status collisions (Rev. 2) | Statuses are never retried, so two colliding relays on a single-path line can leave a live node stale for one period (S13 seeds 32 and 188). Working assumption: tolerable; the next period heals it. |
| 12 | Dedup window persistence cadence (Rev. 2) | Windows are saved at most every 10 s when dirty, which is a flash-wear concern under steady traffic. Working assumption: fine on FRAM or EEPROM emulation with wear leveling; revisit when the storage medium is chosen. |
| 13 | Message ids without storage (Rev. 2) | Without a storage port the id counter restarts at 1 on every boot, so X3 depends on the platform providing storage. Working assumption: the development boards provide it. |
| 14 | Eviction of an in-flight record (Rev. 2) | The store can evict a record that the link layer is transmitting; an origin record then reports Failed even if the hop accepts it, and the late link result is ignored. Working assumption: rare, since eviction only happens with a full store; exclude in-flight records from eviction if S14 or the bench shows it. |
| 15 | `delivered` counter (Rev. 2) | Counts both messages delivered to the application here and this node's own messages confirmed by receipt. Working assumption: fine for `stats`; the simulator computes delivery ratio from events. Split it if the campaign needs the two separately. |
| 16 | Shadow eviction under origin overload (Rev. 2) | SPEC 9.7 evicts origin shadows before refusing a submit, so under overload the origin reports Failed/evicted for messages that are in the network and may deliver. Options: refuse the submit before evicting a shadow (the application is told, nothing already sent loses its tracking), or a distinct terminal reason the interface shows as "sent, unconfirmed". Working assumption: keep SPEC 9.7 until the bench shows sustained overload; the demo traffic is far below it. |

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

---

## 9. Revision 2: status, measurements, deviations

*26 October 2026. Implementation waves 1 to 3 were done by one agent per module group, each owning its sources and tests; the findings below are collected from their reports and from sweeps run afterwards.*

### 9.1 Status by phase

| Phase | State |
|---|---|
| P0 Skeleton | Done. |
| P1 Frame | Done; unit tests include the RFC 4493 CMAC vectors. |
| P2 Link | Done; `link` and `flood` unit-tested. The simulator medium has per-link loss, half-duplex receivers, kill and revive, link cut, and a frame-inspection hook. |
| P3 Routing | Done. S1 to S6 pass; S3 and S6 pass 200 of 200 seeds after the route-request hop-limit fix below. |
| P4 Messaging | Done. `store`, `dedup`, `message` unit-tested (456 checks in `test_message` alone); S7 to S12, S12L and S14 pass; S9, S10 and S11 pass 200 of 200 seeds, S8, S12, S12L and S14 50 of 50. |
| P5 Status and interface | Code done: `status`, `host` unit-tested; S13 passes. The scripted interface session is not written. |
| P6 Target build | Started: PlatformIO project for the Nucleo-L476RG builds with an empty main; the library compiles clean on the ARM toolchain. Port not written. |
| P7 Bench | Not started. |

### 9.2 Measurements from the simulator (defaults, seed sweeps of 200)

| Scenario | Value | Measured |
|---|---|---|
| S1 | discovery, two nodes | 4.5 s (seed 1); ETX reaches 1.0 once the 16-HELLO window fills, about 82 s |
| S2 | route from A to D over a line of four | 6 s (seed 1); metric 3.0 plus any half-duplex loss |
| S3 | first convergence, 4 to 16 random nodes | mean 8.2 s, worst 21 s; stable convergence worst 355 s because of the HELLO collisions in 9.3 |
| S4 | reroute after killing A's next hop in a diamond | mean 30.4 s, worst 35 s (the HELLO miss limit of three periods dominates) |
| S5 | reroute after cutting the link A to B | recovery worst 56 s |
| S6 | reconvergence after a random kill with a surviving path | mean 21.8 s, worst 147 s (seed 38); request frames after the kill mean 86, worst 586; zero loops in every run |
| S13 | killed node marked stale at every other node | mean 121 s, worst 150 s after the kill (bound `LMESH_NODE_STALE_MS`) |
| S7 | text over a line of four | 500 ms submit to Delivered at the origin, 211 ms to the destination (seed 1); receipt path observed hop by hop |
| S8 | command over a line of four | executed once; the replayed receipt for a second copy carries the stored code; 5.6 s to Delivered (seed 1) |
| S9 | 100 texts at 1 per second, 30 % loss per link | delivery ratio 1.0 of accepted messages, 0 duplicates; per-run mean destination latency 113 s (p95 248 s), origin latency to Delivered 205 s (worst message 532 s); the origin refuses submits with `LMESH_ERR_FULL` once its store fills, see 9.3 |
| S10 | both relays dead, mixed priorities, one 30 s lifetime, one relay revived | all held with no frames sent; the 30 s message Expired; the rest delivered urgent first; heal to first delivery worst 22.3 s, to last 31.3 s |
| S11 | urgent saturation at loads 2 to 32, one routine message | routine handed over within 3 handovers at every load (bound 7) |
| S12 | relay killed on its hop acknowledgment (diamond) | re-sent by the origin shadow with `RESEND`, delivered once, one Delivered event; 34.3 s from kill to delivery (seed 1) |
| S12L | same on a line, relay revived after 15 s | delivered once through the revived relay, zero loops, 50 of 50 seeds (after the two fixes in 9.3) |
| S14 | 136 submits into a 64-record store | 72 evictions in the specified order, 7 refusals, counters and events agree (seed 1) |

Every run checks the route graph for loops at each sample and the per-hop metric identity (a node's metric equals its link ETX plus the next hop's metric): zero violations over all sweeps. Every message scenario also checks the four invariants of Section 4: no duplicate delivery to an application, exactly one terminal state and at most one Delivered per originated message, no DATA hop count above the node count, and every disappearance of a stored record matched to an observed custody transfer, expiry or counted eviction. Sweep CSVs with one row per run and one row per message are in `sim/results/`.

### 9.3 Deviations and findings

Design decisions are folded into SPEC.md Revision 2 and marked "(Rev. 2)" there. The rest are code facts and cleanup candidates:

- **Route requests could not reach a far destination** (fixed). Requests were flooded with the generic 4-hop limit, so after a failure a node more than four hops from the destination never received the sequence-number bump and stayed without a route while a path existed: 25 of 200 S6 seeds, every one with a post-kill diameter of 5 to 9. SPEC 6.4 now gives `ROUTE_REQ` its own limit `LMESH_ROUTE_REQ_HOPS` (default `LMESH_MAX_NODES`) and raises the `STATUS` limit to `LMESH_MAX_HOPS`. The failing seeds are kept in `sim/regression_seeds.txt` and pass.
- **The destination confirmed messages it never delivered** (fixed). The 32-bit duplicate window treated an id more than 32 behind the newest as a duplicate, answered it with a DELIVERED receipt, and dropped it; under urgent saturation (S11, load 32) hop backoff delayed urgent messages past 32 newer ids in 13 of 200 runs, 46 messages. The window is now 128 bits, an id older than the window is accepted and counted (`dedup_outside`), and the last four command results per origin are kept for receipt replay (SPEC 9.6 Rev. 2).
- **The origin's re-send died at the first relay** (fixed). A relay answered DUPLICATE from its forward cache for any copy it had forwarded before, including the origin's `RESEND` copy, so a message lost beyond the first hop (relay killed while holding the only copy, S12L) could never be recovered on a line. A relay now accepts a `RESEND` copy that is only in its cache and forwards it with the bit preserved (SPEC 9.6 Rev. 2).
- **A relay rebooting faster than the miss limit caused a short loop** (fixed). The rebooted node had no feasibility distance and adopted its neighbor's route that still pointed back through its old self, a 1 to 2 s loop in 23 of 50 S12L seeds. Three additions in SPEC 7.3 and 8.3 Rev. 2: restart detection from the HELLO sequence and own sequence number, a complete HELLO replaces the neighbor's whole vector, and an echo filter ignores finite routes from a neighbor whose vector predates the receiver's current sequence number. Zero loops in 50 S12L seeds and no change to the S3, S4, S5, S6 and S13 sweeps.
- **Origin overload is reported as refusal and as eviction.** S9 submits one message per second over 30 % loss links, more than one hop in flight can carry, so the origin's store fills with normal-priority copies; a normal submit then returns `LMESH_ERR_FULL` (correct per SPEC 9.7, and S9 counts it as a metric). Before the store fills, arrivals evict origin shadows nearest expiry, so a delivered message can be reported Failed/evicted at the origin and its receipt then finds no record: 13.6 % of messages in the pre-fix S9 sweep. See open item 16.
- **HELLO collisions can fake a neighbor loss.** With half-duplex receivers and no channel sensing, two neighbors whose HELLO timers line up for three consecutive periods lose each other's HELLOs and declare each other down (S3 seeds 46, 47, 115, traced to a 120 ms overlap three periods running). This is the medium, not the routing; a channel-busy check on the real radio or a collision model in the simulator (SPEC Section 16) would show how much of it survives on air.
- **Half-duplex loss shows in the metric.** Even at 0 % configured loss, links show ETX 1.1 to 1.25 after a dropped HELLO, so the simulator checks the metric exactly only on links with no recent drop and counts the rest as perturbed.
- **Relay forward cache API.** `lmesh_dedup_relay_accept` inserted on lookup, which would have lost a message that a relay then refused for a full store; it is now split into `lmesh_dedup_relay_seen` and `lmesh_dedup_relay_remember`, and the message layer records a forward only after custody passes.
- **Frame decode** reports every error as `LMESH_ERR_ARG`; the counters distinguish bad tag, foreign network, and malformed.
- **Host events** `boot` and `status` carry fewer fields than SPEC 11.2 (`boot` has no id, net, or reset flag; `status` carries only the node id, the full row comes from `nodes`); `last_heard_ms` in query rows is an absolute clock value.
- **Route table** pagination never triggers at the default sizes (16 destinations fit one HELLO), and a metric saturated at 254 can be infeasible against a feasibility distance of 254.
- **Status**: an older status arriving late can overwrite a newer one (no sequence check); `lmesh_status_now` sends without rescheduling the periodic one.
- **Link**: a frame already in flight to a neighbor that is declared down runs out its retries before being reported; `lmesh_link_cancel` returns false both for "on the air" and for "not queued", so `lmesh_cancel` answers `LMESH_ERR_STATE` in both cases.
- **Node**: `lmesh_radio_rx` does not refresh `now_ms`, so times stamped during receive are up to one tick (1 s) stale; lifetime accounting errs on the safe side.
- **Simulator conventions**: `lmesh_sim <scenario> [seed] [csv]` runs one scenario and appends a metrics row; `lmesh_sim sweep <scenario> <n> [--known file]` runs seeds 1 to n, skipping seeds listed in the file; `lmesh_sim regress <file>` re-runs the listed seeds; `-v` or `LMESH_SIM_VERBOSE` prints every node's event stream. CTest registers seed 1 of every scenario and 20-seed sweeps of S3, S6, S9 and S11; the host build defaults to Release so the suite runs in under half a minute.

