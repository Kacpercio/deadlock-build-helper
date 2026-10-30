#include "AbilityBar.h"

#include "VisionEngine.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Measured on real 1080p frames: 48 px glyphs, centred at these offsets from the bottom centre of the
// screen. The HUD hangs from there, so this holds at any aspect ratio.
constexpr int kReferenceHeight = 1080;
constexpr int kGlyphSize = 48;
const cv::Point kGlyphCentres[4] = {{-134, -58}, {-45, -74}, {44, -74}, {133, -58}};
constexpr int kSlack = 6;

// How the HUD draws a glyph.
constexpr float kGlyphShade = 40.0f;
constexpr float kDiscShade = 235.0f;
constexpr double kMaskRadius = 0.48;

// Measured on real frames at 720p to 1440p: your hero 0.945-0.975, the next-best hero at most 0.42.
constexpr double kThreshold = 0.80;
constexpr double kMinLead = 0.15;

// The search windows, at reference_height, grey.
std::array<cv::Mat, 4> searchWindows(const cv::Mat& frameBgr)
{
    const double scale = static_cast<double>(frameBgr.rows) / kReferenceHeight;
    const int window = kGlyphSize + 2 * kSlack;
    std::array<cv::Mat, 4> windows;
    for (size_t i = 0; i < windows.size(); ++i) {
        const cv::Point centre(frameBgr.cols / 2 + static_cast<int>(std::lround(kGlyphCentres[i].x * scale)),
                               frameBgr.rows + static_cast<int>(std::lround(kGlyphCentres[i].y * scale)));
        const int size = static_cast<int>(std::lround(window * scale));
        const cv::Rect box = cv::Rect(centre.x - size / 2, centre.y - size / 2, size, size) &
                             cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
        if (box.width != size || box.height != size) {
            continue;
        }
        cv::Mat gray;
        cv::cvtColor(frameBgr(box), gray, cv::COLOR_BGR2GRAY);
        cv::resize(gray, windows[i], cv::Size(window, window), 0, 0, scale > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    }
    return windows;
}

}  // namespace

size_t AbilityBar::load(const fs::path& dir, const std::set<std::string>& known)
{
    heroes_.clear();
    std::map<std::string, int> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string stem = entry.path().stem().string();
        const auto bar = stem.rfind('_');
        if (entry.path().extension() != ".png" || bar == std::string::npos || bar + 2 != stem.size() ||
            stem[bar + 1] < '1' || stem[bar + 1] > '4') {
            continue;
        }
        const std::string id = stem.substr(0, bar);
        if (!known.empty() && !known.contains(id)) {
            continue;
        }
        const cv::Mat art = readImage(entry.path(), cv::IMREAD_UNCHANGED);
        if (art.empty() || art.channels() != 4 || art.depth() != CV_8U) {
            continue;
        }
        std::vector<cv::Mat> channels;
        cv::split(art, channels);
        cv::Mat alpha;
        cv::resize(channels[3], alpha, cv::Size(kGlyphSize, kGlyphSize), 0, 0, cv::INTER_AREA);
        alpha.convertTo(alpha, CV_32F, 1.0 / 255.0);

        Glyph glyph;
        cv::Mat shade = kGlyphShade * alpha + kDiscShade * (1.0f - alpha);
        shade.convertTo(glyph.gray, CV_8U);
        glyph.mask = cv::Mat::zeros(kGlyphSize, kGlyphSize, CV_8U);
        cv::circle(glyph.mask, {kGlyphSize / 2, kGlyphSize / 2}, static_cast<int>(kGlyphSize * kMaskRadius), cv::Scalar(255),
                   cv::FILLED);
        heroes_[id][static_cast<size_t>(stem[bar + 1] - '1')] = std::move(glyph);
        ++found[id];
    }
    // A hero with an ability missing cannot be told apart reliably.
    std::erase_if(heroes_, [&](const auto& hero) { return found[hero.first] != 4; });
    return heroes_.size();
}

HeroVerdict AbilityBar::recognize(const cv::Mat& frameBgr, const std::string& expected) const
{
    if (heroes_.empty() || frameBgr.empty()) {
        return {};
    }
    const std::array<cv::Mat, 4> windows = searchWindows(frameBgr);
    if (windows[0].empty() || windows[1].empty() || windows[2].empty() || windows[3].empty()) {
        return {};
    }

    const auto scoreOf = [&](const Glyphs& glyphs) {
        std::array<double, 4> scores{};
        cv::Mat response;
        for (size_t i = 0; i < glyphs.size(); ++i) {
            matchNormed(windows[i], glyphs[i].gray, glyphs[i].mask, response);
            cv::minMaxLoc(response, nullptr, &scores[i]);
        }
        std::ranges::sort(scores, std::greater{});
        return (scores[0] + scores[1] + scores[2]) / 3.0;
    };

    HeroVerdict verdict;
    if (const auto known = heroes_.find(expected); known != heroes_.end()) {
        verdict.id = expected;
        verdict.score = scoreOf(known->second);
        verdict.accepted = verdict.score >= kThreshold;
        if (verdict.accepted) {
            return verdict;
        }
        verdict = {};
    }

    for (const auto& [id, glyphs] : heroes_) {
        const double score = scoreOf(glyphs);
        if (score > verdict.score) {
            verdict.runnerUp = verdict.score;
            verdict.score = score;
            verdict.id = id;
        } else if (score > verdict.runnerUp) {
            verdict.runnerUp = score;
        }
    }
    verdict.accepted = verdict.score >= kThreshold && verdict.score - verdict.runnerUp >= kMinLead;
    return verdict;
}
