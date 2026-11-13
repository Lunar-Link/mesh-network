# interface

A browser page for a laptop plugged into a mesh node. It shows the node's status, neighbors, routes and the whole network from status broadcasts, and it sends and receives messages. It speaks only the line protocol of [SPEC.md](../SPEC.md) Section 11, so it works with any node and needs no internet connection.

**Status (27 October 2026):** first version. Tested against the simulator (8 nodes, kill, reroute, plug into another node, text and telemetry delivered); the USB path has not yet run against a board.

## Files

| File | Contents |
|---|---|
| `index.html` | The page: one file, no libraries. Web Serial for USB, a simulator mode, polling of the node's tables, the send form, the console. |
| `serve.py` | Serves the page on localhost (Web Serial needs a secure origin) and, with `--sim`, runs `lmesh_console` and bridges it to the page. Python standard library only. |

## Use with a board

Requires Chrome or Edge (Web Serial). Close any other serial monitor on the port first (PlatformIO, PuTTY).

```
python serve.py
```

Open http://localhost:8000, press **Connect USB**, pick the ST-LINK port (115200 baud). The page asks for `id` and then polls `links`, `routes`, `nodes`, `msgs` and `stats` every 5 s, and sooner after an event.

## Use without a board

Build the simulator console from the repository root (`cmake --build build`), then from this folder:

```
python serve.py --sim --speed 10
```

Open http://localhost:8000/?sim. The page connects to node 1 of a simulated network running the real library. The simulator bar can plug into any node, kill and revive nodes, and cut or join links. Options: `--topo line|ring|diamond|random`, `--nodes N`, `--seed S`, `--loss PCT`, `--speed X` (simulated seconds per real second).

The simulator panel also changes the run while it goes; the command-line options only set where it starts:

- **Clock:** pause and run, speed from ×0.1 to ×100, and **Step** to advance a set amount of simulated time (0.5 s to 15 min) per click, or press → . Stepping pauses the clock first.
- **Link loss:** one loss percentage applied to every link that is up, and to links joined from the panel.
- **Random outages:** the share of time nodes and links spend down, and the mean outage length. Outages start at random and end on their own; the plugged-in node can be spared. **Heal all** ends them now.
- **Scheduled downtime:** take a node down after a delay, for a time, once or repeating (for example 300 s down every 900 s, like a node that sleeps to save power).
- **Restart network:** a new topology, node count or seed without restarting `serve.py`.

All of these run in `serve.py` in simulated time, using the console's `@t`, `@kill`, `@revive`, `@cut`, `@join` and `@topo` lines, so they keep working when the clock is stepped or sped up. Nodes and links taken down this way are labeled in the panel; a node killed by hand is never revived automatically.

## Terrain link colors

The Network graph defaults to **Terrain margin (dB)**: red below 10 dB, yellow from 10 to below 20 dB, and green at 20 dB or above. Margin is received power above the radio sensitivity, not RSSI or SNR. An undirected line uses the weaker of the two directional margins. Hover or keyboard-focus a line to read its margin. Gray means the margin is unknown, a node is offline, or the link was manually cut. Dashed lines are predicted-only, unavailable, or based on stale reports; a colored prediction is not a delivery confirmation. **Delivery quality (ETX)** retains the original measured-quality colors. The color selection is saved in this browser.

The ordinary ring/line/random simulator does not have terrain margins. To drive the graph with the existing terrain link table, rebuild and launch from the repository root:

```
cmake --build build
python3 interface/serve.py --sim --terrain sim/terrain/plan0.txt --speed 5 --port 8001
```

Open `http://localhost:8001/?sim`. The table controls node count, relay deployment times, LoRa airtime, and time-varying loss, RSSI and SNR, using the same terrain model as the command-line scenario. The graph stays circular; its positions do not represent physical distance. In terrain color mode, all modeled pairs between online nodes are shown, including weak pairs the protocol has not discovered. The included table has four nodes. Another node count requires a matching table.

