/* Simulator: the Simulator tab, the header clock, and control requests to the bridge. */
"use strict";
/* Simulator controls */
function renderSim() {
  $("simbar").style.display = st.sim ? "block" : "none";
  $("tab-sim").hidden = !st.sim;
  if (st.sim && restoreSim) selectTab($("tab-sim"));
  if (!st.sim && currentTab() === $("tab-sim")) selectTab($("tab-overview"));
  if (!st.sim || !st.sim.alive) return;
  const alive = new Set(st.sim.alive);
  const ids = simNodeIds();
  $("sim-nodes").innerHTML = ids.map(id => {
    const live = alive.has(id), att = id === st.sim.attached;
    const why = simStatus && simStatus.down[id];
    return `<div class="sn ${live ? "" : "dead"} ${att ? "att" : ""}"><b>${id}</b>
      ${why ? `<span class="why">${why.map(r => r.startsWith("schedule") ? "scheduled" : r).join(", ")}</span>` : ""}
      ${att ? '<span class="muted">attached</span>' : `<button data-attach="${id}">plug in</button>`}
      <button data-sim="${live ? "@kill" : "@revive"} ${id}">${live ? "kill" : "revive"}</button></div>`;
  }).join("");
  const cut = simStatus ? simStatus.links_down.map(([a, b]) => `${a}-${b} (outage)`) : [];
  $("sim-links").textContent = "links: " + st.sim.links.map(([a, b, loss]) => `${a}-${b}${loss ? ` (${loss} %)` : ""}`).concat(cut).join("  ");
  const sel = $("sim-sn"), keep = sel.value;
  const opts = ids.map(id => `<option value="${id}">${id}</option>`).join("");
  if (sel.dataset.opts !== opts) { sel.innerHTML = opts; sel.dataset.opts = opts; if (keep) sel.value = keep; }
}

/* Clock, loss, outages, schedule: the bridge owns them; the page shows its status and posts changes. */
async function simControl(req) {
  $("sim-err").textContent = "";
  try {
    const r = await fetch("/sim/control", { method: "POST", body: JSON.stringify(req) });
    const o = await r.json();
    if (!o.ok) { $("sim-err").textContent = o.reason || "refused"; log(`simulator: ${o.reason || "refused"}`, "err"); return null; }
    onSimStatus(o);
    return o;
  } catch (e) { log(`simulator: ${e.message}`, "err"); return null; }
}

function onSimStatus(s) {
  const restarted = simStatus && s.generation !== simStatus.generation;
  simStatus = s;
  speed = s.paused ? 0 : s.speed;
  st.clock = { t: s.sim_ms, at: performance.now() };  // a simulated node's clock is the simulator's clock
  if (restarted && transport && transport.kind === "sim") {
    st = fresh();
    st.sim = { alive: null, links: [] };
    simIds = new Set();
    $("log").innerHTML = "";
    log(`simulator restarted: ${s.args.topo} of ${s.args.nodes}, seed ${s.args.seed}`, "ev");
    renderAll();
    refresh();
  }
  renderSimCtl();
}

const nearestSpeed = x => SPEEDS.reduce((bi, v, i) => Math.abs(Math.log(v / x)) < Math.abs(Math.log(SPEEDS[bi] / x)) ? i : bi, 0);
const idle = el => document.activeElement !== el;  // leave a control alone while the user is on it
function clockFmt(ms) {
  const s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return `${h}:${String(m).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}.${Math.floor(ms % 1000 / 100)}`;
}
const secs = ms => duration(ms / 1000);

function renderSimCtl() {
  const s = simStatus;
  if (!s) return;
  $("sim-run").textContent = s.paused ? "▶ Run" : "❚❚ Pause";
  if (idle($("sim-speed"))) $("sim-speed").value = nearestSpeed(s.speed);
  $("sim-speed-v").textContent = `×${s.speed}`;
  $("simclock").hidden = !(transport && transport.kind === "sim");
  $("sim-clock").innerHTML = s.paused ? `<span class="paused">${clockFmt(s.sim_ms)}</span>` : clockFmt(s.sim_ms);
  if (idle($("sim-loss"))) $("sim-loss").value = s.loss;
  $("sim-loss-v").textContent = `${s.loss} %`;
  if (idle($("sim-cn"))) $("sim-cn").value = s.chaos.node_pct;
  if (idle($("sim-cl"))) $("sim-cl").value = s.chaos.link_pct;
  $("sim-cn-v").textContent = `${s.chaos.node_pct} %`;
  $("sim-cl-v").textContent = `${s.chaos.link_pct} %`;
  if (idle($("sim-cm"))) $("sim-cm").value = s.chaos.mean_s;
  $("sim-cs").checked = s.chaos.spare_attached;
  if (idle($("sim-rt"))) $("sim-rt").value = s.args.topo;
  if (idle($("sim-rn")) && !$("sim-rn").value) $("sim-rn").value = s.args.nodes;
  if (idle($("sim-rs")) && $("sim-rs").value === "") $("sim-rs").value = s.args.seed;
  $("sim-sched-list").innerHTML = s.schedule.map(e => {
    const when = e.next_ms === null ? "done" : `${e.down ? "back up" : "goes down"} in ${secs(e.next_ms - s.sim_ms)}`;
    return `<div class="se"><b>node ${e.node}</b> down ${secs(e.dur_ms)} ${e.every_ms ? `every ${secs(e.every_ms)}` : "once"}
      ${e.down ? '<span class="badge b-poor">down now</span>' : ""}<span class="muted">${when}</span>
      <button data-unsched="${e.id}">remove</button></div>`;
  }).join("");
}

