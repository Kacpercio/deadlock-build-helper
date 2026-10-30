#pragma once

#include "Config.h"
#include "CounterDatabase.h"
#include "HeroRecognizer.h"
#include "Types.h"
#include "VisionEngine.h"

#include <filesystem>
#include <vector>

struct ArchetypeRule {
    double share = 0.6;  // one shop slot must hold at least this share of the items...
    int minItems = 3;    // ...of an enemy with at least this many recognised items
};

// Settings for item icons and for hero portraits, taken from the app config.
VisionConfig itemVisionConfig(const AppConfig& cfg);
HeroConfig heroConfig(const AppConfig& cfg);

// Normalized box -> pixels of a frame of `size`, clipped to the frame.
cv::Rect toPixels(const NormRect& box, cv::Size size);
std::vector<cv::Rect> toPixels(const std::vector<NormRect>& boxes, cv::Size size);  // keeps empty rects

// Smallest rect containing all non-empty `rects`.
cv::Rect boundingBox(const std::vector<cv::Rect>& rects);

// Sets each detection's `slot` to the row its centre lies in and drops hits that fall between
// rows (they belong to nobody's item bar). Empty rects in `rows` never match.
void assignToRows(std::vector<Detection>& detections, const std::vector<cv::Rect>& rows);

// Classifies portrait i as enemy i, appends the recognised heroes to `detections` and returns what
// each box looked like, recognised or not.
std::vector<HeroVerdict> addHeroes(const HeroRecognizer& heroes, const cv::Mat& frameBgr,
                                   const std::vector<cv::Rect>& portraits, size_t rowCount,
                                   std::vector<Detection>& detections);

// The same boxes on the other side of the screen: your team's columns, from the enemy's.
std::vector<NormRect> mirrored(std::vector<NormRect> boxes);

// A picture of your nickname as the scoreboard draws it: only the text counts, not what is behind it.
struct NameTemplate {
    cv::Mat gray;
    cv::Mat mask;
    bool empty() const { return gray.empty(); }
};
NameTemplate makeNameTemplate(const cv::Mat& frameBgr, cv::Rect box);
bool saveNameTemplate(const std::filesystem::path& file, const NameTemplate& name);
NameTemplate loadNameTemplate(const std::filesystem::path& file);

// The box showing that nickname, or -1. `score` gets its match score, when given.
int findName(const cv::Mat& frameBgr, const std::vector<cv::Rect>& boxes, const NameTemplate& name,
             double* score = nullptr);

// One line per item an enemy owns on this scan but did not on `before`, such as "Haze bought Metal
// Skin". Only compares the same hero in the same column.
std::vector<std::string> newPurchases(const std::vector<EnemyPlayer>& before, const std::vector<EnemyPlayer>& after);

// Turns one scan's detections (items and hero portraits, each with its row in `slot`) into one
// EnemyPlayer per row that showed anything, and fills `input` for CounterDatabase::suggest.
std::vector<EnemyPlayer> describePlayers(const std::vector<Detection>& detections, size_t rowCount,
                                         const CounterDatabase& counters, const ArchetypeRule& rule,
                                         SuggestInput& input);
