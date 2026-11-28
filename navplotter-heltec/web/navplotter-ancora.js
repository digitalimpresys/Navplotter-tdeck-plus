// NavPlotter: anchor watch.
// Loaded after the page script and after navplotter-meteo.js and navplotter-mob.js. It adds an anchor button that
// floats over every page, like the MOB one. A tap opens the anchor card: "Drop anchor here" sets the anchor at the
// boat's position with the chosen swinging radius; from then on, if the boat stays outside the circle (it drags,
// or the rode parts) an alarm goes off, on the chart, in a banner over every page and with sound.
// When the node watches the anchor itself (the T-Deck: "anchor" in /api/state) the node is the one that decides
// and sounds, also with the phone off; the page only shows and changes it (POST /api/anchor). On a node that does
// not know the anchor yet, the page watches on its own, as long as it stays open.
(function () {
  'use strict';
  if (typeof drawMap !== 'function' || typeof map === 'undefined') return;

  const isIt = () => (typeof lang === 'string' ? lang : document.documentElement.lang || '').startsWith('it');
  const L = (i, e) => (isIt() ? i : e);
  const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} };
  const load = (k) => { try { return JSON.parse(localStorage.getItem(k) || 'null'); } catch (e) { return null; } };
  const two = (n) => String(n).padStart(2, '0');
  const since = (s) => { s = Math.max(0, Math.floor(s)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60; return h ? h + ' h ' + two(m) + ' min' : m + ' min'; };
  const meters = (lat1, lon1, lat2, lon2) => {
    const dy = (lat2 - lat1) * 110540, dx = (lon2 - lon1) * 111320 * Math.cos(lat1 * Math.PI / 180);
    let b = Math.atan2(dx, dy) * 180 / Math.PI; if (b < 0) b += 360;
    return { m: Math.hypot(dx, dy), brg: b };
  };
  const RADII = [20, 30, 40, 50, 75, 100, 150];
  const R_MIN = 10, R_MAX = 500, OUT_S = 8, IN_S = 20, GPS_LOST_S = 60, MUTE_MS = 300000;

  // ---------- the state: from the node, or kept by the page ----------
  const nodeMode = () => !!(st && st.anchor);
  let local = load('np-anchor') || { on: false, lat: 0, lon: 0, r: 40, t: 0 };
  const lw = { outS: 0, inS: 0, alarm: false, gpsLost: false, lastGood: Date.now(), dist: -1, brg: 0, max: 0, mutedAt: 0 };
  let lastR = load('np-anchor-r') || local.r || 40;
  let busy = false;

  function A() {
    if (nodeMode()) return st.anchor;
    return { on: local.on, lat: local.lat, lon: local.lon, r: local.r, dist: lw.dist, brg: lw.brg, max: lw.max, alarm: lw.alarm,
      gpsLost: lw.gpsLost, muted: !!lw.mutedAt && Date.now() - lw.mutedAt < MUTE_MS, age: local.t ? (Date.now() - local.t) / 1000 : 0, outS: lw.outS };
  }
  const fix = () => (st && st.own && st.own.fix ? st.own : null);

  function post(params) {
    busy = true;
    return fetch('/api/anchor', { method: 'POST', body: new URLSearchParams(params) })
      .then((r) => r.json().catch(() => ({})).then((j) => ({ ok: r.ok, j })))
      .then(({ ok, j }) => { if (j && j.anchor && st) st.anchor = j.anchor; return ok ? j : Promise.reject(j); })
      .finally(() => { busy = false; refresh(); });
  }

  function drop(r) {
    r = Math.max(R_MIN, Math.min(R_MAX, Math.round(r)));
    lastR = r; save('np-anchor-r', r);
    const o = fix();
    if (!o) { alert(L('Ancora: serve la posizione GPS. Aspetta il fix e riprova.', 'Anchor: a GPS position is needed. Wait for the fix and try again.')); return; }
    unlock();
    if (nodeMode()) {
      post({ on: 1, lat: o.lat.toFixed(7), lon: o.lon.toFixed(7), r }).catch(() => alert(L('Il nodo non ha accettato l\'ancora.', 'The node did not take the anchor.')));
    } else {
      local = { on: true, lat: o.lat, lon: o.lon, r, t: Date.now() }; save('np-anchor', local);
      Object.assign(lw, { outS: 0, inS: 0, alarm: false, gpsLost: false, lastGood: Date.now(), dist: 0, max: 0, mutedAt: 0 });
      refresh();
    }
    if (navigator.vibrate) navigator.vibrate(60);
  }
  function weigh() {
    if (!confirm(L('Salpare? La guardia all\'ancora si spegne.', 'Weigh anchor? The anchor watch switches off.'))) return;
    if (nodeMode()) post({ on: 0 }).catch(() => {});
    else { local.on = false; save('np-anchor', local); lw.alarm = false; refresh(); }
  }
  function setR(r) {
    r = Math.max(R_MIN, Math.min(R_MAX, Math.round(r)));
    lastR = r; save('np-anchor-r', r);
    if (nodeMode()) post({ r }).catch(() => {});
    else { local.r = r; save('np-anchor', local); lw.outS = lw.inS = 0; refresh(); }
  }
  function mute() {
    if (nodeMode()) post({ mute: 1 }).catch(() => {});
    else { lw.mutedAt = Date.now(); refresh(); }
  }

  // The page's own watch, once a second, for nodes that do not watch the anchor
  setInterval(() => {
    if (nodeMode() || !local.on) return;
    const o = fix(), was = lw.alarm;
    if (o) {
      lw.lastGood = Date.now();
      const d = meters(local.lat, local.lon, o.lat, o.lon);
      lw.dist = d.m; lw.brg = d.brg; if (d.m > lw.max) lw.max = d.m;
      if (d.m > local.r) { lw.outS++; lw.inS = 0; } else { lw.inS++; lw.outS = 0; }
      if (lw.gpsLost) { lw.gpsLost = false; if (lw.outS === 0) lw.alarm = false; }
      if (lw.outS >= OUT_S) lw.alarm = true;
      if (lw.alarm && lw.inS >= IN_S) lw.alarm = false;
    } else if (Date.now() - lw.lastGood > GPS_LOST_S * 1000) {
      lw.dist = -1; lw.gpsLost = true; lw.alarm = true;
    }
    if (!lw.alarm) lw.mutedAt = 0;
    if (lw.alarm && !was) nextSound = 0;
  }, 1000);

  // ---------- the alarm: banner, mark in the status bar, sound ----------
  const nativeAlarm = () => { try { return !!(window.NavPlotterApp && window.NavPlotterApp.watching && window.NavPlotterApp.watching()); } catch (e) { return false; } };
  let ac = null, nextSound = 0;
  function unlock() {
    if (!ac) { try { ac = new (window.AudioContext || window.webkitAudioContext)(); } catch (e) { return; } }
    if (ac.state === 'suspended') ac.resume();
  }
  // Browsers start the sound only after a touch: after a reload with the anchor alarm on, the first touch starts it
  ['pointerdown', 'keydown'].forEach((ev) => document.addEventListener(ev, () => { if (A().on) unlock(); }, true));
  function tone(f, at, len) {
    const o = ac.createOscillator(), g = ac.createGain();
    o.type = 'square'; o.frequency.value = f;
    g.gain.setValueAtTime(0.0001, at); g.gain.exponentialRampToValueAtTime(0.3, at + 0.02);
    g.gain.setValueAtTime(0.3, at + len - 0.03); g.gain.exponentialRampToValueAtTime(0.0001, at + len);
    o.connect(g); g.connect(ac.destination); o.start(at); o.stop(at + len + 0.02);
  }
  // Low, low, high, four times: not like the MOB (high-low) nor the collision beeps
  function alarmOnce() {
    if (!ac || ac.state !== 'running') return;
    const t0 = ac.currentTime + 0.05;
    for (let i = 0; i < 4; i++) { tone(700, t0 + i * 0.45, 0.12); tone(700, t0 + i * 0.45 + 0.14, 0.12); tone(1100, t0 + i * 0.45 + 0.28, 0.14); }
  }

  const css = document.createElement('style');
  css.textContent =
    '#ancBtn{position:fixed;z-index:2147482990;width:58px;height:58px;margin:0;padding:0;border-radius:50%;border:3px solid #fff;' +
    'background:rgba(18,41,74,.72);color:#fff;font:700 26px/1 sans-serif;display:flex;flex-direction:column;align-items:center;' +
    'justify-content:center;gap:1px;box-shadow:0 3px 12px rgba(0,0,0,.45);touch-action:none;user-select:none;-webkit-user-select:none;' +
    '-webkit-touch-callout:none;-webkit-tap-highlight-color:transparent;cursor:pointer}' +
    '#ancBtn small{font:700 10px/1 sans-serif;display:none}#ancBtn.on small{display:block}' +
    '#ancBtn.on{background:#1f8a4c}#ancBtn.alarm{background:#e5383b;animation:ancPulse 1s ease-in-out infinite}' +
    '#ancBtn.moving{outline:3px dashed #ffd400;outline-offset:3px;transform:scale(1.12);animation:none}' +
    '@keyframes ancPulse{0%,100%{box-shadow:0 0 0 0 rgba(229,56,59,.8)}50%{box-shadow:0 0 0 12px rgba(229,56,59,0)}}' +
    '#ancAlarm{display:none;background:#e5383b;color:#fff;font-weight:700;padding:6px 6px 6px 12px;font-size:15px;align-items:center;gap:8px;' +
    'border-radius:14px;box-shadow:0 4px 14px rgba(0,0,0,.35)}#ancAlarm.on{display:flex}' +
    '#ancAlarm span{flex:1;text-align:center;animation:blink 1s steps(2) infinite}#ancAlarm.muted span{animation:none}' +
    '#ancAlarm button{flex:none;border:2px solid #fff;background:none;color:#fff;border-radius:9px;padding:7px 10px;font-size:14px;font-weight:700}' +
    // Silenced, the banner leaves the page like the collision one: the red mark in the status bar stays, a tap opens the card
    '#ancAlarm.muted{display:none}' +
    '#sAnc{display:none;font-weight:700;font-size:13px;border-radius:999px;padding:2px 9px;background:#1f8a4c;color:#fff;cursor:pointer}' +
    '#sAnc.on{display:inline-block}#sAnc.alarm{background:#e5383b}' +
    '#ancCard{position:fixed;z-index:2147483100;left:50%;bottom:16px;transform:translateX(-50%);width:min(440px,calc(100% - 20px));' +
    'max-height:calc(100% - 32px);overflow:auto;background:var(--panel,#12294a);color:var(--text,#eef3fa);border:1px solid var(--line,#27456f);' +
    'border-radius:16px;box-shadow:0 8px 30px rgba(0,0,0,.55);padding:14px 14px 12px;display:none}#ancCard.on{display:block}' +
    '#ancCard h3{margin:0 0 4px;font-size:18px;display:flex;align-items:center;gap:8px}#ancCard h3 .x{margin-left:auto;background:none;border:0;' +
    'color:var(--dim,#9db0c9);font-size:26px;line-height:1;padding:0 4px}' +
    '#ancCard .hint{color:var(--dim,#9db0c9);font-size:13px;margin:4px 0 10px}' +
    '#ancCard .big{font-size:30px;font-weight:800;font-variant-numeric:tabular-nums}#ancCard .big small{font-size:15px;font-weight:600;color:var(--dim,#9db0c9)}' +
    '#ancCard .grid{display:grid;grid-template-columns:1fr 1fr;gap:4px 12px;font-size:14px;margin:8px 0}#ancCard .grid span:nth-child(odd){color:var(--dim,#9db0c9)}' +
    '#ancCard .chips{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0 10px}#ancCard .chips button{border:1px solid var(--line,#27456f);' +
    'background:none;color:inherit;border-radius:999px;padding:7px 12px;font:600 14px sans-serif}' +
    '#ancCard .chips button.on{background:var(--accent,#3da5ff);border-color:var(--accent,#3da5ff);color:#04121f}' +
    '#ancCard .row{display:flex;gap:8px;margin-top:10px}#ancCard .row button{flex:1;border:0;border-radius:12px;padding:13px 10px;font:700 16px sans-serif}' +
    '#ancCard .go{background:#1f8a4c;color:#fff}#ancCard .stop{background:#e5383b;color:#fff}#ancCard .sec{background:var(--line,#27456f);color:inherit}' +
    '#ancCard .bar{height:10px;border-radius:5px;background:var(--line,#27456f);overflow:hidden;margin:6px 0 2px}' +
    '#ancCard .bar i{display:block;height:100%;background:#2fd16b}#ancCard .bar.warn i{background:#f4a300}#ancCard .bar.bad i{background:#e5383b}' +
    '#ancCard .err{color:#ff6b6b;font-weight:700;margin-top:6px}';
  document.head.appendChild(css);

  // Banner over every page, next to the page's collision banner
  const banner = document.createElement('div');
  banner.id = 'ancAlarm';
  banner.innerHTML = '<span></span><button type="button"></button>';
  const bText = banner.querySelector('span'), bMute = banner.querySelector('button');
  bText.onclick = () => openCard();
  bMute.onclick = () => mute();
  const banners = document.getElementById('banners');
  const pageAlarm = document.getElementById('alarm');
  if (banners) banners.insertBefore(banner, pageAlarm && pageAlarm.parentNode === banners ? pageAlarm.nextSibling : banners.firstChild); else document.body.appendChild(banner);

  // Mark in the status bar: distance over radius, a tap opens the card
  const mark = document.createElement('span');
  mark.id = 'sAnc'; mark.onclick = () => openCard();
  const sAlarm = document.getElementById('sAlarm');
  if (sAlarm && sAlarm.parentNode) sAlarm.parentNode.insertBefore(mark, sAlarm);

  // ---------- the button ----------
  const btn = document.createElement('button');
  btn.id = 'ancBtn'; btn.type = 'button';
  btn.innerHTML = '<span>⚓</span><small></small>';
  const sub = btn.querySelector('small');
  document.body.appendChild(btn);
  const label = () => { btn.title = L('Guardia all\'ancora: tocca per aprirla. Tocco lungo per spostare il pulsante.', 'Anchor watch: tap to open it. Long press to move the button.'); btn.setAttribute('aria-label', btn.title); };
  label();

  const SIZE = 58, EDGE = 6;
  let pos = load('np-anc-pos');
  function place(x, y) {
    const W = window.innerWidth, H = window.innerHeight;
    x = Math.max(EDGE, Math.min(W - SIZE - EDGE, x)); y = Math.max(EDGE, Math.min(H - SIZE - EDGE, y));
    btn.style.left = x + 'px'; btn.style.top = y + 'px';
  }
  function placeSaved() {
    const W = window.innerWidth, H = window.innerHeight;
    if (pos && typeof pos.fx === 'number') place(pos.fx * (W - SIZE), pos.fy * (H - SIZE));
    else place(16, Math.round(H * 0.45) + 82); // first time: under the MOB button
  }
  placeSaved();
  window.addEventListener('resize', placeSaved);

  let pd = null;
  btn.addEventListener('pointerdown', (e) => {
    e.preventDefault(); e.stopPropagation(); unlock();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    const r = btn.getBoundingClientRect();
    pd = { id: e.pointerId, x0: e.clientX, y0: e.clientY, bx: r.left, by: r.top, moving: false, dragged: false, slid: false, timer: 0 };
    pd.timer = setTimeout(() => { if (!pd || pd.slid) return; pd.moving = true; btn.classList.add('moving'); if (navigator.vibrate) navigator.vibrate(30); }, 600);
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
      save('np-anc-pos', pos);
      return;
    }
    if (!p.slid) toggleCard();
  }
  btn.addEventListener('pointerup', end);
  btn.addEventListener('pointercancel', end);
  btn.addEventListener('contextmenu', (e) => e.preventDefault());
  btn.addEventListener('click', (e) => { if (e.detail === 0) toggleCard(); });

  // ---------- the card ----------
  const card = document.createElement('div');
  card.id = 'ancCard';
  document.body.appendChild(card);
  let cardHtml = '', pickR = null;
  function openCard() { pickR = null; card.classList.add('on'); renderCard(true); }
  function closeCard() { card.classList.remove('on'); }
  function toggleCard() { if (card.classList.contains('on')) closeCard(); else openCard(); }

  function chips(cur) {
    const list = RADII.includes(cur) ? RADII : [...RADII, cur].sort((a, b) => a - b);
    return '<div class="chips">' + list.map((r) => '<button type="button" data-r="' + r + '" class="' + (r === cur ? 'on' : '') + '">' + r + ' m</button>').join('') +
      '<button type="button" data-r="ask">' + L('Altro…', 'Other…') + '</button></div>';
  }
  function renderCard(force) {
    if (!card.classList.contains('on')) return;
    if (!force && pd) return;
    const a = A(), o = fix();
    let h = '<h3>⚓ ' + L('Guardia all\'ancora', 'Anchor watch') + '<button type="button" class="x" data-act="close" aria-label="' + L('Chiudi', 'Close') + '">×</button></h3>';
    if (!a.on) {
      const r = pickR || lastR;
      h += '<div class="hint">' + L('Dai fondo quando la barca è sopra l\'ancora. Raggio: catena filata più la lunghezza della barca, con un po\' di margine.',
        'Drop it when the boat is over the anchor. Radius: rode let out plus the boat\'s length, with some margin.') + '</div>';
      h += '<div>' + L('Raggio di guardia', 'Swinging radius') + '</div>' + chips(r);
      if (!o) h += '<div class="err">' + L('In attesa della posizione GPS…', 'Waiting for the GPS position…') + '</div>';
      h += '<div class="row"><button type="button" class="go" data-act="drop"' + (o ? '' : ' disabled') + '>' + L('Dai fondo qui', 'Drop anchor here') + '</button></div>';
    } else {
      const pct = a.dist >= 0 ? Math.min(100, a.dist / a.r * 100) : 0;
      const cls = a.alarm ? 'bad' : pct > 80 ? 'warn' : '';
      h += '<div class="big">' + (a.dist >= 0 ? Math.round(a.dist) : '--') + ' <small>/ ' + a.r + ' m</small></div>';
      h += '<div class="bar ' + cls + '"><i style="width:' + pct.toFixed(0) + '%"></i></div>';
      if (a.alarm) h += '<div class="err">' + (a.gpsLost ? L('GPS perso: la posizione non arriva da più di un minuto.', 'GPS lost: no position for more than a minute.')
        : L('La barca è fuori dal cerchio: l\'ancora ara!', 'The boat is outside the circle: the anchor is dragging!')) + '</div>';
      else if (a.outS > 0) h += '<div class="hint">' + L('Fuori dal cerchio da ', 'Outside the circle for ') + a.outS + ' s…</div>';
      h += '<div class="grid"><span>' + L('Dall\'ancora', 'From the anchor') + '</span><span>' + (a.dist >= 0 ? Math.round(a.brg) + '° · ' + Math.round(a.dist) + ' m' : '--') + '</span>' +
        '<span>' + L('Massimo', 'Furthest') + '</span><span>' + Math.round(a.max || 0) + ' m</span>' +
        '<span>' + L('All\'ancora da', 'At anchor for') + '</span><span>' + since(a.age || 0) + '</span>' +
        '<span>' + L('Sorveglia', 'Watched by') + '</span><span>' + (nodeMode() ? L('il nodo, anche a telefono spento', 'the node, also with the phone off') : L('questa pagina: tienila aperta', 'this page: keep it open')) + '</span></div>';
      h += '<div>' + L('Raggio di guardia', 'Swinging radius') + '</div>' + chips(a.r);
      h += '<div class="row">' + (a.alarm ? '<button type="button" class="sec" data-act="mute">' + (a.muted ? L('Silenziato', 'Silenced') : L('Silenzia 5 min', 'Silence 5 min')) + '</button>' : '') +
        '<button type="button" class="sec" data-act="here"' + (o ? '' : ' disabled') + '>' + L('Ancora qui', 'Anchor here') + '</button>' +
        '<button type="button" class="stop" data-act="weigh">' + L('Salpa', 'Weigh') + '</button></div>';
    }
    if (busy) h = h.replace(/<button type="button" class="(go|stop|sec)"/g, '<button type="button" disabled class="$1"');
    if (h !== cardHtml) { card.innerHTML = h; cardHtml = h; }
  }
  card.addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b || b.disabled) return;
    unlock();
    const a = A();
    if (b.dataset.r) {
      let r = +b.dataset.r;
      if (b.dataset.r === 'ask') { r = parseInt(prompt(L('Raggio in metri (10-500)', 'Radius in metres (10-500)'), String(a.on ? a.r : lastR)), 10); if (!(r > 0)) return; }
      r = Math.max(R_MIN, Math.min(R_MAX, r));
      if (a.on) setR(r); else { pickR = r; lastR = r; save('np-anchor-r', r); renderCard(true); }
      return;
    }
    const act = b.dataset.act;
    if (act === 'close') closeCard();
    else if (act === 'drop') drop(pickR || lastR);
    else if (act === 'here') { if (confirm(L('Spostare il punto dell\'ancora dove si trova ora la barca?', 'Move the anchor point to where the boat is now?'))) drop(a.r); }
    else if (act === 'weigh') weigh();
    else if (act === 'mute') mute();
  });

  // ---------- once every half second: button, banner, status mark, card, sound ----------
  function refresh() {
    const a = A();
    btn.classList.toggle('on', !!a.on);
    btn.classList.toggle('alarm', !!a.alarm);
    sub.textContent = a.on && a.dist >= 0 ? Math.round(a.dist) + 'm' : '';
    banner.classList.toggle('on', !!a.alarm);
    banner.classList.toggle('muted', !!a.muted);
    if (a.alarm) {
      bText.textContent = '⚓ ' + (a.gpsLost ? L('ANCORA: GPS PERSO', 'ANCHOR: GPS LOST') : L('L\'ANCORA ARA: ', 'ANCHOR DRAGGING: ') + Math.round(a.dist) + ' / ' + a.r + ' m');
      bMute.textContent = a.muted ? L('Silenziato', 'Silenced') : L('Silenzia', 'Silence');
    }
    mark.classList.toggle('on', !!a.on);
    mark.classList.toggle('alarm', !!a.alarm);
    if (a.on) mark.textContent = '⚓ ' + (a.dist >= 0 ? Math.round(a.dist) : '--') + '/' + a.r + ' m';
    renderCard(false);
    // The node sounds on its own speaker; the page sounds too, so that a phone in the cabin wakes the crew
    if (a.alarm && !a.muted && !nativeAlarm() && Date.now() >= nextSound) {
      alarmOnce(); nextSound = Date.now() + 2500;
      if (navigator.vibrate) navigator.vibrate([300, 120, 300]);
    }
    if (!a.alarm) nextSound = 0;
  }
  setInterval(refresh, 500);

  // ---------- on the chart: the swinging circle, the anchor and a line to the boat ----------
  function drawAnchor() {
    const a = A(); if (!a.on) return;
    const cv = document.getElementById('map'), c = cv.getContext('2d'), dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight, z = map.z;
    const ox = Math.round(lonToX(map.lon, z) - w / 2), oy = Math.round(latToY(map.lat, z) - h / 2);
    const x = lonToX(a.lon, z) - ox, y = latToY(a.lat, z) - oy;
    const mpp = 40075016.686 * Math.cos(a.lat * Math.PI / 180) / worldSize(z);
    const rp = a.r / mpp, col = a.alarm ? '#e5383b' : '#1f8a4c';
    c.save(); c.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (rp < 20000 && x > -rp - 40 && y > -rp - 40 && x < w + rp + 40 && y < h + rp + 40) {
      c.beginPath(); c.arc(x, y, Math.max(rp, 3), 0, 2 * Math.PI);
      c.fillStyle = a.alarm ? 'rgba(229,56,59,.12)' : 'rgba(31,138,76,.12)'; c.fill();
      c.lineWidth = 2.5; c.strokeStyle = col; c.setLineDash(a.alarm ? [] : [8, 5]); c.stroke(); c.setLineDash([]);
    }
    const o = fix();
    if (o) {
      const bx = lonToX(o.lon, z) - ox, by = latToY(o.lat, z) - oy;
      c.beginPath(); c.moveTo(x, y); c.lineTo(bx, by); c.lineWidth = 1.5; c.strokeStyle = col; c.stroke();
    }
    if (x > -30 && y > -30 && x < w + 30 && y < h + 30) {
      c.font = '700 22px sans-serif'; c.textAlign = 'center'; c.textBaseline = 'middle';
      c.lineWidth = 4; c.strokeStyle = 'rgba(255,255,255,.95)'; c.strokeText('⚓', x, y);
      c.fillStyle = '#04121f'; c.fillText('⚓', x, y);
      if (rp > 30) {
        c.font = '700 12px sans-serif'; c.textBaseline = 'alphabetic';
        const s = a.r + ' m';
        c.lineWidth = 3; c.strokeText(s, x, y - rp - 6); c.fillStyle = col; c.fillText(s, x, y - rp - 6);
      }
    }
    c.restore();
  }
  const pageDraw = window.drawMap;
  window.drawMap = function () {
    pageDraw.apply(this, arguments);
    if (!document.getElementById('pMap').classList.contains('on')) return;
    try { drawAnchor(); } catch (e) { console.warn('anchor', e); }
  };

  const pageLang = window.applyLang;
  window.applyLang = function () { pageLang.apply(this, arguments); label(); cardHtml = ''; renderCard(true); };
})();