let lastSpeedSent = null;
$("sim-run").onclick = () => simStatus && simControl({ op: "pause", value: !simStatus.paused });
$("sim-speed").oninput = () => {
  const v = SPEEDS[$("sim-speed").value];
  $("sim-speed-v").textContent = `×${v}`;
  if (v !== lastSpeedSent) { lastSpeedSent = v; simControl({ op: "speed", value: v }); }
};
async function simStep() {
  if (!simStatus) return;
  if (!simStatus.paused) await simControl({ op: "pause", value: true });
  await simControl({ op: "step", seconds: Number($("sim-step-size").value) });
  setTimeout(refresh, 250);
}
$("sim-step").onclick = simStep;
document.addEventListener("keydown", ev => {
  if (ev.key !== "ArrowRight" || !simStatus || !transport || transport.kind !== "sim") return;
  if (ev.target.closest("input, select, textarea, [role=tab]")) return;
  ev.preventDefault();
  simStep();
});
$("sim-loss").oninput = () => { $("sim-loss-v").textContent = `${$("sim-loss").value} %`; };
$("sim-loss").onchange = async () => { await simControl({ op: "loss", value: Number($("sim-loss").value) }); refresh(); };
for (const [id, key] of [["sim-cn", "node_pct"], ["sim-cl", "link_pct"]]) {
  $(id).oninput = () => { $(id + "-v").textContent = `${$(id).value} %`; };
  $(id).onchange = () => simControl({ op: "chaos", [key]: Number($(id).value) });
}
$("sim-cm").onchange = () => simControl({ op: "chaos", mean_s: Number($("sim-cm").value) || 60 });
$("sim-cs").onchange = () => simControl({ op: "chaos", spare_attached: $("sim-cs").checked });
$("sim-heal").onclick = async () => { await simControl({ op: "chaos", heal: true }); refresh(); };
$("sim-sadd").onclick = async () => {
  await simControl({ op: "sched_add", node: Number($("sim-sn").value), start_in_s: Number($("sim-ss").value) || 0,
    dur_s: Number($("sim-sd").value) || 60, every_s: Number($("sim-se").value) || 0 });
  refresh();
};
$("sim-sched-list").addEventListener("pointerdown", async ev => {  // pointerdown: the list is redrawn as its countdowns tick
  const b = ev.target.closest("button[data-unsched]");
  if (b) { await simControl({ op: "sched_remove", id: Number(b.dataset.unsched) }); refresh(); }
});
$("sim-restart").onclick = () => simControl({ op: "restart", topo: $("sim-rt").value,
  nodes: Number($("sim-rn").value) || 8, seed: Number($("sim-rs").value) || 0 });
async function attachTo(id) {
  if (!transport || !st.sim || id === st.sim.attached) return;
  const res = await command(`@attach ${id}`, false).catch(e => ({ ok: false, reason: e.message }));
  if (res.ok) {
    st = Object.assign(fresh(), { sim: { ...st.sim, attached: id }, sent: st.sent });
    mapSel = null;
    $("log").innerHTML = "";
    log(`plugged into node ${id}`, "ev");
    renderAll();
  }
  refresh();
}
$("simbar").addEventListener("click", async ev => {
  const at = ev.target.closest("button[data-attach]");
  if (at) { attachTo(Number(at.dataset.attach)); return; }
  const b = ev.target.closest("button[data-sim]");
  if (!b) return;
  await command(b.dataset.sim, false).catch(() => {});
  refresh();
});
async function simLink(verb) {
  const a = $("cut-a").value, b = $("cut-b").value;
  if (!a || !b) return;
  const loss = verb === "@join" && simStatus ? ` ${simStatus.loss}` : "";
  await command(`${verb} ${a} ${b}${loss}`, false).catch(() => {});
  refresh();
}
$("btn-cut").onclick = () => simLink("@cut");
$("btn-join").onclick = () => simLink("@join");
