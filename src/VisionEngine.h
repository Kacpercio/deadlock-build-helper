#pragma once

#include "Types.h"

#include <opencv2/core.hpp>

#include <filesystem>
#include <string>
#include <vector>

// cv::imread/imwrite take narrow paths, which on Windows go through the ANSI code page and fail
// for characters outside it. These go through std::filesystem (wide paths) instead.
cv::Mat readImage(const std::filesystem::path& file, int flags = 1 /* cv::IMREAD_COLOR */);
bool writeImage(const std::filesystem::path& file, const cv::Mat& image);

// Every image in a template folder. The file name up to an "@" is the id, so
//   health_stimpak.png, health_stimpak@active.png  -> both report "health_stimpak".
// Transparent pixels (alpha < 128) end up as zeros in `mask`.
struct TemplateFile {
    std::string id;
    cv::Mat image;  // BGR
    cv::Mat mask;   // empty = every pixel counts
};
std::vector<TemplateFile> readTemplateFolder(const std::filesystem::path& dir);

// TM_CCOEFF_NORMED with an optional mask, and without the NaN / inf scores flat areas produce.
void matchNormed(const cv::Mat& search, const cv::Mat& templ, const cv::Mat& mask, cv::Mat& response);

struct VisionConfig {
    double threshold = 0.85;    // TM_CCOEFF_NORMED score needed to count as a hit
    double downscale = 1.0;     // search image scale; templates are scaled to match
    int referenceHeight = 1080; // screen height the templates' sizes refer to
    double iconScale = 1.0;     // correction when icons on screen are larger/smaller than expected
    int templateHeight = 0;     // > 0: every template is resized to this height at referenceHeight
                                //       (lets 200 px API icons stand in for 27 px scoreboard icons)
    double cornerMask = 0.0;    // ignore this fraction of each template's top-right corner (a
                                // triangle), where the scoreboard draws a tier badge the API icons lack
};

struct ScaleTune {
    double scale = 1.0;
    int detections = 0;
    double meanScore = 0.0;
};

// Finds item icons in a screenshot with normalized cross-correlation template matching.
//
// Templates are PNGs in one folder (see readTemplateFolder); the id is the one used in counters.json.
class VisionEngine {
public:
    explicit VisionEngine(VisionConfig cfg);

    size_t loadTemplates(const std::filesystem::path& dir);

    // Adds the shop art of active items. The scoreboard draws it round, with a label across the bottom,
    // so only the part of the art both have in common is compared.
    size_t addActiveItems(const std::filesystem::path& dir);

    size_t templateCount() const { return templates_.size(); }

    // Searches only inside `roi` (full-frame pixels); returned boxes are full-frame pixels.
    // Thorough but slow: every template against every pixel of the area, in colour.
    std::vector<Detection> detect(const cv::Mat& frameBgr, cv::Rect roi);

    // Same result for scoreboard columns at a fraction of the CPU time: finds each template's best
    // spot per column in grey at half size, and confirms it in colour only if that looks promising.
    // Reports an item at most once per column, which is all a player can own.
    std::vector<Detection> scan(const cv::Mat& frameBgr, const std::vector<cv::Rect>& columns);

    // scan() needs a score high enough to be sure with nothing else to go on. But the scoreboard fills
    // two icons per line in buying order, so whatever comes before the last hit of a column must be an
    // item too. Those places are compared with every item again and need much less; when even that fails
    // they are reported as kUnknownItem. Places after the last hit have nothing to vouch for them, so
    // they must score nearly as much as a hit. `hits`: scan()'s result with slots assigned; `columns`:
    // what those slots refer to.
    void fillGaps(const cv::Mat& frameBgr, const std::vector<cv::Rect>& columns, std::vector<Detection>& hits);

    double iconScale() const { return cfg_.iconScale; }

    // Tries icon scales in [from, to] (coarse pass, then a fine pass around the best) and keeps
    // the one that finds the most icons, ties broken by mean score. Leaves the engine at that
    // scale; keeps the current one if nothing is found at any scale.
    ScaleTune tuneScale(const cv::Mat& frameBgr, const std::vector<cv::Rect>& columns, double from = 0.80,
                        double to = 1.25);

private:
    struct Template {
        std::string id;
        cv::Mat original;      // BGR as loaded
        cv::Mat originalMask;  // from the alpha channel; empty = every pixel counts
        cv::Mat scaled;        // resized for the current frame height, downscale and icon scale
        cv::Mat scaledMask;    // empty = unmasked (faster) matching
        cv::Mat coarse;        // grey, half size of `scaled`: scan()'s first pass
        cv::Mat coarseMask;
        bool activeLook = false;  // see addActiveItems()
    };

    struct CellMatch {
        const Template* best = nullptr;
        cv::Rect box;  // in the frame
        double score = -1.0;
        double runnerUp = -1.0;  // the best other item
        bool activeLabel = false;
    };
    CellMatch matchCell(const cv::Mat& frameBgr, cv::Rect window) const;

    void prepareTemplates(int frameHeight);
    void collectPeaks(cv::Mat& response, const Template& t, cv::Point roiOrigin,
                      std::vector<Detection>& out) const;

    VisionConfig cfg_;
    std::vector<Template> templates_;
    int scaledForHeight_ = 0;
    double scaledForIconScale_ = 0.0;
};
