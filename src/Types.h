#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Detection::itemId of a place on the scoreboard that must hold an item nothing could be matched to.
inline constexpr const char* kUnknownItem = "?";

// One icon found on screen. `box` is in full-frame (= monitor) pixel coordinates.
struct Detection {
    std::string itemId;  // item id, or hero id for portrait detections
    cv::Rect box;
    double score = 0.0;
    int slot = -1;  // index of the enemy row it sits in; -1 until assigned
    bool isHero = false;
};

// An enemy item aggregated across the scoreboard (e.g. 2 enemies own Healing Rite -> count 2).
struct EnemyItem {
    std::string itemId;
    std::string displayName;
    int count = 0;
};

// Everything known about one enemy's scoreboard row.
struct EnemyPlayer {
    int slot = 0;
    std::string heroId;  // empty if the portrait was not recognised
    std::string hero;    // display name
    std::string build;   // "Gun build" / "Spirit build" / "Tank build" / "Hybrid", empty with few items
    int weaponItems = 0;
    int spiritItems = 0;
    int vitalityItems = 0;
    std::vector<std::string> itemIds;
    std::vector<std::string> items;  // display names, same order as itemIds
    int unknownItems = 0;            // items seen on the scoreboard but not recognised
    std::vector<std::string> tips;   // how to play against their items
    std::vector<std::string> avoid;  // items that do badly against their hero
};

// When to buy something, going by how far along you are.
enum class BuyTiming { Unknown, Now, Next, Later };

// A recommended purchase. `answers` lists what it counters (items, heroes, builds).
struct Suggestion {
    std::string item;    // display name
    std::string itemId;  // empty if counters.json does not list the item itself
    std::string slot;    // shop tab: "weapon" / "vitality" / "spirit"
    int tier = 0;
    int cost = 0;        // souls still to pay: less what you own of it; 0 = unknown
    BuyTiming timing = BuyTiming::Unknown;
    int buyMinute = 0;   // the match minute players usually buy it at; 0 = unknown
    std::string upgrades;                // the item you own that this one builds on
    std::vector<std::string> buildFrom;  // cheaper items it builds on, which you don't own yet
    std::string reason;
    int score = 0;
    std::vector<std::string> answers;
};

struct AnalysisResult {
    std::vector<EnemyItem> enemyItems;
    std::vector<EnemyPlayer> players;
    std::vector<Suggestion> suggestions;
    std::vector<Detection> detections;
    double elapsedMs = 0.0;
    std::string error;
    std::string warning;        // the scan ran, but something it needs is missing

    // You: your HUD says which hero you play, or else the nickname you picked in the menu says where you are.
    bool nicknameSet = false;
    std::vector<EnemyPlayer> allies;       // your team, in the same form as `players`
    int enemyItemSouls = 0;     // what the items recognised on each team cost
    int allyItemSouls = 0;

    std::string myHeroId;       // from your HUD; empty if not recognised
    std::string howFound;       // what the HUD and the nickname each said, to tell a wrong answer's cause
    int mySlot = -1;            // your column on your team, -1 = not found on the scoreboard
    EnemyPlayer me;             // what is on that column; what you own is left out of the suggestions
    int mySouls = 0;            // what your items cost
    std::vector<std::string> myWeakItems;  // items you own that do badly against these enemies

    // Top of the frame this came from (holds the scoreboard), and the frame's full size.
    cv::Mat preview;
    cv::Size frameSize;
    std::uint64_t version = 0;  // 0 = no scan has finished yet
};
