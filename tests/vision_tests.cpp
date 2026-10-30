// End-to-end test on synthetic scoreboards: the real VisionEngine, PlayerAnalysis and
// counters.json, with the row/portrait layout from the shipped config.json.
#include "Config.h"
#include "CounterDatabase.h"
#include "HeroRecognizer.h"
#include "PlayerAnalysis.h"
#include "VisionEngine.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++checks;                                                         \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

// Measured on a real 1080p scoreboard: 27 px item icons, two per row, 35 px apart, starting
// 10 px right of / 6 px below the top-left of each item box; hero card art (280x380) starts at the
// top of the screen, 5 px into the portrait box, drawn at 0.25-0.29x.
constexpr int kIcon = 27;
constexpr int kPitch = 35;
constexpr int kOffsetX = 10;
constexpr int kOffsetY = 6;
const cv::Size kArt(280, 380);
constexpr int kArtOffsetX = 5;
constexpr int kRefHeight = 1080;

// Knobs for robustness sweeps (environment variables), defaults = the normal test.
static double knob(const char* name, double fallback)
{
    const char* v = std::getenv(name);
    return v ? std::atof(v) : fallback;
}
// 0.4 makes synthetic 27 px icons score like the real ones did on a 1080p screenshot (0.95+).
static const double kBlur = knob("DBH_BLUR", 0.4);          // rendering softness (Gaussian sigma)
static const double kIconScale = knob("DBH_ICON_SCALE", 1.0); // on-screen size vs what templates expect

// Like a Deadlock item: a tile in its category colour with a white-ish glyph on it.
cv::Mat makeIcon(std::mt19937& rng, const cv::Scalar& category)
{
    std::uniform_int_distribution<int> tint(-12, 12), pos(3, kIcon - 4), rad(2, 5), glyph(200, 255);
    cv::Mat m(kIcon, kIcon, CV_8UC3,
              cv::Scalar(category[0] + tint(rng), category[1] + tint(rng), category[2] + tint(rng)));
    for (int k = 0; k < 5; ++k) {
        const cv::Scalar white(glyph(rng), glyph(rng), glyph(rng));
        const cv::Point a(pos(rng), pos(rng)), b(pos(rng), pos(rng));
        switch (k % 3) {
        case 0: cv::line(m, a, b, white, 2, cv::LINE_AA); break;
        case 1: cv::circle(m, a, rad(rng), white, cv::FILLED, cv::LINE_AA); break;
        default: cv::rectangle(m, a, a + cv::Point(rad(rng), rad(rng)), white, cv::FILLED); break;
        }
    }
    return m;
}

// Like the API's hero cards: a figure on a transparent background.
cv::Mat makeHeroArt(std::mt19937& rng)
{
    std::uniform_int_distribution<int> c(20, 235), px(0, kArt.width - 1), py(0, kArt.height - 1), rad(15, 55);
    cv::Mat bgr(kArt, CV_8UC3, cv::Scalar(c(rng), c(rng), c(rng)));
    for (int k = 0; k < 16; ++k) {
        cv::circle(bgr, {px(rng), py(rng)}, rad(rng), cv::Scalar(c(rng), c(rng), c(rng)), cv::FILLED, cv::LINE_AA);
    }
    cv::Mat alpha(kArt, CV_8U, cv::Scalar(0));
    cv::ellipse(alpha, {kArt.width / 2, kArt.height / 2}, {kArt.width / 2 - 5, kArt.height / 2 + 40}, 0, 0, 360,
                cv::Scalar(255), cv::FILLED);
    cv::Mat art;
    cv::merge(std::vector<cv::Mat>{bgr, alpha}, art);
    return art;
}

cv::Scalar categoryColor(const std::string& slot)
{
    if (slot == "weapon") return {40, 140, 230};    // BGR orange
    if (slot == "vitality") return {70, 175, 75};   // green
    return {190, 80, 150};                          // purple
}

// Paste `img` scaled for this resolution, softened like real rendering.
void paste(cv::Mat& frame, const cv::Mat& img, cv::Point topLeft, double scale)
{
    cv::Mat scaled;
    cv::resize(img, scaled, {}, scale, scale, cv::INTER_LINEAR);
    scaled.convertTo(scaled, -1, 0.95, 6.0);  // slight brightness/contrast shift
    cv::GaussianBlur(scaled, scaled, {0, 0}, kBlur);
    scaled.copyTo(frame(cv::Rect(topLeft, scaled.size())));
}

