/* UI submits only physical positions. The server computes and commits the radio medium. */
(() => {
  const map = $('map'), hint = map.nextElementSibling;
  let mode = false, meta = null, nodes = [], radio = { power: 22, attenuation: 0 }, selected = 1;
  let generation = null, revision = null, applied = null, busy = false, queued = false, edit = 0, drag = null;
  let view = { x: 4387.5, y: 121632.5, width: 5000 }, overlay = null, coverageTimer, coverageToken = 0, loading = false;
  let image = null, initializedGeneration = null;
  const live = () => transport?.kind === 'sim' && !!st.sim;
  const bar = document.createElement('div'); bar.className = 'placement-bar'; bar.hidden = true;
  bar.innerHTML = '<label>View <select id="placement-view"><option value="graph">Network graph</option><option value="terrain">Terrain placement</option></select></label><span id="placement-active"></span>';
  map.before(bar);
  const panel = document.createElement('div'); panel.hidden = true;
  panel.innerHTML = `
    <div class="placement-bar"><button id="placement-enable" type="button">Enable live placement</button><button id="placement-reload" type="button">Reload current placement</button><button id="placement-fit" type="button">Fit nodes</button><button id="placement-zoom-in" type="button" aria-label="Zoom in">+</button><button id="placement-zoom-out" type="button" aria-label="Zoom out">−</button><label><input id="placement-coverage" type="checkbox" checked> Selected-node coverage</label><label><input id="placement-observed" type="checkbox" checked> Observed links</label></div>
    <div class="placement-map-wrap"><canvas id="placement-map" tabindex="0" role="img" aria-label="Malapert terrain. Drag nodes to reposition, drag background to pan. The form below also edits coordinates."></canvas><span id="placement-scale" class="placement-scale"></span></div>
    <div class="placement-info" id="placement-info">
      <button id="placement-info-button" type="button" aria-describedby="placement-info-tooltip" aria-expanded="false">More info ⓘ</button>
      <div id="placement-info-tooltip" class="placement-info-tooltip" role="tooltip">
        <p id="placement-status">Choose Terrain placement to load the local model.</p>
        <p>Drag a node and release to apply its new position. The server recalculates link conditions; the mesh then discovers and reroutes naturally. Routes may take simulated seconds to settle.</p>
        <p>Coverage is a sampled one-hop prediction to a 1.5 m receiver. A red link with positive margin may still communicate. Manual placement freezes recorded crew tracks and deploys pending relays; explicit kills, cuts and scheduled outages remain. Restart the network to return to its original scenario.</p>
        <p>Local 20 m elevation grid and R2 / P.526 propagation model from the <a href="https://lunar-link.github.io/terrain-sims/routing-website/" target="_blank" rel="noopener">Lunar Link Malapert planner</a>. Coverage is an estimate, not a delivery guarantee.</p>
      </div>
    </div>
    <p id="placement-error" role="alert" hidden></p>
    <details id="placement-details" class="placement-details">
      <summary>Node controls &amp; link details</summary>
      <div class="placement-details-content">
    <form id="placement-form" class="placement-form"><label>Node<select id="placement-node"></select></label><label>Map X from base (m)<input id="placement-x" type="number" step="1" required></label><label>Map Y from base (m)<input id="placement-y" type="number" step="1" required></label><label>Mast height (m)<input id="placement-height" type="number" min="0.5" max="30" step="0.5" required></label><button type="submit">Apply position</button><button id="placement-attach" type="button">Connect here</button></form>
    <div class="placement-bar"><span>433 MHz · SF9 / 125 kHz · isotropic antennas</span><label>Power <input id="placement-power" type="number" min="0" max="22" value="22"> dBm</label><label>Attenuation <input id="placement-attenuation" type="number" min="0" max="30" value="0"> dB</label><button id="placement-radio" type="button">Apply radio settings</button><button id="placement-export" type="button">Export</button><label>Import<input id="placement-import" type="file" accept=".json,application/json" style="max-width:170px"></label></div>
    <p id="placement-route"></p><div class="placement-pairs"><table id="placement-pairs"></table></div>
    <p class="placement-note">Margin: <span class="poor">red &lt; 10 dB</span>, <span class="fair">yellow 10–&lt;20 dB</span>, <span class="good">green ≥ 20 dB</span>. Dashed lines predict the weaker directional margin; solid lines are neighbor reports. Blue highlights the connected node’s reported first hop to the selected node. Gray marks offline/cut links. Pending edits are gray until applied.</p>
      </div>
    </details>
    `;
  map.after(panel);
  const cv = $('placement-map'), ctx = cv.getContext('2d');
  function message(text, bad = false) {
    $('placement-status').textContent = text;
    $('placement-error').hidden = !bad;
    $('placement-error').textContent = bad ? text : '';
  }
  const info = $('placement-info'), infoButton = $('placement-info-button');
  function setInfoOpen(open) { info.classList.toggle('is-open', open); infoButton.setAttribute('aria-expanded', String(open)); }
  infoButton.onclick = () => setInfoOpen(!info.classList.contains('is-open'));
  info.addEventListener('keydown', ev => { if (ev.key === 'Escape') { setInfoOpen(false); infoButton.blur(); ev.stopPropagation(); } });
  document.addEventListener('pointerdown', ev => { if (!info.contains(ev.target)) setInfoOpen(false); });
  async function request(url, body) {
    const response = await fetch(url, body === undefined ? {} : { method: 'POST', body: JSON.stringify(body) });
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.reason || 'Placement request failed');
    return data;
  }
  function editor() {
    const n = nodes.find(n => n.id === selected); if (!n || !meta) return;
    if ($('placement-node').options.length !== nodes.length) $('placement-node').replaceChildren(...nodes.map(n => new Option('Node ' + n.id, n.id)));
    for (const [id, value] of [['placement-node', selected], ['placement-x', Math.round(n.x - meta.terrain.bx)], ['placement-y', Math.round(n.y - meta.terrain.by)], ['placement-height', n.h], ['placement-power', radio.power], ['placement-attenuation', radio.attenuation]]) {
      if (document.activeElement !== $(id)) $(id).value = value;
    }
    $('placement-attach').disabled = !live() || st.sim.attached === selected || !st.sim.alive?.includes(selected);
    $('placement-enable').textContent = applied ? 'Recalculate links' : 'Enable live placement';
  }
  function valid(n) {
    const t = meta.terrain;
    return Number.isInteger(n.id) && [n.x, n.y, n.h].every(Number.isFinite) && n.h >= 0.5 && n.h <= 30 && Math.hypot(n.x - t.bx, n.y - t.by) <= t.rdisk - 40;
  }
  async function load() {
    if (loading || !live()) return;
    loading = true; message('Loading local terrain model…');
    const expected = simStatus?.generation;
    try {
      const result = await request('/sim/placement');
      if (expected !== simStatus?.generation || !mode) return;
      meta = result.meta; generation = result.generation; initializedGeneration = generation;
      revision = result.revision; nodes = result.nodes; radio = result.settings; applied = result.active ? result : null;
      if (!nodes.some(n => n.id === selected)) selected = nodes[0].id;
      image = new Image(); image.onload = draw; image.onerror = () => message('Terrain hillshade could not load.', true); image.src = '/sim/placement/map';
      editor(); fit(); table(); message(applied ? 'Live placement loaded. Drag a node to recalculate links.' : 'Preview only. Enable live placement, or drag a node to apply these positions.');
    } catch (error) { message(error.message, true); }
    finally { loading = false; }
  }
  async function commit() {
    if (!meta || !live() || !nodes.length) return;
    edit++; queued = true; overlay = null; coverageToken++;
    if (busy) { message('Newer position queued; calculating…'); return; }
    busy = true;
    while (queued && live()) {
      queued = false;
      const token = edit, expectedGeneration = generation;
      const snapshot = nodes.map(n => ({ ...n })), settings = { ...radio };
      message('Calculating terrain and updating the running radio medium…'); draw();
      try {
        const result = await request('/sim/control', { op: 'placement', generation, base_revision: revision, nodes: snapshot, settings });
        if (expectedGeneration !== simStatus?.generation) { queued = false; break; }
        revision = result.placement_revision; applied = result.placement;
        onSimStatus(result);
        if (token === edit) {
          nodes = applied.nodes.map(n => ({ ...n })); radio = { ...applied.settings };
          message('Placement applied. The protocol is discovering paths; send a message to test delivery.');
        }
        refresh();
      } catch (error) { queued = false; message(error.message + ' Use Reload current placement before retrying.', true); break; }
    }
    busy = false; editor(); table(); draw(); coverage();
    if (mode && initializedGeneration !== simStatus?.generation) load();
  }
  function dimensions() {
    const r = cv.getBoundingClientRect(), scale = devicePixelRatio || 1;
    if (cv.width !== Math.round(r.width * scale) || cv.height !== Math.round(r.height * scale)) { cv.width = Math.round(r.width * scale); cv.height = Math.round(r.height * scale); }
    return { w: r.width, h: r.height, scale };
  }
  function bounds() { const { w, h } = dimensions(); return { x: view.x - view.width / 2, y: view.y + view.width * h / Math.max(w, 1) / 2, width: view.width, height: view.width * h / Math.max(w, 1) }; }
  const point = (n, b, w) => [(n.x - b.x) / b.width * w, (b.y - n.y) / b.width * w];
  function world(ev) { const r = cv.getBoundingClientRect(), b = bounds(); return { x: b.x + (ev.clientX - r.left) / r.width * b.width, y: b.y - (ev.clientY - r.top) / r.width * b.width }; }
  function draw() {
    if (!mode || panel.hidden || !ctx) return;
    const { w, h, scale } = dimensions(); if (!w || !h) return;
    const b = bounds(); ctx.setTransform(scale, 0, 0, scale, 0, 0); ctx.clearRect(0, 0, w, h); ctx.fillStyle = '#111820'; ctx.fillRect(0, 0, w, h);
    if (!meta) return;
    const t = meta.terrain, extent = (t.n - 1) * t.cell;
    if (image?.complete && image.naturalWidth) { const [x, y] = point({ x: t.x0, y: t.ytop }, b, w); ctx.drawImage(image, x, y, extent / b.width * w, extent / b.width * w); }
    if (overlay && $('placement-coverage').checked && st.sim?.alive?.includes(selected) && !busy && !drag) {
      const [x, y] = point({ x: overlay.bounds.x, y: overlay.bounds.y }, b, w); ctx.imageSmoothingEnabled = false; ctx.drawImage(overlay.canvas, x, y, overlay.bounds.width / b.width * w, overlay.bounds.height / b.width * w); ctx.imageSmoothingEnabled = true;
    }
    const byId = new Map(nodes.map(n => [n.id, n]));
    const unavailable = new Set((st.sim?.margins || []).filter(p => p[4]).map(([a, b]) => LMeshLinkQuality.key(a, b)));
    function line(a, z, color, dashed, width = 2) {
      if (!byId.has(a) || !byId.has(z)) return;
      const [x, y] = point(byId.get(a), b, w), [xx, yy] = point(byId.get(z), b, w);
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(xx, yy); ctx.strokeStyle = color; ctx.lineWidth = width; ctx.setLineDash(dashed ? [5, 5] : []); ctx.stroke(); ctx.setLineDash([]);
    }
    for (const p of applied?.pairs || []) {
      const margin = Math.min(p.ab.margin, p.ba.margin), disabled = unavailable.has(LMeshLinkQuality.key(p.a, p.b));
      const c = disabled || busy || drag ? '#929eae' : margin < 10 ? '#ff8989' : margin < 20 ? '#edc46a' : '#6ed79a';
      line(p.a, p.b, c, true);
    }
    if ($('placement-observed').checked && !busy && !drag) {
      const add = (a, z, etx, stale) => line(a, z, stale ? '#929eae' : etx <= 1.5 ? '#6ed79a' : etx <= 3 ? '#edc46a' : '#ff8989', false, 2.5);
      if (st.ident) for (const l of st.links) if (l.up) add(st.ident.id, l.id, l.etx, false);
      for (const n of st.nodes) for (const [z, etx] of n.neighbors) add(n.node, z, etx, n.stale);
      const route = st.routes.find(r => r.dest === selected && !r.retracted);
      if (route && st.ident) line(st.ident.id, route.next, '#80b3ff', false, 5);
    }
    for (const n of nodes) {
      const [x, y] = point(n, b, w), alive = st.sim?.alive?.includes(n.id), attached = st.sim?.attached === n.id;
      ctx.beginPath(); ctx.arc(x, y, selected === n.id ? 19 : 15, 0, 2 * Math.PI); ctx.fillStyle = alive ? '#1d2833' : '#47262b'; ctx.fill(); ctx.strokeStyle = attached ? '#80b3ff' : selected === n.id ? '#fff' : alive ? '#6ed79a' : '#ff8989'; ctx.lineWidth = 3; ctx.stroke();
      ctx.fillStyle = '#fff'; ctx.font = 'bold 13px Arial'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.fillText(n.id, x, y); ctx.font = '12px Arial'; ctx.fillText('Node ' + n.id + (alive ? '' : ' · offline'), x, y + 29);
    }
    const meters = view.width < 4000 ? 500 : view.width < 15000 ? 1000 : 5000;
    ctx.strokeStyle = '#fff'; ctx.beginPath(); ctx.moveTo(15, h - 43); ctx.lineTo(15 + meters / b.width * w, h - 43); ctx.stroke(); $('placement-scale').textContent = `${meters} m · map north ↑`;
  }
  function table() {
    $('placement-pairs').innerHTML = '<tr><th>Pair</th><th>Distance</th><th>Weaker margin</th><th>Loss → / ←</th><th>Terrain path</th></tr>' + (applied?.pairs || []).filter(p => p.a === selected || p.b === selected).map(p => {
      const margin = Math.min(p.ab.margin, p.ba.margin), extra = simStatus?.loss || 0;
      const loss = p => Math.round(100 * (1 - (1 - p.loss / 100) * (1 - extra / 100)));
      return `<tr><td>${p.a} ↔ ${p.b}</td><td>${(p.ab.distance / 1000).toFixed(2)} km</td><td class="${margin < 10 ? 'poor' : margin < 20 ? 'fair' : 'good'}">${margin.toFixed(2)} dB</td><td>${loss(p.ab)}% / ${loss(p.ba)}%</td><td>${p.ab.los && p.ba.los ? 'Clear ray' : 'Obstructed'}</td></tr>`;
    }).join('');
    const route = st.routes.find(r => r.dest === selected && !r.retracted);
    $('placement-route').textContent = st.ident ? `Node ${st.ident.id} → node ${selected}: ${st.ident.id === selected ? 'this is the connected node' : route ? `protocol reports next hop ${route.next}, ETX ${route.metric}` : 'no route reported; unexpired messages wait in the store'}.` : 'Waiting for connected node reports.';
  }
  function coverage() {
    clearTimeout(coverageTimer); const token = ++coverageToken; overlay = null;
    if (!mode || !applied || busy || !$('placement-coverage').checked || !cv.clientWidth) return;
    coverageTimer = setTimeout(async () => {
      try {
        const result = await request('/sim/coverage', { generation, revision, node: selected, bounds: bounds() });
        if (token !== coverageToken || result.generation !== generation || result.revision !== revision) return;
        const canvas = document.createElement('canvas'); canvas.width = canvas.height = result.size;
        const pixels = new Uint8ClampedArray(result.size * result.size * 4);
        result.values.forEach((v, i) => { if (v !== null) pixels.set([...(v < 10 ? [255, 137, 137] : v < 20 ? [237, 196, 106] : [110, 215, 154]), v < 0 ? 42 : 90], i * 4); });
        canvas.getContext('2d').putImageData(new ImageData(pixels, result.size, result.size), 0, 0); overlay = { canvas, bounds: result.bounds }; draw();
      } catch (error) { if (token === coverageToken) message('Coverage unavailable: ' + error.message, true); }
    }, 200);
  }
  function fit() {
    if (!nodes.length) return;
    const xs = nodes.map(n => n.x), ys = nodes.map(n => n.y), { w, h } = dimensions();
    view = { x: (Math.min(...xs) + Math.max(...xs)) / 2, y: (Math.min(...ys) + Math.max(...ys)) / 2,
      width: Math.min(40000, Math.max(3000, (Math.max(...xs) - Math.min(...xs)) * 1.5, (Math.max(...ys) - Math.min(...ys)) * Math.max(1, w / Math.max(1, h)) * 1.5)) };
    draw(); coverage();
  }
  function zoom(factor) { view.width = Math.max(500, Math.min(40000, view.width * factor)); draw(); coverage(); }
  function render() {
    bar.hidden = !live(); panel.hidden = !mode || !live();
    map.toggleAttribute('hidden', !panel.hidden); hint.hidden = !panel.hidden;
    $('map-color').closest('label').hidden = !panel.hidden;
    $('placement-active').textContent = simStatus?.placement ? `Live placement · revision ${simStatus.placement_revision}` : '';
    if (!panel.hidden) { draw(); editor(); table(); return true; }
    return false;
  }
  $('placement-view').onchange = () => {
    mode = $('placement-view').value === 'terrain'; render();
    if (mode && initializedGeneration !== simStatus?.generation) load(); else if (mode) { draw(); coverage(); } else renderMap();
  };
  $('placement-enable').onclick = commit; $('placement-reload').onclick = () => { if (!busy) load(); };
  $('placement-fit').onclick = fit; $('placement-zoom-in').onclick = () => zoom(0.7); $('placement-zoom-out').onclick = () => zoom(1 / 0.7);
  $('placement-node').onchange = () => { selected = Number($('placement-node').value); editor(); table(); draw(); coverage(); };
  $('placement-attach').onclick = () => attachTo(selected);
  $('placement-coverage').onchange = () => { coverage(); draw(); }; $('placement-observed').onchange = draw;
  $('placement-form').onsubmit = ev => {
    ev.preventDefault(); if (!meta) return;
    const n = { id: selected, x: Number($('placement-x').value) + meta.terrain.bx, y: Number($('placement-y').value) + meta.terrain.by, h: Number($('placement-height').value) };
    if (!valid(n)) { message('Position must be inside the terrain disk; mast height must be 0.5–30 m.', true); return; }
    Object.assign(nodes.find(p => p.id === selected), n); commit();
  };
  $('placement-radio').onclick = () => {
    const power = Number($('placement-power').value), attenuation = Number($('placement-attenuation').value);
    if (!Number.isFinite(power) || power < 0 || power > 22 || !Number.isFinite(attenuation) || attenuation < 0 || attenuation > 30) { message('Power must be 0–22 dBm; attenuation 0–30 dB.', true); return; }
    radio = { power, attenuation }; commit();
  };
  cv.addEventListener('pointerdown', ev => {
    if (!meta || ev.button !== 0) return;
    const p = world(ev), tolerance = view.width / cv.clientWidth * 24;
    const n = nodes.find(n => Math.hypot(n.x - p.x, n.y - p.y) < tolerance);
    drag = { pointer: ev.pointerId, node: n, x: view.x, y: view.y, clientX: ev.clientX, clientY: ev.clientY, original: n ? { ...n } : null };
    cv.setPointerCapture(ev.pointerId); if (n) { selected = n.id; editor(); table(); } draw();
  });
  cv.addEventListener('pointermove', ev => {
    if (!drag || drag.pointer !== ev.pointerId) return;
    if (drag.node) { const next = { ...drag.node, ...world(ev) }; if (valid(next)) Object.assign(drag.node, next); }
    else { view.x = drag.x - (ev.clientX - drag.clientX) / cv.clientWidth * view.width; view.y = drag.y + (ev.clientY - drag.clientY) / cv.clientWidth * view.width; }
    overlay = null; coverageToken++; draw();
  });
  cv.addEventListener('pointerup', ev => { if (!drag || drag.pointer !== ev.pointerId) return; const moved = drag.node && (drag.node.x !== drag.original.x || drag.node.y !== drag.original.y); drag = null; editor(); if (moved) commit(); else coverage(); });
  cv.addEventListener('pointercancel', () => { if (drag?.node) Object.assign(drag.node, drag.original); drag = null; editor(); draw(); coverage(); });
  cv.addEventListener('wheel', ev => { ev.preventDefault(); zoom(ev.deltaY > 0 ? 1.15 : 1 / 1.15); }, { passive: false });
  cv.addEventListener('keydown', ev => {
    if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', '+', '-', 'Home'].includes(ev.key)) return;
    ev.preventDefault(); ev.stopPropagation();
    if (ev.key === 'Home') fit(); else if (ev.key === '+') zoom(0.7); else if (ev.key === '-') zoom(1 / 0.7);
    else { view.x += (ev.key === 'ArrowRight' ? 1 : ev.key === 'ArrowLeft' ? -1 : 0) * view.width / 10; view.y += (ev.key === 'ArrowUp' ? 1 : ev.key === 'ArrowDown' ? -1 : 0) * view.width / 10; draw(); coverage(); }
  });
  $('placement-export').onclick = () => { const url = URL.createObjectURL(new Blob([JSON.stringify({ format: 'lmesh-placement-v2', nodes, settings: radio }, null, 2)], { type: 'application/json' })); const a = document.createElement('a'); a.href = url; a.download = 'placement.json'; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); };
  $('placement-import').onchange = async () => {
    try {
      const file = $('placement-import').files[0]; if (!file || !meta) return;
      if (file.size > 100000) throw new Error('Placement file is too large');
      const value = JSON.parse(await file.text());
      if (value.format !== 'lmesh-placement-v2' || !Array.isArray(value.nodes) || value.nodes.length !== nodes.length || !value.nodes.every(valid) || new Set(value.nodes.map(n => n.id)).size !== nodes.length || !nodes.every(n => value.nodes.some(p => p.id === n.id))) throw new Error('Import a placement with the same node IDs');
      if (value.settings) {
        const { power, attenuation } = value.settings;
        if (!Number.isFinite(power) || power < 0 || power > 22 || !Number.isFinite(attenuation) || attenuation < 0 || attenuation > 30) throw new Error('Invalid imported radio settings');
        radio = { power, attenuation };
      }
      nodes = value.nodes.map(n => ({ id: n.id, x: n.x, y: n.y, h: n.h })).sort((a, b) => a.id - b.id); editor(); fit(); commit();
    } catch (error) { message(error.message, true); }
    $('placement-import').value = '';
  };
  function onStatus(s) {
    if (generation !== null && s.generation !== generation) {
      generation = s.generation; initializedGeneration = null; applied = null; queued = false; edit++; coverageToken++; overlay = null; nodes = [];
      if (mode && !busy && !loading) load();
    }
    if (!busy && !drag && s.placement && s.placement_revision !== revision) {
      applied = s.placement; nodes = applied.nodes.map(n => ({ ...n })); radio = { ...applied.settings }; revision = s.placement_revision; generation = s.generation; coverage();
    }
    render();
  }
  window.addEventListener('resize', () => { draw(); coverage(); });
  window.meshPlacement = { render, onStatus }; render();
})();
