/* Move live panels into an Overview layout without copying controls or connection state. */
(() => {
  const overview = document.getElementById('panel-overview');
  const key = 'lunar-mesh-dashboard-v1';
  const cards = new Map();
  let dragging = null;
  let maximized = null;
  let saved;
  try { saved = JSON.parse(localStorage.getItem(key)); } catch {}
  const defaults = [['self'], ['network']];
  const columns = [0, 1].map(() => {
    const column = document.createElement('div');
    column.className = 'dashboard-column';
    return column;
  });
  function button(label, action) {
    const el = document.createElement('button');
    el.type = 'button'; el.textContent = label; el.onclick = action;
    return el;
  }
  // Compact window-style control: glyph shown, label used for tooltip and screen readers.
  function iconButton(glyph, label, action) {
    const el = button(glyph, action);
    el.className = 'dashboard-icon'; setLabel(el, glyph, label);
    return el;
  }
  function setLabel(el, glyph, label) { el.textContent = glyph; el.title = label; el.setAttribute('aria-label', label); }
  function setMin(card, on) {
    setLabel(card.min, on ? '+' : '–', on ? 'Expand' : 'Minimize');
    card.min.setAttribute('aria-expanded', String(!on));
  }
  function persist() {
    const layout = columns.map(c => [...c.children].filter(e => e.dataset.card).map(e => e.dataset.card));
    const minimized = [...cards].filter(([, c]) => c.el.classList.contains('is-minimized')).map(([id]) => id);
    try { localStorage.setItem(key, JSON.stringify({ layout, minimized })); } catch {}
  }
  function resize() { requestAnimationFrame(() => window.dispatchEvent(new Event('resize'))); }
  function maximize(card) {
    if (maximized && maximized !== card) maximize(maximized);
    const on = !card.el.classList.contains('is-maximized');
    card.el.classList.toggle('is-maximized', on);
    if (on) { card.el.classList.remove('is-minimized'); setMin(card, false); }
    setLabel(card.max, on ? '❐' : '□', on ? 'Restore size (Esc)' : 'Maximize');
    card.max.setAttribute('aria-pressed', String(on));
    maximized = on ? card : null;
    persist(); resize();
  }
  function makeCard(id, el, heading, tab = null) {
    el.dataset.card = id; el.classList.add('dashboard-card');
    const handle = document.createElement('span');
    handle.className = 'dashboard-handle'; handle.textContent = '⠿'; handle.setAttribute('aria-hidden', 'true');
    heading.prepend(handle);
    heading.classList.add('dashboard-titlebar'); heading.draggable = true;
    heading.title = 'Drag to move this panel; double-click to maximize';
    const actions = document.createElement('span'); actions.className = 'dashboard-actions';
    const card = { el, tab, heading };
    card.min = iconButton('–', 'Minimize', () => {
      if (maximized === card) maximize(card);
      setMin(card, el.classList.toggle('is-minimized')); persist(); resize();
    });
    card.min.setAttribute('aria-expanded', 'true');
    card.max = iconButton('□', 'Maximize', () => maximize(card));
    card.max.setAttribute('aria-pressed', 'false');
    actions.append(card.min, card.max);
    if (tab) actions.append(iconButton('×', 'Return to tab', () => undock(id)));
    heading.append(actions);
    const interactive = t => t.closest?.('button, a, input, select, textarea, summary, .dashboard-actions, .tools');
    let pressedOn = null;
    heading.addEventListener('pointerdown', ev => { pressedOn = ev.target; });
    heading.addEventListener('dragstart', ev => {
      if (ev.target !== heading || interactive(pressedOn)) { ev.preventDefault(); return; }
      startDrag(ev, id);
    });
    heading.addEventListener('dragend', endDrag);
    heading.addEventListener('dblclick', ev => { if (!interactive(ev.target)) { window.getSelection()?.removeAllRanges(); maximize(card); } });
    cards.set(id, card);
    return card;
  }
  const sections = [...overview.children];
  makeCard('self', sections[0], sections[0].querySelector('h2'));
  makeCard('network', sections[1], sections[1].querySelector('h2'));
  for (const id of ['messages', 'network', 'sim', 'console']) {
    const tab = document.getElementById('tab-' + id);
    const panel = document.getElementById('panel-' + id);
    const cardId = 'tab-' + id;
    tab.draggable = true;
    tab.title = 'Drag into Overview to dock as a panel';
    tab.addEventListener('dragstart', ev => startDrag(ev, cardId));
    tab.addEventListener('dragend', endDrag);
    const wrapper = document.createElement('section');
    const heading = document.createElement('h2'); heading.textContent = tab.childNodes[0].textContent.trim();
    wrapper.append(heading);
    const card = makeCard(cardId, wrapper, heading, tab);
    card.panel = panel;
  }
  const toolbar = document.createElement('div'); toolbar.className = 'dashboard-toolbar';
  const hint = document.createElement('span'); hint.className = 'layout-hint';
  hint.textContent = 'Drag a tab into either column. Drag a panel title to rearrange; double-click it to maximize.';
  toolbar.append(hint, button('Reset layout', () => {
    for (const [id, card] of cards) {
      if (card.tab) undock(id);
      card.el.classList.remove('is-minimized'); setMin(card, false);
    }
    if (maximized) maximize(maximized);
    defaults.forEach((ids, i) => ids.forEach(id => dock(id, columns[i])));
    selectTab(document.getElementById('tab-overview')); persist();
  }));
  overview.classList.add('dashboard'); overview.append(toolbar, ...columns);
  function dock(id, column, before = null) {
    const card = cards.get(id); if (!card) return;
    if (card.tab) {
      card.el.append(card.panel); card.panel.hidden = false;
      card.panel.removeAttribute('role'); card.panel.removeAttribute('aria-labelledby');
      card.tab.dataset.docked = 'true';
    }
    column.insertBefore(card.el, before || column.querySelector('.dashboard-drop'));
    persist(); resize();
  }
  function undock(id) {
    const card = cards.get(id); if (!card?.tab) return;
    if (maximized === card) maximize(card);
    document.querySelector('main').append(card.panel);
    card.panel.setAttribute('role', 'tabpanel'); card.panel.setAttribute('aria-labelledby', card.tab.id);
    card.panel.hidden = true; card.el.remove(); delete card.tab.dataset.docked;
    card.tab.focus();
    persist(); resize();
  }
  function startDrag(ev, id) {
    dragging = id; ev.dataTransfer.setData('text/plain', id); ev.dataTransfer.effectAllowed = 'move';
    document.body.classList.add('dashboard-dragging');
    setTimeout(() => selectTab(document.getElementById('tab-overview')), 0);
  }
  function endDrag() {
    dragging = null; document.body.classList.remove('dashboard-dragging');
    document.querySelectorAll('.is-over').forEach(e => e.classList.remove('is-over'));
  }
  for (const column of columns) {
    const drop = document.createElement('div'); drop.className = 'dashboard-drop'; drop.textContent = 'Drop a tab or panel here';
    column.append(drop);
    column.addEventListener('dragover', ev => {
      if (!dragging) return; ev.preventDefault(); ev.dataTransfer.dropEffect = 'move'; drop.classList.add('is-over');
    });
    column.addEventListener('dragleave', ev => { if (!column.contains(ev.relatedTarget)) drop.classList.remove('is-over'); });
    column.addEventListener('drop', ev => {
      if (!dragging) return; ev.preventDefault();
      const target = ev.target.closest('.dashboard-card');
      let before = drop;
      if (target && target.parentElement === column && target.dataset.card !== dragging) {
        before = ev.clientY < target.getBoundingClientRect().top + target.offsetHeight / 2 ? target : target.nextSibling;
      }
      dock(dragging, column, before); endDrag();
    });
  }
  window.meshDashboard = {
    openDocked(id) {
      const card = cards.get(id);
      if (!card || !overview.contains(card.el)) return false;
      selectTab(document.getElementById('tab-overview'));
      card.el.scrollIntoView({ block: 'nearest' }); card.min.focus();
      return true;
    },
  };
  new MutationObserver(() => {
    const card = cards.get('tab-sim');
    card.el.hidden = card.tab.hidden;
  }).observe(document.getElementById('tab-sim'), { attributes: true, attributeFilter: ['hidden'] });
  document.addEventListener('keydown', ev => { if (ev.key === 'Escape' && maximized) { const card = maximized; maximize(card); card.max.focus(); } });
  const layout = Array.isArray(saved?.layout) ? saved.layout : defaults;
  const seen = new Set();
  columns.forEach((column, i) => {
    for (const id of Array.isArray(layout[i]) ? layout[i] : []) {
      if (!cards.has(id) || seen.has(id)) continue;
      seen.add(id); dock(id, column, column.lastElementChild);
    }
  });
  for (const [i, id] of ['self', 'network'].entries()) if (!seen.has(id)) dock(id, columns[i], columns[i].lastElementChild);
  for (const id of Array.isArray(saved?.minimized) ? saved.minimized : []) {
    const card = cards.get(id); if (card) { card.el.classList.add('is-minimized'); setMin(card, true); }
  }
  const simCard = cards.get('tab-sim'); simCard.el.hidden = simCard.tab.hidden;
  persist();
})();
