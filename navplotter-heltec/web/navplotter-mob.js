// NavPlotter: man overboard (MOB).
// The same module as the Android app (app-NavPlotter, www/navplotter-mob.js), loaded after the page script and after
// navplotter-meteo.js. It adds a red MOB button that floats over every page (it is never hidden by the chart, the
// lists or the sheets). A tap puts a waypoint at the boat's position, marked with a lifebuoy, makes it the
// destination and sounds an alarm. The alarm goes on, on every screen showing the web app, until the MOB waypoint
// is deleted in the Waypoints page. A long press picks the button up: it follows the finger and stays where it is
// left, so it can be moved out of the way of whatever is under it.
(function () {
  'use strict';
  if (typeof drawMap !== 'function' || typeof map === 'undefined' || typeof ud === 'undefined') return;

  const isIt = () => (typeof lang === 'string' ? lang : document.documentElement.lang || '').startsWith('it');
  const L = (i, e) => (isIt() ? i : e);
  const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} };
  const load = (k) => { try { return JSON.parse(localStorage.getItem(k) || 'null'); } catch (e) { return null; } };
  const two = (n) => String(n).padStart(2, '0');
  const hhmm = (ms) => { const d = new Date(ms); return two(d.getHours()) + ':' + two(d.getMinutes()); };
  const ago = (ms) => { const s = Math.max(0, Math.floor((Date.now() - ms) / 1000)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60; return (h ? h + ':' + two(m) : m) + ':' + two(s % 60); };

  // A MOB waypoint has mob: true. The name is checked too, because a node with a screen of its own may keep only
  // the fields it knows when it changes the waypoints.
  const isMob = (p) => !!p && (p.mob === true || /^MOB\b/.test(p.name || ''));
  const mobs = () => ud.wps.filter(isMob);

  // ---------- the last position: if the fix drops just as the button is pressed, the last one still counts ----------
  let lastFix = null;
  setInterval(() => { if (st && st.own && st.own.fix) lastFix = { lat: st.own.lat, lon: st.own.lon, at: Date.now() }; }, 1000);

  // ---------- the man overboard ----------
  let lastMob = 0;
  function mob() {
    unlock();
    if (Date.now() - lastMob < 3000) return; // a double press is one person
    const fix = st && st.own && st.own.fix ? { lat: st.own.lat, lon: st.own.lon } : lastFix && Date.now() - lastFix.at < 120000 ? lastFix : null;
    if (!fix) { alarmOnce(); alert(L('MOB: nessuna posizione GPS. Segna il punto sulla carta con un tocco lungo.', 'MOB: no GPS position. Mark the spot on the chart with a long press.')); return; }
    lastMob = Date.now();
    const id = ud.nextId++;
    ud.wps.push({ id, name: 'MOB ' + hhmm(lastMob), lat: Math.round(fix.lat * 1e6) / 1e6, lon: Math.round(fix.lon * 1e6) / 1e6, mob: true, t: lastMob });
    // Straight back to him: the MOB is the destination, a followed route stops
    ud.dest = id; ud.route = null;
    saveUd();
    if (navigator.vibrate) navigator.vibrate([400, 150, 400, 150, 400]);
    showPage('pMap'); setFollow(true);
    nextSound = 0; tick();
    if (nativeAlarm()) alarmOnce();
  }

  // ---------- the alarm ----------
  // Its own audio. In the Android app, while its watch runs, the watch sounds the MOB on the phone's alarm, also
  // with the screen off: the page then plays only the first alarm, at the press, so as not to sound twice.
  const nativeAlarm = () => { try { return !!(window.NavPlotterApp && window.NavPlotterApp.watching && window.NavPlotterApp.watching()); } catch (e) { return false; } };
  let ac = null, nextSound = 0;
  function unlock() {
    if (!ac) { try { ac = new (window.AudioContext || window.webkitAudioContext)(); } catch (e) { return; } }
    if (ac.state === 'suspended') ac.resume();
  }
  // Browsers start the sound only after a touch: after a reload with a MOB still there, the first touch starts it
  ['pointerdown', 'keydown'].forEach((ev) => document.addEventListener(ev, () => { if (mobs().length) unlock(); }, true));
  function tone(f, at, len) {
    const o = ac.createOscillator(), g = ac.createGain();
    o.type = 'square'; o.frequency.value = f;
    g.gain.setValueAtTime(0.0001, at); g.gain.exponentialRampToValueAtTime(0.35, at + 0.02);
    g.gain.setValueAtTime(0.35, at + len - 0.03); g.gain.exponentialRampToValueAtTime(0.0001, at + len);
    o.connect(g); g.connect(ac.destination); o.start(at); o.stop(at + len + 0.02);
  }
  // Two tones, high and low, three times: not like the page's other beeps
  function alarmOnce() {
    if (!ac || ac.state !== 'running') return;
    const t0 = ac.currentTime + 0.05;
    for (let i = 0; i < 3; i++) { tone(1300, t0 + i * 0.5, 0.24); tone(850, t0 + i * 0.5 + 0.25, 0.24); }
  }
  function tick() {
    const on = mobs().length > 0;
    btn.classList.toggle('on', on);
    const m = mobs().filter((p) => p.t).sort((a, b) => a.t - b.t)[0];
    sub.textContent = m ? ago(m.t) : '';
    if (on && !nativeAlarm() && Date.now() >= nextSound) { alarmOnce(); nextSound = Date.now() + 2500; }
    if (!on) nextSound = 0;
  }
  setInterval(tick, 500);

  // ---------- the button ----------
  const css = document.createElement('style');
  css.textContent =
    '#mobBtn{position:fixed;z-index:2147483000;width:66px;height:66px;margin:0;padding:0;border-radius:50%;border:3px solid #fff;' +
    'background:rgba(229,56,59,.62);color:#fff;font:800 18px/1 sans-serif;letter-spacing:.5px;display:flex;flex-direction:column;' +
    'align-items:center;justify-content:center;gap:2px;box-shadow:0 3px 12px rgba(0,0,0,.45);touch-action:none;user-select:none;' +
    '-webkit-user-select:none;-webkit-touch-callout:none;-webkit-tap-highlight-color:transparent;cursor:pointer}' +
    '#mobBtn:active{background:rgba(229,56,59,.9)}' +
    '#mobBtn small{font:700 11px/1 sans-serif;display:none}#mobBtn.on small{display:block}' +
    '#mobBtn.on{background:#e5383b;animation:mobPulse 1s ease-in-out infinite}' +
    '#mobBtn.moving{background:rgba(229,56,59,.9);outline:3px dashed #ffd400;outline-offset:3px;transform:scale(1.12);animation:none}' +
    '@keyframes mobPulse{0%,100%{box-shadow:0 0 0 0 rgba(229,56,59,.8)}50%{box-shadow:0 0 0 12px rgba(229,56,59,0)}}' +
    '#wpList .card.mob{border-color:#e5383b;box-shadow:0 0 0 1px #e5383b inset}' +
    '.mobT{color:#ff6b6b;font-weight:700;margin-top:4px}';
  document.head.appendChild(css);

  const btn = document.createElement('button');
  btn.id = 'mobBtn'; btn.type = 'button';
  btn.innerHTML = '<span>MOB</span><small></small>';
  const sub = btn.querySelector('small');
  document.body.appendChild(btn);
  const label = () => { btn.title = L('Uomo in mare: tocca per segnare il punto. Tocco lungo per spostare il pulsante.', 'Man overboard: tap to mark the spot. Long press to move the button.'); btn.setAttribute('aria-label', btn.title); };
  label();

  // Where the button is: kept per screen, inside the window even when it gets smaller (phone turned, say)
  const SIZE = 66, EDGE = 6;
  let pos = load('np-mob-pos');
  function place(x, y) {
    const W = window.innerWidth, H = window.innerHeight;
    x = Math.max(EDGE, Math.min(W - SIZE - EDGE, x)); y = Math.max(EDGE, Math.min(H - SIZE - EDGE, y));
    btn.style.left = x + 'px'; btn.style.top = y + 'px';
    return { x, y };
  }
  function placeSaved() {
    const W = window.innerWidth, H = window.innerHeight;
    if (pos && typeof pos.fx === 'number') place(pos.fx * (W - SIZE), pos.fy * (H - SIZE));
    else place(12, Math.round(H * 0.45)); // first time: on the left, halfway down, away from the chart's buttons
  }
  placeSaved();
  window.addEventListener('resize', placeSaved);

  // Tap: MOB. Long press: the button is picked up and follows the finger. A long press let go without moving is a
  // MOB too: in a hurry a finger can stay down longer, and a MOB too many is better than one missed.
  // A finger that slides off before the long press (brushing past) does nothing.
  let pd = null;
  btn.addEventListener('pointerdown', (e) => {
    e.preventDefault(); e.stopPropagation();
    unlock();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    const r = btn.getBoundingClientRect();
    pd = { id: e.pointerId, x0: e.clientX, y0: e.clientY, bx: r.left, by: r.top, moving: false, dragged: false, slid: false, timer: 0 };
    pd.timer = setTimeout(() => {
      if (!pd || pd.slid) return;
      pd.moving = true; btn.classList.add('moving');
      if (navigator.vibrate) navigator.vibrate(30);
    }, 600);
  });
  btn.addEventListener('pointermove', (e) => {
    if (!pd || e.pointerId !== pd.id) return;
    e.preventDefault(); e.stopPropagation();
    const dx = e.clientX - pd.x0, dy = e.clientY - pd.y0;
    if (pd.moving) { if (Math.hypot(dx, dy) > 4) pd.dragged = true; place(pd.bx + dx, pd.by + dy); }
    else if (Math.hypot(dx, dy) > 12) { pd.slid = true; clearTimeout(pd.timer); }
  });
  function end(e) {
    if (!pd || e.pointerId !== pd.id) return;
    e.preventDefault(); e.stopPropagation();
    clearTimeout(pd.timer); btn.classList.remove('moving');
    const p = pd; pd = null;
    if (e.type === 'pointercancel') return;
    if (p.moving && p.dragged) {
      const r = btn.getBoundingClientRect(), W = window.innerWidth, H = window.innerHeight;
      pos = { fx: Math.max(0, Math.min(1, r.left / Math.max(1, W - SIZE))), fy: Math.max(0, Math.min(1, r.top / Math.max(1, H - SIZE))) };
      save('np-mob-pos', pos);
      return;
    }
    if (!p.slid) mob();
  }
  btn.addEventListener('pointerup', end);
  btn.addEventListener('pointercancel', end);
  btn.addEventListener('contextmenu', (e) => e.preventDefault());
  // Keyboard (a Raspberry Pi with a keyboard): Enter or space on the button
  btn.addEventListener('click', (e) => { if (e.detail === 0) mob(); });

  // ---------- on the chart: a lifebuoy, with the time since he went in ----------
  function drawMobs() {
    const list = mobs(); if (!list.length) return;
    const cv = $('map'), c = cv.getContext('2d'), dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight, z = map.z;
    const ox = Math.round(lonToX(map.lon, z) - w / 2), oy = Math.round(latToY(map.lat, z) - h / 2);
    c.save(); c.setTransform(dpr, 0, 0, dpr, 0, 0);
    for (const p of list) {
      const x = lonToX(p.lon, z) - ox, y = latToY(p.lat, z) - oy;
      if (x < -40 || y < -40 || x > w + 40 || y > h + 40) continue;
      // Four red and white quarters between two dark edges, over the page's waypoint cross
      c.lineWidth = 6;
      for (let i = 0; i < 4; i++) {
        c.beginPath(); c.arc(x, y, 10, i * Math.PI / 2 - Math.PI / 4, (i + 1) * Math.PI / 2 - Math.PI / 4);
        c.strokeStyle = i % 2 ? '#ffffff' : '#e5383b'; c.stroke();
      }
      c.lineWidth = 1.5; c.strokeStyle = '#04121f';
      c.beginPath(); c.arc(x, y, 13, 0, 7); c.stroke();
      c.beginPath(); c.arc(x, y, 7, 0, 7); c.stroke();
      if (p.t) {
        c.font = '700 12px sans-serif'; c.textAlign = 'center'; c.lineJoin = 'round';
        const s = ago(p.t);
        c.lineWidth = 3; c.strokeStyle = 'rgba(255,255,255,.9)'; c.strokeText(s, x, y + 28); c.fillStyle = '#e5383b'; c.fillText(s, x, y + 28);
      }
    }
    c.restore();
  }
  const pageDraw = window.drawMap;
  window.drawMap = function () {
    pageDraw.apply(this, arguments);
    if (!$('pMap').classList.contains('on')) return;
    try { drawMobs(); } catch (e) { console.warn('mob', e); }
  };

  // ---------- in the Waypoints page and in the waypoint's sheet: when he went in ----------
  const mobText = (p) => p.t ? L('Uomo in mare alle ', 'Man overboard at ') + hhmm(p.t) + ' · ' + ago(p.t) + L(' fa', ' ago') : L('Uomo in mare', 'Man overboard');
  const pageRenderWp = window.renderWp;
  window.renderWp = function () {
    pageRenderWp.apply(this, arguments);
    const cards = document.querySelectorAll('#wpList .card');
    if (cards.length !== ud.wps.length) return;
    ud.wps.forEach((p, i) => {
      const card = cards[i], on = isMob(p);
      card.classList.toggle('mob', on);
      let d = card.querySelector('.mobT');
      if (!on) { if (d) d.remove(); return; }
      if (!d) {
        d = document.createElement('div'); d.className = 'mobT';
        const btns = card.querySelector('.btns'); card.insertBefore(d, btns);
      }
      d.textContent = '\u{1F6DF} ' + mobText(p) + ' — ' + L('l\'allarme suona finché non lo elimini', 'the alarm sounds until you delete it');
    });
  };
  const pageShowWp = window.showWp;
  window.showWp = function (id) {
    pageShowWp.apply(this, arguments);
    const p = ud.wps.find((x) => x.id === id);
    if (!isMob(p)) return;
    const d = document.createElement('div'); d.className = 'mobT'; d.textContent = '\u{1F6DF} ' + mobText(p);
    const row = $('sheet').querySelector('.row');
    if (row) row.parentNode.insertBefore(d, row);
  };

  const pageLang = window.applyLang;
  window.applyLang = function () { pageLang.apply(this, arguments); label(); };
})();
