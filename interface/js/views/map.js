/* Network map: layout, drawing, hover highlight, and node picking. */
"use strict";
/* Map: the node's own links plus the neighbor lists in every status it has heard.
   Resting the pointer on a node dims everything not directly linked to it. */
let mapAdj = new Map(), mapHover = null, mapSel = null, mapPos = new Map();
function renderMap() {
  const svg = $("map");
  const self = st.ident && Number.isInteger(st.ident.id) ? st.ident.id : null;
  const info = new Map(st.nodes.map(n => [n.node, n]));
  const ids = new Set(info.keys());
  if (self !== null) ids.add(self);
  const sim = !!(transport && st.sim && st.sim.alive);
  if (sim) {
    simNodeIds().forEach(id => ids.add(id));  // every simulated node is drawn, so any can be plugged into
    ids.add(st.sim.attached);
  }
  const attached = sim ? st.sim.attached : self;
  const edges = new Map();
  const addEdge = (a, b, etx, stale) => {
    if (a === b) return;
    const key = a < b ? `${a}-${b}` : `${b}-${a}`;
    const old = edges.get(key);
    const worse = !old || etx === "inf" || (old.etx !== "inf" && etx > old.etx);
    if (!old) edges.set(key, { a, b, etx, stale });
    else edges.set(key, { a, b, etx: worse ? etx : old.etx, stale: old.stale && stale });
    ids.add(a); ids.add(b);
  };
  if (self !== null) for (const l of st.links) if (l.up) addEdge(self, l.id, l.etx, false);
  for (const n of st.nodes) for (const [id, e] of n.neighbors) addEdge(n.node, id, e, n.stale);

  const list = [...ids].sort((a, b) => a - b);
  mapAdj = new Map(list.map(id => [id, new Set()]));
  for (const e of edges.values()) { mapAdj.get(e.a).add(e.b); mapAdj.get(e.b).add(e.a); }
  $("net-count").textContent = list.length ? `${list.length} node${list.length === 1 ? "" : "s"}` : "";
  const w = svg.clientWidth || 600, h = svg.clientHeight || 300, cx = w / 2, cy = h / 2, r = Math.min(w, h) / 2 - 34;
  const pos = new Map(list.map((id, i) => {
    const a = -Math.PI / 2 + 2 * Math.PI * i / list.length;
    return [id, list.length === 1 ? [cx, cy] : [cx + r * Math.cos(a), cy + r * Math.sin(a)]];
  }));
  const color = c => `var(--${c})`;
  let out = "";
  for (const e of edges.values()) {
    const [x1, y1] = pos.get(e.a), [x2, y2] = pos.get(e.b);
    out += `<line class="edge" data-a="${e.a}" data-b="${e.b}" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${e.stale ? color("stale") : color(etxClass(e.etx))}" stroke-width="2" ${e.stale ? 'stroke-dasharray="4 4"' : ""}/>`;
  }
  for (const id of list) {
    const [x, y] = pos.get(id);
    const n = info.get(id);
    const me = id === attached, dead = simDead(id);
    const stale = n ? n.stale : !me;
    const fill = dead ? "var(--node-dead)" : me ? "var(--node-self)" : "var(--panel)";
    const stroke = dead ? "var(--poor)" : me ? "var(--accent)" : stale ? "var(--stale)" : n && n.flags & 21 ? "var(--poor)" : "var(--good)";
    const textStyle = [me ? "font-weight:700" : "", dead ? "fill:var(--poor);text-decoration:line-through" : stale && !me ? "fill:var(--stale)" : ""].join(";");
    const label = dead ? "killed" : n && n.station ? n.station : "";
    out += `<g class="node hit${id === mapSel ? " sel" : ""}" data-n="${id}" data-pick="${id}">`;
    out += `<title>Node ${id}${dead ? " (killed)" : me ? " (this node)" : ""}: click for details</title>`;
    if (me && sim) out += `<circle cx="${x}" cy="${y}" r="22" fill="none" stroke="var(--accent)" stroke-width="1.5"/>`;
    out += `<circle class="body" cx="${x}" cy="${y}" r="17" fill="${fill}" stroke="${stroke}" stroke-width="2.5" ${dead ? 'stroke-dasharray="4 3"' : ""}/>`;
    out += `<text x="${x}" y="${y}" style="${textStyle}">${id}</text>`;
    if (label) out += `<text class="lbl" x="${x}" y="${y + 27}" ${dead ? 'style="fill:var(--poor)"' : ""}>${esc(label)}</text>`;
    out += "</g>";
  }
  if (!list.length) out = `<text x="${cx}" y="${cy}" class="lbl">${transport ? "Waiting for the first status broadcasts" : "Not connected"}</text>`;
  svg.setAttribute("viewBox", `0 0 ${w} ${h}`);
  svg.innerHTML = out;
  setMapHover(mapHover);
  mapPos = pos;
  renderSide();
  $("map-hint").textContent = !list.length ? "" : sim ? "Click a node for details, to plug in, or to kill it. " : "Click a node for details. ";
}

function setMapHover(id) {
  const svg = $("map");
  mapHover = id != null && mapAdj.has(id) ? id : null;
  svg.classList.toggle("focus", mapHover !== null);
  const near = mapHover === null ? null : mapAdj.get(mapHover);
  for (const el of svg.querySelectorAll(".node")) { const n = Number(el.dataset.n); el.classList.toggle("near", !!near && (n === mapHover || near.has(n))); }
  for (const el of svg.querySelectorAll(".edge")) el.classList.toggle("near", !!near && (Number(el.dataset.a) === mapHover || Number(el.dataset.b) === mapHover));
}
window.addEventListener("resize", renderMap);
$("map").addEventListener("pointerover", ev => { const node = ev.target.closest("[data-n]"); setMapHover(node ? Number(node.dataset.n) : null); });
$("map").addEventListener("pointerleave", () => setMapHover(null));
$("map").addEventListener("click", ev => {
  const g = ev.target.closest("[data-pick]");
  if (g) pickNode(Number(g.dataset.pick));
  else unpick();  // a click on empty map returns to This node
});