// Paste hero art without its transparent part, clipped to the frame.
void pasteArt(cv::Mat& frame, const cv::Mat& art, cv::Point topLeft, double scale)
{
    cv::Mat scaled;
    cv::resize(art, scaled, {}, scale, scale, cv::INTER_AREA);
    std::vector<cv::Mat> channels;
    cv::split(scaled, channels);
    const cv::Mat mask = channels[3] > 127;
    channels.pop_back();
    cv::Mat color;
    cv::merge(channels, color);
    color.convertTo(color, -1, 0.95, 6.0);
    cv::GaussianBlur(color, color, {0, 0}, kBlur);
    const cv::Rect area = cv::Rect(topLeft, scaled.size()) & cv::Rect(0, 0, frame.cols, frame.rows);
    const cv::Rect source = area - topLeft;
    color(source).copyTo(frame(area), mask(source));
}

struct Scenario {
    std::string hero;                 // empty = empty portrait box
    std::vector<std::string> items;   // template file stems (variants allowed)
    double artScale = 0.25;           // how large the scoreboard draws this hero's card art
};

struct RunResult {
    std::map<int, std::set<std::string>> itemsPerRow;
    std::map<int, std::string> heroPerRow;
    std::vector<EnemyPlayer> players;
    std::vector<Suggestion> suggestions;
    double detectMs = 0;
    double heroMs = 0;
    double tunedScale = 1.0;
    double tuneMs = 0;
    int distractorHits = 0;
    int gapHits = 0;
};

