#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Rectangle in normalized screen coordinates (0..1), so one config works at any resolution.
struct NormRect {
    double x = 0.0;
    double y = 0.0;
    double w = 1.0;
    double h = 1.0;
};

// Windows virtual-key codes. config.json names them ("TAB", "F9", "MOUSE4", ...).
struct Hotkeys {
    int scoreboard = 0x09;  // VK_TAB
    int calibrate = 0x76;   // VK_F7
    int reload = 0x77;      // VK_F8
    int debugDump = 0x78;   // VK_F9
    int tuneScale = 0x75;   // VK_F6
    int pinPanel = 0x74;    // VK_F5
    int menu = 0x79;        // VK_F10
};

struct AppConfig {
    std::string gameWindowTitle = "Deadlock";
    int monitorIndex = 0;
    int captureDelayMs = 250;      // let the scoreboard fade in before grabbing a frame
    int calibrationScanMs = 400;   // live rescan interval while calibrating
    int scanThreads = 2;           // CPU threads a scan may use; every extra one is taken from the game
    Hotkeys hotkeys;
    std::filesystem::path templateDir = "assets/templates";
    std::filesystem::path countersFile = "assets/counters.json";
    int referenceHeight = 1080;  // screen height the template PNGs were cropped at
    double matchThreshold = 0.90;
    double downscale = 1.0;      // < 1 is faster but costs accuracy on the small (27 px) icons
    int itemIconSize = 27;       // item icon height on the scoreboard at reference_height; 0 = use template size
    double itemCornerMask = 0.5; // ignored top-right share of each item icon (tier badge)
    double iconScale = 1.0;      // on-screen icon size vs reference_height; set by F6 tuning
    NormRect enemyRoi{0.10, 0.10, 0.80, 0.80};
    std::vector<NormRect> enemyRows;  // one per enemy player; {enemyRoi} when not configured

    // Hero recognition: one portrait box per enemy row; empty = disabled.
    std::vector<NormRect> enemyPortraits;
    std::filesystem::path heroTemplateDir = "assets/heroes";
    double heroThreshold = 0.80;
    double heroMargin = 0.25;    // how far the best hero must lead the next-best one

    // Where your team's nicknames are; lets the helper find you by the picture of your nickname.
    std::vector<NormRect> allyNames;

    // Build archetype: a slot (gun / spirit / vitality) owning this share of an enemy's items.
    double archetypeShare = 0.6;
    int archetypeMinItems = 3;
    double panelX = 0.02;
    double panelY = 0.30;
    int maxSuggestions = 6;
};

struct ConfigLoad {
    AppConfig config;   // defaults when loading failed
    std::string error;  // empty on success
};

// Also applies, from the same folder when present: calibration.json (written by F6 scale tuning) and
// settings.json (written by the in-game menu), unless `withOverrides` is false.
ConfigLoad loadConfig(const std::filesystem::path& file, bool withOverrides = true);

// Writes what differs from config.json to settings.json next to it. It survives rebuilds, which
// overwrite config.json. Returns false when the file cannot be written.
bool saveSettings(const std::filesystem::path& configFile, const AppConfig& cfg);
bool resetSettings(const std::filesystem::path& configFile);

// Writes calibration.json next to config.json, so hand-written config.json stays untouched.
bool saveCalibration(const std::filesystem::path& configFile, double iconScale);

// "TAB", "F9", ... for a virtual-key code; "0x.." for keys without a name.
std::string keyName(int vk);

// True for keys config.json has a name for, the only ones that can be saved.
bool isNamedKey(int vk);

std::filesystem::path resolvePath(const std::filesystem::path& base, const std::filesystem::path& p);
