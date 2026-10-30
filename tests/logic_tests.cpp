// Checks of the platform-independent logic: config parsing, counter ranking
// and the per-player analysis. Vision is covered by vision_tests.cpp; capture and overlay need Windows.
#include "Config.h"
#include "CounterDatabase.h"
#include "PlayerAnalysis.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++checks;                                                         \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static std::filesystem::path writeTemp(const std::string& name, const std::string& text)
{
    const auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream(p) << text;
    return p;
}

static Detection item(const std::string& id, int slot) { return {id, {}, 0.9, slot, false}; }
static Detection hero(const std::string& id, int slot) { return {id, {}, 0.9, slot, true}; }

static const Suggestion* find(const std::vector<Suggestion>& s, const std::string& name)
{
    const auto it = std::ranges::find(s, name, &Suggestion::item);
    return it != s.end() ? &*it : nullptr;
}

int main(int, char** argv)
{
    const std::filesystem::path assets = argv[1];

    // ---- Config -------------------------------------------------------------------------
    {
        const ConfigLoad l = loadConfig(assets / "config.json");
        CHECK(l.error.empty());
        CHECK(l.config.hotkeys.scoreboard == 0x09);
        CHECK(l.config.hotkeys.calibrate == 0x76);
        CHECK(l.config.hotkeys.reload == 0x77);
        CHECK(l.config.hotkeys.debugDump == 0x78);
        CHECK(l.config.hotkeys.tuneScale == 0x75);
        CHECK(near(l.config.iconScale, 1.0));
        // Column layout measured on a real 1080p scoreboard: enemies step right, same height.
        CHECK(l.config.enemyRows.size() == 6);
        CHECK(near(l.config.enemyRows[5].x, 0.5555 + 5 * 0.04586));
        CHECK(near(l.config.enemyRows[5].y, 0.2741));
        CHECK(l.config.captureDelayMs == 120 && l.config.rescanDelayMs == 300);
        CHECK(l.config.panelHoldMs == 5000 && l.config.reminderMinutes == 3);
        CHECK(l.config.allyNames.size() == 6);
        CHECK(near(l.config.allyNames[1].x, 0.4021 - 0.04586));
        CHECK(l.config.hotkeys.menu == 0x79);
        CHECK(l.config.enemyPortraits.size() == 6);
        CHECK(near(l.config.enemyPortraits[2].x, 0.5555 + 2 * 0.04586));
        CHECK(near(l.config.enemyPortraits[2].y, 0.0));
        CHECK(near(l.config.enemyPortraits[0].w, 0.0417));
        CHECK(near(l.config.downscale, 1.0));
        CHECK(near(l.config.matchThreshold, 0.90));
        CHECK(l.config.itemIconSize == 27);
        CHECK(near(l.config.itemCornerMask, 0.5));
        CHECK(near(l.config.heroThreshold, 0.80));
        CHECK(near(l.config.heroMargin, 0.25));
        CHECK(l.config.scanThreads == 2);
        CHECK(l.config.hotkeys.pinPanel == 0x74);
        CHECK(near(l.config.archetypeShare, 0.6));
        CHECK(l.config.archetypeMinItems == 3);
        CHECK(l.config.heroTemplateDir == "assets/heroes");
    }
    {
        const auto p = writeTemp("dbh_keys.json",
            R"({"hotkeys":{"scoreboard":"mouse4","calibrate":"f12","reload":"numpad5","debug_dump":"nonsense"}})");
        const ConfigLoad l = loadConfig(p);
        CHECK(l.error.empty());
        CHECK(l.config.hotkeys.scoreboard == 0x05);
        CHECK(l.config.hotkeys.calibrate == 0x7B);
        CHECK(l.config.hotkeys.reload == 0x65);
        CHECK(l.config.hotkeys.debugDump == 0x78);  // default kept
        CHECK(l.config.enemyRows.size() == 1);      // no enemy_rows -> {enemy_roi}
        CHECK(l.config.enemyPortraits.empty());     // no portraits -> hero recognition off
    }
    for (const int vk : {0x09, 0x05, 0x7B, 0x87, 0x65, int{'A'}, int{'7'}, 0x21, 0x23}) {
        const auto p = writeTemp("dbh_rt.json", "{\"hotkeys\":{\"scoreboard\":\"" + keyName(vk) + "\"}}");
        CHECK(loadConfig(p).config.hotkeys.scoreboard == vk);
    }
    CHECK(keyName(0xFF) == "0xFF");
    {
        const ConfigLoad l = loadConfig(writeTemp("dbh_bad.json", R"({"monitor_index": 3, "match_threshold": })"));
        CHECK(!l.error.empty());
        CHECK(l.config.monitorIndex == 0);
        CHECK(l.config.enemyRows.size() == 1);
    }
    CHECK(!loadConfig("/nonexistent/config.json").error.empty());
    {
        // Unusable numbers are clamped (with a warning) instead of failing later inside OpenCV.
        const ConfigLoad l = loadConfig(writeTemp("dbh_clamp.json",
            R"({"downscale": 0, "reference_height": 0, "match_threshold": 5, "icon_scale": -1,
                "max_suggestions": 0, "archetype_share": 0.1, "panel": {"x": 3}})"));
        CHECK(l.error.empty());
        CHECK(near(l.config.downscale, 0.2));
        CHECK(l.config.referenceHeight == 240);
        CHECK(near(l.config.matchThreshold, 0.999));
        CHECK(near(l.config.iconScale, 0.5));
        CHECK(l.config.maxSuggestions == 1);
        CHECK(near(l.config.archetypeShare, 0.34));
        CHECK(near(l.config.panelX, 0.95));
    }
    {
        // F6 writes calibration.json next to config.json; it overrides icon_scale on every load.
        const auto dir = std::filesystem::temp_directory_path() / "dbh_calib";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "config.json") << R"({"icon_scale": 1.1})";
        CHECK(near(loadConfig(dir / "config.json").config.iconScale, 1.1));
        CHECK(saveCalibration(dir / "config.json", 0.9375));
        CHECK(near(loadConfig(dir / "config.json").config.iconScale, 0.9375));
        std::ofstream(dir / "calibration.json") << "{ broken";
        CHECK(near(loadConfig(dir / "config.json").config.iconScale, 1.1));  // broken file ignored
        std::filesystem::remove_all(dir);
    }
    {
        const ConfigLoad l = loadConfig(writeTemp("dbh_rows.json",
            R"({"enemy_rows":{"first":{"x":0.1,"y":0.1,"w":0.2,"h":0.05},"count":40,"step_x":0.1,"step_y":0}})"));
        CHECK(l.config.enemyRows.size() == 12);
        CHECK(near(l.config.enemyRows[3].x, 0.4));
        CHECK(near(l.config.enemyRows[3].y, 0.1));
    }

    // ---- Counter database ---------------------------------------------------------------
    CounterDatabase db;
    CHECK(db.load(assets / "counters.json"));
    CHECK(db.size() == 173);
    CHECK(db.heroCount() == 44);
    CHECK(db.displayName("health_stimpak") == "Healing Rite");
    CHECK(db.displayName("unknown_item") == "unknown_item");
    CHECK(db.heroName("haze") == "Haze");
    CHECK(db.heroName("mo_krill") == "Mo & Krill");
    CHECK(db.archetypeName("spirit") == "Spirit build");
    CHECK(db.slotOf("toxic_bullets") == "weapon");
    CHECK(db.slotOf("healbane") == "vitality");
    CHECK(!db.tipFor("unstoppable").empty());
    CHECK(db.tipFor("health").empty());
    CHECK(db.avoidAgainst("haze").size() == 3);

    {
        // Items only: Healing Rite (w1) + Leech (w2) + Cursed Relic (w3).
        SuggestInput in;
        in.items = {{"health_stimpak", 1}, {"damage_recycler", 1}, {"glitch", 1}, {"not_in_db", 3}};
        const auto s = db.suggest(in, 10);
        CHECK(s.size() == 10);
        CHECK(s[0].score == 9 && s[3].score == 9);
        CHECK(s[4].item == "Inhibitor" && s[4].score == 8);
        const Suggestion* toxic = find(s, "Toxic Bullets");
        CHECK(toxic && toxic->answers.size() == 2);
    }
    {
        // Tankbuster ignores spirit resist, so no resist item may be suggested against it.
        SuggestInput in;
        in.items = {{"magic_shock", 1}};
        for (const auto& t : db.suggest(in, 50)) {
            CHECK(t.item != "Spirit Resilience" && t.item != "Enchanter's Emblem" && t.item != "Witchmail");
        }
    }
    {
        // Hero counters come from match data: Metal Skin is the top answer to Haze.
        SuggestInput in;
        in.heroes = {{"haze", 1}};
        const auto s = db.suggest(in, 6);
        CHECK(!s.empty() && s[0].item == "Metal Skin" && s[0].score == 2 * 3);
        CHECK(!s.empty() && s[0].reason.find("vs Haze") != std::string::npos);
        CHECK(!s.empty() && s[0].answers == std::vector<std::string>{"Haze"});
    }
    {
        // Item + archetype on the same counter: score adds up, reason comes from the bigger part.
        SuggestInput in;
        in.items = {{"magic_burst", 1}};   // spirit_burst, weight 1 -> Spirit Resilience 1*1*3 = 3
        in.archetypes = {{"spirit", 1}};   // archetype weight 2    -> Spirit Resilience 1*2*3 = 6
        const auto all = db.suggest(in, 20);
        const Suggestion* r = find(all, "Spirit Resilience");
        CHECK(r && r->score == 9);
        CHECK(r && r->reason.find("spirit-heavy enemy") != std::string::npos);
        CHECK(r && r->answers.size() == 2);
    }

    // ---- Per-player analysis ------------------------------------------------------------
    {
        const std::vector<Detection> detections = {
            hero("haze", 0), item("glass_cannon", 0), item("critshot", 0), item("sharpshooter", 0),
            item("unstoppable", 0),                                     // W3 V1 -> Gun build
            item("magic_burst", 2), item("toxic_bullets", 2), item("health", 2),  // 1/1/1 -> Hybrid
            item("rapid_rounds", 4), item("health", 4),                 // only 2 items -> no build
            item("glitch", 9),                                          // row out of range: ignored
        };
        SuggestInput in;
        const auto players = describePlayers(detections, 6, db, {0.6, 3}, in);
        CHECK(players.size() == 3);
        CHECK(players[0].slot == 0 && players[1].slot == 2 && players[2].slot == 4);

        const EnemyPlayer& a = players[0];
        CHECK(a.hero == "Haze");
        CHECK(a.build == "Gun build");
        CHECK(a.weaponItems == 3 && a.spiritItems == 0 && a.vitalityItems == 1);
        CHECK(a.items.size() == 4 && std::ranges::is_sorted(a.items));
        CHECK(a.tips.size() == 2);  // Glass Cannon + Unstoppable
        CHECK(a.avoid.size() == 3);

        CHECK(players[1].build == "Hybrid" && players[1].hero.empty());
        CHECK(players[1].avoid.empty());
        CHECK(players[2].build.empty());

        CHECK(in.items.at("health") == 2);
        CHECK(!in.items.contains("glitch"));
        CHECK(in.heroes == (std::map<std::string, int>{{"haze", 1}}));
        CHECK(in.archetypes == (std::map<std::string, int>{{"weapon", 1}}));

        // The whole scan ranks into suggestions that answer items, hero and build together.
        const auto s = db.suggest(in, 6);
        const auto all = db.suggest(in, 50);
        const Suggestion* metal = find(all, "Metal Skin");
        CHECK(s.size() == 6);
        CHECK(metal && std::ranges::find(metal->answers, std::string("Haze")) != metal->answers.end());
        CHECK(metal && std::ranges::find(metal->answers, std::string("Gun build")) != metal->answers.end());
    }

    // A failed load keeps the previous tables.
    CHECK(!db.load(writeTemp("dbh_badcounters.json", "{ nope")));
    {
        // What you own is never suggested.
        SuggestInput in;
        in.heroes = {{"haze", 1}};
        const auto before = db.suggest(in, 6);
        CHECK(!before.empty() && !before[0].itemId.empty());
        in.owned.insert(before[0].itemId);
        const auto after = db.suggest(in, 6);
        CHECK(std::ranges::none_of(after, [&](const Suggestion& s) { return s.itemId == before[0].itemId; }));
    }

    {
        // Buying order: what you can afford soon first, by what you have spent; the rest stays listed.
        SuggestInput in;
        in.heroes = {{"haze", 1}};
        in.items = {{"glass_cannon", 1}, {"health_stimpak", 1}, {"unstoppable", 1}};
        const auto unknown = db.suggest(in, 8);
        CHECK(std::ranges::all_of(unknown, [](const Suggestion& s) { return s.timing == BuyTiming::Unknown; }));

        in.spentSouls = 0;
        const auto early = db.suggest(in, 8);
        CHECK(std::ranges::is_sorted(early, {}, &Suggestion::timing));
        for (const auto& s : early) {
            CHECK(s.timing == (s.tier <= 2 ? BuyTiming::Now : s.tier == 3 ? BuyTiming::Next : BuyTiming::Later));
        }
        in.spentSouls = 8000;
        for (const auto& s : db.suggest(in, 8)) {
            CHECK(s.timing == (s.tier <= 3 ? BuyTiming::Now : BuyTiming::Next));
        }
        in.spentSouls = 20000;
        for (const auto& s : db.suggest(in, 8)) {
            CHECK(s.timing == BuyTiming::Now);
        }

        CHECK(db.costOf("glass_cannon") == 6400);
        CHECK(db.costOf("nonsense") == 0);

        // A threat you already answer counts for less: the other answers drop.
        SuggestInput threat;
        threat.items = {{"glass_cannon", 1}};
        const auto uncovered = db.suggest(threat, 8);
        CHECK(uncovered.size() >= 2);
        threat.owned = {uncovered[0].itemId};
        const auto covered = db.suggest(threat, 8);
        CHECK(!covered.empty() && covered[0].score < uncovered[1].score + 1);
        CHECK(covered[0].score == (uncovered[1].score + 1) / 2);

        // Building on an item you own costs less, and says so.
        CounterDatabase small;
        CHECK(small.load(writeTemp("dbh_components.json", R"({"items": {
            "part": {"display": "Part", "slot": "weapon", "tier": 1},
            "whole": {"display": "Whole", "slot": "weapon", "tier": 3, "buy_minute": 17, "components": ["part"]},
            "threat": {"display": "Threat", "slot": "spirit", "tier": 2, "weight": 1,
                       "counters": [{"item": "Whole", "priority": 2, "reason": "Weapon T3 | why"}]}}})")));
        SuggestInput want;
        want.items = {{"threat", 1}};
        const auto plain = small.suggest(want, 3);
        CHECK(plain.size() == 1 && plain[0].itemId == "whole" && plain[0].cost == 3200 && plain[0].buyMinute == 17);
        CHECK(plain[0].buildFrom == std::vector<std::string>{"Part"} && plain[0].upgrades.empty());
        want.owned = {"part"};
        const auto cheaper = small.suggest(want, 3);
        CHECK(cheaper.size() == 1 && cheaper[0].cost == 2400 && cheaper[0].upgrades == "Part" && cheaper[0].buildFrom.empty());
    }

    // ---- What enemies bought since the last look ----------------------------------------------
    {
        EnemyPlayer mine;
        mine.items = {"Fury Trance", "Metal Skin"};
        EnemyPlayer haze;
        haze.hero = "Haze";
        haze.avoid = {"Fury Trance (-4.5 pp)", "Active Reload (-2.2 pp)"};
        CHECK(weakItems(mine, {haze}) == std::vector<std::string>{"Fury Trance is weak against Haze (-4.5 pp)"});
        CHECK(weakItems(EnemyPlayer{}, {haze}).empty());

        EnemyPlayer a;
        a.slot = 0; a.heroId = "haze"; a.hero = "Haze"; a.itemIds = {"x"}; a.items = {"X"};
        EnemyPlayer later = a;
        later.itemIds = {"x", "y"}; later.items = {"X", "Y"};
        CHECK(newPurchases({a}, {later}) == std::vector<std::string>{"Haze bought Y"});
        CHECK(newPurchases({later}, {a}).empty());       // sold: nothing to warn about
        CHECK(newPurchases({}, {later}).empty());        // nothing to compare with
        EnemyPlayer other = later;
        other.heroId = "abrams";
        CHECK(newPurchases({a}, {other}).empty());       // another hero: another match
    }

    // ---- settings.json: only what differs from config.json, surviving a failed read ----------
    {
        const fs::path dir = fs::temp_directory_path() / "dbh_settings";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path file = dir / "config.json";
        std::ofstream(file) << R"({"match_threshold": 0.9, "scan_threads": 2,
            "enemy_rows": {"first": {"x": 0.5, "y": 0.3, "w": 0.04, "h": 0.2}, "count": 3, "step_x": 0.05, "step_y": 0.0}})";

        AppConfig cfg = loadConfig(file).config;
        CHECK(saveSettings(file, cfg));
        CHECK(nlohmann::json::parse(std::ifstream(dir / "settings.json")).empty());  // nothing changed

        cfg.matchThreshold = 0.85;
        cfg.hotkeys.menu = 0x7A;  // F11
        for (auto& r : cfg.enemyRows) r.y = 0.28;
        CHECK(saveSettings(file, cfg));
        const auto saved = nlohmann::json::parse(std::ifstream(dir / "settings.json"));
        CHECK(saved.contains("match_threshold") && !saved.contains("scan_threads") && saved.contains("enemy_rows"));

        const AppConfig back = loadConfig(file).config;
        CHECK(near(back.matchThreshold, 0.85));
        CHECK(back.hotkeys.menu == 0x7A);
        CHECK(back.enemyRows.size() == 3 && near(back.enemyRows[2].y, 0.28) && near(back.enemyRows[2].x, 0.6));
        CHECK(near(loadConfig(file, false).config.matchThreshold, 0.9));  // config.json itself is untouched

        std::ofstream(dir / "settings.json") << "{ broken";
        CHECK(near(loadConfig(file).config.matchThreshold, 0.9));

        CHECK(resetSettings(file));
        CHECK(!fs::exists(dir / "settings.json"));

        CHECK(isNamedKey(0x74));   // F5
        CHECK(!isNamedKey(0x01));  // left mouse button
        CHECK(!isNamedKey(0x1B));  // escape
    }

    CHECK(db.size() == 173 && db.heroCount() == 44);
    CHECK(!db.load(writeTemp("dbh_noitems.json", R"({"heroes":{}})")));
    CHECK(db.size() == 173);

    std::printf(failures == 0 ? "ALL %d PASSED\n" : "%d of %d FAILED\n", failures == 0 ? checks : failures, checks);
    return failures == 0 ? 0 : 1;
}
