# interface

A browser page for a laptop plugged into a mesh node. It shows the node's status, neighbors, routes and the whole network from status broadcasts, and it sends and receives messages. It speaks only the line protocol of [SPEC.md](../SPEC.md) Section 11, so it works with any node and needs no internet connection.

**Status (27 October 2026):** first version. Tested against the simulator (8 nodes, kill, reroute, plug into another node, text and telemetry delivered); the USB path has not yet run against a board.

## Files

| File | Contents |
|---|---|
| `index.html` | The page's markup. No libraries and no build step. |
| `styles.css` | All styles, with light and dark themes. |
| `js/` | The page's code as plain scripts loaded in order (they share one global scope): `state.js` shared state, `format.js` formatting and φ math, `glossary.js` term meanings and tooltips, `console.js` the Console tab, `protocol.js` the command queue and node events, `poll.js` refreshing the node's tables, `transport.js` USB and simulator connections, and `main.js` startup. `views/` draws each part of the page: `header.js`, `tabs.js` (also theme and Advanced), `self.js`, `tables.js`, `messages.js`, `map.js`, `side.js`, `sim.js`. |
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

- **Clock** (in the page header, so it stays in reach on every tab): pause and run, speed from ×0.1 to ×100, and **Step** to advance a set amount of simulated time (0.5 s to 15 min) per click, or press → . Stepping pauses the clock first.
- **Link loss:** one loss percentage applied to every link that is up, and to links joined from the panel.
- **Random outages:** the share of time nodes and links spend down, and the mean outage length. Outages start at random and end on their own; the plugged-in node can be spared. **Heal all** ends them now.
- **Scheduled downtime:** take a node down after a delay, for a time, once or repeating (for example 300 s down every 900 s, like a node that sleeps to save power).
- **Restart network:** a new topology, node count or seed without restarting `serve.py`.

All of these run in `serve.py` in simulated time, using the console's `@t`, `@kill`, `@revive`, `@cut`, `@join` and `@topo` lines, so they keep working when the clock is stepped or sped up. Nodes and links taken down this way are labeled in the panel; a node killed by hand is never revived automatically.

## Interface

The header shows the connection, this node's identity, and a health summary: how many of the nodes this node has heard of it can reach, and how many of your sent messages are still waiting for a delivery receipt. It is green when every known node is reachable, amber when some are not, and red when none are or the node has no neighbors. In simulator mode the header also holds the simulator clock. **Overview** is the map and side panel; **Messages** has the send form (type, priority, and lifetime under **Message options**), sent history, and received messages; **Routing** has the node, neighbor, and route tables; **Console** shows the protocol traffic in plain language, or raw with Advanced on. Resting the pointer on a dotted-underlined term shows what it means.

**Advanced** (top right, off by default, remembered in this browser) shows what only matters when debugging. Without it, Routing shows each node's station, battery, health and when it was last heard; each neighbor's state and link quality in words (good, fair, poor); and each route's next hop. The console stays in plain language with polling hidden. Advanced adds uptime, store, loss and neighbor lists for nodes; ETX, φ, HELLO timing, delivery ratios, RSSI and SNR for neighbors; metric and sequence number for routes; raw console output and polling; **More node statistics**; the radio settings in the header; the Command and Telemetry message types; the layout toolbar; and the simulator's link cut/join, random outages, scheduled downtime and restart controls. Turning it off resets the message type to Text and the console to plain language without polling, so no hidden setting stays in effect.

**Ping neighbors** (in **This node**) broadcasts a HELLO beacon at once; neighbors that hear it answer, and are marked **answered** in the neighbor table. Each neighbor also shows φ, the node's suspicion that it has gone silent ([SPEC.md](../SPEC.md) Section 7.3), recomputed every second so it climbs live; the node drops the neighbor at 8.

## Techniques

### Overview

On the **Network** map, rest the pointer on a node to highlight its direct neighbors and links; everything else dims. Click a node to show it in the side panel: its status, battery, our link and route to it, when it was last heard, its health and its neighbors (Advanced adds uptime, store, losses, ETX, route metric and signal), with **Send message**, which opens Messages addressed to it. In simulator mode the panel also has **Plug in** and **Kill** or **Revive**, and This node has a Kill/Revive button for the node you are plugged into. Click the node again, click empty map, press Escape, or use **← This node** to return. The Simulator tab keeps the same buttons for every node.

Overview has a fixed layout: the **Network** map in the wide left column, sized to fill the window, and the side panel (**This node**, or the node picked on the map) on the right. On a narrow window the side panel moves below the map. The other sections are tabs.

- **One command in flight.** Commands go through a queue; dump rows are collected until the `end` line, and event lines (`ev`) are handled whenever they arrive, so events and replies never mix up.
- **Node clock estimate.** "Heard" times are node-clock values, so the page keeps the newest event time plus elapsed wall time to show ages.
- **Simulator bridge.** `lmesh_console` reads commands for the attached node and `@` control lines on stdin; `serve.py` sends `@t <ms>` every 50 ms to advance the virtual clock in step with wall time, and forwards output to the page as server-sent events. The bridge sends its own control lines too, so it records who sent each line that gets a reply and passes only the page's replies to the page. Clock and control status goes to the page as a separate `sim` event, and the page takes a simulated node's clock from it.

## Known gaps

- The node table does not include the node itself, so this node's own battery and health flags are not shown; it needs the library to report its own status, or a firmware `battery` line.
- φ needs firmware from this repository with φ accrual (5 November 2026 or later); with older firmware the φ and HELLO columns show "—" and the ping button gets `unknown_command`.
- The map uses a circle layout, which gets busy past about 10 nodes.
