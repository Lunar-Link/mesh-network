/* Side panel: This node, or the node picked on the map, with its actions. */
"use strict";
/* Side panel: This node, or the node picked on the map. Redrawn with the map, so it follows live updates
   (a killed node's Kill button turns into Revive). Picking this node, or nothing, shows This node. */
const setHtml = (el, html) => { if (el.dataset.html !== html) { el.innerHTML = html; el.dataset.html = html; } };  // unchanged: keep focus
function renderSide() {
  const sim = !!(transport && st.sim && st.sim.alive);
  const self = sim ? st.sim.attached : st.ident ? st.ident.id : null;
  if (mapSel !== null && !mapPos.has(mapSel)) mapSel = null;  // no longer on the map, e.g. after a restart
  const id = mapSel === self ? null : mapSel, picked = id !== null;
  const n = picked ? st.nodes.find(x => x.node === id) : null;
  $("side-title").textContent = picked ? `Node ${id}${n && n.station ? ` · ${n.station}` : ""}` : "This node";
  for (const el of ["self-view", "self-tools", "self-state"]) $(el).hidden = picked;
  for (const el of ["node-view", "node-tools"]) $(el).hidden = !picked;
  if (picked) setHtml($("node-view"), nodeDetails(id, n, sim));
  const shown = picked ? id : self, acts = [];
  if (picked) acts.push(`<button data-send="${id}">Send message</button>`);
  if (sim && shown !== null) {
    const dead = simDead(shown);
    if (picked) acts.push(`<button data-attach="${id}">Plug in</button>`);
    const verb = dead ? "Revive" : "Kill";
    acts.push(`<button data-sim="${dead ? "@revive" : "@kill"} ${shown}"${dead ? "" : ' class="kill"'}>${picked ? verb : `${verb} node ${shown}`}</button>`);
  }
  setHtml($("side-actions"), acts.join(""));
}
/* What this node knows about another: its last status broadcast, our link to it, and our route to it. */
function nodeDetails(id, n, sim) {
  const l = st.links.find(x => x.id === id), r = st.routes.find(x => x.dest === id);
  const tile = (k, v, adv = false) => `<div${adv ? ' class="adv"' : ""}><span>${k}</span><b>${v}</b></div>`;
  const status = sim && simDead(id) ? '<span class="poor">killed</span>' : !n ? '<span class="muted">not heard</span>'
    : n.stale ? '<span class="muted">stale</span>' : '<span class="good">up</span>';
  const link = !l ? '<span class="muted">none</span>' : !l.up ? '<span class="poor">down</span>'
    : !l.bidir ? '<span class="fair">one-way</span>' : `<span class="${etxClass(l.etx)}">${etxClass(l.etx)}</span>`;
  const route = !r || r.retracted ? '<span class="poor">none</span>' : r.next === id ? "direct" : `via ${r.next}`;
  let out = `<div class="kv">${[
    tile("Status", status), tile("Battery", n ? volts(n.batt_mv) : "—"), tile(T("Link"), link), tile("Route", route),
    tile("Heard", n ? ago(n.heard_ms) : "—"),
    tile(T("Uptime"), n ? duration(n.uptime_s) : "—", true), tile(T("Store"), n ? n.store : "—", true), tile(T("Lost"), n ? n.lost : "—", true),
    tile(T("ETX"), l ? etxText(l.etx) : "—", true), tile(T("Metric"), r && !r.retracted ? etxText(r.metric) : "—", true),
    tile("Signal", l ? `${l.rssi} dBm · ${l.snr} dB` : "—", true),
  ].join("")}</div>`;
  if (!n) return out + `<div class="side-row muted">No status broadcast heard from node ${id} yet.</div>`;
  const nbs = n.neighbors.map(([nid, e]) => `<span class="${etxClass(e)}">${nid}</span><span class="adv muted"> (${etxText(e)})</span>`).join(", ");
  out += `<div class="side-row"><span class="k">${T("Health")}</span>${flagBadges(n.flags)}</div>`;
  out += `<div class="side-row"><span class="k">Its neighbors</span>${nbs || '<span class="muted">none</span>'}</div>`;
  return out;
}
function pickNode(id) {
  mapSel = id === mapSel ? null : id;
  renderMap();
  if (mapSel !== null && matchMedia("(max-width: 1000px)").matches) $("side").scrollIntoView({ block: "nearest", behavior: "smooth" });
}
const unpick = () => { if (mapSel !== null) pickNode(mapSel); };
$("btn-unpick").onclick = unpick;
$("side-actions").addEventListener("click", async ev => {
  const b = ev.target.closest("button");
  if (!b) return;
  if (b.dataset.send) { selectTab($("tab-messages")); $("f-dest").value = b.dataset.send; $("f-text").focus(); return; }
  if (b.dataset.attach) { attachTo(Number(b.dataset.attach)); return; }
  await command(b.dataset.sim, false).catch(() => {});
  refresh();
});
document.addEventListener("keydown", ev => {  // Escape returns the side panel to This node
  if (ev.key === "Escape" && mapSel !== null) unpick();
});
