/* Console tab: the log, plain-language explanations, and typed commands. */
"use strict";
/* Console log */
function log(text, cls, meaning = text) {
  const box = $("log");
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 4;
  const div = document.createElement("div");
  if (cls) div.className = cls;
  div.dataset.time = clockText();
  div.dataset.raw = text;
  div.dataset.meaning = meaning;
  div.textContent = `${div.dataset.time}  ${$("console-mode").value === "raw" ? text : meaning}`;
  box.appendChild(div);
  while (box.childElementCount > 600) box.firstChild.remove();
  if (atBottom) box.scrollTop = box.scrollHeight;
}

function explainCommand(line) {
  const [verb, ...args] = line.trim().split(/\s+/);
  const names = {
    id: "Checking this node's identity",
    nodes: "Checking status reports from the network",
    links: "Checking nearby node links",
    routes: "Checking paths to other nodes",
    msgs: "Checking stored messages",
    stats: "Checking node counters",
    status: "Broadcasting this node's status",
    ping: "Pinging neighbors with a HELLO beacon",
  };
  if (names[verb]) return `${names[verb]}…`;
  if (["send", "cmd", "tel"].includes(verb)) {
    const dest = args[0] || "?";
    const action = verb === "send" ? "message" : verb === "cmd" ? "command" : "telemetry";
    return `Sending a ${action} to node ${dest}…`;
  }
  return `Sending command: ${line}`;
}

function explainLine(raw) {
  let o;
  try { o = JSON.parse(raw); } catch { return raw; }

  if (o.ev) {
    switch (o.ev) {
      case "boot": return "Node started.";
      case "error": return `Node reported an error: ${o.what || "unknown error"}.`;
      case "radio":
        return o.state === "ready" ? `Radio is ready (${o.driver || "radio"}).` : `Radio ${o.state || "status changed"}${o.reply ? `: ${o.reply}` : ""}.`;
      case "neighbor":
        return `Neighbor ${o.id} is ${o.state === "up" ? "reachable" : "down"}${o.etx !== undefined ? ` (ETX ${o.etx})` : ""}.`;
      case "link":
        return `Link to node ${o.id}: ETX ${o.etx ?? "—"}, signal ${o.rssi ?? "—"} dBm, signal-to-noise ${o.snr ?? "—"} dB.`;
      case "route":
        return o.next ? `Path to node ${o.dest} now goes through node ${o.next} (cost ${o.metric ?? "unknown"}).` : `No route to node ${o.dest} is currently available.`;
      case "msg": {
        const state = ({ queued: "queued", transmitted: "sent to the next node", delivered: "delivered", expired: "expired before delivery", failed: "failed" })[o.state] || o.state || "updated";
        const extra = o.reason && o.reason !== "none" ? ` (${String(o.reason).replaceAll("_", " ")})` : o.result && o.result !== "delivered" ? ` (${o.result})` : "";
        return `Message ${o.id} to node ${o.dest}: ${state}${extra}.`;
      }
      case "rx":
        return `Received a ${o.type || "data"} message from node ${o.origin} over ${o.hops} hop${o.hops === 1 ? "" : "s"}: ${o.text ?? o.hex ?? "[no content]"}`;
      case "status": return `Received a status update from node ${o.node}.`;
      case "store": return `Message store now has ${o.depth} item${o.depth === 1 ? "" : "s"}.`;
      default: return raw;
    }
  }

  if (o.ok === false && o.reason === "too_soon") return "Ping refused: the last one was too recent.";
  if (o.ok === false) return `Command failed: ${String(o.reason || "unknown reason").replaceAll("_", " ")}.`;
  if (o.id !== undefined && o.net !== undefined) return `This is node ${o.id} on network ${o.net}${o.station ? ` (${o.station})` : ""}.`;
  if (o.dest !== undefined && o.next !== undefined) {
    return o.retracted || !o.next ? `Node ${o.dest} is unreachable.` : `To reach node ${o.dest}, send through node ${o.next} (route cost ${o.metric ?? "unknown"}; sequence ${o.seq ?? "unknown"}).`;
  }
  if (o.batt_mv !== undefined && o.node !== undefined) return `Node ${o.node} status: battery ${volts(o.batt_mv)}, uptime ${duration(o.uptime_s)}, ${o.neighbors?.length ?? 0} neighbors.`;
  if (o.etx !== undefined && o.id !== undefined && o.up !== undefined) {
    return `Neighbor ${o.id} is ${o.up ? "reachable" : "down"}; ${o.bidir ? "the link works both ways" : "the link is not confirmed in both directions"} (ETX ${o.etx}${o.phi !== undefined ? `, φ ${o.phi}` : ""}).`;
  }
  if (o.delivered !== undefined && o.ack_timeouts !== undefined) return `Node counters: ${o.delivered} messages delivered, ${o.ack_timeouts} acknowledgment timeouts, ${o.link_failures} link failures.`;
  if (o.role !== undefined && o.origin !== undefined && o.id !== undefined) return `Message ${o.id} from node ${o.origin}: ${o.state || o.role}${o.dest !== undefined ? `, addressed to node ${o.dest}` : ""}.`;
  if (o.ok === true && o.origin !== undefined && o.id !== undefined) return `Message ${o.id} accepted by node ${o.origin}.`;
  if (o.ok === true) return "Command completed.";
  return raw;
}

