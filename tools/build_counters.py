#!/usr/bin/env python3
"""Generate assets/counters.json from the current Deadlock data.

Three layers end up in the file:

  items       enemy item -> counter items + a tactical tip. Item names, slots and tiers
              come from the public Deadlock API, the threat -> counter rules below are
              hand-written from the items' in-game descriptions.
  archetypes  counters for a whole gun / spirit / vitality build.
  heroes      enemy hero -> items that win measurably more often against that hero,
              computed from the API's match statistics (last 30 days by default).

If a patch removes or renames an item a rule refers to, the script stops and lists those
ids so the rules can be updated.

Usage:
    python tools/build_counters.py                 # fetch everything from the API
    python tools/build_counters.py --no-stats      # skip the match statistics (no hero counters)
    python tools/build_counters.py --items items.json --heroes heroes.json --stats-dir stats/
                                                   # use saved API responses
"""

import argparse
import json
import math
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import date
from pathlib import Path

API = "https://api.deadlock-api.com"
ITEMS_URL = f"{API}/v1/assets/items/by-type/upgrade"
HEROES_URL = f"{API}/v1/assets/heroes?only_active=true"
ITEM_STATS_URL = f"{API}/v1/analytics/item-stats"
ROOT = Path(__file__).resolve().parent.parent

SLOT_NAMES = {"weapon": "Weapon", "vitality": "Vitality", "spirit": "Spirit"}

