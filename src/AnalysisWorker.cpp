#include "AnalysisWorker.h"

#include "PlayerAnalysis.h"

#include <opencv2/core/utility.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <map>
#include <utility>

namespace fs = std::filesystem;

namespace {

// Long enough to cover a run of scans (calibration rescans every 400 ms), short enough that the
// capture is closed for most of a match.
constexpr auto kKeepCaptureOpen = std::chrono::seconds(3);

}  // namespace

AnalysisWorker::AnalysisWorker(ScreenCapture& capture, AppConfig cfg, fs::path root)
    : capture_(capture)
    , root_(std::move(root))
    , monitorIndex_(cfg.monitorIndex)
    , cfg_(std::move(cfg))
    , vision_(itemVisionConfig(cfg_))
    , heroes_(heroConfig(cfg_))
{
    applyThreadLimit();
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

void AnalysisWorker::requestPrepare()
{
    {
        std::lock_guard lock(mutex_);
        pendingPrepare_ = true;
    }
    wake_.notify_one();
}

void AnalysisWorker::requestSettings(AppConfig settings)
{
    {
        std::lock_guard lock(mutex_);
        pendingSettings_ = std::move(settings);
    }
    wake_.notify_one();
}

void AnalysisWorker::requestResetSettings()
{
    {
        std::lock_guard lock(mutex_);
        pendingResetSettings_ = true;
    }
    wake_.notify_one();
}

void AnalysisWorker::requestPickMe(int allySlot)
{
    {
        std::lock_guard lock(mutex_);
        pendingPick_ = allySlot;
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

void AnalysisWorker::applyThreadLimit() const
{
    cv::setNumThreads(cfg_.scanThreads);
}

void AnalysisWorker::run(std::stop_token stop)
{
    // The game comes first: a scan may take longer, but should not take frames from it.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (true) {
        bool scan = false;
        bool debug = false;
        bool reloadFirst = false;
        bool tune = false;
        bool prepare = false;
        bool resetSettingsFile = false;
        std::optional<AppConfig> settings;
        std::optional<int> pick;
        {
            std::unique_lock lock(mutex_);
            const auto pending = [this] {
                return pendingScan_ || pendingReload_ || pendingTune_ || pendingPrepare_ || pendingSettings_ ||
                       pendingResetSettings_ || pendingPick_;
            };
            if (!wake_.wait_for(lock, stop, kKeepCaptureOpen, pending)) {
                if (stop.stop_requested()) {
                    return;
                }
                capture_.suspend();
                if (!wake_.wait(lock, stop, pending)) {
                    return;  // stop requested
                }
            }
            scan = pendingScan_;
            debug = pendingDebug_;
            reloadFirst = pendingReload_;
            tune = pendingTune_;
            pendingScan_ = pendingDebug_ = pendingReload_ = pendingTune_ = false;
            prepare = std::exchange(pendingPrepare_, false);
            resetSettingsFile = std::exchange(pendingResetSettings_, false);
            settings = std::exchange(pendingSettings_, std::nullopt);
            pick = std::exchange(pendingPick_, std::nullopt);
        }

        if (prepare) {
            capture_.prepare();
        }

        const fs::path configFile = root_ / "assets" / "config.json";
        if (resetSettingsFile) {
            reloadFirst = resetSettings(configFile) || reloadFirst;
        } else if (settings) {
            reloadFirst = saveSettings(configFile, *settings) || reloadFirst;
        }
        if (pick) {
            pickMe(*pick);
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
    applyThreadLimit();
    std::string summary = "applied: " + loadResources();
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
    const ScaleTune t = vision_.tuneScale(*frame, toPixels(cfg_.enemyRows, frame->size()));
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
    myName_ = loadNameTemplate(root_ / "assets" / "my_name.png");

    return std::format("{} item / {} hero templates, {} items + {} heroes in counters{}, {} enemy rows",
                       templates, heroTemplates, counters_.size(), counters_.heroCount(),
                       countersLoaded ? "" : " (counters.json not loaded)", cfg_.enemyRows.size());
}

void AnalysisWorker::pickMe(int allySlot)
{
    const fs::path file = root_ / "assets" / "my_name.png";
    if (allySlot < 0) {
        std::error_code ec;
        fs::remove(file, ec);
        myName_ = {};
        publishConfig("nickname forgotten");
        return;
    }
    const std::vector<cv::Rect> names = toPixels(cfg_.allyNames, lastFrameSize_);
    if (lastPreview_.empty() || static_cast<size_t>(allySlot) >= names.size()) {
        publishConfig("no scoreboard to read your nickname from: hold TAB in a match first");
        return;
    }
    NameTemplate name = makeNameTemplate(lastPreview_, names[static_cast<size_t>(allySlot)]);
    if (name.empty() || !saveNameTemplate(file, name)) {
        publishConfig("could not save a nickname from that column");
        return;
    }
    myName_ = std::move(name);
    publishConfig("nickname saved");
}

void AnalysisWorker::addMe(const cv::Mat& frame, AnalysisResult& result, SuggestInput& input)
{
    result.nicknameSet = !myName_.empty();
    if (myName_.empty()) {
        return;
    }
    result.mySlot = findName(frame, toPixels(cfg_.allyNames, frame.size()), myName_);
    if (result.mySlot < 0) {
        return;
    }

    // Your column is laid out like an enemy's, on the other side of the screen.
    const auto slot = static_cast<size_t>(result.mySlot);
    const std::vector<cv::Rect> columns = toPixels(mirrored(cfg_.enemyRows), frame.size());
    const std::vector<cv::Rect> portraits = toPixels(mirrored(cfg_.enemyPortraits), frame.size());
    if (slot >= columns.size()) {
        return;
    }
    std::vector<Detection> mine = vision_.scan(frame, {columns[slot]});
    for (const auto& d : mine) {
        input.owned.insert(d.itemId);
    }
    result.myItems = mine.size();
    for (const auto& d : mine) {
        result.mySouls += counters_.costOf(d.itemId);
    }
    if (slot < portraits.size()) {
        if (const HeroVerdict v = heroes_.recognize(frame, portraits[slot]); v.accepted) {
            result.myHero = counters_.heroName(v.id);
        }
    }
}

void AnalysisWorker::publishConfig(std::string summary)
{
    std::lock_guard lock(mutex_);
    publishedConfig_.config = cfg_;
    publishedConfig_.summary = std::move(summary);
    publishedConfig_.nicknameSet = !myName_.empty();
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

    std::vector<Detection> detections = rows.size() > 1 ? vision_.scan(*frame, rows) : vision_.detect(*frame, searchArea);
    assignToRows(detections, rows);

    const std::vector<HeroVerdict> verdicts = addHeroes(heroes_, *frame, portraits, rows.size(), detections);

    if (vision_.templateCount() == 0) {
        result.warning = "no item icons: run tools/fetch_icons.py";
    } else if (heroes_.heroCount() == 0 && !portraits.empty()) {
        result.warning = "no hero portraits: run tools/fetch_heroes.py";
    }

    SuggestInput input;
    addMe(*frame, result, input);
    result.players = describePlayers(detections, rows.size(), counters_,
                                     {cfg_.archetypeShare, cfg_.archetypeMinItems}, input);
    for (const auto& [id, n] : input.items) {
        result.enemyItems.push_back({id, counters_.displayName(id), n});
    }
    std::ranges::stable_sort(result.enemyItems, std::greater{}, &EnemyItem::count);
    // How far along you are: your own items, or if unknown what the average enemy owns.
    input.spentSouls = result.mySlot >= 0 ? result.mySouls : 0;
    if (result.mySlot < 0 && !result.players.empty()) {
        for (const auto& p : result.players) {
            for (const auto& id : p.itemIds) {
                input.spentSouls += counters_.costOf(id);
            }
        }
        input.spentSouls /= static_cast<int>(result.players.size());
    }
    result.suggestions = counters_.suggest(input, static_cast<size_t>(cfg_.maxSuggestions));
    result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    // Kept for the menu's preview and for picking your nickname. A copy: the debug images draw on the frame.
    constexpr double kPreviewShare = 0.62;  // enough for the scoreboard's item strips
    lastPreview_ = (*frame)(cv::Rect(0, 0, frame->cols, static_cast<int>(frame->rows * kPreviewShare))).clone();
    lastFrameSize_ = frame->size();
    result.preview = lastPreview_;
    result.frameSize = lastFrameSize_;

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
