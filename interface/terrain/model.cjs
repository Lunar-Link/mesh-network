// Upstream Lunar Link R2 model, snapshot 2026-11-13. See ATTRIBUTION.md.
// R2 hybrid link model, P.526 chain, radial coverage sweep, mesh and rate functions; runs under Node, in the page and in a Web Worker.
"use strict";
var LL = (function () {
  var C0 = 299792458.0, R_MOON = 1737400.0, EPS_REG = 2.5, TAND_REG = 0.004, EPS0 = 8.8541878128e-12;
  var FINAL = { lmin: 30.0, gap: 200.0, q: 0.5, rmax: 20000.0 };
  var LOG10 = Math.log(10), PI = Math.PI, DEG = 180 / Math.PI;

  function log10(x) { return Math.log(x) / LOG10; }
  function sigma(f) { return 2 * PI * f * EPS0 * EPS_REG * TAND_REG; }
  function J(v) {
    if (!(v > -0.78)) return 0;
    var a = v - 0.1;
    return 6.9 + 20 * log10(Math.sqrt(a * a + 1) + a);
  }
  function roundEven(x) {
    var r = Math.round(x);
    if (Math.abs(x - Math.trunc(x)) === 0.5 && r % 2 !== 0) r -= 1;
    return r;
  }
  function mapScale(x, y) { var t2 = (x * x + y * y) / (4 * R_MOON * R_MOON); return 1 + t2; }

  // ---- P.526 smooth sphere, sec. 3
  function Params(f) {
    this.f = f; this.lam = C0 / f; this.k = 2 * PI / this.lam; this.sig = sigma(f);
    this.fm = f / 1e6;
    this.ec_re = EPS_REG; this.ec_im = -60 * this.sig * (C0 / f);
    var fm = this.fm, q = 18000 * this.sig / fm;
    this.kfac = Math.pow((EPS_REG - 1) * (EPS_REG - 1) + q * q, -0.25);
    this.ksq = Math.sqrt(EPS_REG * EPS_REG + q * q);
    this.fm13 = Math.cbrt(fm); this.fm23 = this.fm13 * this.fm13;
    this.cache = this.firstTermSetup(R_MOON);
  }
  Params.prototype.firstTermSetup = function (ae) {
    var aek = ae / 1000;
    var KH = 0.36 * Math.pow(aek * this.fm, -1 / 3) * this.kfac;
    var K = KH * this.ksq, K2 = K * K, K4 = K2 * K2;
    var beta = (1 + 1.6 * K2 + 0.67 * K4) / (1 + 4.5 * K2 + 1.53 * K4);
    return { beta: beta, Gmin: 2 + 20 * log10(K), Xf: 2.188 * beta * this.fm13 * Math.pow(aek, -2 / 3), Yf: 9.575e-3 * beta * this.fm23 * Math.pow(aek, -1 / 3) };
  };
  function firstTerm(c, d, h1, h2) {
    var X = c.Xf * (d / 1000);
    var F = X >= 1.6 ? 11 + 10 * log10(X) - 17.6 * X : -20 * log10(X) - 5.6488 * Math.pow(X, 1.425);
    return F + Gh(c, h1) + Gh(c, h2);
  }
  function Gh(c, h) {
    var B = c.beta * (c.Yf * h), g;
    if (B > 2) { var q = Math.max(B - 1.1, 1e-9); g = 17.6 * Math.sqrt(q) - 5 * log10(q) - 8; }
    else g = 20 * log10(B + 0.1 * B * B * B);
    return g > c.Gmin ? g : c.Gmin;
  }
  function sphEarth(p, d, h1, h2) {
    var ae = R_MOON, s1 = Math.sqrt(h1), s2 = Math.sqrt(h2);
    var dlos = Math.sqrt(2 * ae) * (s1 + s2);
    if (d >= dlos) return -firstTerm(p.cache, d, h1, h2);
    var c = (h1 - h2) / (h1 + h2), m = d * d / (4 * ae * (h1 + h2));
    var arg = 1.5 * c * Math.sqrt(3 * m / ((m + 1) * (m + 1) * (m + 1)));
    arg = arg < -1 ? -1 : arg > 1 ? 1 : arg;
    var b = 2 * Math.sqrt((m + 1) / (3 * m)) * Math.cos(PI / 3 + Math.acos(arg) / 3);
    var d1 = d / 2 * (1 + b), d2 = d - d1;
    var hc = ((h1 - d1 * d1 / (2 * ae)) * d2 + (h2 - d2 * d2 / (2 * ae)) * d1) / d;
    var hreq = 0.552 * Math.sqrt(Math.max(d1 * d2, 0) * p.lam / d);
    if (hc > hreq) return 0;
    var q = d / (s1 + s2), aem = 0.5 * q * q;
    var Ah = -firstTerm(p.firstTermSetup(aem), d, h1, h2);
    return Ah < 0 ? 0 : (1 - hc / hreq) * Ah;
  }

  function corrected(Luc, D) { return Luc + (1 - Math.exp(-Luc / 6)) * (10 + 0.02 * D / 1000); }

  // ---- P.526 sec. 4.5 on a single uniformly spaced profile (exact port)
  var bull = { los: false, vmax: 0, itim: 0, irim: 0, Stim: 0, Srim: 0, db: 0, vb: 0, Luc: 0, Lb: 0 };
  function bullington(z, N, D, hts, hrs, lam, ae) {
    var st = D / (N - 1), Stim = -Infinity, Srim = -Infinity, vmax = -Infinity, itim = 1, irim = 1;
    var i2ae = 1 / (2 * ae), rc = 2 * D / lam, ihD = 1 / D;
    for (var i = 1; i < N - 1; i++) {
      var d = i * st, dr = D - d, ze = z[i] + d * dr * i2ae;
      var a = (ze - hts) / d; if (a > Stim) { Stim = a; itim = i; }
      var b = (ze - hrs) / dr; if (b > Srim) { Srim = b; irim = i; }
      var hl = (hts * dr + hrs * d) * ihD;
      var v = (ze - hl) * Math.sqrt(rc / (d * dr)); if (v > vmax) vmax = v;
    }
    var Str = (hrs - hts) / D, los = Stim < Str;
    var den = los ? 1 : Math.max(Stim + Srim, 1e-12);
    var db = (hrs - hts + Srim * D) / den;
    db = db < 1e-3 ? 1e-3 : db > D - 1e-3 ? D - 1e-3 : db;
    var vb = (hts + Stim * db - (hts * (D - db) + hrs * db) / D) * Math.sqrt(2 * D / (lam * db * (D - db)));
    var Luc = los ? J(vmax) : J(vb);
    bull.los = los; bull.vmax = vmax; bull.itim = itim; bull.irim = irim; bull.Stim = Stim; bull.Srim = Srim; bull.db = db; bull.vb = vb; bull.Luc = Luc; bull.Lb = corrected(Luc, D);
    return bull;
  }

  var zero = new Float64Array(0);
  function p526(z, N, D, h_tx, h_rx, p) {
    var hts = z[0] + h_tx, hrs = z[N - 1] + h_rx, st = D / (N - 1), lam = p.lam;
    var b0 = bullington(z, N, D, hts, hrs, lam, R_MOON);
    var Lba = b0.Lb, los = b0.los, vmax = b0.vmax, itim = b0.itim, irim = b0.irim, Stim = b0.Stim, Srim = b0.Srim, db = b0.db, vb = b0.vb, Luc = b0.Luc;
    var v1 = 0, v2 = 0, hobs = -Infinity, aobt = -Infinity, aobr = -Infinity;
    for (var i = 0; i < N - 1; i++) {
      var d0 = i * st, d1 = (i + 1) * st, dd = d1 - d0;
      v1 += dd * (z[i + 1] + z[i]);
      v2 += dd * (z[i + 1] * (2 * d1 + d0) + z[i] * (d1 + 2 * d0));
    }
    var hstip = (2 * v1 * D - v2) / (D * D), hsrip = (v2 - v1 * D) / (D * D);
    for (i = 1; i < N - 1; i++) {
      var d = i * st, h = z[i] - (hts * (D - d) + hrs * d) / D;
      if (h > hobs) hobs = h;
      if (h / d > aobt) aobt = h / d;
      if (h / (D - d) > aobr) aobr = h / (D - d);
    }
    var pos = hobs > 0, den = pos ? aobt + aobr : 1;
    var hstp = pos ? hstip - hobs * aobt / den : hstip, hsrp = pos ? hsrip - hobs * aobr / den : hsrip;
    var hst = Math.min(hstp, z[0]), hsr = Math.min(hsrp, z[N - 1]), htse = hts - hst, hrse = hrs - hsr;
    if (zero.length < N) zero = new Float64Array(N * 2);
    var Lbs = bullington(zero, N, D, htse, hrse, lam, R_MOON).Lb;
    var Lsph = sphEarth(p, D, htse, hrse);
    var Ld = Lba + Math.max(Lsph - Lbs, 0);
    var rng = Math.sqrt(D * D + (hrs - hts) * (hrs - hts));
    var fspl = 20 * log10(4 * PI * rng / lam);
    return { Ld: Ld, Lba: Lba, fspl: fspl, los: los, vmax: vmax, itim: itim, irim: irim, Stim: Stim, Srim: Srim, db: db, vb: vb, Luc: Luc, hts: hts, hrs: hrs, L: fspl + Ld };
  }

  // ---- hybrid terms
  function Tmn(m, n) {
    var mn = m * n, sm = Math.sqrt(m > 0 ? m : 0), m15 = m * sm, m2 = m * m;
    if (mn <= 4) return 7.2 * sm - (2 - 12.5 * n) * m + 3.6 * m15 - 0.8 * m2;
    return -6 - 20 * log10(Math.max(mn, 1e-30)) + 7.2 * sm - (2 - 17 * n) * m + 3.6 * m15 - 0.8 * m2;
  }
  function runT(th0, R0, dc, D, lam, dth, rmax) {
    var th = th0 + dth;
    if (th <= 0) return 0;
    var d1 = dc > 1 ? dc : 1, d2 = D - dc > 1 ? D - dc : 1, R = R0 < rmax ? R0 : rmax;
    var k3 = Math.cbrt(PI * R / lam), m = R * (d1 + d2) / (d1 * d2) / k3, n = th * k3 / m;
    var t = Tmn(m, n);
    return t > 0 ? t : 0;
  }
  function gammaWeighted(p, psi, phi, sinc, w) {
    var s = Math.sin(psi), co = Math.cos(psi), ar = p.ec_re - co * co, ai = p.ec_im;
    var mag = Math.sqrt(Math.sqrt(ar * ar + ai * ai)), ang = Math.atan2(ai, ar) / 2;
    var rr = mag * Math.cos(ang), ri = mag * Math.sin(ang), er = p.ec_re * s, ei = p.ec_im * s;
    var nr = er - rr, ni = ei - ri, dr = er + rr, di = ei + ri, dn = dr * dr + di * di;
    var gr = (nr * dr + ni * di) / dn * w, gi = (ni * dr - nr * di) / dn * w;
    var P = 1 + gr * gr + gi * gi + 2 * (gr * Math.cos(phi) + gi * Math.sin(phi)) * sinc;
    return -10 * log10(P > 1e-20 ? P : 1e-20);
  }
  function solve3c2(a, b) {
    var m = [a[0], a[1], a[2], b[0], a[3], a[4], a[5], b[1], a[6], a[7], a[8], b[2]];
    for (var c = 0; c < 3; c++) {
      var pv = c, mx = Math.abs(m[c * 4 + c]);
      for (var r = c + 1; r < 3; r++) { var q = Math.abs(m[r * 4 + c]); if (q > mx) { mx = q; pv = r; } }
      if (pv !== c) for (var j = 0; j < 4; j++) { var tmp = m[c * 4 + j]; m[c * 4 + j] = m[pv * 4 + j]; m[pv * 4 + j] = tmp; }
      for (r = c + 1; r < 3; r++) {
        var fac = m[r * 4 + c] / m[c * 4 + c];
        for (j = c; j < 4; j++) m[r * 4 + j] -= fac * m[c * 4 + j];
      }
    }
    return m[2 * 4 + 3] / m[2 * 4 + 2];
  }
  function parabolaRadius(ze, st, i0, i1) {
    if (i1 - i0 < 2) return Infinity;
    var n = i1 - i0 + 1, mid = 0.5 * (i0 + i1), S2 = 0, S4 = 0, y0 = 0, y2 = 0;
    for (var i = i0; i <= i1; i++) { var x = i - mid, x2 = x * x; S2 += x2; S4 += x2 * x2; y0 += ze[i]; y2 += ze[i] * x2; }
    var c2 = (n * y2 - S2 * y0) / (n * S4 - S2 * S2) / (st * st);
    return c2 < 0 ? -1 / (2 * c2) : Infinity;
  }
  function foregroundFit(ze, st, i0, i1) {
    var n = i1 - i0 + 1, sx = 0, sxx = 0, sy = 0, sxy = 0;
    for (var i = i0; i <= i1; i++) { var x = i * st; sx += x; sxx += x * x; sy += ze[i]; sxy += ze[i] * x; }
    var den = n * sxx - sx * sx, ok = n >= 2 && Math.abs(den) > 1e-9;
    var slope = ok ? (n * sxy - sx * sy) / den : 0;
    return { slope: slope, icpt: (sy - slope * sx) / Math.max(n, 1) };
  }

  var buf = { ze: new Float64Array(0), yy: new Float64Array(0), hs: new Int32Array(0), runs: [] };
  function ensure(N) {
    if (buf.ze.length < N) { buf.ze = new Float64Array(N * 2); buf.yy = new Float64Array(N * 2); buf.hs = new Int32Array(N * 2); }
  }

  // Full model on one profile: z has N uniformly spaced samples including both ends, D is the true path length.
  function hybridProfile(z, N, D, ht, hr, p, off) {
    var o = FINAL, c = p526(z, N, D, ht, hr, p), lam = p.lam;
    var res = { L: c.L, L526: c.L, fspl: c.fspl, los: c.los, theta: c.los ? 0 : c.Stim + c.Srim, T: 0, A_tx: 0, A_rx: 0, nrun: 0, A_obs: c.Luc };
    var cvv = D / (2 * R_MOON), s0 = (c.hrs - c.hts) / D;
    res.elT = Math.atan(c.los ? s0 - cvv : c.Stim - cvv) * DEG; res.elR = Math.atan(c.los ? -s0 - cvv : c.Srim - cvv) * DEG;
    res.Ld = c.Ld; res.hts = c.hts; res.hrs = c.hrs; res.itim = c.itim; res.irim = c.irim; res.Stim = c.Stim; res.Srim = c.Srim; res.db = c.db; res.runs = [];
    if (off || c.los) return res;
    ensure(N);
    var st = D / (N - 1), ze = buf.ze, yy = buf.yy, hs = buf.hs, hts = c.hts, hrs = c.hrs, i;
    for (i = 0; i < N; i++) { var d = i * st; ze[i] = z[i] + d * (D - d) / (2 * R_MOON); yy[i] = ze[i]; }
    yy[0] = hts; yy[N - 1] = hrs;
    var h = 0;
    for (i = 0; i < N; i++) {
      while (h >= 2) {
        var a = hs[h - 2], b = hs[h - 1];
        if ((b - a) * (yy[i] - yy[a]) - (yy[b] - yy[a]) * (i - a) >= 0) h--; else break;
      }
      hs[h++] = i;
    }
    var runs = [];
    if (h > 2) {
      var ni = h - 2, cur = -1, thSum = 0, ib = -1;
      for (var j = 1; j <= ni; j++) {
        var s0 = (yy[hs[j]] - yy[hs[j - 1]]) / ((hs[j] - hs[j - 1]) * st), s1 = (yy[hs[j + 1]] - yy[hs[j]]) / ((hs[j + 1] - hs[j]) * st), turn = s0 - s1;
        if (cur >= 0 && (hs[j] - hs[j - 1]) * st <= o.gap) { thSum += turn; ib = hs[j]; }
        else { if (cur >= 0) runs.push(makeRun(cur, ib, thSum, st, ze, N, o)); cur = hs[j]; ib = hs[j]; thSum = turn; }
      }
      runs.push(makeRun(cur, ib, thSum, st, ze, N, o));
    }
    var T = 0;
    for (i = 0; i < runs.length; i++) T += runT(runs[i].th, runs[i].R, runs[i].dc, D, lam, 0, o.rmax);
    var Aknife = J(c.vb), Aobs = Aknife + T, th = c.Stim + c.Srim;
    var sides = [0, 0];
    for (var sd = 0; sd < 2; sd++) {
      var ih = sd === 0 ? c.itim : c.irim, ant = sd === 0 ? hts : hrs;
      var ag = sd === 0 ? Math.max(ih * st, 1) : Math.max(D - ih * st, 1);
      var fit = sd === 0 ? foregroundFit(ze, st, 0, ih) : foregroundFit(ze, st, ih, N - 1);
      var lterm = fit.icpt + fit.slope * (sd === 0 ? 0 : D), lhor = fit.icpt + fit.slope * (ih * st);
      var hp = Math.max(ant - lterm, 0.05), he = ze[ih] - lhor, dth = 2 * hp / ag, psi = Math.max((hp + he) / ag, 0);
      var Ai = Aobs, Radj = Infinity;
      var sc = th > 0 ? (th + dth) / Math.max(th, 1e-12) : 1;
      if (runs.length) {
        var r0 = sd === 0 ? runs[0] : runs[runs.length - 1];
        Ai = Aobs + (J(c.vb * sc) - Aknife) + runT(r0.th, r0.R, r0.dc, D, lam, dth, o.rmax) - runT(r0.th, r0.R, r0.dc, D, lam, 0, o.rmax);
        Radj = r0.R;
      }
      var r = Math.pow(10, -(Ai - Aobs) / 20);
      var Rf = sd === 0 ? parabolaRadius(ze, st, 0, ih) : parabolaRadius(ze, st, ih, N - 1);
      var wgt = isFinite(Radj) ? Math.min(Math.max(1 - Radj / Rf, 0), 1) : 1;
      var phi = p.k * 2 * hp * Math.max(he, 0) / ag, qa = o.q * phi, sinc = qa > 1e-9 ? Math.sin(qa) / qa : 1;
      sides[sd] = th > 0 ? gammaWeighted(p, psi, phi, sinc, r * wgt) : 0;
    }
    var L = Math.max(c.L, c.fspl + Aobs + sides[0] + sides[1]);
    res.L = L; res.T = T; res.A_tx = sides[0]; res.A_rx = sides[1]; res.nrun = runs.length; res.A_obs = Aobs; res.runs = runs;
    return res;
  }
  function makeRun(i0, i1, th, st, ze, N, o) {
    var ln = (i1 - i0) * st, R = Math.max(ln, o.lmin) / Math.max(th, 1e-12);
    if (ln < o.lmin) {
      var ic = (i0 + i1) >> 1, nw = Math.max(1, roundEven(0.5 * 2 * o.lmin / st));
      var Rf = parabolaRadius(ze, st, Math.max(ic - nw, 0), Math.min(ic + nw, N - 1));
      if (Rf < R) R = Rf;
    }
    return { th: th, R: R, dc: 0.5 * (i0 + i1) * st, i0: i0, i1: i1 };
  }

  // ---- terrain
  function Terrain(z, nr, nc, x0, ytop, cell, bx, by, rdisk) {
    this.z = z; this.nr = nr; this.nc = nc; this.x0 = x0; this.ytop = ytop; this.cell = cell; this.bx = bx; this.by = by; this.rdisk = rdisk;
  }
  Terrain.prototype.sample = function (x, y) {
    var c = (x - this.x0) / this.cell, r = (this.ytop - y) / this.cell, nc = this.nc, nr = this.nr;
    var c0 = Math.floor(c), r0 = Math.floor(r);
    c0 = c0 < 0 ? 0 : c0 > nc - 2 ? nc - 2 : c0; r0 = r0 < 0 ? 0 : r0 > nr - 2 ? nr - 2 : r0;
    var fc = c - c0, fr = r - r0, z = this.z, i = r0 * nc + c0;
    var top = (1 - fc) * z[i] + fc * z[i + 1], bot = (1 - fc) * z[i + nc] + fc * z[i + nc + 1];
    return (1 - fr) * top + fr * bot;
  };
  Terrain.prototype.profile = function (sx, sy, tx, ty, step) {
    var dm = Math.hypot(tx - sx, ty - sy), N = Math.max(3, Math.ceil(dm / step) + 1), z = new Float64Array(N);
    for (var i = 0; i < N; i++) { var t = i / (N - 1); z[i] = this.sample(sx + (tx - sx) * t, sy + (ty - sy) * t); }
    return { z: z, N: N, D: dm / mapScale((sx + tx) / 2, (sy + ty) / 2) };
  };
  function linkLoss(ter, sx, sy, tx, ty, ht, hr, p, off) {
    var dm = Math.hypot(tx - sx, ty - sy);
    if (dm < 1) return { L: 0, L526: 0, los: true, D: dm, theta: 0, elT: 0, elR: 0 };
    var q = ter.profile(sx, sy, tx, ty, ter.cell), r = hybridProfile(q.z, q.N, q.D, ht, hr, p, off);
    r.D = q.D;
    return r;
  }

  // ---- radial sweep: one source, every sample of a radial as a target, reusing hulls and prefix sums along the radial
  function Sweep(cap) {
    this.cap = 0; this.grow(cap || 2048);
  }
  Sweep.prototype.grow = function (n) {
    this.cap = n;
    var F = Float64Array, I = Int32Array;
    this.z = new F(n); this.y = new F(n); this.A = new F(n); this.B = new F(n);
    this.p0 = new F(n + 1); this.p1 = new F(n + 1); this.p2 = new F(n + 1); this.amax = new F(n);
    this.S = new I(n); this.sl = new F(n); this.cum = new F(n); this.rs = new I(n);
    this.U = new I(n); this.Rc = new F(n);
    this.runTh = new F(256); this.runR = new F(256); this.runDc = new F(256);
  };
  Sweep.prototype.fgLine = function (i0, i1) {
    var n = i1 - i0 + 1, y = this.y, s1, s2, sy, sxy;
    if (n <= 48) {
      sy = 0; sxy = 0; s1 = 0; s2 = 0;
      for (var i = i0; i <= i1; i++) { var x = i - i0; sy += y[i]; sxy += y[i] * x; s1 += x; s2 += x * x; }
    } else {
      s1 = n * (n - 1) / 2; s2 = (n - 1) * n * (2 * n - 1) / 6;
      sy = this.p0[i1 + 1] - this.p0[i0] + n * this.yref;
      var q1 = this.p1[i1 + 1] - this.p1[i0];
      sxy = q1 - i0 * (this.p0[i1 + 1] - this.p0[i0]) + this.yref * s1;
    }
    var den = n * s2 - s1 * s1, b = (n * sxy - s1 * sy) / den;
    this.fa = (sy - b * s1) / n - b * i0; this.fb = b;
  };
  Sweep.prototype.fgRadius = function (i0, i1, st) {
    if (i1 - i0 < 2) return Infinity;
    var n = i1 - i0 + 1, y = this.y, mid = 0.5 * (i0 + i1), v;
    var S2 = n * (n * n - 1) / 12, S4 = n * (n * n - 1) * (3 * n * n - 7) / 240;
    if (n <= 48) {
      var a = 0, b = 0;
      for (var i = i0; i <= i1; i++) { var x = i - mid; a += y[i]; b += y[i] * x * x; }
      v = n * b - S2 * a;
    } else {
      var s0 = this.p0[i1 + 1] - this.p0[i0], s1 = this.p1[i1 + 1] - this.p1[i0], s2 = this.p2[i1 + 1] - this.p2[i0];
      v = n * (s2 - 2 * mid * s1 + mid * mid * s0) - S2 * s0;
    }
    var c2 = v / (n * S4 - S2 * S2) / (st * st);
    return c2 < 0 ? -1 / (2 * c2) : Infinity;
  };
  Sweep.prototype.run = function (ter, sx, sy, sinA, cosA, ht, hr, ds, n, p, lossCut, out, gt, gr) {
    if (n + 2 > this.cap) this.grow(Math.ceil(n * 1.5) + 2);
    var ae = R_MOON, i2ae = 1 / (2 * ae), lam = p.lam, o = FINAL, gap = o.gap, lmin = o.lmin, rmax = o.rmax;
    var z = this.z, y = this.y, A = this.A, B = this.B, p0 = this.p0, p1 = this.p1, p2 = this.p2, amax = this.amax;
    var S = this.S, sl = this.sl, cum = this.cum, rs = this.rs, U = this.U, Rc = this.Rc, runTh = this.runTh, runR = this.runR, runDc = this.runDc;
    var i, k;
    for (i = 0; i <= n; i++) z[i] = ter.sample(sx + i * ds * sinA, sy + i * ds * cosA);
    var mx = sx + n * ds * sinA / 2, my = sy + n * ds * cosA / 2, st0 = ds / mapScale(mx, my), hts = z[0] + ht, yref = z[0];
    var nw = Math.max(1, roundEven(lmin / st0));
    this.yref = yref;
    A[0] = 0; B[0] = 0; p0[0] = 0; p1[0] = 0; p2[0] = 0;
    for (i = 0; i <= n; i++) {
      var di = i * st0, yi = z[i] - di * di * i2ae, dy = yi - yref;
      y[i] = yi; Rc[i] = NaN;
      p0[i + 1] = p0[i] + dy; p1[i + 1] = p1[i] + dy * i; p2[i + 1] = p2[i] + dy * i * i;
      if (i < n) {
        A[i + 1] = A[i] + z[i + 1] + z[i];
        B[i + 1] = B[i] + z[i + 1] * (3 * i + 2) + z[i] * (3 * i + 1);
      }
    }
    amax[1] = -Infinity;
    var top = 0, utop = 0;
    S[0] = 0; rs[0] = 0; cum[0] = 0; sl[0] = 0;
    var yr0 = hts;
    for (k = 2; k <= n; k++) {
      // push terrain sample k - 1 onto both hulls
      var q = k - 1, yq = y[q];
      while (top >= 1) {
        var aa = S[top - 1], yaa = aa === 0 ? yr0 : y[aa], bb = S[top], ybb = bb === 0 ? yr0 : y[bb];
        if ((bb - aa) * (yq - yaa) - (ybb - yaa) * (q - aa) >= 0) top--; else break;
      }
      top++; S[top] = q;
      var pv = S[top - 1], ypv = pv === 0 ? yr0 : y[pv];
      sl[top] = (yq - ypv) / ((q - pv) * st0);
      if (top === 1) { cum[1] = 0; rs[1] = 1; }
      else {
        cum[top] = cum[top - 1] + (sl[top - 1] - sl[top]);
        rs[top] = (q - pv) * st0 <= gap ? rs[top - 1] : top;
      }
      var zq = z[q];
      while (utop >= 2) {
        var u1 = U[utop - 2], u2 = U[utop - 1];
        if ((u2 - u1) * (zq - z[u1]) - (z[u2] - z[u1]) * (q - u1) >= 0) utop--; else break;
      }
      U[utop++] = q;
      var aval = (zq - hts) / q;
      amax[k] = k === 2 ? aval : (aval > amax[k - 1] ? aval : amax[k - 1]);

      // target k
      var hrs = z[k] + hr, sc = 1 + ((sx + (k * ds * sinA) / 2) * (sx + (k * ds * sinA) / 2) + (sy + (k * ds * cosA) / 2) * (sy + (k * ds * cosA) / 2)) / (4 * ae * ae);
      var D = k * ds / sc, st = D / k, yT = y[k] + hr;
      var lo = 0, hi = top;
      while (lo < hi) {
        var mid = (lo + hi + 1) >> 1, p1i = S[mid - 1], p2i = S[mid];
        var y1 = p1i === 0 ? yr0 : y[p1i], y2 = p2i === 0 ? yr0 : y[p2i];
        if ((p2i - p1i) * (yT - y1) - (y2 - y1) * (k - p1i) >= 0) hi = mid - 1; else lo = mid;
      }
      var t = lo, los = t === 0;
      var rng = Math.sqrt(D * D + (hrs - hts) * (hrs - hts)), fspl = 20 * log10(4 * PI * rng / lam);
      var Stim = 0, Srim = 0, vb = 0, Luc, it = 0, ir = 0, th = 0, T = 0, Lhyb = 0, nruns = 0, Aobs = 0;
      if (!los) {
        it = S[1]; ir = S[t];
        var dit = it * st, dir = ir * st;
        Stim = (y[it] - hts) / dit + D * i2ae;
        Srim = (y[ir] + dir * D * i2ae - hrs) / (D - dir);
        th = Stim + Srim;
        var den = th > 1e-12 ? th : 1e-12;
        var db = (hrs - hts + Srim * D) / den;
        db = db < 1e-3 ? 1e-3 : db > D - 1e-3 ? D - 1e-3 : db;
        vb = (hts + Stim * db - (hts * (D - db) + hrs * db) / D) * Math.sqrt(2 * D / (lam * db * (D - db)));
        Luc = J(vb);
        if (fspl + Luc - 14 > lossCut) { out[k] = Infinity; continue; }
        // runs on the string, last to first
        var sT = (yT - y[ir]) / ((k - ir) * st0);
        var e = t, turnEnd = sl[t] - sT;
        while (e >= 1) {
          var s = rs[e], thr = cum[e] - cum[s] + (e === t ? turnEnd : (sl[e] - sl[e + 1]));
          var i0 = S[s], i1 = S[e], ln = (i1 - i0) * st, R = (ln > lmin ? ln : lmin) / (thr > 1e-12 ? thr : 1e-12);
          if (ln < lmin) {
            var ic = (i0 + i1) >> 1, Rf;
            if (ic + nw <= k && ic - nw >= 0 && Rc[ic] === Rc[ic]) Rf = Rc[ic];
            else {
              Rf = this.fgRadius(Math.max(ic - nw, 0), Math.min(ic + nw, k), st0);
              if (ic + nw <= k && ic - nw >= 0) Rc[ic] = Rf;
            }
            if (Rf < R) R = Rf;
          }
          var dc = 0.5 * (i0 + i1) * st;
          if (nruns >= runTh.length) { var g = nruns * 2; runTh = this.runTh = copyF(runTh, g); runR = this.runR = copyF(runR, g); runDc = this.runDc = copyF(runDc, g); }
          runTh[nruns] = thr; runR[nruns] = R; runDc[nruns] = dc; nruns++;
          T += runT(thr, R, dc, D, lam, 0, rmax);
          e = s - 1;
        }
        Aobs = Luc + T;
        if (fspl + Aobs - 14 > lossCut) { out[k] = Infinity; continue; }
        // terminal terms
        Lhyb = fspl + Aobs;
        for (var sd = 0; sd < 2; sd++) {
          var ih = sd === 0 ? it : ir, ag, hp, he, Rfg, ri = sd === 0 ? nruns - 1 : 0;
          if (sd === 0) {
            this.fgLine(0, ih);
            ag = ih * st; ag = ag > 1 ? ag : 1;
            hp = hts - this.fa; he = y[ih] - (this.fa + this.fb * ih);
            Rfg = this.fgRadius(0, ih, st);
          } else {
            this.fgLine(ih, k);
            ag = D - ih * st; ag = ag > 1 ? ag : 1;
            hp = (y[k] + hr) - (this.fa + this.fb * k); he = y[ih] - (this.fa + this.fb * ih);
            Rfg = this.fgRadius(ih, k, st);
          }
          hp = hp > 0.05 ? hp : 0.05;
          var dth = 2 * hp / ag, psi = (hp + he) / ag; psi = psi > 0 ? psi : 0;
          var scl = th > 0 ? (th + dth) / (th > 1e-12 ? th : 1e-12) : 1;
          var T0 = runT(runTh[ri], runR[ri], runDc[ri], D, lam, 0, rmax), Td = runT(runTh[ri], runR[ri], runDc[ri], D, lam, dth, rmax);
          var Ai = Aobs + (J(vb * scl) - Luc) + Td - T0;
          var r = Math.pow(10, -(Ai - Aobs) / 20);
          var Radj = runR[ri], wgt = 1 - Radj / Rfg; wgt = wgt < 0 ? 0 : wgt > 1 ? 1 : wgt;
          var phi = p.k * 2 * hp * (he > 0 ? he : 0) / ag, qa = 0.5 * phi, sinc = qa > 1e-9 ? Math.sin(qa) / qa : 1;
          if (th > 0) Lhyb += gammaWeighted(p, psi, phi, sinc, r * wgt);
        }
      } else {
        // line of sight: Bullington's largest nu over the interior
        var vmax = -Infinity, rc = 2 * D / lam, ihD = 1 / D;
        for (i = 1; i < k; i++) {
          var d = i * st, dr = D - d;
          var ze = y[i] + d * D * i2ae, hl = (hts * dr + hrs * d) * ihD;
          var v = (ze - hl) * Math.sqrt(rc / (d * dr));
          if (v > vmax) vmax = v;
        }
        Luc = J(vmax);
      }
      var Lba = corrected(Luc, D);
      // effective smooth sphere
      var slopeIdx = (hrs - hts) / k;
      var ul = 0, uh = utop - 1;
      while (ul < uh) {
        var um = (ul + uh) >> 1;
        if ((z[U[um + 1]] - z[U[um]]) / (U[um + 1] - U[um]) > slopeIdx) ul = um + 1; else uh = um;
      }
      var ub = U[ul], hobs = z[ub] - (hts + slopeIdx * ub);
      var tl = 0, th2 = utop - 1;
      while (tl < th2) {
        var tm = (tl + th2) >> 1, ua = U[tm], ub2 = U[tm + 1];
        if ((z[ua] - hrs) / (k - ua) < (z[ub2] - hrs) / (k - ub2)) tl = tm + 1; else th2 = tm;
      }
      var ur = U[tl];
      var aobr = (z[ur] - hrs) / ((k - ur) * st) + (hrs - hts) / D;
      var aobt = amax[k] / st - (hrs - hts) / D;
      var hstip = (2 * A[k] * k - B[k]) / (k * k), hsrip = (B[k] - A[k] * k) / (k * k);
      var hstp = hstip, hsrp = hsrip;
      if (hobs > 0) { var dn = aobt + aobr; hstp = hstip - hobs * aobt / dn; hsrp = hsrip - hobs * aobr / dn; }
      var hst = hstp < z[0] ? hstp : z[0], hsr = hsrp < z[k] ? hsrp : z[k];
      var htse = hts - hst, hrse = hrs - hsr;
      var Lsph = sphEarth(p, D, htse, hrse), Ld = Lba;
      if (Lsph > 0) {
        var Lbs = smoothBull(k, st, D, htse, hrse, lam);
        if (Lsph > Lbs) Ld = Lba + Lsph - Lbs;
      }
      var L526 = fspl + Ld, Lk = !los && Lhyb > L526 ? Lhyb : L526;
      if (gt) {
        var cvv = D * i2ae, s0 = (hrs - hts) / D;
        Lk -= elGain(gt, Math.atan(los ? s0 - cvv : Stim - cvv) * DEG) + elGain(gr, Math.atan(los ? -s0 - cvv : Srim - cvv) * DEG);
      }
      out[k] = Lk;
    }
    if (n >= 2) out[1] = out[2];
    out[0] = 0;
  };
  function copyF(a, n) { var b = new Float64Array(n); b.set(a); return b; }
  function smoothBull(k, st, D, h1, h2, lam) {
    var ae = R_MOON, i2ae = 1 / (2 * ae), Stim, Srim;
    var f = function (i) { var d = i * st; return (D - d) * i2ae - h1 / d; };
    var g = function (i) { var d = i * st; return d * i2ae - h2 / (D - d); };
    var cs = Math.floor(Math.sqrt(2 * ae * h1) / st);
    var a = cs < 1 ? 1 : cs > k - 1 ? k - 1 : cs, b = cs + 1 < 1 ? 1 : cs + 1 > k - 1 ? k - 1 : cs + 1;
    Stim = Math.max(f(a), f(b));
    var cr = k - Math.floor(Math.sqrt(2 * ae * h2) / st);
    a = cr < 1 ? 1 : cr > k - 1 ? k - 1 : cr; b = cr - 1 < 1 ? 1 : cr - 1 > k - 1 ? k - 1 : cr - 1;
    Srim = Math.max(g(a), g(b));
    var Str = (h2 - h1) / D, Luc;
    if (Stim < Str) {
      var rc = 2 * D / lam, ihD = 1 / D;
      var v = function (i) { var d = i * st, dr = D - d; return (d * dr * i2ae - (h1 * dr + h2 * d) * ihD) * Math.sqrt(rc / (d * dr)); };
      var lo = 1, hi = k - 1;
      while (hi - lo > 2) {
        var m1 = lo + ((hi - lo) / 3 | 0), m2 = hi - ((hi - lo) / 3 | 0);
        if (v(m1) < v(m2)) lo = m1; else hi = m2;
      }
      var vm = -Infinity;
      for (var i = lo; i <= hi; i++) { var w = v(i); if (w > vm) vm = w; }
      Luc = J(vm);
    } else {
      var den = Math.max(Stim + Srim, 1e-12), db = (h2 - h1 + Srim * D) / den;
      db = db < 1e-3 ? 1e-3 : db > D - 1e-3 ? D - 1e-3 : db;
      Luc = J((h1 + Stim * db - (h1 * (D - db) + h2 * db) / D) * Math.sqrt(2 * D / (lam * db * (D - db))));
    }
    return corrected(Luc, D);
  }

  // ---- antennas: free-space pattern tables, az clockwise from boresight by azStep, el from the horizon by elStep from elMin; switched types take the best of their beam states
  var AG = { elMin: -90, elStep: 2 };
  function Antenna(s) {
    this.id = s.id; this.g = s.g; this.naz = s.naz; this.nel = s.nel; this.azStep = s.azStep; this.elStep = s.elStep; this.elMin = s.elMin; this.beams = s.beams || 1; this.iso = !!s.iso;
    AG.elMin = s.elMin; AG.elStep = s.elStep;
  }
  Antenna.prototype.raw = function (az, el) {
    var fa = (((az % 360) + 360) % 360) / this.azStep, fe = (el - this.elMin) / this.elStep, naz = this.naz;
    var a0 = Math.floor(fa), ta = fa - a0, e0 = Math.floor(fe), te = fe - e0;
    if (e0 < 0) { e0 = 0; te = 0; } else if (e0 >= this.nel - 1) { e0 = this.nel - 2; te = 1; }
    a0 %= naz; var a1 = (a0 + 1) % naz, g = this.g, r0 = e0 * naz, r1 = r0 + naz;
    return (1 - te) * ((1 - ta) * g[r0 + a0] + ta * g[r0 + a1]) + te * ((1 - ta) * g[r1 + a0] + ta * g[r1 + a1]);
  };
  Antenna.prototype.gain = function (az, el) {
    if (this.iso) return 0;
    if (this.beams === 1) return this.raw(az, el);
    var b = -Infinity, st = 360 / this.beams;
    for (var k = 0; k < this.beams; k++) { var v = this.raw(az - k * st, el); if (v > b) b = v; }
    return b;
  };
  Antenna.prototype.row = function (az, tl) {
    var r = new Float64Array(this.nel);
    for (var e = 0; e < this.nel; e++) { var el = this.elMin + e * this.elStep; r[e] = tl ? this.gainTilted(az, el, tl) : this.gain(az, el); }
    return r;
  };
  // A mast leaning t degrees toward bearing ta: world bearing and elevation into the antenna's own frame. tl = { t, ta, bore }; az is world bearing minus bore.
  function tiltFrame(t, ta) {
    var tr = t / DEG, ar = ta / DEG, h = [Math.sin(ar), Math.cos(ar), 0], z = [Math.sin(tr) * h[0], Math.sin(tr) * h[1], Math.cos(tr)];
    var y = [-z[1] * z[0], 1 - z[1] * z[1], -z[1] * z[2]], n = Math.hypot(y[0], y[1], y[2]);
    y = [y[0] / n, y[1] / n, y[2] / n];
    var x = [y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]];
    return { x: x, y: y, z: z };
  }
  Antenna.prototype.gainTilted = function (az, el, tl) {
    if (this.iso) return 0;
    var f = tl.f || (tl.f = tiltFrame(tl.t, tl.ta)), b = (az + tl.bore) / DEG, e = el / DEG, ce = Math.cos(e);
    var d = [Math.sin(b) * ce, Math.cos(b) * ce, Math.sin(e)];
    var dz = d[0] * f.z[0] + d[1] * f.z[1] + d[2] * f.z[2], dx = d[0] * f.x[0] + d[1] * f.x[1] + d[2] * f.x[2], dy = d[0] * f.y[0] + d[1] * f.y[1] + d[2] * f.y[2];
    return this.gain(Math.atan2(dx, dy) * DEG - tl.bore, Math.asin(Math.max(-1, Math.min(1, dz))) * DEG);
  };
  function elGain(row, el) {
    var f = (el - AG.elMin) / AG.elStep, i = Math.floor(f), t = f - i, n = row.length;
    if (i < 0) return row[0];
    if (i >= n - 1) return row[n - 1];
    return row[i] + t * (row[i + 1] - row[i]);
  }
  function bearing(sx, sy, tx, ty) { var a = Math.atan2(tx - sx, ty - sy) * DEG; return a < 0 ? a + 360 : a; }

  // Number of samples along a radial before it leaves the disk.
  function radialEnd(ter, sx, sy, sinA, cosA, ds) {
    var ox = sx - ter.bx, oy = sy - ter.by, b = ox * sinA + oy * cosA, c = ox * ox + oy * oy - ter.rdisk * ter.rdisk;
    var disc = b * b - c;
    if (disc <= 0) return 2;
    return Math.max(2, Math.floor((-b + Math.sqrt(disc)) / ds));
  }

  // One block of radials for a node; returns per-radial sample counts and losses.
  function coverageBlock(ter, sw, job, p, ants) {
    var nAz = job.nAz, ds = job.ds, a0 = job.a0, a1 = job.a1, maxN = job.maxN, sx = job.x, sy = job.y;
    var out = new Float32Array((a1 - a0) * (maxN + 1)), line = new Float64Array(maxN + 1), counts = new Int32Array(a1 - a0);
    var at = ants && job.ant ? ants[job.ant] : null, ar = ants && job.crew ? ants[job.crew] : null, use = (at && !at.iso) || (ar && !ar.iso);
    var gr = use ? (ar ? ar.row(0) : new Float64Array(at.nel)) : null;
    for (var a = a0; a < a1; a++) {
      var az = 2 * PI * a / nAz, sn = Math.sin(az), cs = Math.cos(az), n = Math.min(radialEnd(ter, sx, sy, sn, cs, ds), maxN);
      var gt = use ? (at ? at.row(az * DEG - (job.bore || 0), job.tilt ? { t: job.tilt, ta: job.tiltAz || 0, bore: job.bore || 0 } : null) : new Float64Array(gr.length)) : null;
      sw.run(ter, sx, sy, sn, cs, job.ht, job.hr, ds, n, p, job.lossCut, line, gt, gr);
      counts[a - a0] = n;
      var base = (a - a0) * (maxN + 1);
      for (var i = 0; i <= n; i++) out[base + i] = line[i];
    }
    return { out: out, counts: counts };
  }

  // Polar losses to a raster of best-so-far loss on the map grid, nearest radial and sample.
  function rasterize(polar, counts, nAz, maxN, ds, sx, sy, grid, into) {
    var w = grid.w, h = grid.h, x0 = grid.x0, ytop = grid.ytop, cell = grid.cell, mask = grid.mask, tw = nAz / (2 * PI), row = maxN + 1;
    for (var r = 0; r < h; r++) {
      var y = ytop - r * cell, dy = y - sy;
      for (var c = 0; c < w; c++) {
        var i = r * w + c;
        if (mask && !mask[i]) continue;
        var dx = x0 + c * cell - sx, rr = Math.sqrt(dx * dx + dy * dy), k = (rr / ds + 0.5) | 0;
        var a = Math.atan2(dx, dy); if (a < 0) a += 2 * PI;
        var ai = (a * tw + 0.5) | 0; if (ai >= nAz) ai = 0;
        if (k > counts[ai]) continue;
        var L = polar[ai * row + k];
        if (L < into[i]) into[i] = L;
      }
    }
  }

  // ---- mesh: widest chain from the base over the weaker direction of each hop
  function widestChain(hop) {
    var n = hop.length, best = new Array(n), par = new Array(n), done = new Array(n);
    for (var i = 0; i < n; i++) { best[i] = -Infinity; par[i] = -1; done[i] = false; }
    best[0] = Infinity;
    for (var it = 0; it < n; it++) {
      var u = -1, bv = -Infinity;
      for (i = 0; i < n; i++) if (!done[i] && best[i] > bv) { bv = best[i]; u = i; }
      if (u < 0) break;
      done[u] = true;
      for (i = 0; i < n; i++) if (!done[i]) {
        var w = Math.min(best[u], hop[u][i]);
        if (w > best[i]) { best[i] = w; par[i] = u; }
      }
    }
    return { par: par, best: best };
  }

  // ---- rate models
  var K_DBM = -198.6, T_REF = 290, FANO = 2.73, Q_REAL = 2, Q_COIL = 300, T_PHYS = 250;
  function tSkyCane(fm) {
    var tau = 5 * Math.pow(fm, -2.1), fac = tau > 1e-9 ? (1 - Math.exp(-tau)) / tau : 1;
    var bright = 2.48e-20 * Math.pow(fm, -0.52) * fac + 1.06e-20 * Math.pow(fm, -0.8) * Math.exp(-tau);
    var l = C0 / (fm * 1e6);
    return bright * l * l / (2 * 1.380649e-23);
  }
  function tSys(fm, nf) {
    var a = Math.pow(10, -0.05), ant = tSkyCane(fm);
    return a * ant + (1 - a) * T_REF + T_REF * (Math.pow(10, nf / 10) - 1);
  }
  function qRad(f, am) { var ka = 2 * PI * am * f / C0; return Math.max(Q_REAL * (1 / (ka * ka * ka) + 1 / ka), 1); }
  function shannonRate(f, L, opts) {
    var pd = opts.p_dbm, am = opts.a_m, nf = opts.nf_db, mg = opts.margin_db, gap = opts.gap_db, nb = 121;
    var fm = f / 1e6, ts = tSys(fm, nf), trx = T_REF * (Math.pow(10, nf / 10) - 1), tsky = ts - trx;
    var q = qRad(f, am), bm = FANO * f / q, ecap = Q_COIL / (Q_COIL + q);
    var lo = 2, hi = log10(0.2 * f), best = 0, bb = 0;
    for (var i = 0; i < nb; i++) {
      var b = Math.pow(10, lo + (hi - lo) * i / (nb - 1));
      var eta = Math.min(ecap, bm / b), teff = tsky + trx / eta + (1 / eta - 1) * T_PHYS;
      var n0 = K_DBM + 10 * log10(teff), cn0 = pd + 10 * log10(eta) - L - n0 - mg - gap;
      var r = b * Math.log2(1 + Math.pow(10, cn0 / 10) / b);
      if (r > best) { best = r; bb = b; }
    }
    return { rate: best, bw: bb };
  }
  // Largest loss that still holds rate target with no fade margin, by bisection.
  function lossForRate(f, target, opts) {
    var lo = 40, hi = 260, o = { p_dbm: opts.p_dbm, a_m: opts.a_m, nf_db: opts.nf_db, margin_db: 0, gap_db: opts.gap_db };
    for (var i = 0; i < 40; i++) {
      var m = 0.5 * (lo + hi);
      if (shannonRate(f, m, o).rate >= target) lo = m; else hi = m;
    }
    return lo;
  }
  function hopShare(n) {
    var pts = [[1, 1], [2, 0.5], [3, 0.3], [4, 0.2], [8, 0.05]];
    if (n <= 1) return 1;
    for (var i = 0; i < pts.length - 1; i++) {
      var a = pts[i], b = pts[i + 1];
      if (n <= b[0]) return a[1] + (b[1] - a[1]) * (n - a[0]) / (b[0] - a[0]);
    }
    return 0.05;
  }
  // Attenuated Shannon for the 5G presets, fiveg_relays/budgets.py.
  function nrRate(L, tx, gains, nf, fade) {
    var bws = [0.36e6, 1.44e6, 3.6e6, 10.8e6, 20e6, 40e6, 100e6], best = 0;
    for (var i = 0; i < bws.length; i++) {
      var bw = bws[i], snr = tx + gains - L - fade - (-174 + 10 * log10(bw) + nf);
      if (snr < -6.5) continue;
      var se = Math.min(0.75 * Math.log2(1 + Math.pow(10, snr / 10)), 4.8);
      if (0.229 * bw * se > best) best = 0.229 * bw * se;
    }
    return best;
  }

  // ---- worker jobs, shared by the page's Web Workers and the Node timing harness
  function handle(state, m) {
    if (m.type === "init") {
      var t = m.terrain;
      state.ter = new Terrain(m.dem, t.nr, t.nc, t.x0, t.ytop, t.cell, t.bx, t.by, t.rdisk);
      state.sw = new Sweep(2048); state.params = {}; state.ants = {};
      if (m.ants) for (var id in m.ants) state.ants[id] = new Antenna(m.ants[id]);
      return { type: "ready" };
    }
    var f = m.f, p = state.params[f] || (state.params[f] = new Params(f));
    if (m.type === "cov") {
      var r = coverageBlock(state.ter, state.sw, m, p, state.ants);
      return { type: "cov", id: m.id, a0: m.a0, a1: m.a1, out: r.out, counts: r.counts, transfer: [r.out.buffer, r.counts.buffer] };
    }
    if (m.type === "raster") {
      var into = new Float32Array(m.grid.w * m.grid.h).fill(Infinity);
      rasterize(m.polar, m.counts, m.nAz, m.maxN, m.ds, m.x, m.y, m.grid, into);
      return { type: "raster", id: m.id, into: into, transfer: [into.buffer] };
    }
    if (m.type === "links") {
      var n = m.links.length / 6, L = new Float64Array(n * 2), E = new Float32Array(n * 2);
      for (var i = 0; i < n; i++) {
        var o = i * 6, q = linkLoss(state.ter, m.links[o], m.links[o + 1], m.links[o + 2], m.links[o + 3], m.links[o + 4], m.links[o + 5], p, false);
        L[i * 2] = q.L; L[i * 2 + 1] = q.los ? 1 : 0; E[i * 2] = q.elT; E[i * 2 + 1] = q.elR;
      }
      return { type: "links", id: m.id, L: L, E: E, transfer: [L.buffer, E.buffer] };
    }
    throw new Error("unknown job " + m.type);
  }

  return {
    handle: handle, C0: C0, R_MOON: R_MOON, FINAL: FINAL, J: J, sigma: sigma, mapScale: mapScale, Params: Params, p526: p526, hybridProfile: hybridProfile,
    Terrain: Terrain, linkLoss: linkLoss, Sweep: Sweep, radialEnd: radialEnd, coverageBlock: coverageBlock, rasterize: rasterize, widestChain: widestChain,
    tSys: tSys, shannonRate: shannonRate, lossForRate: lossForRate, hopShare: hopShare, nrRate: nrRate, K_DBM: K_DBM,
    Antenna: Antenna, elGain: elGain, bearing: bearing
  };
})();
if (typeof module !== "undefined" && module.exports) module.exports = LL;
