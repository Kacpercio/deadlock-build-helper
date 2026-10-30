// Runs the helper's whole recognition pipeline on a saved screenshot - no game, no Windows APIs:
//
//   screenshot_check <screenshot.png> <assets dir> [--threads n] [--out annotated.png]
//                    [--save-name column file.png] [--my-name file.png]
//
// Uses config.json (+ calibration.json), the templates and counters.json from <assets dir>,
// exactly like the app, for both teams.
// --save-name saves the nickname in a column of your team; --my-name looks for a saved one.
#include "AbilityBar.h"
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

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: screenshot_check <screenshot.png> <assets dir> [--threads n] [--out annotated.png]\n");
        return 2;
    }
    const fs::path shot = argv[1];
    const fs::path assets = argv[2];
    int saveSlot = -1;
    fs::path nameFile;
    fs::path out = "screenshot_check.png";
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--save-name") == 0 && i + 2 < argc) {
            saveSlot = std::atoi(argv[++i]);
            nameFile = argv[++i];
        } else if (std::strcmp(argv[i], "--my-name") == 0 && i + 1 < argc) {
            nameFile = argv[++i];
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
    items.loadTemplates(resolvePath(root, cfg.templateDir));
    items.addActiveItems(resolvePath(root, cfg.templateDir).parent_path() / "icons");
    const size_t itemTemplates = items.templateCount();
    const size_t heroTemplates = heroes.loadTemplates(resolvePath(root, cfg.heroTemplateDir), counters.heroIds());
    std::printf("%dx%d screenshot, %zu item templates, %zu hero templates, threshold %.2f, downscale %.2f\n",
                frame.cols, frame.rows, itemTemplates, heroTemplates, cfg.matchThreshold, cfg.downscale);

    if (!nameFile.empty()) {
        const auto names = toPixels(cfg.allyNames, frame.size());
        if (saveSlot >= 0) {
            saveNameTemplate(nameFile, makeNameTemplate(frame, names.at(static_cast<size_t>(saveSlot))));
        }
        double score = 0.0;
        const int found = findName(frame, names, loadNameTemplate(nameFile), &score);
        std::printf("your nickname: column %d of your team, score %.3f\n", found, score);
    }

    AbilityBar bar;
    bar.load(assets / "abilities", counters.heroIds());
    const HeroVerdict you = bar.recognize(frame);
    std::printf("you, by your abilities: %s %.3f, next %.3f: %s (%zu heroes loaded)\n", you.id.c_str(), you.score, you.runnerUp,
                you.accepted ? "recognised" : "rejected", bar.heroCount());

    double artScore = 0.0;
    const int artColumn = you.accepted ? heroes.findHero(frame, toPixels(mirrored(cfg.enemyPortraits), frame.size()), you.id, &artScore) : -1;
    std::printf("you, by your hero's art: column %d of your team, score %.3f\n", artColumn, artScore);

    cv::Mat annotated = frame.clone();
    const auto rowCount = static_cast<int>(cfg.enemyRows.size());
    const auto start = std::chrono::steady_clock::now();
    std::vector<HeroVerdict> verdicts;
    const std::vector<Detection> detections = scanBoard(frame, items, heroes, cfg, &verdicts);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("\nboard read in %.0f ms, %zu hits\n", ms, detections.size());

    int teamSouls[2] = {0, 0};
    for (int team = 0; team < 2; ++team) {
        std::vector<Detection> side;
        for (const auto& d : detections) {
            if (d.slot / rowCount == team) {
                side.push_back(d);
                side.back().slot %= rowCount;
            }
        }
        SuggestInput input;
        const auto players = describePlayers(side, cfg.enemyRows.size(), counters,
                                             {cfg.archetypeShare, cfg.archetypeMinItems}, input);
        std::printf("\n== %s\n", team == 0 ? "enemies" : "your team");
        for (int i = 0; i < rowCount; ++i) {
            const HeroVerdict& v = verdicts[static_cast<size_t>(team * rowCount + i)];
            std::printf("  portrait #%d: %-14s %.3f, next %.3f: %s\n", i + 1, v.id.c_str(), v.score, v.runnerUp,
                        v.accepted ? "recognised" : "rejected");
        }
        for (const auto& p : players) {
            std::printf("  #%d %s%s%s, %zu items + %d not recognised\n", p.slot + 1, p.hero.empty() ? "(hero not recognised)" : p.hero.c_str(),
                        p.build.empty() ? "" : " - ", p.build.c_str(), p.itemIds.size(), p.unknownItems);
            for (const auto& d : side) {
                if (d.slot == p.slot && !d.isHero) {
                    std::printf("      %-28s %.3f at %d,%d\n", counters.displayName(d.itemId).c_str(), d.score, d.box.x, d.box.y);
                    teamSouls[team] += counters.costOf(d.itemId);
                }
            }
        }
        if (team == 0) {
            for (const auto& s : counters.suggest(input, static_cast<size_t>(cfg.maxSuggestions))) {
                std::printf("  buy %-22s +%-3d %s\n", s.item.c_str(), s.score, s.reason.c_str());
            }
        }
    }
    std::printf("\nitems recognised, your team against the enemy: %d - %d souls\n", teamSouls[1], teamSouls[0]);

    for (const auto& d : detections) {
        const cv::Scalar color = d.isHero ? cv::Scalar(255, 80, 255) : cv::Scalar(0, 255, 0);
        cv::rectangle(annotated, d.box, color, 1);
        cv::putText(annotated, d.itemId, d.box.tl() + cv::Point(0, -2), cv::FONT_HERSHEY_PLAIN, 0.7, color, 1);
    }
    writeImage(out, annotated);
    std::printf("\nannotated image: %s\n", out.string().c_str());
    return 0;
}
