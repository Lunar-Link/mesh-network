# mesh-network

Node software for the Resilient Lunar Surface Communications Network capstone: a portable C library that makes a set of identical radio nodes discover each other, route around failures, and carry messages through outages until they can be delivered and confirmed. It is written to the brief's requirements and is independent of the radio and the microcontroller.

**Status (27 October 2026):** every layer is implemented and unit-tested on the host, and all fourteen simulator scenarios of PLAN.md Section 5 pass, with the routing and messaging sweeps at 200 of 200 seeds. The board firmware now has the platform port with flash storage, the line protocol on the USB serial port, and a driver for the REYAX RYLR999 radio; it builds but has not yet run on a board. A driver for the Waveshare Core1121-LF (Semtech LR1121) at 433 MHz was added on 31 October and tested against a fake chip; no module has run it yet. This is a starting point to be improved.

| Document | Contents |
|---|---|
| [SPEC.md](SPEC.md) | Requirements, system model, architecture, wire formats, state machines, port interface, parameters, traceability. |
| [PLAN.md](PLAN.md) | Ground rules, phases with exit criteria, test strategy, simulator scenarios, target build, open items. |

## Layout

```
mesh-network/
  platformio.ini      board build: Nucleo-L476RG (STM32L476RG), STM32Cube HAL, one environment per radio driver
  src/                board application and platform port (table below)
    radio/<driver>/     one folder per radio driver; the build environment picks one
  include/            firmware headers: pin map, port, radio interface, node identity
  lib/lmesh/          the protocol library, found by PlatformIO automatically
    library.json        manifest and the library's compiler flags
    include/lmesh/      public headers, included by the platform and the application
    src/                library modules, one per protocol concern
  sim/                host simulator: virtual clock and medium, scenario runner
  tests/unit/         host unit tests
  CMakeLists.txt      host build: library, simulator, tests
```

The folder is a PlatformIO project: `src/` is the firmware and `lib/lmesh/` is the library in the PlatformIO library layout, so it can also be a dependency of another project. The simulator and tests build only with CMake on the host and are never compiled into firmware. The library has no radio or board code; that belongs in `src/` behind the port in `lmesh_port.h`.

### Public headers

| File | Contents |
|---|---|
| `lmesh.h` | The API: init, tick, next wake time, radio receive and transmit-done entry points, send and cancel, HELLO ping, queries for the interface, the line protocol. |
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
| `neighbor.c` | routing | Neighbor table: φ accrual liveness from HELLO arrival intervals, failed unicasts, restart detection, HELLO loss bitmap, bidirectional ETX. |
| `hello.c` | routing | HELLO encode and decode, periodic, triggered and ping sends, pagination of the route list, replacement of a neighbor's route vector. |
| `route.c` | routing | Route table: feasibility condition, selection with hysteresis, retraction, expiry, sequence-number requests. |
| `store.c` | message | Fixed message store: checksummed records, lifetime countdown, eviction order, message-id counter. |
| `message.c` | message | Submit, forward, deliver; custody transfer; receipts; origin re-send; message states and events; weighted round-robin scheduler. |
| `dedup.c` | message | Per-origin sliding windows at the destination; recent-forward cache at relays. |
| `status.c` | message | Status frame encode and decode, node table, station identifier. |
| `host.c` | interface | Line protocol: command parser, JSON formatting of events and table dumps. |
| `node.c` | glue | Initialization, tick, dispatch of received frames to modules, event delivery, queries. |
| `internal.h` | all | Byte-order helpers, wrap-safe time comparison, shared helpers. |

### Firmware

| File | Contents |
|---|---|
| `src/main.c` | Start-up and the main loop: console, radio, `lmesh_tick()` when due or after input, sleep between interrupts. |
| `src/board.c`, `include/board.h` | 80 MHz clock, pin map, radio SPI and UART, I2C, USB serial, battery ADC, hardware RNG, radio reset line. |
| `src/port.c` | The `lmesh_port` functions: SysTick clock, hardware random numbers, the radio driver, battery voltage; the node identity. |
| `src/console.c` | Line protocol on the ST-LINK virtual COM port: command lines in, replies and events out, never blocking. |
| `src/storage.c` | The library's storage slots in a wear-leveled log over the top 128 KB of flash, safe against power loss mid-write. |
| `src/serial.c` | Interrupt-driven UART receive and transmit rings shared by the console and UART radios. |
| `src/stm32l4xx_it.c` | Interrupt handlers, including the NMI a flash ECC error raises. |
| `include/radio.h` | The interface every radio driver implements: init, send, airtime estimate, poll, and optional channel sensing. |
| `include/node_config.h` | Node id, network id, network key and callsign, set per board with `-D` flags. |
| `src/radio/rylr999/` | REYAX RYLR999 (868/915 MHz LoRa, AT commands over UART4): configures the module, sends frames as base64url text, parses `+RCV`, estimates airtime with Semtech's formula. |
| `src/radio/lr1121/` | Waveshare Core1121-LF (Semtech LR1121 over SPI1): own SPI command layer with BUSY handling, bring-up with TCXO, RF switch and calibration, LoRa with raw binary frames, CAD channel sensing, power down. |
| `src/radio/none/` | No radio: frames go nowhere and complete after the simulator's airtime model. For testing the console on one board. |