# Priority of a counter within a threat: 3 = first pick, 2 = good, 1 = situational.
# Enemy item weight: how much one copy of it should push the suggestions (1-3).
THREATS = {
    "healing": {
        "items": {
            "vampire": 1, "health_stealing_magic": 1, "damage_recycler": 2, "surging_power": 2,
            "health_stimpak": 1, "health_nova": 2, "rescue_beam": 2, "healing_booster": 1,
            "healbuff": 2, "infuser": 2, "resonant_healing": 1, "mystic_regeneration": 1,
            "boxing_glove": 1, "lifestrike_gauntlets": 1, "fury_trance": 1, "tech_overflow": 1,
            "patrons_blessing": 1, "celestial_guidance": 2, "chonky": 1, "active_reload": 1,
            "fervor": 1, "mystic_reverb": 1,
        },
        "counters": [
            ("toxic_bullets", 3, "Bullets build a bleed that also cuts their incoming healing by 35%."),
            ("healbane", 3, "Your spirit damage cuts their incoming healing by 35%."),
            ("rupture", 2, "Active damage over time and -50% incoming healing on one target."),
            ("inhibitor", 2, "Bullets build up -40% incoming healing and -30% outgoing damage."),
            ("spirit_burn", 2, "Big spirit hits ignite them: burn plus -70% incoming healing."),
            ("banshee_slugs", 1, "Headshots cut their healing by 35% and both resists by 16%."),
            ("eldritch_shot", 1, "Next bullet: -40% healing, -40% damage output and a slow."),
            ("nullification_aura", 1, "Active: strips their buffs and blocks healing in the area."),
        ],
    },
    "anti_heal": {
        "items": {
            "toxic_bullets": 1, "healbane": 1, "rupture": 1, "inhibitor": 1,
            "banshee_slugs": 1, "spirit_burn": 1, "eldritch_shot": 1,
        },
        "counters": [
            ("damage_recycler", 1, "Only if you rely on lifesteal: weakens their healing reduction."),
        ],
    },
    "gun_burst": {
        "items": {
            "glass_cannon": 2, "critshot": 2, "sharpshooter": 1, "long_range": 1,
            "hollow_point_rounds": 1, "express_shot": 1, "intensifying_clip": 1, "berserker": 1,
            "headhunter": 1, "burst_fire": 1, "ricochet": 1, "close_quarter_combat": 1,
            "split_shot": 1, "aprounds": 2, "infinite_rounds": 2, "banshee_slugs": 1,
            "active_reload": 1, "blood_tribute": 1, "fervor": 1, "enchanted_holsters": 1,
            "chain_lightning": 1, "dps_aura": 1, "pristine_emblem": 1, "headshot_booster": 1,
            "bulletshredimbue": 1, "reinforcing_casings": 1, "kinetic_sash": 1,
        },
        "counters": [
            ("improved_bullet_armor", 3, "+30% bullet resist, more when below 50% health."),
            ("metal_skin", 3, "Active: become immune to bullets for a few seconds."),
            ("weapon_shielding", 2, "Barrier after a burst of weapon damage, plus 18% bullet resist."),
            ("deflecting_armor", 2, "Chance to deflect bullets entirely or ignore their on-hit effects."),
            ("greater_withering_whip", 2, "Active: disarm the shooter and lower their bullet resist."),
            ("regenerating_bullet_shield", 1, "Cheap 18% bullet resist."),
            ("withering_whip", 1, "Active: cut the shooter's fire rate."),
            ("juggernaut", 1, "Enemies who shoot you get their fire rate slowed."),
        ],
    },
    "bullet_procs": {
        "items": {
            "toxic_bullets": 1, "inhibitor": 1, "slowing_bullets": 1, "weighted_shots": 1,
            "proc_silence": 1, "siphon_bullets": 1,
        },
        "counters": [
            ("deflecting_armor", 2, "50% chance to ignore the on-hit effects of each bullet."),
        ],
    },
    "spirit_burst": {
        "items": {
            "magic_burst": 1, "magic_shock": 2, "escalating_exposure": 2, "magic_vulnerability": 1,
            "spirit_snatch": 1, "ultimate_burst": 2, "shivas_bracelet": 2, "mystic_reverb": 2,
            "boundless_spirit": 1, "patrons_blessing": 2, "ethereal_bullets": 1, "tech_overflow": 1,
            "infuser": 1, "arctic_blast": 1, "spirit_burn": 1, "focus_lens": 1, "prism_blast": 2,
            "thermal_detonator": 1, "discord": 1, "quick_silver": 1, "crackshot": 1,
            "tech_damage_pulse": 1,
        },
        "counters": [
            ("tech_purge", 3, "+30% spirit resist, more when below 50% health."),
            ("spellbreaker", 3, "The next big spirit hit you take is reduced by 65%."),
            ("spirit_bubble", 2, "Barrier after a burst of spirit damage, plus 18% spirit resist."),
            ("absorbing_armor", 2, "22% spirit resist; heavy spirit hits lower your cooldowns."),
            ("counterspell", 2, "Parry an enemy ability to block its damage and effects."),
            ("proc_silence", 2, "Your bullets cut their spirit damage by 25% and build to a silence."),
            ("magic_shield", 1, "Cheap 18% spirit resist."),
            ("spirit_sap", 1, "Active: -30 spirit power and -9% spirit resist on the target."),
            ("self_bubble", 1, "Active: briefly invulnerable, then +30% spirit resist."),
        ],
    },
    "ability_spam": {
        "items": {
            "transcendent_cooldown": 2, "ability_refresher": 2, "ability_power_shard": 1,
            "cooldown_reduction": 1, "rapid_recharge": 1, "omnicharge_pendant": 2, "magic_tempo": 1,
            "magic_storm": 1, "extra_charge": 1, "arcane_surge": 1,
        },
        "counters": [
            ("proc_silence", 2, "Your bullets build up to a silence and cut their spirit damage."),
            ("targeted_silence", 2, "Active: silence everyone the wave hits."),
            ("focus_lens", 2, "Active: silence one target and burst them when it ends."),
            ("glitch", 2, "Active: silence, disarm and lock their items."),
            ("counterspell", 1, "Parry their key ability to block it."),
        ],
    },
    "stun": {
        "items": {
            "target_stun": 2, "mystical_piano": 3, "crushing_fists": 1, "ultimate_burst": 1,
            "unstable_concoction": 1,
        },
        "counters": [
            ("unstoppable", 3, "Active: immune to stun, silence, sleep, root and disarm. Use it before the stun lands."),
            ("auto_cleanse", 3, "Automatically cleanses the next stun, silence, sleep or root."),
            ("debuff_reducer", 2, "-25% duration of every debuff on you."),
            ("counterspell", 2, "Parry an enemy ability or item to block it."),
            ("vex_barrier", 1, "Barrier whenever you get stunned, silenced or immobilized."),
            ("cloak_of_opportunity", 1, "Blocks the next stun or movement lock and makes you Unstoppable."),
        ],
    },
    "silence": {
        "items": {
            "glitch": 3, "targeted_silence": 2, "focus_lens": 2, "proc_silence": 1,
        },
        "counters": [
            ("unstoppable", 3, "Active: immune to silence and disarm. Use it before the silence lands."),
            ("auto_cleanse", 3, "Automatically cleanses the next silence or stun."),
            ("reduce_debuff_duration", 2, "Active: purge non-ultimate debuffs and heal."),
            ("counterspell", 2, "Parry an enemy ability or item to block it."),
            ("debuff_reducer", 2, "-25% duration of every debuff on you."),
            ("vex_barrier", 1, "Barrier whenever you get silenced or stunned."),
        ],
    },
    "disarm": {
        "items": {
            "greater_withering_whip": 2, "phantom_strike": 2, "glitch": 2,
        },
        "counters": [
            ("unstoppable", 3, "Active: immune to disarm, stun and silence."),
            ("reduce_debuff_duration", 2, "Active: purge the disarm and heal."),
            ("counterspell", 2, "Parry the item to block it."),
            ("debuff_reducer", 1, "-25% duration of every debuff on you."),
        ],
    },
    "slow": {
        "items": {
            "slowing_bullets": 1, "weighted_shots": 1, "magic_slow": 1, "cold_front": 1,
            "arctic_blast": 2, "containment": 2, "aoe_root": 2, "capacitor": 2,
            "eldritch_shot": 1, "colossus": 1, "mystic_reverb": 1,
        },
        "counters": [
            ("fleetfoot_boots", 2, "40% slow resist and no move-speed penalty while shooting."),
            ("cardio_calibrator", 2, "25% slow resist plus move speed."),
            ("juggernaut", 1, "50% slow resist."),
            ("reduce_debuff_duration", 1, "Active: purge the slow and heal."),
            ("magic_carpet", 1, "Active: fly away, immune to slows while flying."),
        ],
    },
    "mobility": {
        "items": {
            "magic_carpet": 1, "warp_stone": 1, "rocket_booster": 1, "superior_stamina": 1,
            "icarus_wings": 2, "shrink_ray": 1, "electric_slippers": 1,
        },
        "counters": [
            ("containment", 3, "Active: slows them and silences their movement items and abilities."),
            ("capacitor", 2, "Active: strong slow, no stamina, movement items silenced."),
            ("aoe_root", 2, "Active: pulls them in and applies Slowing Hex."),
            ("slowing_bullets", 1, "Bullets build up a movement slow."),
        ],
    },
    "melee": {
        "items": {
            "crushing_fists": 2, "runed_gauntlets": 2, "melee_charge": 1, "boxing_glove": 1,
            "spirit_snatch": 1, "acolytes_glove": 1, "shadow_strike": 2, "lifestrike_gauntlets": 1,
        },
        "counters": [
            ("melee_rebuttal", 3, "Parrying a melee heals you and returns the damage; 18% melee resist."),
            ("close_quarter_combat", 2, "30% melee resist and punishes them at close range."),
            ("close_range", 1, "Cheap 20% melee resist."),
            ("juggernaut", 1, "25% melee resist."),
            ("tech_damage_pulse", 1, "18% melee resist and damages enemies who stay close."),
        ],
    },
    "resist_shred": {
        "items": {
            "bullet_resist_shredder": 1, "headshot_booster2": 1, "tech_defense_shredders": 1,
            "spellslinger_headshots": 1, "bullet_armor_reduction_aura": 1, "magic_vulnerability": 1,
            "spirit_sap": 1, "withering_whip": 1, "greater_withering_whip": 1, "thermal_detonator": 1,
            "escalating_exposure": 1, "banshee_slugs": 1, "hollow_point_rounds": 1, "weapon_backstabber": 1,
        },
        "counters": [
            ("reduce_debuff_duration", 2, "Active: purge the resist reduction and heal."),
            ("debuff_reducer", 2, "-25% duration of every debuff on you, resist shred included."),
            ("discord", 1, "Active: +40% spirit resist and debuff resist on you or an ally."),
        ],
    },
    "debuffs": {
        "items": {
            "toxic_bullets": 1, "rupture": 1, "spirit_burn": 1, "siphon_bullets": 1, "inhibitor": 1,
            "suppressor": 1,
        },
        "counters": [
            ("reduce_debuff_duration", 2, "Active: purge bleeds and other debuffs and heal."),
            ("divine_barrier", 1, "Active: remove non-stun debuffs and gain a 600 barrier. Can target an ally."),
            ("debuff_reducer", 1, "-25% duration of every debuff on you."),
        ],
    },
    "stealth": {
        "items": {
            "cloaking_device_active": 2, "veil_walker": 1, "shadow_strike": 2,
        },
        "counters": [
            ("weapon_backstabber", 1, "Wounded enemies are revealed through walls."),
            ("thermal_detonator", 1, "Area damage; taking damage briefly reveals Shadow Weave users."),
        ],
    },
    "defensive_buffs": {
        "items": {
            "metal_skin": 1, "fury_trance": 1, "surging_power": 1, "colossus": 1, "infuser": 1,
            "divine_barrier": 1, "diviners_kevlar": 1,
        },
        "counters": [
            ("glitch", 2, "Active: removes all non-ultimate buffs and blocks their items."),
            ("nullification_aura", 2, "Active: removes positive buffs from enemies in the area."),
        ],
    },
}