In terrain mode, **Link loss** adds independent packet loss to the model; it does not change signal margin. Manual and random link cuts persist across terrain updates, and Join/healing restores the modeled link rather than forcing a physical path through an obstruction. Manually killed nodes stay down until revived. Switching to another topology and restarting returns to synthetic links; the terrain option remains available for the table supplied at launch. No elevation-map assets or placement editor are required.

Checks for the color boundaries and terrain bridge:

```
node --test tests/interface/link-quality.test.cjs
python3 -B tests/interface/test_console_terrain.py
```

## Interface

### Live terrain node placement

Manual terrain placement requires **Node.js 18+** for server-side terrain calculations. Python still serves the interface, and the normal graph/table simulator can run without Node. No npm packages or internet connection are needed for placement after checkout. Source attribution and assumptions are in `terrain/ATTRIBUTION.md`.

From the repository root:

```
cmake --build build
python3 interface/serve.py --sim --nodes 8 --speed 5 --port 8001
```

Open `http://localhost:8001/?sim` and choose **Network → View → Terrain placement**. The initial view is a preview; **Enable live placement**, dragging a node and releasing it, or **Apply position** starts the physical placement medium. Edit a node's projected X/Y offsets from the base and mast height in the form as an alternative to dragging. Drag the background to pan, use the zoom controls or wheel, and **Fit nodes** to recenter. Map pan/zoom is keyboard accessible. **Connect here** selects the node whose reports and messages the interface receives.

Each edit sends only positions, mast heights and radio settings to the server. The server validates every node and calculates both directions of every pair using the actual Malapert elevation grid and the vendored R2/P.526 model. It sends one validated, complete `@placement` transaction to the C console. The console applies the new loss, RSSI, SNR and margins between clock advances, preserving all nodes' route tables, message IDs and stored messages. It does not select routes on behalf of the mesh: HELLO exchange, liveness detection, retries, receipts, and the routing algorithm respond to the changed medium over simulated time. An edit can apply while paused, but discovery and rerouting require the clock to run or step.

The client serializes rapid edits, retaining the newest pending position. Generation and revision checks reject stale edits or edits to a restarted network. Failed validation or a rejected C transaction leaves the existing medium intact. Multiple browser clients share the server's committed placement; **Reload current placement** recovers after a stale-edit rejection. Restarting the simulator clears manual placement. When entering from a recorded terrain scenario, current positions are captured, automatic movement stops, and pending relays deploy immediately; explicitly killed nodes and existing cuts/outage schedules remain respected.

Dashed links predict terrain margin, using the weaker direction and the same red/yellow/green thresholds as the graph. Solid lines show neighbor reports and their ETX, and may lag a move. Blue highlights the connected node's reported first hop toward the selected node; the route text gives that next hop. Predictions are gray while an edit is pending. The optional 60 × 60 coverage overlay predicts one-hop margin from the selected online node to a 1.5 m receiver over the visible map; it is refreshed after placement, selection and view changes. Extra packet loss and random/manual outages still affect the C medium. A route or green prediction is not a delivery receipt.

**Export** saves node positions, mast heights and radio settings. **Import** applies a MESH `lmesh-placement-v2` file with the same node IDs. Browser reloads can retrieve the committed server placement while that server remains running. Restarting the server creates a new network; export first if you want to preserve a layout.

For a reproducible routing demonstration, start with **four nodes**, then import `sim/terrain/placement-diamond.json`. Allow discovery to settle, select node 4, and note the reported next hop from node 1. The direct 1–4 path is terrain-blocked; nodes 2 and 3 each offer a viable relay path. Move the current next-hop relay to map offsets **X = 9000, Y = 9000**, run/step the clock, and watch the next hop change to the other relay. Send to node 4 to confirm delivery. Moving node 4 to those offsets creates a partition; send an unexpired message, restore node 4 to **X = -1500, Y = -750**, and observe its automatic delivery. No messages or nodes are reset by either move.

