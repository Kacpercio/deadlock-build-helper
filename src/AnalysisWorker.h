#pragma once

#include "AbilityBar.h"
#include "Config.h"
#include "CounterDatabase.h"
#include "HeroRecognizer.h"
#include "PlayerAnalysis.h"
#include "ScreenCapture.h"
#include "Types.h"
#include "VisionEngine.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The configuration the worker is currently running with, handed to the main thread.
struct ConfigSnapshot {
    AppConfig config;
    std::string summary;  // what the last load/reload did, for the console and the overlay
    bool nicknameSet = false;
    std::vector<std::pair<std::string, std::string>> itemNames;  // (display name, id), for the menu
    std::uint64_t version = 0;
};

// Runs capture -> template matching -> counter lookup on a background thread so the
// overlay keeps rendering at full frame rate. It owns the templates and the counter table,
// which lets it reload them between scans without any locking around the vision code.
class AnalysisWorker {
public:
    // Loads templates and counters described by `cfg` before the thread starts.
    AnalysisWorker(ScreenCapture& capture, AppConfig cfg, std::filesystem::path root);

    // Gets the capture ready, so that a scan requested shortly after does not have to.
    void requestPrepare();

    // Queues one scan. Requests arriving while a scan runs are merged into the next one.
    void requestScan(bool saveDebugImages);

    // Re-reads config.json, counters.json and the template folder before the next scan.
    void requestReload();

    // Finds the icon scale that matches the open scoreboard, applies it and saves calibration.json.
    void requestScaleTune();

    // Saves these settings to settings.json and runs with them from the next scan on.
    void requestSettings(AppConfig settings);
    void requestResetSettings();

    // Saves the frame of the last scan as debug_last_scan.png next to the exe.
    void requestSaveLastScan();

    // Remembers the nickname in this column of your team on the last scan as yours. -1 forgets it.
    void requestPickMe(int allySlot);

    // Saves this part of the last scan as the look of an item, for the icons the shop art does not
    // match. `box` is in frame pixels.
    void requestLearnIcon(std::string itemId, cv::Rect box);

    bool busy() const { return busy_.load(); }

    std::optional<AnalysisResult> takeIfNewer(std::uint64_t knownVersion);
    std::optional<ConfigSnapshot> takeConfigIfNewer(std::uint64_t knownVersion);

private:
    void run(std::stop_token stop);
    AnalysisResult analyze(bool saveDebugImages);
    void reload();
    void tuneScale();
    std::string loadResources();
    void publishConfig(std::string summary);
    void applyThreadLimit() const;
    void pickMe(int allySlot);
    void learnIcon(const std::string& itemId, cv::Rect box);
    void saveLastScan();
    size_t loadItemTemplates(VisionEngine& vision) const;  // returns how many are active items
    void addMe(const cv::Mat& frame, AnalysisResult& result, SuggestInput& input);
    int itemSouls(const std::vector<EnemyPlayer>& team) const;
    void saveDebugImages(cv::Mat& frame, const cv::Rect& searchArea, const std::vector<cv::Rect>& rows,
                         const std::vector<cv::Rect>& portraits, const std::vector<Detection>& detections) const;

    ScreenCapture& capture_;
    const std::filesystem::path root_;
    const int monitorIndex_;  // the capture cannot switch monitors at runtime

    // Used only by the worker thread once it is running.
    AppConfig cfg_;
    VisionEngine vision_;    // item icons
    HeroRecognizer heroes_;  // hero portraits
    CounterDatabase counters_;
    AbilityBar abilityBar_;
    // Where you were last found: kept while nothing says otherwise and that column still has that hero,
    // for when the ability bar is under an effect.
    size_t activeTemplates_ = 0;
    int lastMySlot_ = -1;
    std::string lastMyHero_;
    NameTemplate myName_;
    ScoreboardMemory memory_;
    cv::Mat lastFrame_;

    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool pendingScan_ = false;
    bool pendingDebug_ = false;
    bool pendingReload_ = false;
    bool pendingTune_ = false;
    bool pendingPrepare_ = false;
    bool pendingSaveScan_ = false;
    std::optional<AppConfig> pendingSettings_;
    bool pendingResetSettings_ = false;
    std::optional<int> pendingPick_;
    std::optional<std::pair<std::string, cv::Rect>> pendingLearn_;
    std::atomic<bool> busy_{false};
    AnalysisResult latest_;
    ConfigSnapshot publishedConfig_;

    // Last member: started at the end of the constructor, stopped and joined before the rest is destroyed.
    std::jthread thread_;
};
