#!/usr/bin/env python3
"""Draws nautical-style chart tiles from OpenStreetMap data, for the SD card of the plotter and of the Heltec node.

Output: <out>/tiles/<z>/<x>/<y>.jpg (256x256, baseline JPEG).

Data needed (free downloads, see docs in the repository README):
  - one or more OpenStreetMap extracts (.osm.pbf) from download.geofabrik.de
  - the "land-polygons-split-4326" shapefile from osmdata.openstreetmap.de (the coastline as land polygons)

Usage:
  python render_charts.py extract --bbox S,W,N,E --pbf a.osm.pbf b.osm.pbf --land land-polygons-split-4326/land_polygons.shp --cache chart.pkl
  python render_charts.py render  --cache chart.pkl --zoom 8-16 --out ~/nautical-tiles
  python render_charts.py preview --cache chart.pkl --zoom 13 --center 45.43,12.33 --size 3x3 --png preview.png

Low zooms cover the whole area. From --coastal-from (default 14) upwards only tiles near the sea are drawn, and
tiles that are nothing but open sea are never written: the plotter shows its own sea colour there.

Not for navigation: OpenStreetMap has no depths and its seamarks are entered by volunteers.
Needs: pip install osmium shapely pyshp pillow
"""
import argparse, math, os, pickle, sys, time
from collections import defaultdict

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

TILE = 256
SS = 2            # supersampling: everything is drawn twice as large and scaled down, for smooth edges
META = 8          # tiles per side drawn in one go
BUFFER = 96       # extra pixels around a metatile, so labels and symbols crossing its edge are complete

# ---- Colours (the sea is the same colour the plotter uses where it has no tile) ----
SEA = (156, 218, 239)
LAND = (247, 239, 205)
URBAN = (232, 220, 190)
GREEN = (214, 232, 190)
MARSH = (170, 208, 150)        # salt marsh (barene): above water most of the time
SHALLOW = (196, 230, 226)      # tidal flats and other wetland: shallow, may dry
BEACH = (250, 232, 150)
INLAND_WATER = (156, 218, 239)
PIER = (120, 110, 100)
COAST = (70, 80, 90)
ROAD_MAJOR = (214, 150, 90)
ROAD_MINOR = (255, 255, 255)
ROAD_CASING = (170, 160, 140)
RAIL = (120, 120, 120)
FERRY = (60, 90, 170)
FAIRWAY = (190, 60, 170)
TEXT = (30, 30, 30)
TEXT_WATER = (20, 70, 140)
HALO = (255, 255, 255)
SEAMARK_COLOURS = {"red": (220, 30, 40), "green": (20, 150, 60), "yellow": (240, 200, 0), "black": (20, 20, 20),
                   "white": (255, 255, 255), "orange": (240, 130, 20), "blue": (30, 90, 200)}
LIGHT = (200, 40, 170)

