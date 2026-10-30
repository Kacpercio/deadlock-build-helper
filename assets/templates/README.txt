Item templates: one PNG per item, file name = item id (see ITEM_IDS.txt).

  python tools/fetch_icons.py      downloads all 123 PASSIVE items (shop art, 200 px)
  <id>@active.png                  crop ACTIVE items yourself from debug_roi.png (TAB+F9):
                                   the scoreboard draws them round with an "ACTIVE" label
  <id>@anything.png                more variants of the same item (text after @ is ignored)

Every template is resized to item_icon_size (27 px at 1080p), so any size works. Transparent
pixels are ignored, and so is the top-right tier badge (item_corner_mask).
