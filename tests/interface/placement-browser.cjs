/* Optional real-Chrome QA. Run against a dedicated four-node test server; needs playwright-core. */
const assert = require('node:assert/strict');
const option = (name, fallback) => { const i = process.argv.indexOf(name); return i < 0 ? fallback : process.argv[i + 1]; };
const { chromium } = require(option('--playwright-module', 'playwright-core'));
const url = option('--url', 'http://localhost:8015/?sim');
const diamond = require('../../sim/terrain/placement-diamond.json').nodes;
async function main() {
  const browser = await chromium.launch({ headless: true, executablePath: option('--chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome') });
  const page = await browser.newPage({ viewport: { width: 1500, height: 1100 } });
  const errors = [];
  await page.addInitScript(() => {
    window.__qaCoverageDraws = 0;
    const draw = CanvasRenderingContext2D.prototype.drawImage;
    CanvasRenderingContext2D.prototype.drawImage = function (source, ...args) {
      if (this.canvas.id === 'placement-map' && source instanceof HTMLCanvasElement) window.__qaCoverageDraws++;
      return draw.call(this, source, ...args);
    };
  });
  page.on('pageerror', error => errors.push(error.message));
  const control = req => page.evaluate(async req => { const r = await fetch('/sim/control', { method: 'POST', body: JSON.stringify(req) }); const data = await r.json(); if (!r.ok) throw Error(data.reason); onSimStatus(data); return data; }, req);
  const waitApplied = rev => page.waitForFunction(rev => simStatus.placement_revision > rev && document.getElementById('placement-status').textContent.startsWith('Placement applied'), rev);
  async function step(seconds) {
    const before = await page.evaluate(() => simStatus.sim_ms);
    await control({ op: 'step', seconds });
    await page.waitForFunction(target => simStatus.sim_ms >= target, before + seconds * 1000);
    await page.evaluate(() => refresh());
  }
  async function importDiamond() {
    const rev = await page.evaluate(() => simStatus.placement_revision);
    await page.locator('#placement-import').setInputFiles({ name: 'diamond.json', mimeType: 'application/json',
      buffer: Buffer.from(JSON.stringify({ format: 'lmesh-placement-v2', nodes: diamond, settings: { power: 22, attenuation: 0 } })) });
    await waitApplied(rev);
  }
  async function editNode(id, x, y) {
    await page.selectOption('#placement-node', String(id));
    await page.fill('#placement-x', String(x)); await page.fill('#placement-y', String(y));
    const rev = await page.evaluate(() => simStatus.placement_revision);
    await page.locator('#placement-form button[type=submit]').click(); await waitApplied(rev);
  }
  async function send(content) {
    await page.locator('#tab-messages').click();
    await page.fill('#f-dest', '4'); await page.fill('#f-text', content); await page.locator('#send button[type=submit]').click();
    await page.waitForFunction(content => [...st.sent.values()].some(m => m.content === content), content);
  }
  const state = content => page.evaluate(content => [...st.sent.values()].find(m => m.content === content)?.state, content);
  try {
    await page.goto(url); await page.waitForFunction(() => transport?.kind === 'sim' && st.sim?.nodes?.length === 4);
    await control({ op: 'pause', value: true });
    await page.selectOption('#placement-view', 'terrain');
    await page.waitForFunction(() => document.getElementById('placement-node').options.length === 4);
    assert.equal(await page.locator('#map').isVisible(), false, 'SVG graph is hidden in terrain view');
    assert.equal(await page.locator('#placement-form').isVisible(), false, 'node controls start collapsed');
    await page.locator('#placement-details > summary').click();
    await importDiamond();
    assert.equal(await page.evaluate(() => simStatus.paused), true);
    await step(120);
    const first = await page.evaluate(() => st.routes.find(r => r.dest === 4 && !r.retracted)?.next);
    assert([2, 3].includes(first), 'initial route uses a real relay');
    await send('browser-before-move'); await step(60);
    assert.equal(await state('browser-before-move'), 'delivered');
    await page.locator('#tab-overview').click();
    await page.locator('#placement-fit').click();
    // Drag a real canvas node. Compute its display coordinate from the documented fit rule.
    const rect = await page.locator('#placement-map').boundingBox();
    const x0 = 3637.5, y0 = 121382.5;
    const width = Math.max(3000, 2250, 1500 * Math.max(1, rect.width / rect.height));
    const n = diamond.find(n => n.id === first);
    const sx = rect.x + rect.width / 2 + (n.x - x0) / width * rect.width;
    const sy = rect.y + rect.height / 2 - (n.y - y0) / width * rect.width;
    const rev = await page.evaluate(() => simStatus.placement_revision);
    await page.mouse.move(sx, sy); await page.mouse.down();
    await page.mouse.move(rect.x + 20, rect.y + rect.height - 50, { steps: 12 }); await page.mouse.up();
    await waitApplied(rev);
    const moved = await page.evaluate(id => simStatus.placement.nodes.find(n => n.id === id), first);
    assert(Math.hypot(moved.x - n.x, moved.y - n.y) > 100, 'drag changed physical coordinates');
    const oldMargin = await page.evaluate(id => simStatus.placement.pairs.find(p => p.a === 1 && p.b === id).ab.margin, first);
    assert(oldMargin < 0, 'dragged relay now has a terrain-blocked first hop');
    await step(180);
    assert.equal(await page.evaluate(() => st.routes.find(r => r.dest === 4 && !r.retracted)?.next), 5 - first);
    await send('browser-after-drag'); await step(60);
    assert.equal(await state('browser-after-drag'), 'delivered');
    await page.locator('#tab-overview').click();
    await editNode(4, 9000, 9000); await step(180);
    assert.equal(await page.evaluate(() => st.routes.some(r => r.dest === 4 && !r.retracted)), false);
    await send('browser-held'); await step(60); assert.equal(await state('browser-held'), 'queued');
    await page.locator('#tab-overview').click();
    await editNode(4, -1500, -750); await step(180);
    assert.equal(await state('browser-held'), 'delivered');
    // Hold the first HTTP response while a newer edit is submitted. Only the newest draft should win.
    await importDiamond(); await page.selectOption('#placement-node', '2');
    let release, held;
    const gate = new Promise(resolve => { release = resolve; });
    const seen = new Promise(resolve => { held = resolve; });
    let intercept = true;
    await page.route('**/sim/control', async route => {
      if (intercept && route.request().postDataJSON()?.op === 'placement') {
        intercept = false; const response = await route.fetch(); held(); await gate; await route.fulfill({ response });
      } else await route.continue();
    });
    await page.fill('#placement-x', '-950'); await page.locator('#placement-form button[type=submit]').click(); await seen;
    await page.fill('#placement-x', '-850'); await page.locator('#placement-form button[type=submit]').click(); release();
    await page.waitForFunction(() => simStatus.placement.nodes.find(n => n.id === 2).x === 3537.5 && document.getElementById('placement-status').textContent.startsWith('Placement applied'));
    await page.unroute('**/sim/control');
    // A rejected edit must leave the actual radio medium unchanged.
    const stableRevision = await page.evaluate(() => simStatus.placement_revision);
    await page.fill('#placement-x', '999999'); await page.locator('#placement-form button[type=submit]').click();
    assert.equal(await page.evaluate(() => simStatus.placement_revision), stableRevision);
    await page.locator('#placement-reload').click();
    await page.waitForFunction(() => document.getElementById('placement-status').textContent.startsWith('Live placement loaded'));
    await page.waitForFunction(() => window.__qaCoverageDraws > 0);
    await page.selectOption('#placement-view', 'graph');
    assert.equal(await page.locator('#map').isVisible(), true);
    await page.selectOption('#placement-view', 'terrain');
    assert.equal(await page.locator('#map').isVisible(), false);
    const coverageCount = await page.evaluate(() => window.__qaCoverageDraws);
    await page.waitForFunction(n => window.__qaCoverageDraws > n, coverageCount);
    await page.screenshot({ path: option('--screenshot', '/private/tmp/mesh-placement-verified.png'), fullPage: true });
    assert.deepEqual(errors, [], 'no uncaught browser errors');
    console.log('PASS: real canvas drag -> server DEM -> C medium -> alternate route -> delivery; partition recovery; paused edits; rapid-edit fencing; invalid edit rejection');
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
