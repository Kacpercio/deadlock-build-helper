#include "PlayerAnalysis.h"

#include <algorithm>
#include <string>
#include <utility>

namespace {

void addUnique(std::vector<std::string>& list, const std::string& value)
{
    if (!value.empty() && std::ranges::find(list, value) == list.end()) {
        list.push_back(value);
    }
}

}  // namespace

VisionConfig itemVisionConfig(const AppConfig& cfg)
{
    return {cfg.matchThreshold, cfg.downscale, cfg.referenceHeight, cfg.iconScale, cfg.itemIconSize,
            cfg.itemCornerMask};
}

HeroConfig heroConfig(const AppConfig& cfg)
{
    return {cfg.heroThreshold, cfg.heroMargin, cfg.referenceHeight, cfg.iconScale};
}

cv::Rect toPixels(const NormRect& box, cv::Size size)
{
    const cv::Rect px(static_cast<int>(box.x * size.width), static_cast<int>(box.y * size.height),
                      static_cast<int>(box.w * size.width), static_cast<int>(box.h * size.height));
    return px & cv::Rect(cv::Point(0, 0), size);
}

std::vector<cv::Rect> toPixels(const std::vector<NormRect>& boxes, cv::Size size)
{
    std::vector<cv::Rect> out;
    out.reserve(boxes.size());
    for (const auto& b : boxes) {
        out.push_back(toPixels(b, size));
    }
    return out;
}

cv::Rect boundingBox(const std::vector<cv::Rect>& rects)
{
    cv::Rect box;
    for (const auto& r : rects) {
        if (!r.empty()) {
            box = box.empty() ? r : (box | r);
        }
    }
    return box;
}

void assignToRows(std::vector<Detection>& detections, const std::vector<cv::Rect>& rows)
{
    for (auto& d : detections) {
        const cv::Point center(d.box.x + d.box.width / 2, d.box.y + d.box.height / 2);
        const auto row = std::ranges::find_if(rows, [&](const cv::Rect& r) { return r.contains(center); });
        d.slot = row != rows.end() ? static_cast<int>(row - rows.begin()) : -1;
    }
    std::erase_if(detections, [](const Detection& d) { return d.slot < 0; });
}

std::vector<HeroVerdict> addHeroes(const HeroRecognizer& heroes, const cv::Mat& frameBgr,
                                   const std::vector<cv::Rect>& portraits, size_t rowCount,
                                   std::vector<Detection>& detections)
{
    std::vector<HeroVerdict> verdicts;
    for (size_t i = 0; i < portraits.size() && i < rowCount; ++i) {
        verdicts.push_back(heroes.recognize(frameBgr, portraits[i]));
        const HeroVerdict& v = verdicts.back();
        if (v.accepted) {
            detections.push_back({v.id, v.box, v.score, static_cast<int>(i), true});
        }
    }
    return verdicts;
}

std::vector<EnemyPlayer> describePlayers(const std::vector<Detection>& detections, size_t rowCount,
                                         const CounterDatabase& counters, const ArchetypeRule& rule,
                                         SuggestInput& input)
{
    std::vector<EnemyPlayer> players(rowCount);
    std::vector<bool> seen(rowCount, false);

    for (const auto& d : detections) {
        if (d.slot < 0 || static_cast<size_t>(d.slot) >= rowCount) {
            continue;
        }
        const auto slot = static_cast<size_t>(d.slot);
        EnemyPlayer& p = players[slot];
        seen[slot] = true;
        if (d.isHero) {
            p.heroId = d.itemId;
            p.hero = counters.heroName(d.itemId);
            continue;
        }
        ++input.items[d.itemId];
        p.itemIds.push_back(d.itemId);
        addUnique(p.tips, counters.tipFor(d.itemId));
        const std::string shopSlot = counters.slotOf(d.itemId);
        if (shopSlot == "weapon") {
            ++p.weaponItems;
        } else if (shopSlot == "spirit") {
            ++p.spiritItems;
        } else if (shopSlot == "vitality") {
            ++p.vitalityItems;
        }
    }

    std::vector<EnemyPlayer> out;
    for (size_t i = 0; i < rowCount; ++i) {
        if (!seen[i]) {
            continue;
        }
        EnemyPlayer& p = players[i];
        p.slot = static_cast<int>(i);
        std::ranges::sort(p.itemIds, {}, [&](const std::string& id) { return counters.displayName(id); });
        for (const auto& id : p.itemIds) {
            p.items.push_back(counters.displayName(id));
        }

        if (!p.heroId.empty()) {
            ++input.heroes[p.heroId];
            p.avoid = counters.avoidAgainst(p.heroId);
        }

        // A build is "gun" / "spirit" / "tank" when one shop slot clearly dominates its items.
        const int total = p.weaponItems + p.spiritItems + p.vitalityItems;
        if (total >= rule.minItems) {
            const std::pair<int, const char*> slots[] = {
                {p.weaponItems, "weapon"}, {p.spiritItems, "spirit"}, {p.vitalityItems, "vitality"}};
            const auto& [top, shopSlot] = *std::ranges::max_element(slots, {}, &std::pair<int, const char*>::first);
            if (top >= rule.share * total) {
                ++input.archetypes[shopSlot];
                p.build = counters.archetypeName(shopSlot);
            } else {
                p.build = "Hybrid";
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}
