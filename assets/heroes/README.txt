Hero portrait templates: one PNG per hero, file name = hero id (see HERO_IDS.txt).

Download them all with:

    python tools/fetch_heroes.py

That saves the API's hero card art (280x380) as abrams.png, lady_geist.png, ... The helper tries it
at every scale the scoreboard may draw it. With no templates here, hero recognition does nothing
and the panel tells you to run the script.

If a hero is not recognised (a skin, say), add a variant next to it: haze@skin2.png also reports
"haze". It can be a crop from debug_roi.png (TAB+F9): the top-left of the portrait, at most 80x72 px
at 1080p, as the enemy side shows it. Hold TAB and press F9 to log each portrait's score.
