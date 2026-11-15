/* Routing tab: node, neighbor, and route tables. */
"use strict";
const FLAGS = [[1, "low battery", "poor"], [2, "store > 75 %", "fair"], [4, "radio fault", "poor"], [8, "rebooted", "fair"], [16, "no neighbors", "poor"]];

function flagBadges(f) {
  const out = FLAGS.filter(([bit]) => f & bit).map(([, name, c]) => `<span class="badge b-${c}">${name}</span>`);
  return out.length ? out.join("") : `<span class="muted">ok</span>`;
}

function renderNodes() {
  const rows = [...st.nodes].sort((a, b) => a.node - b.node);
  if (!rows.length) {
    $("nodes").innerHTML = `<tr><td class="empty">${transport ? st.down ? "Node is down." : "No status heard yet." : "Connect to a node."}</td></tr>`;
    return;
  }
  $("nodes").innerHTML = `<tr><th>Node</th><th>${T("Station")}</th><th>Battery</th><th class="adv">${T("Uptime")}</th><th class="adv">${T("Store")}</th><th class="adv">${T("Lost")}</th><th>${T("Health")}</th><th>Heard</th><th class="adv">Its neighbors (${T("ETX")})</th></tr>` +
    rows.map(n => `<tr class="${n.stale ? "stale" : ""}">
      <td><b>${n.node}</b>${simDead(n.node) ? ' <span class="badge b-poor">killed</span>' : n.stale ? ' <span class="badge term" data-term="stale">stale</span>' : ""}</td>
      <td>${esc(n.station || "—")}</td><td>${volts(n.batt_mv)}</td><td class="adv">${duration(n.uptime_s)}</td>
      <td class="adv">${n.store}</td><td class="adv">${n.lost}</td><td>${flagBadges(n.flags)}</td><td>${ago(n.heard_ms)}</td>
      <td class="wrap adv">${n.neighbors.map(([id, e]) => `${id} <span class="${etxClass(e)}">(${etxText(e)})</span>`).join(", ") || "—"}</td></tr>`).join("");
}

function renderLinks() {
  renderPing();
  $("nb-count").textContent = st.links.length ? `${st.links.filter(l => l.up).length} up` : "";
  if (!st.links.length) {
    $("links").innerHTML = `<tr><td class="empty">${transport ? st.down ? "Node is down." : "No neighbors heard." : "—"}</td></tr>`;
    return;
  }
  const ping = st.ping;
  $("links").innerHTML = `<tr><th>Node</th><th>State</th><th class="basic">Link</th><th class="adv">${T("ETX")}</th><th class="adv">${T("φ")}</th><th class="adv">${T("HELLO every")}</th><th class="adv">${T("We hear")}</th><th class="adv">${T("Hears us")}</th><th class="adv">${T("RSSI")}</th><th class="adv">${T("SNR")}</th><th>Heard</th></tr>` +
    [...st.links].sort((a, b) => a.id - b.id).map(l => {
      const state = !l.up ? '<span class="badge b-poor">down</span>' : l.bidir ? '<span class="badge b-good">up</span>' : '<span class="badge b-fair term" data-term="one-way">one-way</span>';
      const answered = ping && pingAnswered(l) ? ' <span class="badge b-good term" data-term="answered">answered</span>' : "";
      return `<tr class="${l.up ? "" : "stale"}"><td><b>${l.id}</b></td><td>${state}${answered}</td>
        <td class="basic ${etxClass(l.etx)}">${l.up ? etxClass(l.etx) : "—"}</td>
        <td class="adv ${etxClass(l.etx)}">${etxText(l.etx)}</td><td class="adv">${phiCell(l)}</td><td class="adv">${helloText(l)}</td><td class="adv">${pct(l.rxq)}</td><td class="adv">${pct(l.txq)}</td>
        <td class="adv">${l.rssi} dBm</td><td class="adv">${l.snr} dB</td><td>${ago(l.heard_ms)}</td></tr>`;
    }).join("");
}

function renderRoutes() {
  $("rt-count").textContent = st.routes.length ? `${st.routes.filter(r => !r.retracted).length} reachable` : "";
  if (!st.routes.length) {
    $("routes").innerHTML = `<tr><td class="empty">${transport ? st.down ? "Node is down." : "No routes yet." : "—"}</td></tr>`;
    return;
  }
  $("routes").innerHTML = `<tr><th>Destination</th><th>${T("Next hop")}</th><th class="adv">${T("Metric")} (ETX)</th><th class="adv">${T("Seq")}</th><th></th></tr>` +
    [...st.routes].sort((a, b) => a.dest - b.dest).map(r => `<tr class="${r.retracted ? "stale" : ""}">
      <td><b>${r.dest}</b></td><td>${r.retracted ? "—" : r.next === r.dest ? `${r.next} <span class="muted">direct</span>` : r.next}</td>
      <td class="adv">${etxText(r.metric)}</td><td class="adv">${r.seq}</td>
      <td>${r.retracted ? '<span class="badge b-poor term" data-term="lost">lost</span>' : ""}</td></tr>`).join("");
}
