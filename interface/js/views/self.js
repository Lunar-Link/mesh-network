/* This node: summary tiles, statistics, and Ping neighbors. */
"use strict";
const FRAME_TYPES = ["HELLO", "DATA", "HOP_ACK", "STATUS", "ROUTE_REQ"];

function renderSelf() {
  const s = st.stats;
  const now = nodeNow();
  const up = st.links.filter(l => l.up && l.bidir).length;
  const reach = st.routes.filter(r => !r.retracted).length;
  const held = st.msgs.filter(m => m.role !== "history").length;
  const tiles = [
    ["Uptime", now === null || st.down ? "—" : duration(now / 1000)],
    ["Neighbors", st.ident ? `${up}` : "—"],
    ["Reachable", st.ident ? `${reach}` : "—"],
    ["Delivered", s ? s.delivered : "—"],
  ];
  const more = [
    ["Store", st.storeDepth ?? (st.ident ? held : "—")],
    ["Airtime", s ? duration(s.airtime_ms / 1000) : "—"],
    ["Ack timeouts", s ? s.ack_timeouts : "—"],
    ["Link failures", s ? s.link_failures : "—"],
    ["Dropped", s ? s.evicted + s.expired + s.hop_limit_drops : "—"],
    ["Bad frames", s ? s.rx_bad_tag + s.rx_foreign + s.rx_malformed : "—"],
  ];
  $("self").innerHTML = tiles.map(([k, v]) => `<div><span>${T(k)}</span><b>${esc(v)}</b></div>`).join("");
  $("self-more").innerHTML = more.map(([k, v]) => `<div><span>${T(k)}</span><b>${esc(v)}</b></div>`).join("");
  if (!s) { $("frames").innerHTML = ""; return; }
  $("frames").innerHTML = `<tr><th>${T("Frames")}</th>${FRAME_TYPES.map(t => `<th>${T(t)}</th>`).join("")}</tr>` +
    `<tr><td>Sent</td>${FRAME_TYPES.map((_, i) => `<td>${s.tx_frames[i]}</td>`).join("")}</tr>` +
    `<tr><td>Received</td>${FRAME_TYPES.map((_, i) => `<td>${s.rx_frames[i]}</td>`).join("")}</tr>`;
}

/* Ping: one HELLO now; neighbors that hear it answer with a triggered HELLO within about 1 to 3 s of node time.
   A neighbor counts as answered when its last-HELLO time moves after the ping. */
const PING_WINDOW_MS = 15000;  // node time in which answers are still expected
const PING_SHOW_MS = 30000;    // wall time the result stays on screen
function pingAnswered(l) {
  const p = st.ping;
  return !!p && l.hello_at_ms !== undefined && l.hello_at_ms !== p.before.get(l.id) && l.hello_at_ms >= p.at - 1000;
}
function renderPing() {
  const btn = $("btn-ping");
  btn.disabled = !transport || st.down || !st.ident;
  if (st.ping && performance.now() - st.ping.wall > PING_SHOW_MS) st.ping = null;  // wall time, so the result stays readable at any sim speed
  const p = st.ping, now = nodeNow();
  if (!p) { $("ping-note").textContent = ""; return; }
  const up = st.links.filter(l => l.up);
  const got = up.filter(pingAnswered).length;
  const open = now !== null && now - p.at < PING_WINDOW_MS;
  $("ping-note").textContent = `ping: ${got} of ${up.length} answered${open ? "…" : ""}`;
}
async function sendPing() {
  const before = new Map(st.links.map(l => [l.id, l.hello_at_ms]));
  const at = nodeNow();
  let res;
  try { res = await command("ping", false); } catch (e) { log(String(e.message || e), "err"); return; }
  if (!res.ok) {
    $("ping-note").textContent = res.reason === "too_soon" ? "ping: wait a moment" : `ping failed: ${res.reason}`;
    return;
  }
  st.ping = { at: at ?? 0, before, wall: performance.now() };
  renderLinks();
  const k = speed > 0 ? speed : 1;  // replies come in node time; poll sooner than the 5 s cycle while they arrive
  for (const ms of [1500, 3500, 6000]) setTimeout(refresh, ms / k);
}
$("btn-ping").onclick = sendPing;