# Per-item corrections where a generic counter does not work against that item.
ITEM_OVERRIDES = {
    # Unavoidable bullets ignore bullet resist (and cannot be deflected).
    "aprounds": {
        "exclude": {"improved_bullet_armor", "regenerating_bullet_shield", "deflecting_armor", "metal_skin"},
        "extra": [("inhibitor", 3, "Cuts their outgoing damage by 30%, which bullet resist cannot do against it.")],
    },
    "infinite_rounds": {
        "exclude": {"improved_bullet_armor", "regenerating_bullet_shield", "deflecting_armor", "metal_skin"},
        "extra": [("inhibitor", 3, "Cuts their outgoing damage by 30%, which bullet resist cannot do against it.")],
    },
    # Tankbuster ignores spirit resist.
    "magic_shock": {"exclude": {"tech_purge", "magic_shield", "absorbing_armor"}},
    # Runed Gauntlets pierce through parry.
    "runed_gauntlets": {"exclude": {"melee_rebuttal"}},
    # Cursed Relic also blocks item usage, so actives can't be used once it has landed.
    "glitch": {"exclude": {"reduce_debuff_duration"}},
    # The stun lands 2 s after the cast, which leaves time to purge it.
    "target_stun": {"extra": [("reduce_debuff_duration", 2, "Purge it during the 2 s delay before the stun lands.")]},
    # Leech weakens healing reduction, so the damage penalty matters more than the anti-heal.
    "damage_recycler": {"extra": [("inhibitor", 3, "Its -30% outgoing damage still works; Leech only weakens the anti-heal part.")]},
}