$("console-mode").addEventListener("change", () => {
  const mode = $("console-mode").value;
  const box = $("log");
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 4;
  for (const row of box.children) row.textContent = `${row.dataset.time}  ${row.dataset[mode] || row.dataset.raw}`;
  if (atBottom) box.scrollTop = box.scrollHeight;
});

/* Console input */
const HELP = [  // the line protocol of SPEC.md Section 11; "help" itself is answered by the page, not the node
  ["id", "this node's identity and settings"],
  ["nodes", "status reports heard from the network"],
  ["links", "neighbors, with ETX, signal, and φ"],
  ["routes", "the route table"],
  ["msgs", "messages this node originated or holds"],
  ["stats", "frame and delivery counters"],
  ["status", "broadcast this node's status now"],
  ["ping", "ask neighbors to answer with a HELLO"],
  ["send <dest> <u|n|r> <lifetime_s> <text>", "send a text message (urgent, normal, routine)"],
  ["cmd <dest> <lifetime_s> <text>", "send a command"],
  ["tel <dest> <lifetime_s> <hex>", "send telemetry as hex"],
  ["cancel <id>", "drop a message this node sent, if not yet transmitted"],
];
const SIM_HELP = [
  ["@attach <node>", "plug into another node"],
  ["@kill <node>, @revive <node>", "take a node down or bring it back"],
  ["@cut <a> <b>, @join <a> <b> [loss]", "remove or restore a link"],
];
function showHelp() {
  const rows = transport && transport.kind === "sim" ? [...HELP, ...SIM_HELP] : HELP;
  const width = Math.max(...rows.map(([c]) => c.length));
  log("Commands:", "help");
  for (const [c, what] of rows) log(`  ${c.padEnd(width)}  ${what}`, "help");
}
$("cmd").addEventListener("keydown", async ev => {
  if (ev.key !== "Enter") return;
  const line = $("cmd").value.trim();
  if (line.toLowerCase() === "help" || line === "?") { $("cmd").value = ""; showHelp(); return; }
  if (!line || !transport) return;
  $("cmd").value = "";
  try {
    const res = await command(line, false);
    if (res.ok && /^(send|cmd|tel) /.test(line)) {
      const m = st.sent.get(`${res.origin}:${res.id}`);
      if (m) { m.content = line.split(" ").slice(line.startsWith("send") ? 4 : 3).join(" "); renderSent(); }
    }
  } catch (e) { log(String(e.message || e), "err"); }
});
$("btn-clear").onclick = () => { $("log").innerHTML = ""; };
