#include "AnalysisWorker.h"

#include "PlayerAnalysis.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <map>

namespace fs = std::filesystem;

AnalysisWorker::AnalysisWorker(ScreenCapture& capture, AppConfig cfg, fs::path root)
    : capture_(capture)
    , root_(std::move(root))
    , monitorIndex_(cfg.monitorIndex)
    , cfg_(std::move(cfg))
    , vision_(itemVisionConfig(cfg_))
    , heroes_(heroConfig(cfg_))
{
    publishConfig("loaded: " + loadResources());
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void AnalysisWorker::requestScan(bool saveDebugImages)
{
    {
        std::lock_guard lock(mutex_);
        pendingScan_ = true;
        pendingDebug_ = pendingDebug_ || saveDebugImages;
        busy_ = true;
    }
    wake_.notify_one();
}

void AnalysisWorker::requestReload()
{
    {
        std::lock_guard lock(mutex_);
        pendingReload_ = true;
    }
    wake_.notify_one();
}

void AnalysisWorker::requestScaleTune()
{
    {
        std::lock_guard lock(mutex_);
        pendingTune_ = true;
        busy_ = true;
    }
    wake_.notify_one();
}

std::optional<AnalysisResult> AnalysisWorker::takeIfNewer(std::uint64_t knownVersion)
{
    std::lock_guard lock(mutex_);
    if (latest_.version <= knownVersion) {
        return std::nullopt;
    }
    return latest_;
}

std::optional<ConfigSnapshot> AnalysisWorker::takeConfigIfNewer(std::uint64_t knownVersion)
{
    std::lock_guard lock(mutex_);
    if (publishedConfig_.version <= knownVersion) {
        return std::nullopt;
    }
    return publishedConfig_;
}

void AnalysisWorker::run(std::stop_token stop)
{
    while (true) {
        bool scan = false;
        bool debug = false;
        bool reloadFirst = false;
        bool tune = false;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return pendingScan_ || pendingReload_ || pendingTune_; })) {
                return;  // stop requested
            }
            scan = pendingScan_;
            debug = pendingDebug_;
            reloadFirst = pendingReload_;
            tune = pendingTune_;
            pendingScan_ = pendingDebug_ = pendingReload_ = pendingTune_ = false;
        }

        if (reloadFirst) {
            try {
                reload();
            } catch (const std::exception& e) {
                publishConfig(std::string("reload failed: ") + e.what());
            }
        }

        if (tune) {
            try {
                tuneScale();
            } catch (const std::exception& e) {
                publishConfig(std::string("scale tuning failed: ") + e.what());
            }
            scan = true;  // show what the new scale finds right away
        }

        if (scan) {
            AnalysisResult result;
            try {
                result = analyze(debug);
            } catch (const std::exception& e) {
                result.error = std::string("scan failed: ") + e.what();
                std::cerr << "[worker] " << result.error << '\n';
            }
            std::lock_guard lock(mutex_);
            result.version = latest_.version + 1;
            latest_ = std::move(result);
        }

        std::lock_guard lock(mutex_);
        busy_ = pendingScan_;
    }
}

void AnalysisWorker::reload()
{
    ConfigLoad loaded = loadConfig(root_ / "assets" / "config.json");
    if (!loaded.error.empty()) {
        std::cerr << "[reload] " << loaded.error << '\n';
        publishConfig("reload failed, kept previous config: " + loaded.error);
        return;
    }

    cfg_ = std::move(loaded.config);
    std::string summary = "reloaded: " + loadResources();
    if (cfg_.monitorIndex != monitorIndex_) {
        summary += " (monitor_index needs a restart)";
    }
    publishConfig(std::move(summary));
}

void AnalysisWorker::tuneScale()
{
    auto frame = capture_.grab();
    if (!frame) {
        publishConfig("scale tuning failed: screen capture failed");
        return;
    }
    const ScaleTune t = vision_.tuneScale(*frame, boundingBox(toPixels(cfg_.enemyRows, frame->size())));
    if (t.detections == 0) {
        publishConfig("scale tuning found no icons: hold the scoreboard open and check enemy_rows");
        return;
    }
    heroes_.setIconScale(t.scale);  // portraits are drawn by the same UI at the same scale
    cfg_.iconScale = t.scale;
    const bool saved = saveCalibration(root_ / "assets" / "config.json", t.scale);
    publishConfig(std::format("icon scale tuned to {:.3f} ({} icons, mean score {:.2f}){}", t.scale, t.detections,
                              t.meanScore, saved ? ", saved to calibration.json" : ", could not save calibration.json"));
}