# Shown next to the enemy that owns the item: how to play against it, not what to buy.
TIPS = {
    "unstoppable": "Unstoppable: bait it out before you use stuns or silences.",
    "auto_cleanse": "Indomitable cleanses the first stun/silence: open with a cheap one.",
    "cloak_of_opportunity": "Cloak of Opportunity blocks the first stun or movement lock.",
    "counterspell": "Counterspell parries abilities and items: don't fire key spells into it.",
    "metal_skin": "Metal Skin = bullet immunity: switch to spirit damage or wait it out.",
    "self_bubble": "Ethereal Shift: briefly invulnerable, don't waste burst into it.",
    "prism_blast": "Prism Blast: invulnerable laser spin, back off until it ends.",
    "cheat_death": "Cheat Death: survives lethal damage for 4.5 s at reduced damage.",
    "return_fire": "Return Fire reflects part of your damage while active.",
    "melee_rebuttal": "Rebuttal: don't melee into their parry.",
    "deflecting_armor": "Plated Armor deflects some bullets and blocks on-hit effects.",
    "spellbreaker": "Spellbreaker cuts the first big spirit hit: open with a small spell.",
    "health_stimpak": "Healing Rite breaks when they take hero damage: poke them.",
    "reduce_debuff_duration": "Dispel Magic purges debuffs once (not while stunned).",
    "divine_barrier": "Divine Barrier: removes debuffs and adds a 600 barrier to an ally.",
    "celestial_guidance": "Celestial Blessing: global team cleanse and heal.",
    "cloaking_device_active": "Shadow Weave: stealth ambusher, damage briefly reveals them.",
    "veil_walker": "Veil Walker: stealth after walking through veils.",
    "shadow_strike": "Shadow Strike: invisible after stamina use, watch for a melee ambush.",
    "warp_stone": "Warp Stone: short teleport escape.",
    "magic_carpet": "Magic Carpet: escape that ignores slows.",
    "rocket_booster": "Majestic Leap: locked for 5 s after a hero hits them, so hit first.",
    "juggernaut": "Juggernaut slows the fire rate of anyone shooting them.",
    "colossus": "Colossus: big resist buff and slow aura, kite it out.",
    "unstable_concoction": "Unstable Concoction: explodes and stuns nearby when it ends, back off.",
    "mystical_piano": "Mystical Piano: delayed area stun, leave the circle.",
    "target_stun": "Knockdown stuns 2 s after the cast.",
    "aprounds": "Armor Piercer ignores bullet resist.",
    "infinite_rounds": "Infinite Rounds ignores bullet resist.",
    "magic_shock": "Tankbuster ignores spirit resist.",
    "glitch": "Cursed Relic removes your buffs and blocks item use.",
    "nullification_aura": "Nullification Burst strips buffs and stops healing in an area.",
    "damage_recycler": "Leech weakens your healing reduction.",
    "runed_gauntlets": "Runed Gauntlets pierce parry.",
    "phantom_strike": "Phantom Strike teleports onto you and disarms.",
    "aoe_root": "Vortex Web pulls targets together: spread out.",
    "focus_lens": "Focus Lens: silence that stores damage and bursts when it ends.",
    "glass_cannon": "Glass Cannon lowers their max health: focus them.",
    "icarus_wings": "Seraphim Wings: much stronger while airborne.",
    "fury_trance": "Fury Trance silences them: no abilities while it is active.",
    "electric_slippers": "Electric Slippers evade bullets while sliding.",
    "trophy_collector": "Trophy Collector scales with kills and assists: punish them early.",
    "goose_egg": "Golden Goose Egg grows over time: punish them early.",
    "eternal_gift": "Eternal Gift keeps adding stats: punish them early.",
    "non_player_bonus_sacrifice": "Cultist Sacrifice: farming a big buff, contest their jungle.",
}

