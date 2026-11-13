# Terrain placement source

The local placement model and elevation data come from the [Lunar Link Malapert Node Planner](https://lunar-link.github.io/terrain-sims/routing-website/) in [terrain-sims](https://github.com/Lunar-Link/terrain-sims), retrieved 13 November 2026. Ownership remains with that upstream project.

- `model.cjs` is the upstream page's standalone `LL` R2 hybrid / P.526 script, unchanged apart from a source comment. It samples terrain profiles, includes lunar curvature and the propagation effects implemented by the planner, and returns directional path loss.
- `dem20.gz` is the exact gzip payload decoded from `routing-website/data/dem20.b64.txt`. It holds the byte-plane shuffled Float32 elevation grid, 1621 × 1621 at 20 m spacing. SHA-256: `0531b577c01935f8897967ae94f146e297474314e70394b5ce9bd510ee01577e`.
- `meta.json` contains the matching grid metadata and first reference EVA plan from the page. Coordinates are the planner's projected map meters.
- `service.cjs` is the new MESH adapter: it restores the elevation samples, calculates pair predictions and sampled coverage, and generates a hillshade PNG from the DEM. It uses Node built-ins only and does not contact external servers.

The adapter uses isotropic antennas at 433 MHz, SF9, 125 kHz, CR 4/5, preamble 12. Margin is transmit power minus modeled path loss minus extra attenuation minus the -129 dBm sensitivity. Packet loss follows the existing simulator's logistic rule: 10% at zero margin with a 0.5 dB width, rounded to an integer percentage. RSSI and SNR are bounded to the simulator's reporting ranges. Coverage is a sampled one-hop prediction to a 1.5 m receiver, not a measurement or a guarantee of message delivery. Upstream antenna-pattern databases, automatic planner routes and uncertainty sweeps are not part of manual placement.