Verification:

```
python3 -B tests/interface/test_placement.py -v
ctest --test-dir build --output-on-failure
```

The integration suite runs real DEM predictions through the Python bridge and C library: alternate-path rerouting across five seeds, receipt-confirmed delivery before/after moves, partition storage and exactly-once recovery, malformed/stale updates, atomic C rejection, 16-node matrices, radio/mast changes, manual cuts, random-outage healing, extra loss, track freezing, restart fencing, and coverage/image generation. CTest includes it when Python and Node are available.

An optional browser test uses `playwright-core` and an installed Chrome (neither is a runtime dependency). It must use a dedicated four-node server: it imports/repositions nodes and advances that test network. Launch that server on port 8015, then run:

```
node tests/interface/placement-browser.cjs --playwright-module /path/to/playwright-core
```

The browser test drags a real canvas marker, verifies next-hop changes and message delivery, tests queued-message recovery, delays an HTTP response to verify rapid-edit ordering, checks validation and SVG visibility, verifies coverage painting, and saves a screenshot to `/private/tmp/mesh-placement-verified.png`. Use `--url`, `--chrome`, and `--screenshot` to override the local defaults.

The main view keeps connection status, a short summary of this node, the network map, and message send/receive in view. Message type, priority, and lifetime are under **Message options**. Detailed node, neighbor, and route tables, sent history, and the console are under **Diagnostics and raw console**. The console can show plain-language explanations or raw protocol output. Resting the pointer on a dotted-underlined term shows what it means.

**Ping neighbors** (in **This node**) broadcasts a HELLO beacon at once; neighbors that hear it answer, and are marked **answered** in the neighbor table. Each neighbor also shows φ, the node's suspicion that it has gone silent ([SPEC.md](../SPEC.md) Section 7.3), recomputed every second so it climbs live; the node drops the neighbor at 8.

## Techniques

### Arrange Overview

On the **Network** map, rest the pointer on a node to highlight its direct neighbors and links; everything else dims.

Drag a Messages, Routing, Simulator, or Console tab into either Overview column. Drop below **This node** to stack a panel there, or onto the upper/lower half of another panel to place it before/after that panel. Drag a panel's title bar (marked **⠿**) to rearrange it; drop zones appear while dragging.

Docked tabs remain in the navigation; clicking one opens Overview and focuses its panel. Panels share their original live controls, so docking preserves message drafts, simulator state, and the connection. Each panel's title bar has window-style controls: **–** minimizes (hides contents) and **+** expands, **□** maximizes to fill the window (or double-click the title bar), and **❐** or Escape restores it. **×** returns a docked panel to its tab while keeping Overview open; select its tab when you want to view it again. Layout and minimized state are saved in this browser; **Reset layout** restores the original two panels. Simulator panels appear only when a simulator connection is available.

- **One command in flight.** Commands go through a queue; dump rows are collected until the `end` line, and event lines (`ev`) are handled whenever they arrive, so events and replies never mix up.
- **Node clock estimate.** "Heard" times are node-clock values, so the page keeps the newest event time plus elapsed wall time to show ages.
- **Simulator bridge.** `lmesh_console` reads commands for the attached node and `@` control lines on stdin; `serve.py` sends `@t <ms>` every 50 ms to advance the virtual clock in step with wall time, and forwards output to the page as server-sent events. The bridge sends its own control lines too, so it records who sent each line that gets a reply and passes only the page's replies to the page. Clock and control status goes to the page as a separate `sim` event, and the page takes a simulated node's clock from it.

## Known gaps

- The node table does not include the node itself, so this node's own battery and health flags are not shown; it needs the library to report its own status, or a firmware `battery` line.
- φ needs firmware from this repository with φ accrual (5 November 2026 or later); with older firmware the φ and HELLO columns show "—" and the ping button gets `unknown_command`.
- The map uses a circle layout, which gets busy past about 10 nodes.
