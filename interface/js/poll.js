/* Polling: refresh the node's tables every few seconds and after events. */
"use strict";
const REFRESH_MS = 5000;

/* Polling */
let refreshing = false, refreshAgain = false;
async function dump(line) {
  const res = await command(line, true, { rows: true });
  if (!res.rows) throw new Error(`${line}: ${res.reason || "no rows"}`);
  return res.rows;
}
async function refresh() {
  if (!transport) return;
  if (refreshing) { refreshAgain = true; return; }
  refreshing = true;
  try {
    // Topology first: a killed node answers nothing else, and the controls must still update.
    if (st.sim) {
      const topo = await command("@topo", true, { accept: o => Array.isArray(o.alive) });
      if (topo.ok) noteSim(topo);
    }
    if (st.down) { renderAll(); return; }
    if (!st.ident) {
      const id = await command("id", true, { accept: o => Number.isInteger(o.id) });
      if (id.ok && Number.isInteger(id.id)) { st.ident = id; renderIdent(); }
    }
    st.links = await dump("links");
    st.routes = await dump("routes");
    st.nodes = await dump("nodes");
    st.msgs = await dump("msgs");
    st.stats = (await dump("stats"))[0] || null;
    for (const r of st.msgs) {
      if (st.ident && r.origin === st.ident.id && r.role !== "relay") {
        const key = `${r.origin}:${r.id}`;
        const m = st.sent.get(key) || { origin: r.origin, id: r.id };
        Object.assign(m, { dest: r.dest, type: r.type, prio: r.prio, state: r.state, attempts: r.attempts, lifetime: r.lifetime_s, hops: r.hops });
        st.sent.set(key, m);
      }
    }
    renderAll();
  } catch (e) {
    if (transport) log(String(e.message || e), "err");
    if (st.sim) scheduleRefresh();  // e.g. killed mid-poll: the next pass sees it via @topo
  } finally {
    refreshing = false;
    if (refreshAgain && transport) { refreshAgain = false; setTimeout(refresh, 0); }
  }
}
setInterval(refresh, REFRESH_MS);
setInterval(() => { if (transport) { renderSelf(); renderNodes(); renderLinks(); } }, 1000);

function start() {
  renderAll();
  refresh();
}
