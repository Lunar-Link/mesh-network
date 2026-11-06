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

## Interface

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
