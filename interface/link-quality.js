/* Link margin is received power above sensitivity, not SNR, RSSI or ETX. */
(function (root) {
  const key = (a, b) => a < b ? `${a}-${b}` : `${b}-${a}`;
  function marginClass(margin) {
    if (!Number.isFinite(margin)) return "stale";
    return margin < 10 ? "poor" : margin < 20 ? "fair" : "good";
  }
  function margins(rows) {
    const result = new Map();
    for (const row of Array.isArray(rows) ? rows : []) {
      if (!Array.isArray(row) || row.length < 4) continue;
      const [a, b, ab, ba] = row;
      if (!Number.isInteger(a) || !Number.isInteger(b) || a === b) continue;
      result.set(key(a, b), Number.isFinite(ab) && Number.isFinite(ba) ? Math.min(ab, ba) : null);
    }
    return result;
  }
  const api = { key, marginClass, margins };
  root.LMeshLinkQuality = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