int main(int, char** argv)
{
    const fs::path assets = argv[1];
    const fs::path tmp = fs::temp_directory_path() / "dbh_vision";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "items");
    fs::create_directories(tmp / "heroes");

    CounterDatabase db;
    CHECK(db.load(assets / "counters.json"));
    AppConfig cfg = loadConfig(assets / "config.json").config;
    cfg.downscale = knob("DBH_DOWNSCALE", cfg.downscale);
    std::printf("threads %d, downscale %.2f, blur %.1f, icon scale %.2f\n", cv::getNumThreads(), cfg.downscale, kBlur, kIconScale);

    std::ifstream in(assets / "counters.json");
    const auto j = nlohmann::json::parse(in);

    // ---- Image IO through a path cv::imread/imwrite could not open on Windows ---------------
    {
        const fs::path odd = tmp / "zażółć_Жук";
        fs::create_directories(odd);
        cv::Mat img(10, 12, CV_8UC3, cv::Scalar(10, 200, 30));
        CHECK(writeImage(odd / "x.png", img));
        const cv::Mat back = readImage(odd / "x.png");
        CHECK(back.size() == img.size() && cv::norm(back, img, cv::NORM_INF) == 0);
        CHECK(readImage(odd / "missing.png").empty());
        VisionEngine e({});
        CHECK(e.loadTemplates(odd) == 1);
    }

    // ---- Templates: every real item id and hero id ---------------------------------------
    std::mt19937 rng(1234);
    std::map<std::string, cv::Mat> icons, heroArt;
    for (const auto& [id, node] : j["items"].items()) {
        icons[id] = makeIcon(rng, categoryColor(node.value("slot", "")));
        cv::imwrite((tmp / "items" / (id + ".png")).string(), icons[id]);
    }
    // A variant: a second look of Metal Skin must still report "metal_skin".
    icons["metal_skin@alt"] = makeIcon(rng, categoryColor("vitality"));
    cv::imwrite((tmp / "items" / "metal_skin@alt.png").string(), icons["metal_skin@alt"]);
    std::ofstream(tmp / "items" / "notes.txt") << "not an image";  // must be ignored

    for (const auto& [id, node] : j["heroes"].items()) {
        heroArt[id] = makeHeroArt(rng);
        cv::imwrite((tmp / "heroes" / (id + ".png")).string(), heroArt[id]);
    }
    // A hero no template exists for: must not be reported as anything.
    const cv::Mat strangerArt = makeHeroArt(rng);

    // ---- Hero templates: ids, leftovers of older downloads, screen crops --------------------
    {
        const fs::path dir = tmp / "hero_files";
        fs::create_directories(dir);
        cv::imwrite((dir / "abrams.png").string(), heroArt.at("abrams"));
        cv::imwrite((dir / "atlas.png").string(), heroArt.at("abrams"));  // same art under an unknown id
        cv::imwrite((dir / "haze.png").string(), heroArt.at("haze"));
        cv::imwrite((dir / "haze@crop.png").string(), cv::Mat(66, 70, CV_8UC3, cv::Scalar(30, 90, 200)));
        const std::set<std::string> known = db.heroIds();
        CHECK(known.size() == j["heroes"].size());
        HeroRecognizer h;
        CHECK(h.loadTemplates(dir) == 3);
        CHECK(h.loadTemplates(dir, known) == 2);

        // With "atlas" gone, the portrait has no equal rival and is accepted.
        cv::Mat frame(1080, 1920, CV_8UC3, cv::Scalar(30, 20, 25));
        const cv::Rect box(1066, 0, 80, 72);
        pasteArt(frame, heroArt.at("abrams"), {box.x + kArtOffsetX, 0}, 0.25);
        const HeroVerdict v = h.recognize(frame, box);
        CHECK(v.accepted && v.id == "abrams");

        // Unknown ids allowed: two heroes with the same art are too close to call.
        CHECK(h.loadTemplates(dir) == 3);
        const HeroVerdict twins = h.recognize(frame, box);
        CHECK(!twins.accepted && twins.score > 0.8);

        // A black screen is nobody, and a flat crop (haze@crop) never wins.
        CHECK(!h.recognize(cv::Mat(1080, 1920, CV_8UC3, cv::Scalar(0, 0, 0)), box).accepted);
        CHECK(twins.id != "haze");

        // No templates: nothing to recognise, and no crash.
        HeroRecognizer none;
        CHECK(none.loadTemplates(tmp / "does_not_exist") == 0);
        CHECK(!none.recognize(frame, box).accepted);
        CHECK(!h.recognize(frame, cv::Rect(5000, 5000, 10, 10)).accepted);
    }
    // Icons the engine has never seen: must not be reported as anything.
    std::vector<cv::Mat> distractors;
    for (int k = 0; k < 4; ++k) {
        distractors.push_back(makeIcon(rng, categoryColor(k % 2 ? "spirit" : "weapon")));
    }

    const std::vector<Scenario> enemies = {
        {"haze", {"glass_cannon", "critshot", "sharpshooter", "unstoppable", "metal_skin@alt"}},
        {"lady_geist", {"magic_burst", "escalating_exposure", "boundless_spirit", "tech_purge"}, 0.29},
        {"abrams", {"health", "chonky", "colossus", "healbane", "juggernaut"}},
        {"", {"toxic_bullets", "health_stimpak"}},
        {"seven", {"rapid_rounds", "improved_spirit", "endurance"}, 0.27},
        {"", {}},  // row 6 only holds distractors and a stranger's portrait
    };

    const auto run = [&](int height, double iconScale, bool tune) {
        RunResult r;
        const cv::Size size(height * 16 / 9, height);
        const double scale = static_cast<double>(height) / kRefHeight * iconScale;

        // Dark UI-like background with noise.
        cv::Mat frame(size, CV_8UC3);
        for (int y = 0; y < size.height; ++y) {
            frame.row(y).setTo(cv::Scalar(18 + 20 * y / size.height, 16, 22));
        }
        cv::Mat noise(size, CV_8UC3);
        cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(4));
        frame += noise;

        std::vector<cv::Rect> rows, boxes;
        for (const auto& b : cfg.enemyRows) rows.push_back(toPixels(b, size));
        for (const auto& b : cfg.enemyPortraits) boxes.push_back(toPixels(b, size));

        const int icon = static_cast<int>(kIcon * scale);
        // k-th icon of an item strip: two per row, like the scoreboard.
        const auto slot = [&](const cv::Rect& box, size_t k) {
            return cv::Point(box.x + static_cast<int>((kOffsetX + static_cast<int>(k % 2) * kPitch) * scale),
                             box.y + static_cast<int>((kOffsetY + static_cast<int>(k / 2) * kPitch) * scale));
        };
        for (size_t i = 0; i < enemies.size(); ++i) {
            for (size_t k = 0; k < enemies[i].items.size(); ++k) {
                paste(frame, icons.at(enemies[i].items[k]), slot(rows[i], k), scale);
            }
            if (!enemies[i].hero.empty()) {
                pasteArt(frame, heroArt.at(enemies[i].hero), {boxes[i].x + static_cast<int>(kArtOffsetX * scale), 0},
                         enemies[i].artScale * scale);
            }
        }
        pasteArt(frame, strangerArt, {boxes[5].x + static_cast<int>(kArtOffsetX * scale), 0}, 0.25 * scale);
        for (size_t k = 0; k < distractors.size(); ++k) {
            paste(frame, distractors[k], slot(rows[5], k), scale);
        }
        // A real item centred in the gap between enemy 1's and enemy 2's strips: belongs to no one.
        // Row 4 (slot 6) is empty in both strips, so it never covers a real icon at any scale.
        const int gapCenter = (rows[0].br().x + rows[1].x) / 2;
        paste(frame, icons.at("rupture"), {gapCenter - icon / 2, slot(rows[0], 6).y}, scale);
        cv::imwrite((tmp / std::format("frame_{}_{:.2f}.png", height, iconScale)).string(), frame);

        cv::Rect searchArea;
        for (const auto& row : rows) searchArea = searchArea.empty() ? row : (searchArea | row);

        VisionEngine vision(itemVisionConfig(cfg));
        HeroRecognizer heroes(heroConfig(cfg));
        CHECK(vision.loadTemplates(tmp / "items") == j["items"].size() + 1);
        CHECK(heroes.loadTemplates(tmp / "heroes", db.heroIds()) == j["heroes"].size());

        if (tune) {
            const auto t = std::chrono::steady_clock::now();
            r.tunedScale = vision.tuneScale(frame, searchArea).scale;
            heroes.setIconScale(r.tunedScale);  // portraits are drawn at the same UI scale
            r.tuneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        }

        auto t0 = std::chrono::steady_clock::now();
        std::vector<Detection> detections = vision.detect(frame, searchArea);
        r.detectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        for (const auto& d : detections) {
            const cv::Point c(d.box.x + d.box.width / 2, d.box.y + d.box.height / 2);
            if (c.x >= rows[0].br().x && c.x < rows[1].x) ++r.gapHits;
        }
        assignToRows(detections, rows);
        for (const auto& d : detections) {
            if (d.slot == 5) ++r.distractorHits;
            r.itemsPerRow[d.slot].insert(d.itemId);
        }

        t0 = std::chrono::steady_clock::now();
        addHeroes(heroes, frame, boxes, rows.size(), detections);
        for (const auto& d : detections) {
            if (d.isHero) r.heroPerRow[d.slot] = d.itemId;
        }
        r.heroMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        SuggestInput input;
        r.players = describePlayers(detections, rows.size(), db, {cfg.archetypeShare, cfg.archetypeMinItems}, input);
        r.suggestions = db.suggest(input, static_cast<size_t>(cfg.maxSuggestions));
        return r;
    };

    // Shared checks of one run against the scenario.
    const auto verify = [&](const RunResult& r) {

        for (size_t i = 0; i < 5; ++i) {
            std::set<std::string> expected;
            for (auto id : enemies[i].items) {
                expected.insert(id.substr(0, id.find('@')));
            }
            const auto& got = r.itemsPerRow.count(static_cast<int>(i)) ? r.itemsPerRow.at(static_cast<int>(i))
                                                                       : std::set<std::string>{};
            if (got != expected) {
                std::printf("  row %zu items: expected %zu, got", i + 1, expected.size());
                for (const auto& g : got) std::printf(" %s", g.c_str());
                std::printf("\n");
            }
            CHECK(got == expected);
        }
        CHECK(r.distractorHits == 0);
        CHECK(r.gapHits == 1);                    // found by the search...
        for (const auto& [row, ids] : r.itemsPerRow) {
            CHECK(!ids.contains("rupture"));       // ...but dropped
        }

        CHECK(r.heroPerRow.size() == 4);
        CHECK(r.heroPerRow.count(0) && r.heroPerRow.at(0) == "haze");
        CHECK(r.heroPerRow.count(1) && r.heroPerRow.at(1) == "lady_geist");
        CHECK(r.heroPerRow.count(2) && r.heroPerRow.at(2) == "abrams");
        CHECK(!r.heroPerRow.count(3));            // empty portrait box
        CHECK(r.heroPerRow.count(4) && r.heroPerRow.at(4) == "seven");
        CHECK(!r.heroPerRow.count(5));            // a hero without a template

        CHECK(r.players.size() == 5);
        if (r.players.size() == 5) {
            CHECK(r.players[0].hero == "Haze" && r.players[0].build == "Gun build");
            CHECK(r.players[1].hero == "Lady Geist" && r.players[1].build == "Spirit build");
            CHECK(r.players[2].hero == "Abrams" && r.players[2].build == "Tank build");
            CHECK(r.players[3].hero.empty() && r.players[3].build.empty());
            CHECK(r.players[4].hero == "Seven" && r.players[4].build == "Hybrid");
            CHECK(!r.players[0].tips.empty() && !r.players[0].avoid.empty());
        }
        CHECK(r.suggestions.size() == static_cast<size_t>(cfg.maxSuggestions));

    };

    for (const int height : {1080, 1440}) {
        std::printf("--- %dp\n", height);
        const RunResult r = run(height, kIconScale, false);
        verify(r);
        std::printf("  item search: %.1f ms over %zu templates, heroes: %.1f ms over %zu portraits\n",
                    r.detectMs, j["items"].size() + 1, r.heroMs, cfg.enemyPortraits.size());
        for (const auto& p : r.players) {
            std::printf("  #%d %-10s %-12s items:%zu tips:%zu\n", p.slot + 1, p.hero.empty() ? "?" : p.hero.c_str(),
                        p.build.empty() ? "-" : p.build.c_str(), p.items.size(), p.tips.size());
        }
        for (const auto& s : r.suggestions) {
            std::printf("  buy %-20s %3d  (%zu answers)\n", s.item.c_str(), s.score, s.answers.size());
        }
    }

    // ---- Icons drawn at the wrong size: plain search fails, F6 tuning recovers it ---------
    for (const double actual : {0.88, 0.95, 1.12}) {
        const RunResult plain = run(1080, actual, false);
        size_t plainFound = 0;
        for (const auto& [row, ids] : plain.itemsPerRow) plainFound += ids.size();
        std::printf("--- icons at %.2fx: untuned finds %zu of 19 items\n", actual, plainFound);

        const RunResult tuned = run(1080, actual, true);
        std::printf("    tuned to %.4f in %.0f ms\n", tuned.tunedScale, tuned.tuneMs);
        // Within about one template pixel (~4% at 27 px): finer differences round to the same size.
        CHECK(std::abs(tuned.tunedScale - actual) <= 0.04);
        verify(tuned);
    }

    // ---- Margin: how far the right template is from the best wrong one --------------------
    {
        cv::Mat frame = cv::imread((tmp / "frame_1080_1.00.png").string());
        const double s = cfg.downscale;
        cv::Mat search;
        const cv::Rect row = toPixels(cfg.enemyRows[0], frame.size());
        cv::resize(frame(row), search, {}, s, s, cv::INTER_AREA);
        double worstTrue = 1.0, bestWrong = -1.0;
        for (const auto& [id, img] : icons) {
            cv::Mat t, resp;
            cv::resize(img, t, {}, s, s, cv::INTER_AREA);
            cv::matchTemplate(search, t, resp, cv::TM_CCOEFF_NORMED);
            double mx;
            cv::minMaxLoc(resp, nullptr, &mx);
            const bool inRow = std::ranges::find(enemies[0].items, id) != enemies[0].items.end();
            if (inRow) worstTrue = std::min(worstTrue, mx);
            else bestWrong = std::max(bestWrong, mx);
        }
        std::printf("--- row 1 margin: weakest real match %.3f, strongest wrong template %.3f, threshold %.2f\n",
                    worstTrue, bestWrong, cfg.matchThreshold);
        CHECK(worstTrue >= cfg.matchThreshold);
        CHECK(bestWrong < cfg.matchThreshold);
    }

    std::printf(failures == 0 ? "ALL %d PASSED\n" : "%d of %d FAILED\n", failures == 0 ? checks : failures, checks);
    return failures == 0 ? 0 : 1;
}