std::string AnalysisWorker::loadResources()
{
    VisionEngine vision(itemVisionConfig(cfg_));
    const size_t templates = vision.loadTemplates(resolvePath(root_, cfg_.templateDir));
    vision_ = std::move(vision);

    CounterDatabase counters;
    const bool countersLoaded = counters.load(resolvePath(root_, cfg_.countersFile));
    if (countersLoaded) {
        counters_ = std::move(counters);
    }

    HeroRecognizer heroes(heroConfig(cfg_));
    const size_t heroTemplates = cfg_.enemyPortraits.empty()
                                     ? 0
                                     : heroes.loadTemplates(resolvePath(root_, cfg_.heroTemplateDir), counters_.heroIds());
    heroes_ = std::move(heroes);

    return std::format("{} item / {} hero templates, {} items + {} heroes in counters{}, {} enemy rows",
                       templates, heroTemplates, counters_.size(), counters_.heroCount(),
                       countersLoaded ? "" : " (counters.json not loaded)", cfg_.enemyRows.size());
}

void AnalysisWorker::publishConfig(std::string summary)
{
    std::lock_guard lock(mutex_);
    publishedConfig_.config = cfg_;
    publishedConfig_.summary = std::move(summary);
    ++publishedConfig_.version;
}

AnalysisResult AnalysisWorker::analyze(bool debug)
{
    const auto start = std::chrono::steady_clock::now();
    AnalysisResult result;

    auto frame = capture_.grab();
    if (!frame) {
        result.error = "screen capture failed";
        return result;
    }

    // Empty rects are kept, so indices still match the config.
    const std::vector<cv::Rect> rows = toPixels(cfg_.enemyRows, frame->size());
    const std::vector<cv::Rect> portraits = toPixels(cfg_.enemyPortraits, frame->size());

    // Search the bounding box of all rows once, then attribute each hit to the row it sits in.
    const cv::Rect searchArea = boundingBox(rows);

    std::vector<Detection> detections = vision_.detect(*frame, searchArea);
    assignToRows(detections, rows);

    const std::vector<HeroVerdict> verdicts = addHeroes(heroes_, *frame, portraits, rows.size(), detections);

    if (vision_.templateCount() == 0) {
        result.warning = "no item icons: run tools/fetch_icons.py";
    } else if (heroes_.heroCount() == 0 && !portraits.empty()) {
        result.warning = "no hero portraits: run tools/fetch_heroes.py";
    }

    SuggestInput input;
    result.players = describePlayers(detections, rows.size(), counters_,
                                     {cfg_.archetypeShare, cfg_.archetypeMinItems}, input);
    for (const auto& [id, n] : input.items) {
        result.enemyItems.push_back({id, counters_.displayName(id), n});
    }
    std::ranges::stable_sort(result.enemyItems, std::greater{}, &EnemyItem::count);
    result.suggestions = counters_.suggest(input, static_cast<size_t>(cfg_.maxSuggestions));
    result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    if (debug) {
        saveDebugImages(*frame, searchArea, rows, portraits, detections);
        for (size_t i = 0; i < verdicts.size(); ++i) {
            const HeroVerdict& v = verdicts[i];
            std::cout << std::format("[debug] enemy #{}: {} {:.3f}, next {:.3f}: {}\n", i + 1, v.id.empty() ? "?" : v.id,
                                     v.score, v.runnerUp, v.accepted ? "recognised" : "rejected");
        }
    }
    result.detections = std::move(detections);
    return result;
}

void AnalysisWorker::saveDebugImages(cv::Mat& frame, const cv::Rect& searchArea, const std::vector<cv::Rect>& rows,
                                     const std::vector<cv::Rect>& portraits,
                                     const std::vector<Detection>& detections) const
{
    // Clean crop first: this is the image to cut item icons and hero portraits from.
    cv::Rect cropArea = searchArea;
    for (const auto& p : portraits) {
        if (!p.empty()) {
            cropArea = cropArea.empty() ? p : (cropArea | p);
        }
    }
    if (!cropArea.empty()) {
        writeImage(root_ / "debug_roi.png", frame(cropArea));
    }

    const cv::Scalar yellow(0, 215, 255);
    const cv::Scalar cyan(255, 220, 0);
    const cv::Scalar green(0, 255, 0);
    const cv::Scalar magenta(255, 80, 255);
    for (size_t i = 0; i < rows.size(); ++i) {
        cv::rectangle(frame, rows[i], yellow, 2);
        cv::putText(frame, std::format("#{}", i + 1), rows[i].tl() + cv::Point(4, 16),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, yellow, 1, cv::LINE_AA);
    }
    for (const auto& p : portraits) {
        cv::rectangle(frame, p, cyan, 2);
    }
    for (const auto& d : detections) {
        const cv::Scalar& color = d.isHero ? magenta : green;
        cv::rectangle(frame, d.box, color, 2);
        cv::putText(frame, std::format("{} {:.2f}", d.itemId, d.score), d.box.tl() + cv::Point(0, -4),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, color, 1, cv::LINE_AA);
    }
    writeImage(root_ / "debug_capture.png", frame);

    std::cout << "[debug] saved debug_capture.png and debug_roi.png to " << root_.string() << '\n';
}
