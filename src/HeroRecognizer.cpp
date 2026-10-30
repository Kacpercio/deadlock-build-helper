#include "HeroRecognizer.h"

#include "VisionEngine.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <numeric>

namespace fs = std::filesystem;

namespace {

// Card art is drawn at these scales; every kCoarseStride-th one is tried in the fast first pass.
constexpr double kArtScaleFrom = 0.20;
constexpr double kArtScaleTo = 0.40;
constexpr double kArtStep = 0.01;
constexpr int kCoarseStride = 2;

// Top-left part of the scaled art that is compared, in pixels at reference_height. It stops above
// the level badge and fits the portrait box with a few pixels to spare.
constexpr int kVisibleWidth = 70;
constexpr int kVisibleHeight = 66;

constexpr int kMinArtHeight = 150;   // anything shorter is a crop from the screen, used 1:1
constexpr double kMinOpaque = 0.25;  // a variant showing less of the hero than this says nothing
constexpr double kMinTexture = 6.0;  // summed BGR standard deviation below which a picture is flat
constexpr size_t kFinalists = 3;     // heroes that reach the colour pass

}  // namespace

HeroRecognizer::HeroRecognizer(HeroConfig cfg)
    : cfg_(cfg)
{
}

// Crops to the visible part and prepares both passes. A variant that shows too little of the hero
// stays empty: it keeps its place in the scale order, and the search skips it.
HeroRecognizer::Variant HeroRecognizer::makeVariant(const cv::Mat& image, const cv::Mat& mask)
{
    const cv::Rect visible = cv::Rect(0, 0, kVisibleWidth, kVisibleHeight) & cv::Rect(0, 0, image.cols, image.rows);
    Variant v;
    if (!mask.empty()) {
        if (cv::countNonZero(mask(visible)) < kMinOpaque * visible.area()) {
            return v;
        }
        v.colorMask = mask(visible).clone();
    }
    // Correlation is undefined for a flat picture: it can score 1.0 against anything.
    cv::Scalar mean, deviation;
    cv::meanStdDev(image(visible), mean, deviation, v.colorMask);
    if (deviation[0] + deviation[1] + deviation[2] < kMinTexture) {
        return Variant{};
    }
    v.color = image(visible).clone();

    cv::Mat gray;
    cv::cvtColor(v.color, gray, cv::COLOR_BGR2GRAY);
    const cv::Size half(std::max(1, gray.cols / 2), std::max(1, gray.rows / 2));
    cv::resize(gray, v.coarse, half, 0, 0, cv::INTER_AREA);
    if (!v.colorMask.empty()) {
        cv::resize(v.colorMask, v.coarseMask, half, 0, 0, cv::INTER_NEAREST);
    }
    return v;
}

size_t HeroRecognizer::loadTemplates(const fs::path& dir, const std::set<std::string>& known)
{
    heroes_.clear();
    lastVariant_.clear();
    std::map<std::string, std::vector<Variant>> byId;

    for (const auto& file : readTemplateFolder(dir)) {
        if (!known.empty() && !known.contains(file.id)) {
            std::cerr << "[heroes] ignoring " << file.id << ": no such hero id in counters.json\n";
            continue;
        }

        auto& variants = byId[file.id];
        if (file.image.rows < kMinArtHeight) {
            variants.push_back(makeVariant(file.image, file.mask));
            continue;
        }
        const int steps = static_cast<int>(std::lround((kArtScaleTo - kArtScaleFrom) / kArtStep));
        for (int i = 0; i <= steps; ++i) {
            const double scale = kArtScaleFrom + i * kArtStep;
            const cv::Size size(static_cast<int>(std::lround(file.image.cols * scale)),
                                static_cast<int>(std::lround(file.image.rows * scale)));
            cv::Mat image, mask;
            cv::resize(file.image, image, size, 0, 0, cv::INTER_AREA);
            if (!file.mask.empty()) {
                cv::resize(file.mask, mask, size, 0, 0, cv::INTER_NEAREST);
            }
            variants.push_back(makeVariant(image, mask));
        }
    }

    for (auto& [id, variants] : byId) {
        heroes_.push_back({id, std::move(variants)});
    }
    return heroes_.size();
}

