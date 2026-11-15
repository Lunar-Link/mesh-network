/* Plain-language term meanings and the hover tooltip. */
"use strict";
/* Plain-language meanings for terms on the page. Rest the pointer on a dotted-underlined term to see one. */
const GLOSSARY = {
  "ETX": "Expected transmissions: how many tries a message needs to cross this link, counting the acknowledgment. 1.0 is perfect; higher means a weaker link.",
  "φ": "Suspicion that this neighbor has gone silent, from how regularly its HELLO beacons usually arrive. 1 means a 10 % chance it is alive and only late, 2 means 1 %. The node drops the neighbor at 8.",
  "HELLO": "A short beacon every node broadcasts about every 10 s, so neighbors know it is there and how well they hear it.",
  "HELLO every": "Average time between this neighbor's HELLO beacons, ± how much it varies. Lost beacons make it longer and more spread out.",
  "Ping": "Broadcast a HELLO beacon now. Every neighbor that hears it answers with its own.",
  "answered": "A HELLO from this neighbor arrived after the ping.",
  "RSSI": "Received signal strength, in dBm. Closer to 0 is stronger; around −120 is very weak.",
  "SNR": "Signal-to-noise ratio, in dB: how far the signal stands above background noise. Higher is better.",
  "We hear": "Share of this neighbor's recent HELLO beacons that this node received.",
  "Hears us": "Share of this node's recent HELLO beacons that the neighbor reports receiving.",
  "one-way": "We hear this neighbor, but it has not reported hearing us, so it is not used for routing yet.",
  "Next hop": "The neighbor a message is handed to first on its way to the destination.",
  "Metric": "Total ETX along the path to the destination. Lower is better.",
  "Seq": "Route sequence number set by the destination. A higher number means newer route information.",
  "lost": "No route to this destination right now. Messages for it wait in the store.",
  "stale": "No status heard from this node for a while, so its information may be out of date.",
  "Station": "The amateur radio call sign the node identifies with.",
  "Store": "Messages this node is holding, waiting to be forwarded or confirmed.",
  "Lost": "Messages that node dropped (expired or pushed out of a full store) since its last status.",
  "Health": "Warning flags the node reports about itself.",
  "Uptime": "Time since the node last started.",
  "Neighbors": "Nearby nodes heard in both directions.",
  "Reachable": "Destinations this node currently has a route to.",
  "Delivered": "Messages delivered to this node, plus this node's own messages confirmed by a receipt.",
  "Airtime": "Total time this node's radio has spent transmitting.",
  "Ack timeouts": "Times the next node did not confirm receiving a frame in time.",
  "Link failures": "Times a frame could not reach a neighbor after every retry.",
  "Dropped": "Messages removed before delivery: expired, pushed out of a full store, or past the hop limit.",
  "Bad frames": "Received frames rejected for a bad security tag, another network's id, or damage.",
  "DATA": "Frames carrying messages.",
  "HOP_ACK": "Confirmation from the next node that it took a message.",
  "STATUS": "A node's health report, flooded through the network about once a minute.",
  "ROUTE_REQ": "A request for fresh route information after a route is lost.",
  "Type": "Text is for people. Command asks the destination to run an action and report a result. Telemetry is machine data, entered as hex.",
  "Priority": "Urgent messages go first; routine ones still get a fair share.",
  "Lifetime": "How long the network keeps trying to deliver the message before giving up.",
  "hops": "How many radio links the message crossed.",
  "Network": "Network id. Nodes ignore frames from other networks.",
  "Link loss": "Share of frames the simulator drops on every link that is up, and on links you join.",
  "Random outages": "Share of time nodes and links spend down at random, and how long each outage lasts on average.",
  "Scheduled downtime": "Take a node down after a delay, once or repeating. Repeat 0 means once; for example, down 300 s every 900 s for a node that sleeps to save power.",
};
GLOSSARY.Frames = "Radio frames this node has sent and received, by type. " + GLOSSARY.HELLO;
/* A term with a tooltip if the glossary has it, plain text otherwise. */
const T = (text, key = text) => GLOSSARY[key] ? `<span class="term" data-term="${esc(key)}">${text}</span>` : text;

/* Tooltip: appears after the pointer rests on a term for a moment, follows tables that re-render underneath it. */
const TIP_DELAY_MS = 700;
const tip = document.createElement("div");
tip.id = "tip";
document.body.appendChild(tip);
let tipFor = null, tipTimer = null, mouseX = 0, mouseY = 0;
const tipText = el => el.dataset.tip || GLOSSARY[el.dataset.term] || "";
function hideTip() {
  clearTimeout(tipTimer);
  tipTimer = null; tipFor = null;
  tip.style.display = "none";
}
function showTip() {
  let el = tipFor;
  if (el && !el.isConnected) {  // the table re-rendered: use whatever term is now under the pointer
    const now = document.elementFromPoint(mouseX, mouseY);
    el = now && now.closest("[data-term],[data-tip]");
  }
  const text = el && tipText(el);
  if (!text) { hideTip(); return; }
  tip.textContent = text;
  tip.style.display = "block";
  const r = el.getBoundingClientRect();
  const x = Math.min(Math.max(8, r.left), innerWidth - tip.offsetWidth - 8);
  const below = r.bottom + 6 + tip.offsetHeight < innerHeight;
  tip.style.left = `${x}px`;
  tip.style.top = `${below ? r.bottom + 6 : r.top - tip.offsetHeight - 6}px`;
}
document.addEventListener("mousemove", e => { mouseX = e.clientX; mouseY = e.clientY; });
document.addEventListener("mouseover", e => {
  const el = e.target.closest("[data-term],[data-tip]");
  if (el && tipFor && tipText(el) === tipText(tipFor)) { tipFor = el; return; }  // same term, re-rendered
  hideTip();
  if (!el) return;
  tipFor = el;
  tipTimer = setTimeout(showTip, TIP_DELAY_MS);
});
document.addEventListener("mousedown", hideTip);
window.addEventListener("scroll", hideTip);  // the page, not the console's own auto-scroll
document.addEventListener("wheel", hideTip, { passive: true });
