#pragma once

#include <opencv2/core.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

struct HeroConfig {
    double threshold = 0.80;     // colour score the best hero needs...
    double margin = 0.25;        // ...and how far it must lead the next-best hero
    int referenceHeight = 1080;  // screen height the art scales below refer to
    double iconScale = 1.0;      // same HUD size correction as the item icons (F6)
};

// What one portrait box looks like. The best candidate is kept even when rejected, so a miss can
// be diagnosed from the log.
struct HeroVerdict {
    std::string id;
    cv::Rect box;
    double score = -1.0;
    double runnerUp = -1.0;
    bool accepted = false;
};

// Tells which hero a scoreboard portrait shows, from the API's hero card art (280x380, transparent
// background). The scoreboard draws that art with its top-left corner in the portrait box, at a
// size that differs per hero (measured: 0.25x for Abrams, Infernus and Lash, 0.29x for Venator).
// So every scale in a range is tried: quickly in grey at half size, then in colour at full size for
// the few best heroes. Only the top of the art is used: the level badge covers the rest.
//
// File name without extension = hero id; "@" adds a variant (haze@skin2.png -> "haze"). A picture
// too small to be card art counts as a crop from the screen, used 1:1 at reference_height.
class HeroRecognizer {
public:
    explicit HeroRecognizer(HeroConfig cfg = {});

    // `known` non-empty: ignores files for other ids, such as leftovers of an older download.
    size_t loadTemplates(const std::filesystem::path& dir, const std::set<std::string>& known = {});

    size_t heroCount() const { return heroes_.size(); }

    void setIconScale(double scale) { cfg_.iconScale = scale; }

    HeroVerdict recognize(const cv::Mat& frameBgr, cv::Rect box) const;

private:
    struct Variant {
        cv::Mat color;
        cv::Mat colorMask;   // empty = every pixel counts
        cv::Mat coarse;      // grey, half size
        cv::Mat coarseMask;
    };
    struct Hero {
        std::string id;
        std::vector<Variant> variants;  // card art: one per scale, ascending
    };

    static Variant makeVariant(const cv::Mat& image, const cv::Mat& mask);

    HeroConfig cfg_;
    std::vector<Hero> heroes_;
};
