/* Shared state: the attached node's live tables, the simulator view, and the node clock. */
"use strict";
const $ = id => document.getElementById(id);

let transport = null;
let speed = 1;      // simulated ms per real ms right now (0 while paused)
let simStatus = null;  // the bridge's clock and control settings
const SPEEDS = [0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 50, 100];
let st;

function fresh() {
  return {
    ident: null, radio: null, links: [], routes: [], nodes: [], msgs: [], stats: null,
    sent: new Map(), inbox: [], storeDepth: null, clock: null, sim: null, down: false, ping: null,
  };
}
st = fresh();
let simIds = new Set();  // every simulator node seen this connection, so killed nodes stay on screen

/* Live state the attached node reports; cleared when the simulator kills it. */
function clearLive() {
  Object.assign(st, { links: [], routes: [], nodes: [], msgs: [], stats: null, storeDepth: null });
}

/* Take in a simulator @topo reply and track whether the attached node is alive. */
function noteSim(topo) {
  topo.alive.forEach(id => simIds.add(id));
  topo.links.forEach(([a, b]) => { simIds.add(a); simIds.add(b); });
  const down = !topo.alive.includes(topo.attached);
  if (st.down && !down) {
    st = Object.assign(fresh(), { sim: topo });  // revived: the node restarted with empty tables
    $("log").innerHTML = "";
    log(`node ${topo.attached} revived`, "ev");
  } else {
    if (down && !st.down) { clearLive(); log(`node ${topo.attached} is down`, "err"); }
    st.sim = topo;
  }
  st.down = down;
}
function simNodeIds() {
  return [...simIds].filter(x => Number.isInteger(x) && x > 0 && x < 255).sort((a, b) => a - b);
}

/* Node clock: the latest event time plus elapsed wall time. */
function nodeNow() {
  if (!st.clock) return null;
  return st.clock.t + (performance.now() - st.clock.at) * speed;
}
function noteClock(t) {
  if (typeof t === "number") st.clock = { t, at: performance.now() };
}

/* True when the simulator says this node is killed. */
const simDead = id => !!(st.sim && st.sim.alive && !st.sim.alive.includes(id));
