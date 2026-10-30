#include "PlayerAnalysis.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace {

// Measured on real frames: the same nickname scores 0.998 or more, other text at most 0.46.
constexpr double kNameThreshold = 0.80;
constexpr double kMinTextBrightness = 120.0;
constexpr double kTextShareOfBrightest = 0.65;

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

std::vector<NormRect> mirrored(std::vector<NormRect> boxes)
{
    for (auto& b : boxes) {
        b.x = 1.0 - b.x - b.w;
    }
    return boxes;
}

NameTemplate makeNameTemplate(const cv::Mat& frameBgr, cv::Rect box)
{
    box &= cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
    if (box.empty()) {
        return {};
    }
    cv::Mat gray;
    cv::cvtColor(frameBgr(box), gray, cv::COLOR_BGR2GRAY);

    // The text is the brightest thing in the box; trim to it.
    double brightest = 0.0;
    cv::minMaxLoc(gray, nullptr, &brightest);
    cv::Mat text;
    cv::threshold(gray, text, std::max(kMinTextBrightness, brightest * kTextShareOfBrightest), 255, cv::THRESH_BINARY);
    if (cv::countNonZero(text) == 0) {
        return {};
    }
    constexpr int kPadding = 1;
    const cv::Rect area = cv::Rect(cv::boundingRect(text).tl() - cv::Point(kPadding, kPadding),
                                   cv::boundingRect(text).br() + cv::Point(kPadding, kPadding)) &
                          cv::Rect(0, 0, gray.cols, gray.rows);
    cv::Mat mask;
    cv::dilate(text, mask, cv::Mat::ones(3, 3, CV_8U));
    return {gray(area).clone(), mask(area).clone()};
}

bool saveNameTemplate(const std::filesystem::path& file, const NameTemplate& name)
{
    cv::Mat image;
    cv::merge(std::vector<cv::Mat>{name.gray, name.gray, name.gray, name.mask}, image);
    return writeImage(file, image);
}

NameTemplate loadNameTemplate(const std::filesystem::path& file)
{
    const cv::Mat image = readImage(file, cv::IMREAD_UNCHANGED);
    if (image.empty() || image.channels() != 4 || image.depth() != CV_8U) {
        return {};
    }
    std::vector<cv::Mat> channels;
    cv::split(image, channels);
    return {channels[0], channels[3]};
}

int findName(const cv::Mat& frameBgr, const std::vector<cv::Rect>& boxes, const NameTemplate& name, double* score)
{
    int found = -1;
    double best = kNameThreshold;
    cv::Mat gray, response;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const cv::Rect box = boxes[i] & cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
        if (name.empty() || box.width < name.gray.cols || box.height < name.gray.rows) {
            continue;
        }
        cv::cvtColor(frameBgr(box), gray, cv::COLOR_BGR2GRAY);
        matchNormed(gray, name.gray, name.mask, response);
        double s = 0.0;
        cv::minMaxLoc(response, nullptr, &s);
        if (s >= best) {
            best = s;
            found = static_cast<int>(i);
        }
    }
    if (score) {
        *score = found >= 0 ? best : 0.0;
    }
    return found;
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

std::vector<std::string> weakItems(const EnemyPlayer& me, const std::vector<EnemyPlayer>& enemies)
{
    std::vector<std::string> lines;
    for (const auto& enemy : enemies) {
        for (const auto& avoid : enemy.avoid) {  // "Fury Trance (-4.5 pp)"
            for (const auto& item : me.items) {
                if (avoid.starts_with(item + " (")) {
                    lines.push_back(item + " is weak against " + enemy.hero + " " + avoid.substr(item.size() + 1));
                }
            }
        }
    }
    return lines;
}

std::vector<std::string> newPurchases(const std::vector<EnemyPlayer>& before, const std::vector<EnemyPlayer>& after)
{
    std::vector<std::string> lines;
    for (const auto& now : after) {
        const auto was = std::ranges::find_if(before, [&](const EnemyPlayer& p) {
            return p.slot == now.slot && p.heroId == now.heroId;
        });
        if (was == before.end() || now.heroId.empty()) {
            continue;
        }
        for (size_t i = 0; i < now.itemIds.size(); ++i) {
            if (std::ranges::find(was->itemIds, now.itemIds[i]) == was->itemIds.end()) {
                lines.push_back(now.hero + " bought " + now.items[i]);
            }
        }
    }
    return lines;
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