### Simulator and tests

| File | Contents |
|---|---|
| `sim/sim_net.c` | Virtual medium: per-link up/down and loss, half-duplex receivers, node kill and revive, link cut, event-driven clock stepping, frame and iteration hooks; the port implementation for simulated nodes. |
| `sim/sim_topo.c` | Topologies: line, diamond, random connected graphs from a seed, BFS distances, diameter, kill candidates that keep the graph connected. |
| `sim/sim_check.c` | Observation and checks: event and frame capture, convergence tracking, route-graph loop walk, per-hop metric identity, metrics and CSV output. |
| `sim/sim_msg.c` | Message tracking: per-message state history, exactly-once delivery, one terminal state per message, hop-count bound, store-record disappearance audit. |
| `sim/sim_scenarios.c` | Scenarios S1 to S14 of PLAN.md Section 5 plus S12L. |
| `sim/sim_console.c` | `lmesh_console [line\|ring\|diamond\|random] [nodes] [seed] [loss_pct]`: a simulated network with one node's line protocol on stdin and stdout, advanced by `@t <ms>` lines; `@kill`, `@revive`, `@cut`, `@join`, `@attach`, `@topo` control it. Drives `../interface` without boards. |
| `sim/sim_main.c` | Runner: `lmesh_sim <scenario> [seed] [csv]`, `lmesh_sim sweep <scenario> <seeds> [--known file]`, `lmesh_sim regress <file>`; `-v` prints every node's event stream. |
| `sim/regression_seeds.txt` | Seeds that once failed, one `<scenario> <seed>` per line; `regress` re-runs them and `sweep --known` skips them. Empty when everything passes. |
| `sim/results/` | Sweep output: one CSV row per run and one per message for the S9 and S11 sweeps. |
| `tests/unit/test_smoke.c` | Initialization contract, argument validation, tick and wake bound. `test_<module>.c` holds each module's tests. |
| `tests/unit/test_lr1121.c` | The LR1121 driver on a fake chip (`lr1121_fake/board.h` stands in for the HAL): command bytes against the user manual, BUSY, reception, and CAD through the real link layer. Built three ways: 433 MHz, HF path, carrier test. |

## Techniques

