/* Line protocol: the command queue, reply matching, and node events. */
"use strict";
/* Command queue: one command on the wire at a time; dump rows collect until the end line. */
const queue = [];
let pending = null;
/* Replies carry no tag, so they are matched to commands by order. A command can say what its reply looks like:
   `rows` for a table dump (rows then "end"), `accept` to recognize its "ok" line. A reply that does not fit, such as
   the late answer to a command that timed out, is dropped instead of being taken as this command's reply. */
function command(line, quiet = true, { rows = false, accept = null } = {}) {
  return new Promise((resolve, reject) => {
    queue.push({ line, quiet, wantRows: rows, accept, resolve, reject });
    pump();
  });
}
function pump() {
  if (pending || !queue.length || !transport) return;
  pending = queue.shift();
  pending.rows = [];
  pending.timer = setTimeout(() => finish(new Error(`no reply to "${pending.line}"`)), 4000);
  if (!pending.quiet || $("show-poll").checked) log("> " + pending.line, "tx", explainCommand(pending.line));
  transport.send(pending.line).catch(e => finish(e));
}
function finish(err, result) {
  const p = pending;
  if (!p) return;
  pending = null;
  clearTimeout(p.timer);
  if (err) p.reject(err); else p.resolve(result);
  pump();
}
function dropQueue() {
  if (pending) finish(new Error("disconnected"));
  while (queue.length) queue.shift().reject(new Error("disconnected"));
}

function onLine(raw) {
  let o;
  try { o = JSON.parse(raw); } catch { log(raw); return; }
  if (o.ev) {
    log(raw, o.ev === "error" || (o.ev === "radio" && o.state === "error") ? "err" : "ev", explainLine(raw));
    onEvent(o);
    return;
  }
  const show = pending && (!pending.quiet || $("show-poll").checked);
  if (show || !pending) log(raw, o.ok === false ? "err" : "row", explainLine(raw));
  if (!pending) return;
  if ("end" in o) {
    if (pending.wantRows || !pending.accept) finish(null, { rows: pending.rows, truncated: !!o.truncated });
    else pending.rows = [];  // the tail of a stray table; keep waiting for this command's own reply
  } else if ("ok" in o) {
    if (o.ok === false || (!pending.wantRows && (!pending.accept || pending.accept(o)))) finish(null, o);
    else pending.rows = [];  // a stray "ok" line
  } else pending.rows.push(o);
}

/* Events */
let refreshSoon = null;
function scheduleRefresh() {
  if (refreshSoon) return;
  refreshSoon = setTimeout(() => { refreshSoon = null; refresh(); }, 800);
}

function onEvent(e) {
  noteClock(e.t);
  switch (e.ev) {
    case "boot":
      st = Object.assign(fresh(), { sim: st.sim });
      $("banner").style.display = "none";
      start();
      return;
    case "error":
      showBanner(`Node reported an error: ${e.what || "unknown"}`);
      break;
    case "radio":
      st.radio = e;
      if (e.state === "error") showBanner(`Radio error: ${e.cmd || ""} → ${e.reply || ""}`);
      renderIdent();
      break;
    case "msg": {
      const key = `${e.origin}:${e.id}`;
      const m = st.sent.get(key) || { origin: e.origin, id: e.id };
      Object.assign(m, { dest: e.dest, type: e.type, prio: e.prio, state: e.state, reason: e.reason, result: e.result, at: clockText() });
      st.sent.set(key, m);
      renderSent();
      scheduleRefresh();
      break;
    }
    case "rx":
      st.inbox.unshift({ ...e, at: clockText() });
      if (st.inbox.length > 200) st.inbox.pop();
      renderInbox();
      break;
    case "store":
      st.storeDepth = e.depth;
      renderSelf();
      break;
    default:
      scheduleRefresh();
  }
}

function showBanner(text) {
  $("banner").textContent = text;
  $("banner").style.display = "block";
}
