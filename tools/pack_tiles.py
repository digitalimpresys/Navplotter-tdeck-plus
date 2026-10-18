#!/usr/bin/env python3
"""Joins the tiles of several regions and packs them into zip files for the SD card.

Usage:
  python pack_tiles.py --out packs --name carte-adriatico region1/tiles region2/tiles ...

Regions drawn one by one overlap on their edges, mostly at the low zooms where one tile covers a lot of sea:
where two regions wrote the same tile, the larger file is kept (it is the one with more detail, the other was
drawn without the data beyond its own area).

Each zip holds tiles/<z>/<x>/<y>.jpg and stays under --max-mb, so that it can be published as a release file;
unzipping all of them in the root of the SD card gives the complete tiles folder.
"""
import argparse, os, zipfile
from collections import Counter


def tiles_in(root):
    for z in os.listdir(root):
        if not z.isdigit():
            continue
        for x in os.listdir(os.path.join(root, z)):
            for f in os.listdir(os.path.join(root, z, x)):
                if f.endswith(".jpg"):
                    yield int(z), int(x), int(f[:-4])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("regions", nargs="+", help="tiles folders of the regions")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="carte")
    ap.add_argument("--max-mb", type=int, default=1900)
    args = ap.parse_args()

    best = {}  # (z, x, y) -> (size, path)
    per_region = Counter()
    for root in args.regions:
        for z, x, y in tiles_in(root):
            p = os.path.join(root, str(z), str(x), "%d.jpg" % y)
            size = os.path.getsize(p)
            per_region[root] += 1
            k = (z, x, y)
            if k not in best or size > best[k][0]:
                best[k] = (size, p)
    for root in args.regions:
        print("%s: %d tiles" % (root, per_region[root]))

    os.makedirs(args.out, exist_ok=True)
    limit = args.max_mb * 1024 * 1024
    part, size, zf = 0, 0, None
    zooms, total = Counter(), 0
    for k in sorted(best):
        s, p = best[k]
        if zf is None or size + s > limit:
            if zf:
                zf.close()
            part += 1
            size = 0
            zf = zipfile.ZipFile(os.path.join(args.out, "%s-parte%d.zip" % (args.name, part)), "w", zipfile.ZIP_STORED)
        z, x, y = k
        zf.write(p, "tiles/%d/%d/%d.jpg" % (z, x, y))
        size += s + 100
        zooms[z] += 1
        total += s
    if zf:
        zf.close()
    print("Tiles: %d, %.0f MB, in %d zip files" % (len(best), total / 1e6, part))
    for z in sorted(zooms):
        print("  zoom %d: %d" % (z, zooms[z]))


if __name__ == "__main__":
    main()
