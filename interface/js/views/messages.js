/* Messages tab: send form, sent history, and received messages. */
"use strict";
const STATE_CLASS = { queued: "", transmitted: "b-fair", delivered: "b-good", expired: "b-poor", failed: "b-poor" };

function renderSent() {
  renderHealth();  // its waiting count follows the sent list
  const rows = [...st.sent.values()].filter(m => !st.ident || m.origin === st.ident.id).sort((a, b) => b.id - a.id);
  if (!rows.length) { $("sent").innerHTML = `<tr><td class="empty">Nothing sent yet.</td></tr>`; return; }
  $("sent").innerHTML = `<tr><th>Id</th><th>To</th><th>${T("Type")}</th><th>${T("Priority")}</th><th>State</th><th>Detail</th><th>Content</th></tr>` +
    rows.map(m => {
      const detail = m.state === "delivered" ? (m.result && m.result !== "delivered" ? m.result : "") : (m.reason && m.reason !== "none" ? m.reason.replace("_", " ") : "");
      return `<tr><td>${m.id}</td><td>${m.dest ?? "—"}</td><td>${m.type || "—"}</td><td>${m.prio || "—"}</td>
        <td><span class="badge ${STATE_CLASS[m.state] || ""}">${m.state || "—"}</span></td><td class="muted">${esc(detail)}</td>
        <td class="wrap">${esc(m.content ?? "")}</td></tr>`;
    }).join("");
}

function renderInbox() {
  const messages = new Map(st.inbox.map(m => [`${m.origin}:${m.id}`, m]));
  if (st.ident) {
    for (const m of st.sent.values()) {
      if (m.origin === st.ident.id || m.dest !== st.ident.id || m.state !== "delivered" || m.content === undefined) continue;
      const key = `${m.origin}:${m.id}`;
      if (!messages.has(key)) messages.set(key, { ...m, recovered: true });
    }
  }
  const rows = [...messages.values()];
  $("rx-count").textContent = rows.length || "";
  rxTotal = rows.length;
  renderRxBadge();
  if (!rows.length) { $("inbox").innerHTML = `<div class="empty">Nothing received yet.</div>`; return; }
  $("inbox").innerHTML = rows.map(m => {
    const hex = m.recovered ? m.type === "tel" : m.text === undefined;
    const content = m.recovered ? m.content : m.text ?? m.hex;
    const hops = m.hops === undefined ? "" : ` · ${m.hops} ${T(m.hops === 1 ? "hop" : "hops", "hops")}`;
    const when = m.recovered ? " · delivery confirmed from this page's send history" : ` · ${m.at}`;
    return `<div class="m">
      <div class="meta">from <b>${m.origin}</b> · ${m.type} · ${m.prio || "normal"}${hops}${when}</div>
      <div class="body ${hex ? "hex" : ""}">${esc(content)}</div></div>`;
  }).join("");
}

function renderKnown() {
  const ids = new Set([...st.nodes.map(n => n.node), ...st.routes.map(r => r.dest)]);
  $("known").innerHTML = [...ids].sort((a, b) => a - b).map(id => `<option value="${id}">`).join("");
}

/* Send form */
$("f-type").onchange = () => { $("f-prio").disabled = $("f-type").value !== "text"; };
$("send").addEventListener("submit", async ev => {
  ev.preventDefault();
  if (!transport) return;
  const dest = $("f-dest").value, type = $("f-type").value, life = $("f-life").value;
  let content = $("f-text").value.replace(/[\r\n]+/g, " ").trim();
  if (!content) return;
  if (type === "tel") content = content.replace(/\s+/g, "");
  const line = type === "text" ? `send ${dest} ${$("f-prio").value} ${life} ${content}` : `${type} ${dest} ${life} ${content}`;
  try {
    const res = await command(line, false);
    if (res.ok) {
      const key = `${res.origin}:${res.id}`;
      const m = st.sent.get(key) || { origin: res.origin, id: res.id, dest: Number(dest), state: "queued" };
      m.content = content;
      st.sent.set(key, m);
      $("f-text").value = "";
      renderSent();
    } else {
      log(`send refused: ${res.reason}`, "err");
    }
  } catch (e) { log(String(e.message || e), "err"); }
});
