// Runs the helper's whole recognition pipeline on a saved screenshot - no game, no Windows APIs:
//
//   screenshot_check <screenshot.png> <assets dir> [--allies] [--thorough] [--threads n] [--out annotated.png]
//
// Uses config.json (+ calibration.json), the templates and counters.json from <assets dir>,
// exactly like the app. --allies also reads your own team's columns (the enemy boxes mirrored
// around the screen centre), handy when the screenshot only shows items on your side.
// --thorough uses the full template search instead of the fast scan, to compare the two.
#include "Config.h"
#include "CounterDatabase.h"
#include "HeroRecognizer.h"
#include "PlayerAnalysis.h"
#include "VisionEngine.h"

#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<NormRect> mirrored(std::vector<NormRect> boxes)
{
    for (auto& b : boxes) {
        b.x = 1.0 - b.x - b.w;
    }
    return boxes;
}

struct Side {
    const char* name;
    std::vector<NormRect> rows;
    std::vector<NormRect> portraits;
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: screenshot_check <screenshot.png> <assets dir> [--allies] [--thorough] [--threads n] [--out annotated.png]\n");
        return 2;
    }
    const fs::path shot = argv[1];
    const fs::path assets = argv[2];
    bool allies = false;
    bool thorough = false;
    fs::path out = "screenshot_check.png";
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allies") == 0) {
            allies = true;
        } else if (std::strcmp(argv[i], "--thorough") == 0) {
            thorough = true;
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            cv::setNumThreads(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        }
    }

    cv::Mat frame = readImage(shot);
    if (frame.empty()) {
        std::printf("cannot read %s\n", shot.string().c_str());
        return 1;
    }
    const ConfigLoad loaded = loadConfig(assets / "config.json");
    if (!loaded.error.empty()) {
        std::printf("config: %s (using defaults)\n", loaded.error.c_str());
    }
    const AppConfig& cfg = loaded.config;
    const fs::path root = assets.parent_path();

    VisionEngine items(itemVisionConfig(cfg));
    HeroRecognizer heroes(heroConfig(cfg));
    CounterDatabase counters;
    counters.load(resolvePath(root, cfg.countersFile));
    const size_t itemTemplates = items.loadTemplates(resolvePath(root, cfg.templateDir));
    const size_t heroTemplates = heroes.loadTemplates(resolvePath(root, cfg.heroTemplateDir), counters.heroIds());
    std::printf("%dx%d screenshot, %zu item templates, %zu hero templates, threshold %.2f, downscale %.2f\n",
                frame.cols, frame.rows, itemTemplates, heroTemplates, cfg.matchThreshold, cfg.downscale);

    std::vector<Side> sides{{"enemies", cfg.enemyRows, cfg.enemyPortraits}};
    if (allies) {
        sides.push_back({"your team", mirrored(cfg.enemyRows), mirrored(cfg.enemyPortraits)});
    }

    cv::Mat annotated = frame.clone();
    for (const Side& side : sides) {
        const auto rows = toPixels(side.rows, frame.size());
        const auto portraits = toPixels(side.portraits, frame.size());

        const auto start = std::chrono::steady_clock::now();
        std::vector<Detection> detections = thorough ? items.detect(frame, boundingBox(rows)) : items.scan(frame, rows);
        assignToRows(detections, rows);
        const auto verdicts = addHeroes(heroes, frame, portraits, rows.size(), detections);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        SuggestInput input;
        const auto players = describePlayers(detections, rows.size(), counters,
                                             {cfg.archetypeShare, cfg.archetypeMinItems}, input);
        std::printf("\n== %s: %zu hits in %.0f ms\n", side.name, detections.size(), ms);
        for (size_t i = 0; i < verdicts.size(); ++i) {
            std::printf("  portrait #%zu: %-14s %.3f, next %.3f: %s\n", i + 1, verdicts[i].id.c_str(), verdicts[i].score,
                        verdicts[i].runnerUp, verdicts[i].accepted ? "recognised" : "rejected");
        }
        for (const auto& p : players) {
            std::printf("  #%d %s%s%s\n", p.slot + 1, p.hero.empty() ? "(hero not recognised)" : p.hero.c_str(),
                        p.build.empty() ? "" : " - ", p.build.c_str());
            for (const auto& d : detections) {
                if (d.slot == p.slot && !d.isHero) {
                    std::printf("      %-28s %.3f\n", counters.displayName(d.itemId).c_str(), d.score);
                }
            }
            for (const auto& t : p.tips) {
                std::printf("      > %s\n", t.c_str());
            }
        }
        for (const auto& s : counters.suggest(input, static_cast<size_t>(cfg.maxSuggestions))) {
            std::printf("  buy %-22s +%-3d %s\n", s.item.c_str(), s.score, s.reason.c_str());
        }

        for (const auto& r : rows) cv::rectangle(annotated, r, {0, 215, 255}, 1);
        for (const auto& r : portraits) cv::rectangle(annotated, r, {255, 220, 0}, 1);
        for (const auto& d : detections) {
            const cv::Scalar color = d.isHero ? cv::Scalar(255, 80, 255) : cv::Scalar(0, 255, 0);
            cv::rectangle(annotated, d.box, color, 1);
            cv::putText(annotated, d.itemId, d.box.tl() + cv::Point(0, -2), cv::FONT_HERSHEY_PLAIN, 0.7, color, 1);
        }
    }
    writeImage(out, annotated);
    std::printf("\nannotated image: %s\n", out.string().c_str());
    return 0;
}
