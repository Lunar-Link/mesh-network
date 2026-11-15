/* Formatting helpers and the phi (φ) math, shared by every view. */
"use strict";
/* Formatting */
const esc = s => String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
function etxClass(e) {
  if (e === "inf" || e === undefined || e === null) return "poor";
  return e <= 1.5 ? "good" : e <= 3 ? "fair" : "poor";
}
const etxText = e => e === "inf" ? "∞" : Number(e).toFixed(2);
function ago(ms) {
  const now = nodeNow();
  if (now === null || ms === undefined) return "—";
  const s = Math.max(0, Math.round((now - ms) / 1000));
  return s < 90 ? `${s} s ago` : s < 5400 ? `${Math.round(s / 60)} min ago` : `${Math.round(s / 3600)} h ago`;
}
function duration(s) {
  if (s === null || s === undefined) return "—";
  s = Math.floor(s);
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : m ? `${m} min ${s % 60} s` : `${s} s`;
}
const volts = mv => mv ? (mv / 1000).toFixed(2) + " V" : "—";
const pct = q => Math.round(q / 255 * 100) + " %";

/* Phi accrual, as the node computes it (SPEC 7.3): phi = log10(1 + e^z), z = y (1.5976 + 0.070566 y^2),
   y = (t - mean - pause) / sd, t = time since the neighbor's last HELLO. Computed here between polls so it climbs live. */
const PHI_MAX = 136.6;
function phiAt(t, mean, sd) {
  const y = Math.max(-16, Math.min(16, (t - mean) / Math.max(sd, 1)));
  const z = y * (1.5976 + 0.070566 * y * y);
  const p = z > 0 ? z / Math.LN10 + Math.log10(1 + Math.exp(-z)) : Math.log10(1 + Math.exp(z));
  return Math.min(PHI_MAX, p);
}
const phiDown = () => (st.ident && st.ident.phi_down) || 8;
const phiPause = () => (st.ident && st.ident.phi_pause_ms) || 0;
function livePhi(l) {
  const now = nodeNow();
  if (now === null || l.hello_at_ms === undefined || !l.hello_ms) return l.phi;
  return phiAt(now - l.hello_at_ms, l.hello_ms + phiPause(), l.hello_sd_ms);
}
/* Silence after the last HELLO at which phi reaches the down threshold. */
function silenceLimit(l) {
  let lo = 0, hi = 16;
  for (let i = 0; i < 40; i++) { const m = (lo + hi) / 2; if (phiAt(m, 0, 1) >= phiDown()) hi = m; else lo = m; }
  return l.hello_ms + phiPause() + hi * l.hello_sd_ms;
}
function phiClass(p) {
  return p < 1 ? "good" : p < phiDown() / 2 ? "fair" : "poor";
}
function phiCell(l) {
  if (l.phi === undefined) return "—";
  if (!l.up) return '<span class="muted">—</span>';
  const p = livePhi(l);
  const w = Math.min(100, p / phiDown() * 100);
  const limit = l.hello_ms ? silenceLimit(l) : null;
  const tipLine = limit === null ? GLOSSARY["φ"] : `Dropped after ${(limit / 1000).toFixed(0)} s without a HELLO.`;
  return `<span class="phi" data-tip="${tipLine}"><span class="bar"><i class="${phiClass(p)}" style="width:${w.toFixed(0)}%"></i></span><span class="${phiClass(p)}">${p.toFixed(2)}</span></span>`;
}
const helloText = l => l.hello_ms ? `${(l.hello_ms / 1000).toFixed(1)} s <span class="muted">± ${(l.hello_sd_ms / 1000).toFixed(1)}</span>` : "—";
const clockText = () => new Date().toLocaleTimeString([], { hour12: false });
