/* Header: node identity, radio, health summary, and the node-down notice. */
"use strict";
const BW_KHZ = { 7: "125 kHz", 8: "250 kHz", 9: "500 kHz" };

/* Header summary: how much of the network this node can reach, and how many of your messages await a receipt. */
function renderHealth() {
  const el = $("health");
  el.hidden = !transport || (!st.ident && !st.down);
  if (el.hidden) return;
  if (st.down) { el.className = "poor"; el.textContent = "node down"; return; }
  const known = new Set([...st.nodes.map(n => n.node), ...st.routes.map(r => r.dest)]);
  known.delete(st.ident.id);
  const reach = st.routes.filter(r => !r.retracted && r.dest !== st.ident.id).length;
  const neighbors = st.links.filter(l => l.up && l.bidir).length;
  const waiting = [...st.sent.values()].filter(m => m.origin === st.ident.id && (m.state === "queued" || m.state === "transmitted")).length;
  const wait = waiting ? ` · <b>${waiting}</b> waiting` : "";
  if (!known.size) { el.className = neighbors ? "muted" : "poor"; el.innerHTML = (neighbors ? "no other nodes heard yet" : "no neighbors") + wait; return; }
  el.className = !neighbors || !reach ? "poor" : reach < known.size ? "fair" : "good";
  el.innerHTML = `<b>${reach}</b> of <b>${known.size}</b> reachable${wait}`;
}

function renderDown() {
  const down = transport && st.down;
  $("down").style.display = down ? "block" : "none";
  if (down) $("down").textContent = `Node ${st.sim.attached} is down: it was killed in the simulator. Revive it, or click another node on the map to plug in.`;
  $("self-state").innerHTML = down ? '<span class="badge b-poor">down</span>' : "";
}

function renderIdent() {
  const parts = [];
  if (st.ident) {
    parts.push(`<span>Node <b>${st.ident.id}</b></span>`);
    if (st.ident.station) parts.push(`<span>Station <b>${esc(st.ident.station)}</b></span>`);
    parts.push(`<span>${T("Network")} <b>${st.ident.net}</b></span>`);
  }
  if (st.radio) {
    const r = st.radio;
    if (r.state === "ready") {
      const mhz = r.band_hz ? (r.band_hz / 1e6).toFixed(3) + " MHz" : "";
      parts.push(`<span class="adv">Radio <b>${esc(r.driver)}</b> ${mhz} SF${r.sf} ${BW_KHZ[r.bw] || "BW" + r.bw} ${r.power_dbm} dBm</span>`);
    } else {
      parts.push(`<span>Radio <b class="poor">${esc(r.state)}</b></span>`);
    }
  }
  $("ident").innerHTML = parts.join("");
}