# Counters for a whole build, when most of one enemy's items come from one shop slot.
ARCHETYPES = {
    "weapon": {
        "display": "Gun build",
        "counters": [
            ("improved_bullet_armor", 3, "Bullet resist against a gun-heavy enemy."),
            ("weapon_shielding", 2, "Barrier against bursts of weapon damage."),
            ("metal_skin", 2, "Bullet immunity when they commit."),
            ("regenerating_bullet_shield", 1, "Cheap early bullet resist."),
        ],
    },
    "spirit": {
        "display": "Spirit build",
        "counters": [
            ("tech_purge", 3, "Spirit resist against a spirit-heavy enemy."),
            ("spirit_bubble", 2, "Barrier against bursts of spirit damage."),
            ("spellbreaker", 2, "Reduces their biggest spell hit."),
            ("magic_shield", 1, "Cheap early spirit resist."),
        ],
    },
    "vitality": {
        "display": "Tank build",
        "counters": [
            ("toxic_bullets", 2, "Max-health bleed and anti-heal against a tank."),
            ("magic_shock", 2, "Bonus damage based on their current health."),
            ("rupture", 2, "Damage over time based on their current health."),
            ("healbane", 1, "Anti-heal against their sustain."),
        ],
    },
}

# Match-statistics thresholds for the per-hero counters.
MIN_MATCHES = 3000      # per item, in games against the hero
MIN_LIFT_PP = 0.3       # percentage points above the item's normal win rate
MAX_HERO_COUNTERS = 6
MAX_HERO_AVOID = 3


