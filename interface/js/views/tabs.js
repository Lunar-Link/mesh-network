/* Theme, Advanced mode, and tab switching. */
"use strict";
/* Theme: light or dark, remembered across visits. */
function updateThemeButton() {
  const dark = document.documentElement.dataset.theme === "dark";
  $("btn-theme").textContent = dark ? "Light mode" : "Dark mode";
  $("btn-theme").setAttribute("aria-pressed", String(dark));
}
$("btn-theme").onclick = () => {
  const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = theme;
  updateThemeButton();
  try { localStorage.setItem("lunar-mesh-theme", theme); } catch {}
};
updateThemeButton();

/* Advanced mode: off by default; shows what only matters when debugging. Remembered across visits. */
$("adv").checked = "advanced" in document.documentElement.dataset;
$("adv").onchange = () => {
  const on = $("adv").checked;
  document.documentElement.toggleAttribute("data-advanced", on);
  try { localStorage.setItem("lunar-mesh-advanced", on ? "1" : "0"); } catch {}
  if (!on) {
    if ($("f-type").value !== "text") { $("f-type").value = "text"; $("f-type").onchange(); }  // no hidden non-text sends
    if ($("console-mode").value !== "meaning") { $("console-mode").value = "meaning"; $("console-mode").dispatchEvent(new Event("change")); }
    $("show-poll").checked = false;
  }
  renderMap();  // panel widths may have changed
};

/* Tabs: only visibility changes, so the connection, polling, and console history carry on in the background. */
const tabs = [...document.querySelectorAll('[role="tab"]')];
let rxTotal = 0, rxSeen = 0;  // received-message count, and how many were on screen when Messages was last open
const currentTab = () => tabs.find(t => t.getAttribute("aria-selected") === "true");
const tabShown = t => !t.hidden && getComputedStyle(t).display !== "none";  // hidden, or by CSS outside advanced mode
function renderRxBadge() {
  if (currentTab() === $("tab-messages") || rxSeen > rxTotal) rxSeen = rxTotal;
  const unread = rxTotal - rxSeen;
  $("tab-rx-count").textContent = unread ? `(${unread} new)` : "";
}
let restoreSim = false;  // the saved tab was Simulator: open it once the simulator connects
function selectTab(tab) {
  restoreSim = false;
  for (const t of tabs) {
    const on = t === tab;
    t.setAttribute("aria-selected", String(on));
    t.tabIndex = on ? 0 : -1;
    $(t.getAttribute("aria-controls")).hidden = !on;
  }
  try { localStorage.setItem("lunar-mesh-tab", tab.id); } catch {}
  if (tab.id === "tab-overview") renderMap();  // the map sizes itself to its width, which is zero while hidden
  if (tab.id === "tab-console") $("log").scrollTop = $("log").scrollHeight;
  renderRxBadge();
}
for (const tab of tabs) {
  tab.onclick = () => selectTab(tab);
  tab.onkeydown = ev => {  // arrow keys move between tabs, per the WAI-ARIA tabs pattern
    const shown = tabs.filter(tabShown);
    const i = shown.indexOf(tab);
    const next = { ArrowRight: shown[(i + 1) % shown.length], ArrowLeft: shown[(i - 1 + shown.length) % shown.length],
      Home: shown[0], End: shown[shown.length - 1] }[ev.key];
    if (!next) return;
    ev.preventDefault();
    selectTab(next);
    next.focus();
  };
}
