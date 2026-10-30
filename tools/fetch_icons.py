#!/usr/bin/env python3
"""Download item icons from the public Deadlock API as ready-made templates.

The scoreboard shows passive items with exactly the shop artwork the API serves (tested on a
real 1080p screenshot: 8 of 8 passive icons matched at 0.95-0.98 with the tier corner masked,
second-best item at most 0.65). They are saved as assets/templates/<item id>.png at their
original 200 px; the helper resizes them to item_icon_size.

Active items are drawn differently on the scoreboard (round, dark, with a localized "ACTIVE"
label), so their shop art does not match. They go to assets/icons instead, for the panel's
pictures, and are listed at the end: to recognise them, crop those from debug_roi.png (TAB+F9) as
<item id>@active.png.

Usage:
    python tools/fetch_icons.py                  # passive items, keep files that already exist
    python tools/fetch_icons.py --force          # re-download everything (e.g. after a patch)
    python tools/fetch_icons.py --include-active # also use active items' shop art as templates
"""

import argparse
import concurrent.futures
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

ITEMS_URL = "https://api.deadlock-api.com/v1/assets/items/by-type/upgrade"
ROOT = Path(__file__).resolve().parent.parent


def fetch(url, timeout=60):
    request = urllib.request.Request(url, headers={"User-Agent": "deadlock-build-helper"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "assets" / "templates")
    parser.add_argument("--force", action="store_true", help="overwrite icons that already exist")
    parser.add_argument("--include-active", action="store_true", help="also download active items")
    args = parser.parse_args()

    try:
        items = json.loads(fetch(ITEMS_URL))
    except (urllib.error.URLError, json.JSONDecodeError) as e:
        print(f"cannot load the item list from {ITEMS_URL}: {e}", file=sys.stderr)
        return 1

    shop = [i for i in items if i.get("shopable") and i.get("shop_image")]
    active = sorted((i for i in shop if i.get("is_active_item")), key=lambda i: i["name"])
    args.out.mkdir(parents=True, exist_ok=True)
    icons = args.out.parent / "icons"
    icons.mkdir(exist_ok=True)

    def folder(item):
        return icons if item.get("is_active_item") and not args.include_active else args.out

    def download(item):
        target = folder(item) / (item["class_name"].removeprefix("upgrade_") + ".png")
        if target.exists() and not args.force:
            return "kept"
        try:
            data = fetch(item["shop_image"], timeout=30)
        except urllib.error.URLError as e:
            return f"failed: {item['name']}: {e}"
        if not data.startswith(b"\x89PNG"):
            return f"failed: {item['name']}: not a PNG"
        target.write_bytes(data)
        return "saved"

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(download, shop))

    for r in results:
        if r.startswith("failed"):
            print(r, file=sys.stderr)
    print(f"{results.count('saved')} saved, {results.count('kept')} already there, "
          f"{sum(r.startswith('failed') for r in results)} failed -> {args.out}, {icons}")
    if not args.include_active:
        print(f"\n{len(active)} active items need a crop of their own as <id>@active.png:")
        for i in active:
            print(f"  {i['class_name'].removeprefix('upgrade_') + '@active.png':40} {i['name']}")
    return 0 if not any(r.startswith("failed") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