def fetch_json(url, attempts=4):
    """GET a JSON list, retrying rate limits and server errors with a growing pause."""
    request = urllib.request.Request(url, headers={"User-Agent": "deadlock-build-helper"})
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                data = json.load(response)
            if not isinstance(data, list):
                raise ValueError(f"unexpected response from {url}: {str(data)[:200]}")
            return data
        except urllib.error.HTTPError as e:
            if e.code not in (429, 500, 502, 503, 504) or attempt == attempts - 1:
                raise SystemExit(f"API error {e.code} for {url}: {e.read()[:200]!r}")
        except urllib.error.URLError as e:
            if attempt == attempts - 1:
                raise SystemExit(f"cannot reach {url}: {e.reason}")
        time.sleep(5 * (attempt + 1))


def load(path, url):
    return json.loads(Path(path).read_text(encoding="utf-8")) if path else fetch_json(url)


def item_id(item):
    return item["class_name"].removeprefix("upgrade_")


def hero_slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def check_ids(shop):
    referenced = set(TIPS)
    for rules in list(THREATS.values()) + list(ARCHETYPES.values()):
        referenced |= set(rules.get("items", ())) | {c[0] for c in rules["counters"]}
    for override in ITEM_OVERRIDES.values():
        referenced |= set(override.get("exclude", ())) | {c[0] for c in override.get("extra", ())}
    return sorted(referenced - shop.keys())


def counter_list(shop, counters):
    def label(cid):
        i = shop[cid]
        return f"{SLOT_NAMES[i['item_slot_type']]} T{i['item_tier']}"

    return [
        {"item": shop[cid]["name"], "priority": p, "reason": f"{label(cid)} | {r}"}
        for cid, (p, r) in sorted(counters.items(), key=lambda kv: -kv[1][0])
    ]


def buy_minutes(shop, stats):
    """Item id -> the match minute it is usually bought at."""
    by_numeric = {i["id"]: iid for iid, i in shop.items()}
    return {by_numeric[s["item_id"]]: round(s["avg_buy_time_s"] / 60) for s in stats
            if s["item_id"] in by_numeric and s.get("avg_buy_time_s")}


def item_entries(shop, minutes):
    entries = {}
    for iid, item in sorted(shop.items(), key=lambda kv: kv[1]["name"]):
        override = ITEM_OVERRIDES.get(iid, {})
        excluded = override.get("exclude", set())
        counters, weight, threats = {}, 1, []
        for tag, threat in THREATS.items():
            if iid not in threat["items"]:
                continue
            threats.append(tag)
            weight = max(weight, threat["items"][iid])
            for cid, priority, reason in threat["counters"]:
                if cid != iid and cid not in excluded and cid not in counters:
                    counters[cid] = (priority, reason)
        for cid, priority, reason in override.get("extra", ()):
            counters[cid] = (priority, reason)

        entry = {"display": item["name"], "slot": item["item_slot_type"], "tier": item["item_tier"]}
        if iid in minutes:
            entry["buy_minute"] = minutes[iid]
        components = [c.removeprefix("upgrade_") for c in item.get("component_items") or []]
        if any(c in shop for c in components):
            entry["components"] = [c for c in components if c in shop]
        if iid in TIPS:
            entry["tip"] = TIPS[iid]
        if counters:
            entry["threats"] = threats
            entry["weight"] = weight
            entry["counters"] = counter_list(shop, counters)
        entries[iid] = entry
    return entries