- **Layered, externally clocked library.** Link, routing, and message layers behind a thin platform port. A single `lmesh_tick()` runs every timer; `lmesh_next_wake_ms()` lets the platform sleep. No heap, no blocking, no floating point.
- **Authenticated frames.** An 8-byte header and a 4-byte tag, CRC-32 for the bench or truncated AES-128-CMAC under a network key. Authentication without encryption keeps the design inside amateur-radio rules.
- **Hop-acknowledged unicast with custody.** Each hop acknowledges a data frame only after storing the message, so custody passes hop by hop. Timeouts and backoffs scale from the radio's airtime estimate rather than fixed constants.
- **Proactive distance-vector routing.** Periodic HELLOs carry each node's route vector; link cost is bidirectional ETX. Loop avoidance follows the Babel routing protocol: destination sequence numbers, a feasibility condition on accepted updates, explicit retractions, triggered updates, and sequence-number requests to unblock after failures. A neighbor that reboots faster than the down-detection time is recognized from its HELLO and its stale route vector dropped, and a node ignores a neighbor's routes while that neighbor still advertises an old sequence number for it.
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
./build/lmesh_sim S4 1
./build/lmesh_sim sweep S6 200
```

The browser interface for a node is in `../interface`; `python ../interface/serve.py --sim` runs it against `lmesh_console`.

CTest runs the unit tests, seed 1 of every scenario, and 20-seed sweeps of S3, S6, S9 and S11. The build defaults to Release; a sweep of 200 seeds takes seconds.

The board firmware builds with PlatformIO (the VS Code extension, or `pio` from its core). Each radio driver has an environment; `rylr999` is the default, `lr1121` is the 433 MHz design radio, and `none` runs without a radio:

```
pio run
pio run -e lr1121
pio run -e none
pio run -t upload
pio device monitor
```

Give each board its own node id at build time, and the callsign before transmitting (PowerShell shown):

```
$env:PLATFORMIO_BUILD_FLAGS='-DNODE_ID=2 -DSTATION_ID=\"N0CALL\"'
pio run -t upload --upload-port COM5
```

Message ids, the route sequence number and the duplicate windows survive resets and re-flashing; a full chip erase clears them. On connect the node prints a `boot` event and, with the RYLR999, a `radio` event with `"state":"ready"` once the module is configured; then the commands of SPEC.md Section 11 work from any serial terminal.

**RYLR999 wiring.** Module RXD_LoRa to A0 (PA0), TXD_LoRa to A1 (PA1), RST to D9 (PC7), GND to GND. The module needs 5 V at up to 950 mA at +30 dBm, more than the Nucleo's USB supply gives, so power it from a separate 5 V supply with grounds joined. The driver defaults to +10 dBm for the bench. The driver queries the module's settings at start and writes only those that differ, because the module keeps them in flash. RF settings are `-D` flags in `rylr999.c` and must match on every node. It never sets the module's `AT+CPIN` password, which would encrypt the link (not allowed under Part 97).

**Core1121-LF wiring.** The module is 3.3 V only, on supply and logic, and the driver needs every signal below. Pin numbers are the schematic's; check them against the signal names printed on the module.

| Module pin | Signal | Nucleo pin | Arduino header | `board.h` name |
|---|---|---|---|---|
| 8 | 3V3 | 3V3 | CN6 | |
| 2, 7, 15 | GND | GND | CN6 | |
| 14 | NSS | PB6 | D10 | `RADIO_NSS` |
| 13 | SCK | PA5 | D13 | `RADIO_SCK` |
| 12 | MOSI | PA7 | D11 | `RADIO_MOSI` |
| 11 | MISO | PA6 | D12 | `RADIO_MISO` |
| 10 | NRESET | PC7 | D9 | `RADIO_RESET` |
| 9 | BUSY | PA9 | D8 | `RADIO_BUSY` |
| 3 | DIO9 | PA10 | D2 | `RADIO_DIO0` (the board's radio interrupt line, not the chip's DIO0) |
| 1, 4, 5, 6, 16 | ANT, DIO8, DIO7, NC, 2.4G_ANT | not connected | | |

**LR1121 settings.** Every RF setting is a `-D` flag at the top of `lr1121.c`, checked with `_Static_assert`, and must match on every node. The defaults are the RF study's: 433 MHz, LoRa SF9, 125 kHz, CR 4/5, preamble 12, explicit header, CRC on, RX boosted, HP PA at +22 dBm with a 48 µs ramp. The sub-GHz limits are the module's 410 to 510 MHz, not the chip's 150 to 960 MHz. Frames go on air as raw bytes, and the driver never uses the chip's crypto engine, so nothing is encrypted (Part 97). `STATION_ID` works as with the other drivers. The `lr1121` environment uses the same mesh timers as `rylr999`; keep the two in step. `-DLR1121_HF=1` selects the module's other path, the HF PA and the 2.4G_ANT connector, at 2425 MHz and +13 dBm by default.

The driver senses the channel with CAD through `radio_channel_busy()`, which the port hands to the library as `radio_busy`. The link layer calls it before each queued frame and on busy puts the frame back for 1 to 4 slots; hop acknowledgments go out without sensing. `-DLR1121_CAD=0` turns it off. The call blocks the main loop for the CAD, about 17 ms at SF9.

### Core1121-LF bring-up checklist

Do these in order, on one board first. Nothing here has run on real hardware yet, so each step is also a test of the driver. Stop at the first step that fails. A failed bring-up prints a `radio` error event and retries every 2 s.

**Before powering anything**

1. Confirm the module is 3.3 V from the Nucleo and shares its ground. 5 V on any pin can damage it.
2. Connect the antenna or a dummy load to the sub-GHz connector before any transmission. The schematic calls it ANT2 (`LORA_ANT`); ANT1 is 2.4 GHz. Match them against Waveshare's pinout picture, since the wiki does not state which is which in text. Do not transmit with this connector open.
3. Set the callsign: `$env:PLATFORMIO_BUILD_FLAGS='-DNODE_ID=1 -DSTATION_ID=\"YOURCALL\"'`.

**Step 1: SPI and version readback, no RF needed**

1. `pio run -e lr1121 -t upload`, then `pio device monitor`.
2. Pass: `{"ev":"radio","driver":"lr1121","state":"ready","version":"xx.03.xx.xx",...}` within a second of the `boot` event. The second byte `03` means LR1121 in transceiver mode; note the firmware version in the last two bytes.
3. If it fails, read the `reply` field:

| Reply | Likely cause | Check |
|---|---|---|
| `boot timeout` | BUSY stuck high | BUSY on D8, NRESET on D9, 3.3 V present. |
| `spi` | BUSY went high and stayed there during a command | Same, then the SPI wires. |
| `not an LR1121 in transceiver mode` | Wrong bytes back, or other firmware | MISO and MOSI swapped. A use case of `DF` is the bootloader; Waveshare's demo package has a firmware update example. |
| `errors 0x0020` | TCXO did not start | Try `-DLR1121_TCXO_DELAY=600`, then other `LR1121_TCXO_TUNE` codes; ask Waveshare for the TCXO part. |
| `errors 0x0010` or `0x0080` | Image calibration or PLL lock | Band and `LR1121_CAL_FREQ1`/`2` settings. |

**Step 2: transmit power**

1. Connect the sub-GHz connector through at least 30 dB of attenuation to a power meter or spectrum analyzer rated for +22 dBm. This step keys a carrier, so use a dummy load path, not an antenna.
2. Build with a 30 s carrier after bring-up: `$env:PLATFORMIO_BUILD_FLAGS='-DLR1121_TEST_CW_MS=30000 -DSTATION_ID=\"YOURCALL\"'`, then `pio run -e lr1121 -t upload`.
3. Record output power, frequency error at 433.000 MHz, and supply current. Expect about +22 dBm less the switch and filter losses; the datasheet gives +19 dBm minimum and +22 typical for the HP PA, ±2 dB step accuracy, about 100 mA at 434/490 MHz, and Waveshare states 115 mA at +22 dBm.
4. Semtech's PA settings are for its own 868 to 928 MHz match. If power is low or harmonics are high, try `LR1121_PA_DUTY` 2 to 4 with `LR1121_PA_HP_SEL` 7, and record what gave the best result. Check the second and third harmonics against Part 97 limits.
5. Rebuild without `LR1121_TEST_CW_MS` when done, or the node keys up at every reset.

**Step 3: two-node link**

1. Flash a second board with `-DNODE_ID=2`, otherwise identical. Start with `-DLR1121_POWER_DBM=0` and the nodes a few meters apart, or use attenuators, so the receivers are not overloaded.
2. Pass: each node prints a `neighbor` event within two HELLO periods (120 s). `links` shows the other node with plausible RSSI and SNR.
3. `send 2 n 600 hello` on node 1. Pass: `rx` on node 2, and Delivered on node 1. Repeat 100 times and read `stats` on both.
4. Time a frame on a scope, DIO9 rise against the start of `SetTx`. The driver's estimate is 243 ms on air for 30 bytes, 653 ms for 120, plus a 10 ms margin.

**Step 4: channel sensing, which the study's delivery figures assume**

1. With node 2 sending a long frame, confirm node 1 defers: its `tx_frames` in `stats` stop rising until the frame ends, and the driver's `cad_busy` counter rises in the debugger.
2. Find the CAD detection limit. Step attenuation on the link and record the detection rate at the demodulation limit and 6 dB below it. The study's harness treats everything down to 6 dB below as detected. Tune `LR1121_CAD_PEAK`, `LR1121_CAD_MIN` and `LR1121_CAD_SYMBOLS`; the manual says these must be tested at sensitivity to limit false detections, and Semtech's value of 53 comes from its own measurements.
3. Measure false detections with both nodes silent.
4. Measure time from the start of CAD to the first transmitted symbol. Compare with the 26 ms estimate. It is how long two nodes can both see a clear channel.

Warnings are errors on every target. Override a parameter with, for example, `-DCMAKE_C_FLAGS=-DLMESH_STORE_SIZE=32`.

## Conventions

C11, no dynamic allocation, no radio names in `lib/lmesh/`, American English, and minimal comments: one line at the top of a file saying what it is, and a line before a large section when its purpose is not evident.
