// NavPlotter: dead reckoning (stima) on the chart.
// Loaded after the page script and the other modules. The node sends, in the `dr` block of /api/state, the position
// it estimates and how uncertain it is (navplotter/stima.py in Navplotter-Linux). A node that sends no `dr` block
// leaves this module with nothing to do.
// - While the GPS has a fix nothing changes on the chart: the estimate runs in its shadow. Only a GPS that jumps
//   away from the estimate is shown, with a warning.
// - Without a fix the boat is drawn at the estimated position, in amber and never in the colour of the GPS position,
//   inside the ellipse that holds it 95 times in 100, with a banner that says so and for how long. With a compass
//   (the `att` block) the estimate follows the turns of the boat, and the banner says that instead.
// - In Setup a card shows what the estimate is doing, what the compass reads and how well it is calibrated, and
//   gives the recordings of the black box to download.
(function () {
  'use strict';
  if (typeof drawMap !== 'function' || typeof map === 'undefined' || typeof ud === 'undefined') return;

  const isIt = () => (typeof lang === 'string' ? lang : document.documentElement.lang || '').startsWith('it');
  const L = (i, e) => (isIt() ? i : e);
  const two = (n) => String(n).padStart(2, '0');
  const mmss = (s) => { s = Math.max(0, Math.floor(s)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60; return (h ? h + ':' + two(m) : m) + ':' + two(s % 60); };
  const dist = (m) => (m >= 926 ? (m / 1852).toFixed(m >= 18520 ? 0 : 1) + ' NM' : Math.round(m) + ' m');
  const AMBER = '#b45309';

  const dr = () => (typeof st !== 'undefined' && st && st.dr ? st.dr : null);
  const estimated = (d) => !!d && d.mode === 'dr';
  const jumped = (d) => !!d && d.mode === 'gps' && d.jump;
  // An estimate given up after having been one: said too, or the boat would just vanish from the chart
  const givenUp = (d) => !!d && d.mode === 'off' && d.age != null && d.age >= 5 && !(st.own && st.own.fix);

  // ---------- the banner over the chart ----------
  const css = document.createElement('style');
  css.textContent = '#stimaBar{position:absolute;left:10px;top:10px;right:66px;display:none;background:rgba(180,83,9,.95);color:#fff;' +
    'border:1px solid #fff;border-radius:8px;padding:6px 10px;font-size:14px;font-weight:700;line-height:1.25;pointer-events:none}' +
    '#stimaBar small{display:block;font-weight:400;font-size:12px}#stimaBar.off{background:rgba(71,85,105,.95)}' +
    '#stimaCard .sub{white-space:pre-line}';
  document.head.appendChild(css);
  const bar = document.createElement('div');
  bar.id = 'stimaBar';
  $('pMap').appendChild(bar);

  function banner() {
    const d = dr();
    let head = '', sub = '';
    if (estimated(d)) {
      head = L('STIMA · senza GPS da ', 'DEAD RECKONING · no GPS for ') + mmss(d.age);
      sub = L('Posizione stimata, entro ', 'Estimated position, within ') + dist(d.a) + (d.hdg
        ? L('. Segue le virate con la bussola; la velocità è l\'ultima nota.', '. It follows the turns by the compass; the speed is the last one known.')
        : L('. Vale solo se rotta e velocità non sono cambiate.', '. Good only if course and speed have not changed.'));
    } else if (jumped(d)) {
      head = L('GPS: salto di ', 'GPS: jump of ') + dist(d.gap);
      sub = L('La posizione GPS non torna con la rotta seguita: controllala.', 'The GPS position does not fit the course sailed: check it.');
    } else if (givenUp(d)) {
      head = L('Senza GPS da ', 'No GPS for ') + mmss(d.age);
      sub = L('La stima non è più affidabile: fai il punto con altri mezzi.', 'The dead reckoning is no longer reliable: fix the position by other means.');
    }
    bar.style.display = head ? 'block' : 'none';
    bar.classList.toggle('off', !!head && !estimated(d) && !jumped(d));
    if (head) { bar.textContent = head; const s = document.createElement('small'); s.textContent = sub; bar.appendChild(s); }
  }

  // ---------- on the chart ----------
  function draw() {
    const d = dr();
    if (!estimated(d) && !jumped(d)) return;
    const cv = $('map'), c = cv.getContext('2d'), dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight, z = map.z;
    const ox = Math.round(lonToX(map.lon, z) - w / 2), oy = Math.round(latToY(map.lat, z) - h / 2);
    const x = lonToX(d.lon, z) - ox, y = latToY(d.lat, z) - oy;
    const mpp = 156543.03392 * Math.cos(d.lat * D2R) / Math.pow(2, z);
    c.save(); c.setTransform(dpr, 0, 0, dpr, 0, 0);
    // The ellipse: the long half axis lies on the bearing th, so on the canvas it is turned th - 90 degrees
    const a = Math.max(6, d.a / mpp), b = Math.max(6, d.b / mpp);
    c.beginPath(); c.ellipse(x, y, a, b, (d.th - 90) * D2R, 0, 2 * Math.PI);
    c.fillStyle = 'rgba(245,158,11,.22)'; c.fill();
    c.setLineDash([7, 5]); c.lineWidth = 2; c.strokeStyle = AMBER; c.stroke(); c.setLineDash([]);
    if (jumped(d) && st.own && st.own.fix) {
      // From where the GPS says to where the boat should be
      const gx = lonToX(st.own.lon, z) - ox, gy = latToY(st.own.lat, z) - oy;
      c.beginPath(); c.moveTo(gx, gy); c.lineTo(x, y); c.setLineDash([3, 5]); c.strokeStyle = AMBER; c.lineWidth = 2; c.stroke(); c.setLineDash([]);
    }
    if (typeof drawSymbol === 'function') drawSymbol(c, typeof cfg !== 'undefined' ? cfg.type : 0, x, y, d.cog, AMBER, 13, true);
    c.font = '700 12px sans-serif'; c.textAlign = 'center'; c.lineJoin = 'round';
    const s = L('STIMA', 'DR');
    c.lineWidth = 3; c.strokeStyle = 'rgba(255,255,255,.9)'; c.strokeText(s, x, y + 30); c.fillStyle = AMBER; c.fillText(s, x, y + 30);
    c.restore();
  }

  const pageDraw = window.drawMap;
  window.drawMap = function () {
    // The chart that follows the boat follows the estimate too, when it is all there is
    const d = dr();
    if (estimated(d) && map.follow) { map.lat = d.lat; map.lon = d.lon; }
    pageDraw.apply(this, arguments);
    try { banner(); } catch (e) { console.warn('stima', e); }
    if (!$('pMap').classList.contains('on')) return;
    try { draw(); } catch (e) { console.warn('stima', e); }
  };

  // ---------- in Setup: what the estimate is doing, and the black box ----------
  const card = document.createElement('div');
  card.className = 'card'; card.id = 'stimaCard'; card.style.display = 'none';
  const title = document.createElement('div'); title.className = 'title';
  const sub = document.createElement('div'); sub.className = 'sub';
  const link = document.createElement('a'); link.className = 'btn sec'; link.href = '/api/log.zip'; link.download = 'navplotter-log.zip';
  link.style.cssText = 'display:none;margin-top:10px;text-decoration:none';
  card.appendChild(title); card.appendChild(sub); card.appendChild(link);
  const wrap = document.querySelector('#pSetup .wrap');
  if (wrap) wrap.appendChild(card);

  function renderCard() {
    const d = dr();
    card.style.display = d ? '' : 'none';
    if (!d) return;
    title.textContent = L('Stima', 'Dead reckoning');
    let s;
    if (d.mode === 'gps') {
      s = d.jump ? L('Il GPS è saltato di ', 'The GPS has jumped ') + dist(d.gap) + L(': non lo sta seguendo.', ': not following it.')
        : L('In ombra al GPS · scarto ', 'In the shadow of the GPS · gap ') + dist(d.gap || 0) + L(' · incertezza ', ' · uncertainty ') + dist(d.a);
    } else if (d.mode === 'dr') {
      s = L('Attiva: senza GPS da ', 'Active: no GPS for ') + mmss(d.age) + L(' · incertezza ', ' · uncertainty ') + dist(d.a);
    } else {
      s = d.age == null ? L('In attesa del primo punto GPS.', 'Waiting for the first GPS fix.') : L('Non affidabile: senza GPS da ', 'Not reliable: no GPS for ') + mmss(d.age);
    }
    const a = st.att;
    if (a) {
      const heel = Math.abs(a.roll) < 0.5 ? L('dritta', 'upright') : Math.abs(a.roll).toFixed(0) + '° ' + (a.roll > 0 ? L('a dritta', 'to starboard') : L('a sinistra', 'to port'));
      s += '\n' + L('Bussola: ', 'Compass: ') + String(Math.round(a.hdg) % 360).padStart(3, '0') + '° M · ' + L('sbandamento ', 'heel ') + heel +
        ' · ' + L('taratura ', 'calibration ') + a.acc + '/3' + (a.acc < 2 ? L(' (fai qualche giro lento)', ' (make a few slow turns)') : '');
    } else if (d.hdg === false && st.att === null) {
      s += '\n' + L('Bussola: non collegata.', 'Compass: not connected.');
    }
    const g = st.log;
    if (g) {
      s += '\n' + L('Scatola nera: ', 'Black box: ') + g.days + L(g.days === 1 ? ' giorno' : ' giorni', g.days === 1 ? ' day' : ' days') + ', ' +
        (g.bytes / 1048576).toFixed(1) + ' MB' + L(' (tiene ' + g.keepDays + ' giorni)', ' (keeps ' + g.keepDays + ' days)');
    }
    sub.textContent = s;
    link.style.display = g && g.days ? 'inline-block' : 'none';
    link.textContent = L('Scarica le registrazioni', 'Download the recordings');
  }
  setInterval(() => { try { renderCard(); } catch (e) { console.warn('stima', e); } }, 1000);
})();
