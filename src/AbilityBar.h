#pragma once

#include "HeroRecognizer.h"

#include <opencv2/core.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <string>

// Tells which hero you play from your HUD: the ability bar at the bottom of the screen shows your
// hero's four abilities, always in the same place, and no two heroes share them. This needs nothing
// from you, and works whichever column of your team you are in.
//
// Reads assets/abilities/<hero id>_<1-4>.png, the API's white ability glyphs, downloaded by
// tools/fetch_heroes.py.
class AbilityBar {
public:
    // `known` non-empty: ignores files for other ids, such as leftovers of an older download.
    size_t load(const std::filesystem::path& dir, const std::set<std::string>& known = {});

    size_t heroCount() const { return heroes_.size(); }

    // `score` is how well the three best abilities match, which leaves room for one under a cooldown
    // effect, and `runnerUp` the same for the next-best hero. `expected`, when given, is tried alone
    // first, which spares most of the work once your hero is known.
    HeroVerdict recognize(const cv::Mat& frameBgr, const std::string& expected = {}) const;

private:
    struct Glyph {
        cv::Mat gray;  // dark on light, like the HUD
        cv::Mat mask;
    };
    using Glyphs = std::array<Glyph, 4>;

    std::map<std::string, Glyphs> heroes_;
};