HeroRecognizer::Search HeroRecognizer::prepareSearch(const cv::Mat& frameBgr, cv::Rect box) const
{
    // The box grows and shrinks with the HUD, like what it holds.
    box.width = static_cast<int>(std::lround(box.width * cfg_.iconScale));
    box.height = static_cast<int>(std::lround(box.height * cfg_.iconScale));
    box &= cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
    Search search{box, static_cast<double>(cfg_.referenceHeight) / (frameBgr.rows * cfg_.iconScale), {}};
    if (!box.empty()) {
        // Templates are fixed at reference_height, so bring the search area to that size instead.
        cv::resize(frameBgr(box), search.image, {}, search.toReference, search.toReference,
                   search.toReference < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    }
    return search;
}

void HeroRecognizer::bestOf(const Hero& hero, size_t first, size_t last, const Search& search, double& score,
                            cv::Rect& box, size_t& variant) const
{
    cv::Mat response;
    for (size_t v = first; v <= last; ++v) {
        const Variant& t = hero.variants[v];
        if (t.color.empty() || t.color.cols > search.image.cols || t.color.rows > search.image.rows) {
            continue;
        }
        matchNormed(search.image, t.color, t.colorMask, response);
        double s = 0.0;
        cv::Point loc;
        cv::minMaxLoc(response, nullptr, &s, nullptr, &loc);
        if (s > score) {
            score = s;
            variant = v;
            box = cv::Rect(search.box.x + static_cast<int>(loc.x / search.toReference),
                           search.box.y + static_cast<int>(loc.y / search.toReference),
                           static_cast<int>(t.color.cols / search.toReference),
                           static_cast<int>(t.color.rows / search.toReference));
        }
    }
}

HeroVerdict HeroRecognizer::confirm(const cv::Mat& frameBgr, cv::Rect box, const std::string& id) const
{
    const auto hero = std::ranges::find(heroes_, id, &Hero::id);
    const auto known = lastVariant_.find(id);
    if (hero == heroes_.end() || known == lastVariant_.end()) {
        return {};
    }
    const Search search = prepareSearch(frameBgr, box);
    if (search.image.empty()) {
        return {};
    }
    HeroVerdict verdict;
    verdict.id = id;
    const size_t first = known->second > 0 ? known->second - 1 : 0;
    const size_t last = std::min(known->second + 1, hero->variants.size() - 1);
    size_t variant = known->second;
    bestOf(*hero, first, last, search, verdict.score, verdict.box, variant);
    verdict.accepted = verdict.score >= cfg_.threshold;
    return verdict;
}

HeroVerdict HeroRecognizer::recognize(const cv::Mat& frameBgr, cv::Rect box) const
{
    const Search search = prepareSearch(frameBgr, box);
    if (search.image.empty() || heroes_.empty()) {
        return {};
    }
    cv::Mat coarseSearch;
    cv::cvtColor(search.image, coarseSearch, cv::COLOR_BGR2GRAY);
    cv::resize(coarseSearch, coarseSearch, {}, 0.5, 0.5, cv::INTER_AREA);

    // First pass: best coarse score and variant of every hero.
    std::vector<double> coarseScore(heroes_.size(), -1.0);
    std::vector<size_t> coarseVariant(heroes_.size(), 0);
    cv::parallel_for_(cv::Range(0, static_cast<int>(heroes_.size())), [&](const cv::Range& range) {
        cv::Mat response;
        for (int h = range.start; h < range.end; ++h) {
            const auto& variants = heroes_[h].variants;
            for (size_t v = 0; v < variants.size(); v += kCoarseStride) {
                const Variant& t = variants[v];
                if (t.coarse.empty() || t.coarse.cols > coarseSearch.cols || t.coarse.rows > coarseSearch.rows) {
                    continue;
                }
                matchNormed(coarseSearch, t.coarse, t.coarseMask, response);
                double score = 0.0;
                cv::minMaxLoc(response, nullptr, &score);
                if (score > coarseScore[h]) {
                    coarseScore[h] = score;
                    coarseVariant[h] = v;
                }
            }
        }
    });

    std::vector<size_t> order(heroes_.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const size_t finalists = std::min(kFinalists, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(finalists), order.end(),
                      [&](size_t a, size_t b) { return coarseScore[a] > coarseScore[b]; });

    // Second pass: the finalists in colour, at the coarse scale and its neighbours.
    HeroVerdict verdict;
    size_t bestVariant = 0;
    for (size_t f = 0; f < finalists; ++f) {
        const Hero& hero = heroes_[order[f]];
        if (coarseScore[order[f]] < 0.0) {
            continue;
        }
        double heroScore = -1.0;
        cv::Rect heroBox;
        size_t heroVariant = 0;
        const size_t centre = coarseVariant[order[f]];
        bestOf(hero, centre > 0 ? centre - 1 : 0, std::min(centre + 1, hero.variants.size() - 1), search, heroScore,
               heroBox, heroVariant);
        if (heroScore > verdict.score) {
            verdict.runnerUp = verdict.score;
            verdict.score = heroScore;
            verdict.id = hero.id;
            verdict.box = heroBox;
            bestVariant = heroVariant;
        } else if (heroScore > verdict.runnerUp) {
            verdict.runnerUp = heroScore;
        }
    }

    verdict.accepted = verdict.score >= cfg_.threshold && verdict.score - verdict.runnerUp >= cfg_.margin;
    if (verdict.accepted) {
        lastVariant_[verdict.id] = bestVariant;
    }
    return verdict;
}