FONT_FILES = ["/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
FONT_ITALIC_FILES = ["/System/Library/Fonts/Supplemental/Arial Italic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf"]
FONT_BOLD_FILES = ["/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]
_fonts = {}


def font(size, style="regular"):
    key = (size, style)
    if key not in _fonts:
        files = {"regular": FONT_FILES, "italic": FONT_ITALIC_FILES + FONT_FILES, "bold": FONT_BOLD_FILES + FONT_FILES}[style]
        for f in files:
            if os.path.exists(f):
                _fonts[key] = ImageFont.truetype(f, size)
                break
        else:
            _fonts[key] = ImageFont.load_default()
    return _fonts[key]


# ---------- Web Mercator ----------
def lon_to_x(lon, z):
    return (lon + 180.0) / 360.0 * TILE * (1 << z)


def lat_to_y(lat, z):
    lat = max(-85.0511, min(85.0511, lat))
    r = math.radians(lat)
    return (1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * TILE * (1 << z)


def tile_of(lat, lon, z):
    return int(lon_to_x(lon, z) // TILE), int(lat_to_y(lat, z) // TILE)


# ============================================
# Extract: OpenStreetMap data -> a compact cache of what the chart needs
# ============================================
# Area classes, drawn in this order
AREA_CLASSES = ["green", "urban", "beach", "shallow", "marsh", "water", "pier"]
# Line classes
LINE_CLASSES = ["river", "ferry", "fairway", "rail", "road3", "road2", "road1", "pier"]


def classify_area(t):
    nat, lu, mm, le, ww = t.get("natural"), t.get("landuse"), t.get("man_made"), t.get("leisure"), t.get("waterway")
    if mm in ("pier", "breakwater", "groyne", "quay"):
        return "pier"
    if nat == "water" or ww in ("riverbank", "dock") or lu in ("reservoir", "basin", "salt_pond"):
        return "water"
    if nat in ("wetland", "mud", "shoal"):
        return "marsh" if t.get("wetland") in ("saltmarsh", "marsh", "reedbed", "swamp", "wet_meadow", "fen", "bog") else "shallow"
    if nat in ("beach", "sand"):
        return "beach"
    if lu in ("residential", "commercial", "industrial", "retail", "port", "harbour") or le == "marina":
        return "urban"
    if lu in ("forest", "meadow", "grass") or nat in ("wood", "scrub") or le in ("park", "nature_reserve"):
        return "green"
    return None


def classify_line(t):
    hw, mm, ww, rw = t.get("highway"), t.get("man_made"), t.get("waterway"), t.get("railway")
    if mm in ("pier", "breakwater", "groyne"):
        return "pier"
    if hw in ("motorway", "trunk", "primary", "motorway_link", "trunk_link"):
        return "road1"
    if hw in ("secondary", "tertiary"):
        return "road2"
    if hw in ("residential", "unclassified", "living_street"):
        return "road3"
    if rw == "rail":
        return "rail"
    if ww in ("river", "canal"):
        return "river"
    if t.get("route") == "ferry":
        return "ferry"
    if ww == "fairway" or t.get("seamark:type") in ("fairway", "navigation_line", "recommended_track"):
        return "fairway"
    return None


PLACE_RANK = {"city": 1, "town": 2, "island": 3, "village": 4, "suburb": 5, "islet": 5, "hamlet": 6, "locality": 7}


def classify_point(t):
    """Returns (kind, info) for things drawn as a symbol and/or a label."""
    sm = t.get("seamark:type")
    if sm:
        return "seamark", seamark_info(t, sm)
    place = t.get("place")
    name = t.get("name")
    if place in PLACE_RANK and name:
        return "place", {"name": name, "rank": PLACE_RANK[place], "pop": int(t.get("population", "0") or 0) if str(t.get("population", "0")).isdigit() else 0}
    if name and (t.get("leisure") == "marina" or t.get("harbour") == "yes" or t.get("landuse") in ("port", "harbour")):
        return "harbour", {"name": name}
    if name and t.get("natural") in ("bay", "strait", "cape"):
        return "waterlabel", {"name": name}
    return None, None


def seamark_info(t, sm):
    info = {"type": sm, "name": t.get("seamark:name") or t.get("name") or ""}
    for key in ("buoy_lateral", "beacon_lateral", "buoy_cardinal", "beacon_cardinal", "buoy_special_purpose", "beacon_special_purpose",
                "buoy_isolated_danger", "beacon_isolated_danger", "buoy_safe_water", "beacon_safe_water"):
        c = t.get("seamark:%s:colour" % key)
        if c:
            info["colour"] = c.split(";")[0]
            info["colours"] = c
        cat = t.get("seamark:%s:category" % key)
        if cat:
            info["category"] = cat
    # Light, when there is one: "Fl.R.5s" style text
    ch = t.get("seamark:light:character") or t.get("seamark:light:1:character")
    if ch or sm in ("light_major", "light_minor"):
        col = t.get("seamark:light:colour") or t.get("seamark:light:1:colour") or ""
        per = t.get("seamark:light:period") or t.get("seamark:light:1:period") or ""
        grp = t.get("seamark:light:group") or ""
        txt = (ch or "")
        if grp:
            txt += "(%s)" % grp
        if col:
            txt += "." + {"red": "R", "green": "G", "white": "W", "yellow": "Y"}.get(col.split(";")[0], "")
        if per:
            txt += ".%ss" % per
        info["light"] = txt.strip(".")
        info["light_colour"] = col.split(";")[0] if col else "white"
    return info


def cmd_extract(args):
    import osmium, shapely.wkb, shapefile

    S, W, N, E = [float(v) for v in args.bbox.split(",")]
    cache = {"bbox": (S, W, N, E), "areas": defaultdict(list), "lines": defaultdict(list), "points": [], "land": []}
    # Data is kept a little beyond the chart area, so that the tiles on its edge are complete
    S, W, N, E = S - 0.06, W - 0.08, N + 0.06, E + 0.08
    biggest = []  # largest wetland polygons, printed at the end as a check of how the area is mapped

    def inside(lon0, lat0, lon1, lat1):
        return not (lon1 < W or lon0 > E or lat1 < S or lat0 > N)

    def ring_bbox(ring):
        xs = [p[0] for p in ring]
        ys = [p[1] for p in ring]
        return min(xs), min(ys), max(xs), max(ys)

    wkb = osmium.geom.WKBFactory()

    class Handler(osmium.SimpleHandler):
        def __init__(self):
            super().__init__()
            self.n = 0

        def node(self, n):
            if not n.tags or not (W <= n.location.lon <= E and S <= n.location.lat <= N):
                return
            kind, info = classify_point(dict(n.tags))
            if kind:
                cache["points"].append((kind, n.location.lon, n.location.lat, info))

        def way(self, w):
            if not w.tags:
                return
            t = dict(w.tags)
            cls = classify_line(t)
            if not cls:
                return
            try:
                pts = [(n.lon, n.lat) for n in w.nodes]
            except osmium.InvalidLocationError:
                return
            if len(pts) < 2:
                return
            bb = ring_bbox(pts)
            if inside(*bb):
                cache["lines"][cls].append((bb, pts, t.get("name", "") if cls in ("river", "fairway") else ""))

        def area(self, a):
            t = dict(a.tags)
            cls = classify_area(t)
            kind, info = classify_point(t)
            if not cls and not kind:
                return
            try:
                geom = shapely.wkb.loads(wkb.create_multipolygon(a), hex=True)
            except Exception:
                return
            bb = geom.bounds
            if not inside(*bb):
                return
            if cls in ("marsh", "shallow"):
                biggest.append(((bb[2] - bb[0]) * (bb[3] - bb[1]), cls, t.get("wetland", "-"), t.get("name", "-")))
                biggest.sort(reverse=True)
                del biggest[6:]
            if cls:
                for poly in geom.geoms:
                    rings = [list(poly.exterior.coords)] + [list(r.coords) for r in poly.interiors]
                    cache["areas"][cls].append((poly.bounds, rings))
            if kind:
                c = geom.representative_point()
                if W <= c.x <= E and S <= c.y <= N:
                    cache["points"].append((kind, c.x, c.y, info))
            self.n += 1

    for pbf in args.pbf:
        print("Reading", pbf, "...", flush=True)
        t0 = time.time()
        h = Handler()
        h.apply_file(pbf, locations=True, idx="flex_mem")
        print("  done in %.0f s" % (time.time() - t0), flush=True)

    print("Reading land polygons ...", flush=True)
    sf = shapefile.Reader(args.land)
    # Land is kept well beyond the chart area: at low zoom the edge tiles reach far outside it
    for shp in sf.iterShapes(bbox=[W - 3, S - 2, E + 3, N + 2]):
        parts = list(shp.parts) + [len(shp.points)]
        rings = [shp.points[parts[k]:parts[k + 1]] for k in range(len(parts) - 1)]
        cache["land"].append((tuple(shp.bbox), [[(p[0], p[1]) for p in r] for r in rings]))

    cache["areas"] = dict(cache["areas"])
    cache["lines"] = dict(cache["lines"])
    with open(args.cache, "wb") as f:
        pickle.dump(cache, f, protocol=pickle.HIGHEST_PROTOCOL)
    print("Cache written:", args.cache)
    print("  land polygons: %d" % len(cache["land"]))
    for k, v in cache["areas"].items():
        print("  areas %-8s %d" % (k, len(v)))
    for k, v in cache["lines"].items():
        print("  lines %-8s %d" % (k, len(v)))
    for size, cls, sub, name in biggest:
        print("  big wetland: %s wetland=%s name=%s (%.3f sq deg)" % (cls, sub, name, size))
    kinds = defaultdict(int)
    for p in cache["points"]:
        kinds[p[0]] += 1
    for k, v in kinds.items():
        print("  points %-10s %d" % (k, v))


# ============================================
# Render
# ============================================
INDEX_Z = 12  # features are indexed by the zoom-12 tiles they touch


class Chart:
    def __init__(self, cache_file):
        with open(cache_file, "rb") as f:
            self.data = pickle.load(f)
        self.bbox = self.data["bbox"]
        self.index = {}
        self._build("land", self.data["land"])
        for cls, items in self.data["areas"].items():
            self._build("a:" + cls, items)
        for cls, items in self.data["lines"].items():
            self._build("l:" + cls, items)
        self.pindex = defaultdict(list)
        for p in self.data["points"]:
            self.pindex[tile_of(p[2], p[1], INDEX_Z)].append(p)
        self._labels = {}

    def _build(self, key, items):
        idx = defaultdict(list)
        for it in items:
            w, s, e, n = it[0]
            x0, y0 = tile_of(n, w, INDEX_Z)
            x1, y1 = tile_of(s, e, INDEX_Z)
            for x in range(x0, x1 + 1):
                for y in range(y0, y1 + 1):
                    idx[(x, y)].append(it)
        self.index[key] = idx

    def query(self, key, w, s, e, n):
        """Features of one class touching a lon/lat box, without duplicates."""
        idx = self.index.get(key)
        if not idx:
            return []
        x0, y0 = tile_of(n, w, INDEX_Z)
        x1, y1 = tile_of(s, e, INDEX_Z)
        seen, out = set(), []
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                for it in idx.get((x, y), ()):
                    if id(it) not in seen:
                        bw, bs, be, bn = it[0]
                        if not (be < w or bw > e or bn < s or bs > n):
                            seen.add(id(it))
                            out.append(it)
        return out

    def points(self, w, s, e, n):
        x0, y0 = tile_of(n, w, INDEX_Z)
        x1, y1 = tile_of(s, e, INDEX_Z)
        out = []
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                for p in self.pindex.get((x, y), ()):
                    if w <= p[1] <= e and s <= p[2] <= n:
                        out.append(p)
        return out

    # ---- Labels are chosen once per zoom for the whole chart, so neighbouring tiles agree on them ----
    def labels(self, z):
        if z in self._labels:
            return self._labels[z]
        cand = []
        for kind, lon, lat, info in self.data["points"]:
            if kind == "place":
                rank = info["rank"]
                minz = {1: 6, 2: 9, 3: 10, 4: 11, 5: 13, 6: 14, 7: 15}[rank]
                if z < minz:
                    continue
                size = {1: 15, 2: 13, 3: 12, 4: 11, 5: 10, 6: 10, 7: 9}[rank] + (2 if z >= 14 else 0)
                cand.append((rank * 10 - min(info["pop"], 900000) / 100000.0, lon, lat, info["name"], size, "bold" if rank <= 2 else "regular", TEXT, 0))
            elif kind == "harbour" and z >= 12:
                cand.append((35, lon, lat, info["name"], 10 + (1 if z >= 14 else 0), "italic", TEXT_WATER, 0))
            elif kind == "waterlabel" and z >= 11:
                cand.append((45, lon, lat, info["name"], 11, "italic", TEXT_WATER, 0))
            elif kind == "seamark" and z >= 13 and (info.get("light") or z >= 16):
                txt = info.get("light") or ""
                name = info.get("name") or ""
                if z >= 16 and name and len(name) <= 18:
                    txt = (name + " " + txt).strip()
                if txt:
                    cand.append((60 if info.get("light") else 70, lon, lat, txt, 9, "regular", LIGHT if info.get("light") else TEXT, 10))
        cand.sort(key=lambda c: c[0])
        placed, grid = [], defaultdict(list)
        cell = 128
        for pr, lon, lat, text, size, style, colour, dy in cand:
            x, y = lon_to_x(lon, z), lat_to_y(lat, z)
            f = font(size * SS, style)
            l, t, r, b = f.getbbox(text)
            w, h = (r - l) / SS + 6, (b - t) / SS + 4
            box = (x - w / 2, y + dy - h / 2, x + w / 2, y + dy + h / 2) if dy == 0 else (x + 7, y - h / 2, x + 7 + w, y + h / 2)
            clash = False
            for gx in range(int(box[0] // cell), int(box[2] // cell) + 1):
                for gy in range(int(box[1] // cell), int(box[3] // cell) + 1):
                    for o in grid[(gx, gy)]:
                        if not (box[2] < o[0] or box[0] > o[2] or box[3] < o[1] or box[1] > o[3]):
                            clash = True
                            break
                    if clash:
                        break
                if clash:
                    break
            if clash:
                continue
            for gx in range(int(box[0] // cell), int(box[2] // cell) + 1):
                for gy in range(int(box[1] // cell), int(box[3] // cell) + 1):
                    grid[(gx, gy)].append(box)
            placed.append((box, text, size, style, colour))
        lab = defaultdict(list)
        for p in placed:
            for gx in range(int(p[0][0] // TILE), int(p[0][2] // TILE) + 1):
                for gy in range(int(p[0][1] // TILE), int(p[0][3] // TILE) + 1):
                    lab[(gx, gy)].append(p)
        self._labels[z] = lab
        return lab


def render_block(chart, z, tx0, ty0, nx, ny):
    """Draws nx x ny tiles starting at tile (tx0, ty0). Returns an RGB image of exactly that size."""
    ox, oy = tx0 * TILE - BUFFER, ty0 * TILE - BUFFER          # world pixel of the image corner
    wpx, hpx = nx * TILE + 2 * BUFFER, ny * TILE + 2 * BUFFER
    scale = TILE * (1 << z)
    W = ox / scale * 360.0 - 180.0
    E = (ox + wpx) / scale * 360.0 - 180.0
    N = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * oy / scale))))
    S = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (oy + hpx) / scale))))

    def px(pts):
        return [((lon_to_x(lon, z) - ox) * SS, (lat_to_y(lat, z) - oy) * SS) for lon, lat in pts]

    img = Image.new("RGB", (wpx * SS, hpx * SS), SEA)
    d = ImageDraw.Draw(img)

    def fill_class(items, colour):
        """Polygons with holes, through a mask: holes show what is underneath. Returns the mask."""
        if not items:
            return None
        mask = Image.new("L", img.size, 0)
        md = ImageDraw.Draw(mask)
        for _, rings in items:
            if len(rings[0]) >= 3:
                md.polygon(px(rings[0]), fill=255)
        for _, rings in items:
            for hole in rings[1:]:
                if len(hole) >= 3:
                    md.polygon(px(hole), fill=0)
        img.paste(colour, (0, 0), mask)
        return mask

    # Land, then what lies on it
    land_mask = fill_class(chart.query("land", W, S, E, N), LAND)
    if z >= 10:
        fill_class(chart.query("a:green", W, S, E, N), GREEN)
    if z >= 9:
        fill_class(chart.query("a:urban", W, S, E, N), URBAN)
    if z >= 11:
        fill_class(chart.query("a:beach", W, S, E, N), BEACH)
    fill_class(chart.query("a:shallow", W, S, E, N), SHALLOW)
    fill_class(chart.query("a:marsh", W, S, E, N), MARSH)
    fill_class(chart.query("a:water", W, S, E, N), INLAND_WATER)

    def lines(cls, colour, width, minz, dash=None, casing=None):
        if z < minz:
            return
        items = chart.query("l:" + cls, W, S, E, N)
        if casing:
            for _, pts, _ in items:
                d.line(px(pts), fill=casing, width=int((width + 2) * SS), joint="curve")
        for _, pts, _ in items:
            p = px(pts)
            if dash:
                draw_dashed(d, p, colour, int(width * SS), dash * SS)
            else:
                d.line(p, fill=colour, width=max(1, int(width * SS)), joint="curve")

    lines("river", INLAND_WATER, 1.5 if z < 13 else 3, 9)
    lines("road3", ROAD_MINOR, 1.5 if z < 16 else 3, 15, casing=ROAD_CASING)
    lines("road2", ROAD_MINOR, 1.5 if z < 14 else 3, 12, casing=ROAD_CASING)
    lines("road1", ROAD_MAJOR, 1.5 if z < 12 else 3, 8, casing=ROAD_CASING if z >= 12 else None)
    lines("rail", RAIL, 1, 11, dash=6)

    # Coastline: the edge of the land mask. (The land polygons come cut into squares, so drawing
    # their outlines would also draw the cuts.)
    if land_mask is not None:
        grow = 3 if z < 12 else 5
        edge = ImageChops.subtract(land_mask.filter(ImageFilter.MaxFilter(grow)), land_mask)
        img.paste(COAST, (0, 0), edge)

    # Piers and breakwaters matter more than anything else on land
    if z >= 12:
        fill_class(chart.query("a:pier", W, S, E, N), PIER)
        lines("pier", PIER, 1.5 if z < 15 else 3, 12)
    if z <= 13:
        lines("ferry", FERRY, 1, 11, dash=5)  # closer in, the many local lines only clutter the water
    lines("fairway", FAIRWAY, 1, 12, dash=8)

    # Seamarks
    if z >= 11:
        for kind, lon, lat, info in chart.points(W, S, E, N):
            if kind == "seamark" and seamark_min_zoom(info) <= z:
                x, y = (lon_to_x(lon, z) - ox) * SS, (lat_to_y(lat, z) - oy) * SS
                draw_seamark(d, x, y, info, z)

    # Labels
    seen = set()
    lab = chart.labels(z)
    for gx in range((ox) // TILE, (ox + wpx) // TILE + 1):
        for gy in range((oy) // TILE, (oy + hpx) // TILE + 1):
            for box, text, size, style, colour in lab.get((gx, gy), ()):
                if id(box) in seen:
                    continue
                seen.add(id(box))
                d.text(((box[0] + 3 - ox) * SS, (box[1] + 1 - oy) * SS), text, font=font(size * SS, style), fill=colour,
                       stroke_width=SS, stroke_fill=HALO)

    img = img.crop((BUFFER * SS, BUFFER * SS, (wpx - BUFFER) * SS, (hpx - BUFFER) * SS))
    return img.resize((nx * TILE, ny * TILE), Image.LANCZOS)


def draw_dashed(d, pts, colour, width, dash):
    for i in range(1, len(pts)):
        (x0, y0), (x1, y1) = pts[i - 1], pts[i]
        length = math.hypot(x1 - x0, y1 - y0)
        if length < 1:
            continue
        n = max(1, int(length / (dash * 2)))
        for k in range(n):
            a, b = k / n, (k + 0.5) / n
            d.line([(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a), (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)], fill=colour, width=max(1, width))


def seamark_min_zoom(info):
    """Lights and the buoys that mark dangers show early; the thousands of channel posts only close up."""
    sm = info["type"]
    if sm == "light_major":
        return 11
    if sm == "light_minor" or "cardinal" in sm or "isolated_danger" in sm or "safe_water" in sm:
        return 12
    if sm.startswith("buoy") or sm in ("wreck", "rock", "obstruction") or info.get("light"):
        return 13
    return 14  # lateral and special beacons, piles, moorings, harbour marks


def draw_seamark(d, x, y, info, z):
    """Simple symbols: buoys are round, beacons are upright posts, lights have a magenta flare."""
    sm = info["type"]
    r = (3 if z < 14 else 4.5 if z < 16 else 6) * SS
    col = SEAMARK_COLOURS.get(info.get("colour", ""), None)
    edge = (20, 20, 20)
    if sm.startswith("light") or (info.get("light") and not sm.startswith(("buoy", "beacon"))):
        d.ellipse([x - r * 0.5, y - r * 0.5, x + r * 0.5, y + r * 0.5], fill=edge)
    elif sm.startswith("buoy"):
        if "cardinal" in sm:
            col = SEAMARK_COLOURS["yellow"]
        elif "isolated_danger" in sm:
            col = col or SEAMARK_COLOURS["black"]
        elif "safe_water" in sm:
            col = col or SEAMARK_COLOURS["red"]
        elif "special" in sm:
            col = col or SEAMARK_COLOURS["yellow"]
        d.ellipse([x - r, y - r, x + r, y + r], fill=col or (150, 150, 150), outline=edge, width=SS)
        if "cardinal" in sm:
            d.rectangle([x - r, y - r * 0.2, x + r, y + r * 0.4], fill=SEAMARK_COLOURS["black"])
    elif sm.startswith("beacon") or sm in ("pile", "mooring"):
        if "cardinal" in sm:
            col = SEAMARK_COLOURS["yellow"]
        elif "special" in sm:
            col = col or SEAMARK_COLOURS["yellow"]
        col = col or (110, 85, 60)
        if z < 15:
            # Channel posts stand a few metres apart: at this scale a dot each, so they read as a dotted line
            d.ellipse([x - SS, y - SS, x + SS, y + SS], fill=col)
        else:
            b = (1.5 if z < 16 else 2.5) * SS
            d.rectangle([x - b, y - b * 2.4, x + b, y + b], fill=col, outline=edge, width=1)
    elif sm in ("rock", "wreck", "obstruction"):
        d.line([(x - r, y), (x + r, y)], fill=edge, width=SS)
        d.line([(x, y - r), (x, y + r)], fill=edge, width=SS)
        if sm == "wreck":
            d.line([(x - r * 0.6, y - r * 0.6), (x + r * 0.6, y + r * 0.6)], fill=edge, width=SS)
    elif sm in ("harbour", "anchorage", "landmark", "small_craft_facility"):
        d.ellipse([x - r * 0.6, y - r * 0.6, x + r * 0.6, y + r * 0.6], outline=FERRY, width=SS)
    else:
        return
    if info.get("light"):
        lc = SEAMARK_COLOURS.get(info.get("light_colour", ""), LIGHT)
        if lc == SEAMARK_COLOURS["white"]:
            lc = LIGHT
        # Flare: a teardrop pointing down-right
        d.polygon([(x + r * 0.3, y + r * 0.3), (x + r * 2.6, y + r * 1.2), (x + r * 1.2, y + r * 2.6)], fill=lc)


def is_plain(tile_img, colour):
    """True when a tile is one flat colour (open sea, or empty land)."""
    ex = tile_img.getextrema()
    return all(abs(e[0] - c) <= 2 and abs(e[1] - c) <= 2 for e, c in zip(ex, colour))


def coastal_tiles(chart, zc):
    """Zoom-zc tiles that hold both sea and land, or seamarks, plus their neighbours."""
    S, W, N, E = chart.bbox
    x0, y0 = tile_of(N, W, zc)
    x1, y1 = tile_of(S, E, zc)
    marked = set()
    step = 16
    for bx in range(x0, x1 + 1, step):
        for by in range(y0, y1 + 1, step):
            nx, ny = min(step, x1 - bx + 1), min(step, y1 - by + 1)
            # A small land/sea picture of the block: 16 pixels per tile is enough to tell mixed from plain
            img = render_mask(chart, zc, bx, by, nx, ny, 16)
            for i in range(nx):
                for j in range(ny):
                    lo, hi = img.crop((i * 16, j * 16, i * 16 + 16, j * 16 + 16)).getextrema()
                    if lo != hi:
                        marked.add((bx + i, by + j))
    for kind, lon, lat, info in chart.data["points"]:
        if kind == "seamark":
            marked.add(tile_of(lat, lon, zc))
    out = set()
    for (x, y) in marked:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if x0 <= x + dx <= x1 and y0 <= y + dy <= y1:
                    out.add((x + dx, y + dy))
    return out


def render_mask(chart, z, tx0, ty0, nx, ny, res):
    """Land = 255, sea = 0, at 'res' pixels per tile."""
    scale = TILE * (1 << z)
    k = res / TILE
    ox, oy = tx0 * TILE, ty0 * TILE
    W = ox / scale * 360.0 - 180.0
    E = (ox + nx * TILE) / scale * 360.0 - 180.0
    N = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * oy / scale))))
    S = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (oy + ny * TILE) / scale))))
    mask = Image.new("L", (nx * res, ny * res), 0)
    md = ImageDraw.Draw(mask)
    for key, val in (("land", 255), ("a:shallow", 128), ("a:marsh", 128)):
        for _, rings in chart.query(key, W, S, E, N):
            if len(rings[0]) >= 3:
                md.polygon([((lon_to_x(lon, z) - ox) * k, (lat_to_y(lat, z) - oy) * k) for lon, lat in rings[0]], fill=val)
            for hole in rings[1:]:
                if len(hole) >= 3 and key == "land":
                    md.polygon([((lon_to_x(lon, z) - ox) * k, (lat_to_y(lat, z) - oy) * k) for lon, lat in hole], fill=0)
    return mask


def cmd_render(args):
    chart = Chart(args.cache)
    S, W, N, E = chart.bbox
    zs = args.zoom.split("-")
    zooms = list(range(int(zs[0]), int(zs[-1]) + 1))
    out = os.path.join(os.path.expanduser(args.out), "tiles")
    coast_z = args.coastal_from - 1
    coastal = None
    written = skipped_sea = 0
    t0 = time.time()

    for z in zooms:
        x0, y0 = tile_of(N, W, z)
        x1, y1 = tile_of(S, E, z)
        if z >= args.coastal_from:
            if coastal is None:
                print("Finding the coastal zone ...", flush=True)
                coastal = coastal_tiles(chart, coast_z)
                print("  %d tiles at zoom %d are near the sea" % (len(coastal), coast_z), flush=True)
            shift = z - coast_z
            blocks = sorted({(x >> 3 << 3, y >> 3 << 3) for cx, cy in coastal
                             for x in range(cx << shift, (cx + 1) << shift, META) for y in range(cy << shift, (cy + 1) << shift, META)})
            wanted = lambda x, y: (x >> shift, y >> shift) in coastal
        else:
            blocks = [(x, y) for x in range(x0 - x0 % META, x1 + 1, META) for y in range(y0 - y0 % META, y1 + 1, META)]
            wanted = lambda x, y: True
        zt = time.time()
        zcount = 0
        for bi, (bx, by) in enumerate(blocks):
            img = render_block(chart, z, bx, by, META, META)
            for i in range(META):
                for j in range(META):
                    x, y = bx + i, by + j
                    if not (x0 <= x <= x1 and y0 <= y <= y1) or not wanted(x, y):
                        continue
                    tile = img.crop((i * TILE, j * TILE, (i + 1) * TILE, (j + 1) * TILE))
                    if is_plain(tile, SEA):
                        skipped_sea += 1
                        continue
                    folder = os.path.join(out, str(z), str(x))
                    os.makedirs(folder, exist_ok=True)
                    # Baseline JPEG: the plotter decoder does not read progressive files
                    tile.save(os.path.join(folder, "%d.jpg" % y), "JPEG", quality=args.quality, progressive=False)
                    written += 1
                    zcount += 1
            if (bi + 1) % 20 == 0:
                print("  zoom %d: block %d/%d, %d tiles so far" % (z, bi + 1, len(blocks), zcount), flush=True)
        print("zoom %d: %d tiles in %.0f s" % (z, zcount, time.time() - zt), flush=True)
    print("Finished: %d tiles written, %d open-sea tiles left out, %.0f min." % (written, skipped_sea, (time.time() - t0) / 60))
    print("Copy the folder %s to the root of the SD card." % out)


def cmd_preview(args):
    chart = Chart(args.cache)
    z = int(args.zoom)
    lat, lon = [float(v) for v in args.center.split(",")]
    nx, ny = [int(v) for v in args.size.lower().split("x")]
    tx, ty = tile_of(lat, lon, z)
    img = render_block(chart, z, tx - nx // 2, ty - ny // 2, nx, ny)
    img.save(args.png)
    print("Preview written:", args.png, img.size)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract", help="read OpenStreetMap data into a cache file")
    e.add_argument("--bbox", required=True, help="south,west,north,east")
    e.add_argument("--pbf", required=True, nargs="+")
    e.add_argument("--land", required=True, help="land_polygons.shp")
    e.add_argument("--cache", required=True)
    r = sub.add_parser("render", help="draw the tiles")
    r.add_argument("--cache", required=True)
    r.add_argument("--zoom", required=True, help="for example 8-16")
    r.add_argument("--out", default="nautical-tiles")
    r.add_argument("--coastal-from", type=int, default=14, help="from this zoom only tiles near the sea are drawn (default 14)")
    r.add_argument("--quality", type=int, default=82)
    p = sub.add_parser("preview", help="draw a few tiles into one PNG, to check the style")
    p.add_argument("--cache", required=True)
    p.add_argument("--zoom", required=True)
    p.add_argument("--center", required=True, help="lat,lon")
    p.add_argument("--size", default="3x3", help="tiles, for example 3x3")
    p.add_argument("--png", required=True)
    args = ap.parse_args()
    {"extract": cmd_extract, "render": cmd_render, "preview": cmd_preview}[args.cmd](args)


if __name__ == "__main__":
    main()
