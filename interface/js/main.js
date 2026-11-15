/* Startup, and renderAll, which redraws every view from the shared state. */
"use strict";
function renderAll() {
  renderDown(); renderIdent(); renderHealth(); renderSelf(); renderMap(); renderNodes(); renderLinks(); renderRoutes(); renderSent(); renderInbox(); renderSim(); renderKnown();
}

(async () => {
  try {
    const r = await fetch("/sim/info");
    if (r.ok) {
      const info = await r.json();
      simAvailable = true;
      speed = info.paused ? 0 : info.speed || 1;
      simStatus = info;
    }
  } catch {}
  setConnected(null);
  renderAll();
  try {
    const saved = $(localStorage.getItem("lunar-mesh-tab"));
    if (saved && tabs.includes(saved)) saved.hidden ? (restoreSim = true) : tabShown(saved) && selectTab(saved);
  } catch {}
  if (simAvailable && new URLSearchParams(location.search).has("sim")) connectSim();
})();