def archetype_entries(shop):
    return {
        slot: {
            "display": rules["display"],
            "weight": 2,
            "counters": counter_list(shop, {cid: (p, r) for cid, p, r in rules["counters"]}),
        }
        for slot, rules in ARCHETYPES.items()
    }


def win_rates(stats, shop_by_numeric_id):
    return {s["item_id"]: (s["wins"], s["matches"]) for s in stats
            if s["item_id"] in shop_by_numeric_id and s["matches"] > 0}


def hero_entries(heroes, shop, stats_dir, offline):
    """Items whose win rate rises measurably when the enemy team has this hero.

    lift = (win rate vs hero - overall win rate of the same item) - average shift of all items
    vs that hero. Subtracting the average shift removes "this hero is weak, so everyone facing
    it wins more"; comparing each item with itself removes "late items always look good".
    Only lifts above 2 standard errors are kept.
    """
    by_numeric = {i["id"]: iid for iid, i in shop.items()}

    def stats_for(name, url):
        if offline:
            return json.loads((stats_dir / name).read_text(encoding="utf-8"))
        data = fetch_json(url)
        if stats_dir:
            (stats_dir / name).write_text(json.dumps(data), encoding="utf-8")
        time.sleep(0.4)  # stay far below the API's 200 requests/minute
        return data

    base = win_rates(stats_for("item_stats_all.json", ITEM_STATS_URL), by_numeric)
    entries = {}
    for hero in sorted(heroes, key=lambda h: h["name"]):
        vs = win_rates(stats_for(f"item_stats_vs_{hero['id']}.json",
                                 f"{ITEM_STATS_URL}?enemy_hero_ids={hero['id']}"), by_numeric)
        rows = []
        for numeric, (wins, matches) in vs.items():
            if numeric not in base or matches < MIN_MATCHES:
                continue
            bw, bm = base[numeric]
            rows.append((by_numeric[numeric], wins / matches, bw / bm, matches))
        if not rows:
            continue
        shift = sum((w - bw) * m for _, w, bw, m in rows) / sum(m for *_, m in rows)
        scored = []
        for iid, w, bw, m in rows:
            lift = (w - bw) - shift
            se = math.sqrt(w * (1 - w) / m)
            if abs(lift) > 2 * se and abs(lift) * 100 >= MIN_LIFT_PP:
                scored.append((lift, iid, m))
        scored.sort(reverse=True)

        def reason(lift, m):
            games = f"{m / 1e6:.1f}M" if m >= 1e6 else f"{m / 1e3:.0f}k"
            return f"{lift * 100:+.1f} pp win rate vs {hero['name']} ({games} games)"

        best = [(l, iid, m) for l, iid, m in scored if l > 0][:MAX_HERO_COUNTERS]
        worst = [(l, iid, m) for l, iid, m in reversed(scored) if l < 0][:MAX_HERO_AVOID]
        entries[hero_slug(hero["name"])] = {
            "display": hero["name"],
            "hero_id": hero["id"],
            "weight": 2,
            "counters": [
                {"item": shop[iid]["name"], "priority": 3 if l >= 0.015 else 2 if l >= 0.007 else 1,
                 "reason": f"{SLOT_NAMES[shop[iid]['item_slot_type']]} T{shop[iid]['item_tier']} | {reason(l, m)}"}
                for l, iid, m in best
            ],
            "avoid": [f"{shop[iid]['name']} ({l * 100:+.1f} pp)" for l, iid, m in worst],
            "enemy_shift_pp": round(shift * 100, 2),
        }
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--items", help="saved /v1/assets/items/by-type/upgrade response")
    parser.add_argument("--heroes", help="saved /v1/assets/heroes response")
    parser.add_argument("--stats-dir", type=Path,
                        help="folder of saved item-stats responses; read when --offline, written otherwise")
    parser.add_argument("--offline", action="store_true", help="read match statistics from --stats-dir")
    parser.add_argument("--no-stats", action="store_true", help="skip per-hero counters")
    parser.add_argument("--out", default=ROOT / "assets" / "counters.json", type=Path)
    args = parser.parse_args()
    if args.offline and not args.stats_dir:
        parser.error("--offline needs --stats-dir with saved responses")

    shop = {item_id(i): i for i in load(args.items, ITEMS_URL) if i.get("shopable")}
    missing = check_ids(shop)
    if missing:
        print("These ids are not in the current shop (renamed or removed by a patch):", file=sys.stderr)
        for m in missing:
            print("  -", m, file=sys.stderr)
        return 1

    minutes = {}
    if not args.no_stats:
        if args.stats_dir:
            args.stats_dir.mkdir(parents=True, exist_ok=True)
        saved = args.stats_dir / "item_stats_all.json" if args.stats_dir else None
        stats = json.loads(saved.read_text(encoding="utf-8")) if args.offline else fetch_json(ITEM_STATS_URL)
        minutes = buy_minutes(shop, stats)

    out = {
        "_note": "Generated by tools/build_counters.py - edit the rules there, not this file.",
        "_source": API,
        "_generated": date.today().isoformat(),
        "items": item_entries(shop, minutes),
        "archetypes": archetype_entries(shop),
    }

    heroes = load(args.heroes, HEROES_URL)
    slugs = [hero_slug(h["name"]) for h in heroes]
    duplicates = {s for s in slugs if slugs.count(s) > 1}
    if duplicates:
        print(f"Hero names collide after slugging: {sorted(duplicates)}", file=sys.stderr)
        return 1
    if not args.no_stats:
        if args.stats_dir:
            args.stats_dir.mkdir(parents=True, exist_ok=True)
        out["heroes"] = hero_entries(heroes, shop, args.stats_dir, args.offline)

    args.out.write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    item_lines = [f"{iid + ('@active' if i.get('is_active_item') else ''):40} {i['name']}  "
                  f"({SLOT_NAMES[i['item_slot_type']]} T{i['item_tier']}{', ACTIVE: crop it yourself' if i.get('is_active_item') else ''})"
                  for iid, i in sorted(shop.items(), key=lambda kv: (kv[1]["item_slot_type"], kv[1]["item_tier"], kv[1]["name"]))]
    (ROOT / "assets" / "templates" / "ITEM_IDS.txt").write_text(
        "Item template file name (without .png) -> item.\n"
        "Passive items: tools/fetch_icons.py downloads them. Active items look different on the\n"
        "scoreboard: crop them from debug_roi.png (TAB+F9) under the @active name.\n\n" + "\n".join(item_lines) + "\n", encoding="utf-8")
    hero_dir = ROOT / "assets" / "heroes"
    hero_dir.mkdir(exist_ok=True)
    hero_lines = [f"{hero_slug(h['name']):20} {h['name']}" for h in sorted(heroes, key=lambda h: h["name"])]
    (hero_dir / "HERO_IDS.txt").write_text(
        "Hero portrait template file name (without .png) -> hero\n\n" + "\n".join(hero_lines) + "\n", encoding="utf-8")

    items = out["items"]
    print(f"{len(items)} items: {sum('counters' in e for e in items.values())} with counters, "
          f"{sum('tip' in e for e in items.values())} with tips")
    if "heroes" in out:
        print(f"{len(out['heroes'])} heroes with match-data counters")
    print(f"-> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
