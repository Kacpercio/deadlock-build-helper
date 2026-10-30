#!/usr/bin/env python3
"""Download hero card art from the public Deadlock API as hero portrait templates.

The scoreboard draws exactly this art (the API's icon_hero_card, 280x380 with a transparent
background), so no hand-made crops are needed. They are saved as assets/heroes/<hero id>.png with
the ids counters.json uses (abrams.png, lady_geist.png, ...).

A .png there without "@" in its name that is not a current hero id is deleted: it would only be
ignored by the helper. Your own variants (haze@skin2.png) are kept.

Usage:
    python tools/fetch_heroes.py          # keep files that already exist
    python tools/fetch_heroes.py --force  # re-download everything (e.g. after a patch)
"""

import argparse
import concurrent.futures
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

HEROES_URL = "https://api.deadlock-api.com/v1/assets/heroes?only_active=true"
ROOT = Path(__file__).resolve().parent.parent


def fetch(url, timeout=60):
    request = urllib.request.Request(url, headers={"User-Agent": "deadlock-build-helper"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def hero_slug(name):  # same ids as tools/build_counters.py
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "assets" / "heroes")
    parser.add_argument("--force", action="store_true", help="overwrite portraits that already exist")
    args = parser.parse_args()

    try:
        heroes = json.loads(fetch(HEROES_URL))
    except (urllib.error.URLError, json.JSONDecodeError) as e:
        print(f"cannot load the hero list from {HEROES_URL}: {e}", file=sys.stderr)
        return 1

    wanted = {hero_slug(h["name"]): h["images"].get("icon_hero_card") for h in heroes}
    args.out.mkdir(parents=True, exist_ok=True)

    stale = [p for p in args.out.glob("*.png") if "@" not in p.stem and p.stem not in wanted]
    for path in stale:
        path.unlink()

    def download(entry):
        hero_id, url = entry
        target = args.out / f"{hero_id}.png"
        if target.exists() and not args.force:
            return "kept"
        if not url:
            return f"failed: {hero_id}: no card art in the API"
        try:
            data = fetch(url, timeout=30)
        except urllib.error.URLError as e:
            return f"failed: {hero_id}: {e}"
        if not data.startswith(b"\x89PNG"):
            return f"failed: {hero_id}: not a PNG"
        target.write_bytes(data)
        return "saved"

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(download, wanted.items()))

    for r in results:
        if r.startswith("failed"):
            print(r, file=sys.stderr)
    print(f"{results.count('saved')} saved, {results.count('kept')} already there, {len(stale)} old files removed, "
          f"{sum(r.startswith('failed') for r in results)} failed -> {args.out}")
    return 0 if not any(r.startswith("failed") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
