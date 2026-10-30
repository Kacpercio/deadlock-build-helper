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

void AnalysisWorker::requestLearnIcon(std::string itemId, cv::Rect box)
{
    {
        std::lock_guard lock(mutex_);
        pendingLearn_ = std::pair(std::move(itemId), box);
    }
    wake_.notify_one();
}

void AnalysisWorker::requestSaveLastScan()
{
    {
        std::lock_guard lock(mutex_);
        pendingSaveScan_ = true;
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
        bool saveScan = false;
        bool resetSettingsFile = false;
        std::optional<AppConfig> settings;
        std::optional<int> pick;
        std::optional<std::pair<std::string, cv::Rect>> learn;
        {
            std::unique_lock lock(mutex_);
            const auto pending = [this] {
                return pendingScan_ || pendingReload_ || pendingTune_ || pendingPrepare_ || pendingSettings_ ||
                       pendingResetSettings_ || pendingPick_ || pendingLearn_ || pendingSaveScan_;
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
            saveScan = std::exchange(pendingSaveScan_, false);
            resetSettingsFile = std::exchange(pendingResetSettings_, false);
            settings = std::exchange(pendingSettings_, std::nullopt);
            pick = std::exchange(pendingPick_, std::nullopt);
            learn = std::exchange(pendingLearn_, std::nullopt);
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
        if (learn) {
            learnIcon(learn->first, learn->second);
        }
        if (saveScan) {
            saveLastScan();
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

size_t AnalysisWorker::loadItemTemplates(VisionEngine& vision) const
{
    const fs::path folder = resolvePath(root_, cfg_.templateDir);
    vision.loadTemplates(folder);
    return vision.addActiveItems(folder.parent_path() / "icons");
}

std::string AnalysisWorker::loadResources()
{
    VisionEngine vision(itemVisionConfig(cfg_));
    activeTemplates_ = loadItemTemplates(vision);
    const size_t templates = vision.templateCount();
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
    abilityBar_.load(root_ / "assets" / "abilities", counters_.heroIds());
    myName_ = loadNameTemplate(root_ / "assets" / "my_name.png");

    return std::format("{} item templates ({} active) / {} hero templates, {} items + {} heroes in counters{}, {} enemy rows",
                       templates, activeTemplates_, heroTemplates, counters_.size(), counters_.heroCount(),
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
    const std::vector<cv::Rect> names = toPixels(cfg_.allyNames, lastFrame_.size());
    if (lastFrame_.empty() || static_cast<size_t>(allySlot) >= names.size()) {
        publishConfig("no scoreboard to read your nickname from: hold TAB in a match first");
        return;
    }
    NameTemplate name = makeNameTemplate(lastFrame_, names[static_cast<size_t>(allySlot)]);
    if (name.empty() || !saveNameTemplate(file, name)) {
        publishConfig("could not save a nickname from that column");
        return;
    }
    myName_ = std::move(name);
    publishConfig("nickname saved");
}

void AnalysisWorker::saveLastScan()
{
    if (lastFrame_.empty()) {
        publishConfig("no scan to save yet: hold TAB in a match first");
        return;
    }
    const fs::path file = root_ / "debug_last_scan.png";
    publishConfig(writeImage(file, lastFrame_) ? "saved " + file.string() : "could not save " + file.string());
}

void AnalysisWorker::learnIcon(const std::string& itemId, cv::Rect box)
{
    box &= cv::Rect(0, 0, lastFrame_.cols, lastFrame_.rows);
    if (box.empty()) {
        publishConfig("nothing to learn that icon from: hold TAB in a match first");
        return;
    }
    const fs::path folder = resolvePath(root_, cfg_.templateDir);
    if (!writeImage(folder / (itemId + "@active.png"), lastFrame_(box))) {
        publishConfig("could not save that icon");
        return;
    }
    loadItemTemplates(vision_);
    publishConfig("learned " + counters_.displayName(itemId));
}

void AnalysisWorker::addMe(const cv::Mat& frame, AnalysisResult& result, SuggestInput& input)
{
    result.nicknameSet = !myName_.empty();

    // Two independent answers: the column with your nickname, and the column showing the art of the hero
    // your HUD says you play.
    double nameScore = 0.0;
    const int byName = myName_.empty() ? -1 : findName(frame, toPixels(cfg_.allyNames, frame.size()), myName_, &nameScore);
    const HeroVerdict look = abilityBar_.recognize(frame, lastMyHero_);
    int byArt = -1;
    double artScore = 0.0;
    if (look.accepted) {
        result.myHeroId = look.id;
        const auto shown = std::ranges::find(result.allies, look.id, &EnemyPlayer::heroId);
        byArt = shown != result.allies.end()
                    ? shown->slot
                    : heroes_.findHero(frame, toPixels(mirrored(cfg_.enemyPortraits), frame.size()), look.id, &artScore);
    }

    // When they disagree the nickname wins: you picked it yourself, and can pick again if it is wrong.
    if (byName >= 0) {
        result.mySlot = byName;
    } else if (byArt >= 0) {
        result.mySlot = byArt;
    } else if (const auto last = std::ranges::find(result.allies, lastMySlot_, &EnemyPlayer::slot);
               last != result.allies.end() && !last->heroId.empty() && last->heroId == lastMyHero_) {
        result.mySlot = lastMySlot_;
    }

    const std::string nameSays = byName >= 0 ? std::format("column {} ({:.2f})", byName + 1, nameScore)
                                             : std::string(myName_.empty() ? "not picked" : "not found");
    const std::string artSays = !look.accepted   ? std::format("HUD unclear ({:.2f})", look.score)
                                : byArt >= 0     ? std::format("{} in column {}", counters_.heroName(look.id), byArt + 1)
                                                 : std::format("{}, not in any column ({:.2f})", counters_.heroName(look.id), artScore);
    const char* verdict = byName >= 0 && byArt >= 0 ? (byName == byArt ? " | both agree" : " | THEY DISAGREE: click your nickname again") : "";
    result.howFound = std::format("nickname: {} | hero art: {}{}", nameSays, artSays, verdict);
    if (result.mySlot < 0) {
        return;
    }

    // Your column can be there with neither a recognised portrait nor a recognised item.
    const auto me = std::ranges::find(result.allies, result.mySlot, &EnemyPlayer::slot);
    if (me == result.allies.end()) {
        result.me = {};
        result.me.slot = result.mySlot;
    } else {
        result.me = *me;
    }
    // A portrait the art search alone could not name is named by the HUD, unless the HUD's hero is elsewhere.
    if (result.me.heroId.empty() && look.accepted && (byArt < 0 || byArt == result.mySlot)) {
        result.me.heroId = look.id;
        result.me.hero = counters_.heroName(look.id);
    }
    lastMySlot_ = result.mySlot;
    lastMyHero_ = result.me.heroId;
    result.me.tips.clear();  // how to play against your own items is no use to you
    result.me.avoid.clear();
    for (const auto& id : result.me.itemIds) {
        input.owned.insert(id);
        result.mySouls += counters_.costOf(id);
    }
}

int AnalysisWorker::itemSouls(const std::vector<EnemyPlayer>& team) const
{
    int souls = 0;
    for (const auto& p : team) {
        for (const auto& id : p.itemIds) {
            souls += counters_.costOf(id);
        }
    }
    return souls;
}

void AnalysisWorker::publishConfig(std::string summary)
{
    std::lock_guard lock(mutex_);
    publishedConfig_.config = cfg_;
    publishedConfig_.summary = std::move(summary);
    publishedConfig_.nicknameSet = !myName_.empty();
    publishedConfig_.itemNames = counters_.itemNames();
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

    const cv::Rect searchArea = boundingBox(rows);

    // Both teams, added to what earlier looks saw. With a single search area there are no columns to
    // remember things by.
    std::vector<HeroVerdict> verdicts;
    std::vector<Detection> detections;
    std::vector<Detection> allyDetections;
    if (rows.size() > 1) {
        const std::map<int, std::string> expected = memory_.heroes();
        for (auto& d : memory_.update(scanBoard(*frame, vision_, heroes_, cfg_, &verdicts, &expected))) {
            if (static_cast<size_t>(d.slot) >= rows.size()) {
                d.slot -= static_cast<int>(rows.size());
                allyDetections.push_back(std::move(d));
            } else {
                detections.push_back(std::move(d));
            }
        }
    } else {
        detections = vision_.detect(*frame, searchArea);
        assignToRows(detections, rows);
    }

    if (vision_.templateCount() == 0) {
        result.warning = "no item icons: run tools/fetch_icons.py";
    } else if (activeTemplates_ == 0) {
        result.warning = "no active item icons: run tools/fetch_icons.py and build again";
    } else if ((heroes_.heroCount() == 0 && !portraits.empty()) || abilityBar_.heroCount() == 0) {
        result.warning = "no hero art: run tools/fetch_heroes.py";
    }

    const ArchetypeRule archetypes{cfg_.archetypeShare, cfg_.archetypeMinItems};
    SuggestInput notForSuggestions;
    result.allies = describePlayers(allyDetections, rows.size(), counters_, archetypes, notForSuggestions);

    SuggestInput input;
    addMe(*frame, result, input);
    result.players = describePlayers(detections, rows.size(), counters_, archetypes, input);
    result.enemyItemSouls = itemSouls(result.players);
    result.allyItemSouls = itemSouls(result.allies);
    for (const auto& [id, n] : input.items) {
        result.enemyItems.push_back({id, counters_.displayName(id), n});
    }
    std::ranges::stable_sort(result.enemyItems, std::greater{}, &EnemyItem::count);
    result.myWeakItems = weakItems(result.me, result.players);

    // How far along you are: your own items, or if unknown what the average enemy owns.
    input.spentSouls = result.mySlot >= 0 ? result.mySouls : 0;
    if (result.mySlot < 0 && !result.players.empty()) {
        input.spentSouls = result.enemyItemSouls / static_cast<int>(result.players.size());
    }
    result.suggestions = counters_.suggest(input, static_cast<size_t>(cfg_.maxSuggestions));
    result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    // Kept for the menu's preview and for picking your nickname. A copy: the debug images draw on the frame.
    constexpr double kPreviewShare = 0.62;  // enough for the scoreboard's item strips
    lastFrame_ = frame->clone();
    result.preview = lastFrame_(cv::Rect(0, 0, frame->cols, static_cast<int>(frame->rows * kPreviewShare)));
    result.frameSize = lastFrame_.size();

    if (debug) {
        saveDebugImages(*frame, searchArea, rows, portraits, detections);
        publishConfig("saved debug_capture.png in " + root_.string());
        std::cout << "[debug] you: " << result.howFound << '\n';
        const HeroVerdict bar = abilityBar_.recognize(*frame);
        std::cout << std::format("[debug] you, by your abilities: {} {:.3f}, next {:.3f}: {}; ability icons {}\n",
                                 bar.id.empty() ? "?" : bar.id, bar.score, bar.runnerUp,
                                 bar.accepted ? "recognised" : "rejected", abilityBar_.heroCount());
        double nameScore = 0.0;
        const int nameSlot = findName(*frame, toPixels(cfg_.allyNames, frame->size()), myName_, &nameScore);
        std::cout << std::format("[debug] you, by your nickname: column {}, score {:.3f}; in use: column {}\n", nameSlot,
                                 nameScore, result.mySlot);
        std::cout << std::format("[debug] your items: {} recognised, {} not\n", result.me.itemIds.size(), result.me.unknownItems);
        for (size_t i = 0; i < verdicts.size(); ++i) {
            const HeroVerdict& v = verdicts[i];
            std::cout << std::format("[debug] {} #{}: {} {:.3f}, next {:.3f}: {}\n", i < rows.size() ? "enemy" : "ally",
                                     i % rows.size() + 1, v.id.empty() ? "?" : v.id, v.score, v.runnerUp,
                                     v.accepted ? "recognised" : "rejected");
        }
    }
    detections.insert(detections.end(), allyDetections.begin(), allyDetections.end());
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
