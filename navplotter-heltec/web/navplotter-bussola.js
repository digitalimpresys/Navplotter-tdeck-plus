// NavPlotter: the compass everywhere it helps.
// Loaded after the page script and after the other modules (navplotter-stima.js included).
// - The Compass page turns with the best heading there is: the magnetic heading of the compass (the `att` block of
//   /api/state: a BNO08x on the node), or else the course over ground of the GPS, or else, without a fix, the course
//   of the dead reckoning (the `dr` block). Under the number it says which one it is. Without any, it stays as before.
// - On the chart, a small see-through compass rose, heading up, with the heading and where it comes from. Like the
//   MOB and anchor buttons it can be moved with a long press and stays where it is put; a tap opens the Compass page.
//   It shows only when there is a heading.
(function () {
  'use strict';
  if (typeof drawCompass !== 'function' || typeof I18N === 'undefined') return;

  const isIt = () => (typeof lang === 'string' ? lang : '').startsWith('it');
  const L = (i, e) => (isIt() ? i : e);
  const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} };
  const load = (k) => { try { return JSON.parse(localStorage.getItem(k) || 'null'); } catch (e) { return null; } };
  const p3 = (d) => String(Math.round(((d % 360) + 360) % 360) % 360).padStart(3, '0');

  // The heading to show, and where it comes from: { deg, src: 'mag' | 'cog' | 'dr' }, or null
  function heading() {
    if (!st) return null;
    const a = st.att, o = st.own, d = st.dr;
    if (a && typeof a.hdg === 'number') return { deg: a.hdg, src: 'mag', acc: a.acc };
    if (o && o.fix && o.cogValid) return { deg: o.cog, src: 'cog' };
    if (d && d.mode === 'dr' && typeof d.cog === 'number') return { deg: d.cog, src: 'dr' };
    return null;
  }
  const srcText = (h) => h.src === 'mag' ? L('dalla bussola (magnetica)', 'from the compass (magnetic)')
    : h.src === 'dr' ? L('rotta stimata, senza GPS', 'estimated course, no GPS') : null;
  const srcShort = (h) => h.src === 'mag' ? 'M' : h.src === 'dr' ? L('STIMA', 'EST') : 'COG';

  // ---------- the Compass page ----------
  // The page reads st.own.cog; for the time of the drawing it is given the heading chosen here, and its own words
  // under the number say where it comes from.
  const pageCompass = window.drawCompass;
  window.drawCompass = function () {
    const h = heading();
    if (!h || h.src === 'cog') return pageCompass.apply(this, arguments);
    const real = st, keep = {};
    const words = { fromCog: srcText(h) };
    for (const l of Object.keys(I18N)) { keep[l] = I18N[l].fromCog; I18N[l].fromCog = words.fromCog; }
    try {
      st = Object.assign({}, real, { own: Object.assign({}, real.own, { cog: h.deg, cogValid: true, moving: true }) });
      return pageCompass.apply(this, arguments);
    } finally {
      st = real;
      for (const l of Object.keys(keep)) I18N[l].fromCog = keep[l];
    }
  };

  // ---------- the small rose on the chart ----------
  const SIZE = 112, EDGE = 6;
  const css = document.createElement('style');
  css.textContent =
    '#bussMini{position:fixed;z-index:2147482980;width:' + SIZE + 'px;height:' + SIZE + 'px;border-radius:50%;display:none;' +
    'touch-action:none;user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;cursor:pointer;opacity:.82}' +
    '#bussMini.on{display:block}#bussMini.moving{outline:3px dashed #ffd400;outline-offset:3px;opacity:1}' +
    '#bussMini canvas{width:100%;height:100%;display:block}';
  document.head.appendChild(css);
  const box = document.createElement('div');
  box.id = 'bussMini';
  const cv = document.createElement('canvas');
  box.appendChild(cv);
  document.body.appendChild(box);
  const label = () => { box.title = L('Bussola: tocca per aprire la pagina Bussola, tocco lungo per spostarla.', 'Compass: tap to open the Compass page, long press to move it.'); };
  label();

  let pos = load('np-buss-pos');
  function place(x, y) {
    const W = window.innerWidth, H = window.innerHeight;
    x = Math.max(EDGE, Math.min(W - SIZE - EDGE, x)); y = Math.max(EDGE, Math.min(H - SIZE - EDGE, y));
    box.style.left = x + 'px'; box.style.top = y + 'px';
  }
  function placeSaved() {
    const W = window.innerWidth, H = window.innerHeight;
    if (pos && typeof pos.fx === 'number') place(pos.fx * (W - SIZE), pos.fy * (H - SIZE));
    else place(W - SIZE - 12, Math.min(H - SIZE - 90, 290));   // first time: on the right, under the chart buttons
  }
  placeSaved();
  window.addEventListener('resize', () => { placeSaved(); draw(); });

  let pd = null;
  box.addEventListener('pointerdown', (e) => {
    e.preventDefault(); e.stopPropagation();
    try { box.setPointerCapture(e.pointerId); } catch (err) {}
    const r = box.getBoundingClientRect();
    pd = { id: e.pointerId, x0: e.clientX, y0: e.clientY, bx: r.left, by: r.top, moving: false, dragged: false, slid: false, timer: 0 };
    pd.timer = setTimeout(() => { if (!pd || pd.slid) return; pd.moving = true; box.classList.add('moving'); if (navigator.vibrate) navigator.vibrate(30); }, 600);
  });
  box.addEventListener('pointermove', (e) => {
    if (!pd || e.pointerId !== pd.id) return;
    e.preventDefault(); e.stopPropagation();
    const dx = e.clientX - pd.x0, dy = e.clientY - pd.y0;
    if (pd.moving) { if (Math.hypot(dx, dy) > 4) pd.dragged = true; place(pd.bx + dx, pd.by + dy); }
    else if (Math.hypot(dx, dy) > 12) { pd.slid = true; clearTimeout(pd.timer); }
  });
  function end(e) {
    if (!pd || e.pointerId !== pd.id) return;
    e.preventDefault(); e.stopPropagation();
    clearTimeout(pd.timer); box.classList.remove('moving');
    const p = pd; pd = null;
    if (e.type === 'pointercancel') return;
    if (p.moving && p.dragged) {
      const r = box.getBoundingClientRect(), W = window.innerWidth, H = window.innerHeight;
      pos = { fx: Math.max(0, Math.min(1, r.left / Math.max(1, W - SIZE))), fy: Math.max(0, Math.min(1, r.top / Math.max(1, H - SIZE))) };
      save('np-buss-pos', pos);
      return;
    }
    if (!p.slid && typeof showPage === 'function') showPage('pCompass');
  }
  box.addEventListener('pointerup', end);
  box.addEventListener('pointercancel', end);
  box.addEventListener('contextmenu', (e) => e.preventDefault());

  function draw() {
    const h = heading(), onMap = document.getElementById('pMap').classList.contains('on');
    box.classList.toggle('on', !!h && onMap);
    if (!h || !onMap) return;
    const dpr = window.devicePixelRatio || 1, n = Math.round(SIZE * dpr);
    if (cv.width !== n) { cv.width = n; cv.height = n; }
    const c = cv.getContext('2d');
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.clearRect(0, 0, SIZE, SIZE);
    const cx = SIZE / 2, cy = SIZE / 2, R = SIZE / 2 - 3, hdg = h.deg;
    const col = h.src === 'dr' ? '#f4a300' : h.src === 'mag' ? '#7fd0ff' : '#fff';
    c.fillStyle = 'rgba(6,13,24,.62)'; c.beginPath(); c.arc(cx, cy, R, 0, 7); c.fill();
    c.strokeStyle = 'rgba(207,216,227,.9)'; c.lineWidth = 1.5; c.beginPath(); c.arc(cx, cy, R - 1, 0, 7); c.stroke();
    // The rose turns, heading up: the ticks every 30 degrees and the cardinal points
    for (let d = 0; d < 360; d += 30) {
      const a = (d - hdg) * Math.PI / 180, len = d % 90 === 0 ? 9 : 5;
      c.strokeStyle = 'rgba(207,216,227,.85)'; c.lineWidth = d % 90 === 0 ? 2 : 1.2;
      c.beginPath(); c.moveTo(cx + (R - 2 - len) * Math.sin(a), cy - (R - 2 - len) * Math.cos(a));
      c.lineTo(cx + (R - 2) * Math.sin(a), cy - (R - 2) * Math.cos(a)); c.stroke();
    }
    c.textAlign = 'center'; c.textBaseline = 'middle';
    ['N', 'E', 'S', 'W'].forEach((s, i) => {
      const a = (i * 90 - hdg) * Math.PI / 180;
      c.font = '700 13px sans-serif'; c.fillStyle = i === 0 ? '#ff5a5f' : '#e8eef5';
      c.fillText(isIt() && s === 'W' ? 'O' : s, cx + (R - 20) * Math.sin(a), cy - (R - 20) * Math.cos(a));
    });
    // The lubber line on top, the heading in the middle, where it comes from under it
    c.fillStyle = '#e5383b'; c.beginPath(); c.moveTo(cx, cy - R + 12); c.lineTo(cx - 6, cy - R - 1); c.lineTo(cx + 6, cy - R - 1); c.closePath(); c.fill();
    c.font = '800 22px sans-serif'; c.fillStyle = col; c.fillText(p3(hdg) + '°', cx, cy + 1);
    c.font = '700 10px sans-serif'; c.fillStyle = 'rgba(232,238,245,.85)';
    c.fillText(srcShort(h) + (h.src === 'mag' && h.acc < 2 ? ' ?' : ''), cx, cy + 19);
  }
  setInterval(draw, 500);
  const pageShow = window.showPage;
  if (typeof pageShow === 'function') window.showPage = function () { const r = pageShow.apply(this, arguments); draw(); return r; };
  const pageLang = window.applyLang;
  if (typeof pageLang === 'function') window.applyLang = function () { pageLang.apply(this, arguments); label(); draw(); };
})();
