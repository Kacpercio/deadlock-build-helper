#include "Config.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

constexpr std::pair<std::string_view, int> kNamedKeys[] = {
    {"TAB", VK_TAB},          {"SPACE", VK_SPACE},     {"CAPSLOCK", VK_CAPITAL},
    {"SHIFT", VK_SHIFT},      {"CTRL", VK_CONTROL},    {"ALT", VK_MENU},
    {"INSERT", VK_INSERT},    {"DELETE", VK_DELETE},   {"HOME", VK_HOME},
    {"END", VK_END},          {"PAGEUP", VK_PRIOR},    {"PAGEDOWN", VK_NEXT},
    {"MOUSE3", VK_MBUTTON},   {"MOUSE4", VK_XBUTTON1}, {"MOUSE5", VK_XBUTTON2},
};

std::string upper(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

bool allDigits(std::string_view s)
{
    return !s.empty() && std::ranges::all_of(s, [](unsigned char c) { return std::isdigit(c) != 0; });
}

// 0 for names it does not know.
int keyFromName(std::string_view name)
{
    const std::string key = upper(name);
    if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
        return key[0];  // VK codes of A-Z and 0-9 are their ASCII values
    }
    if (key.size() <= 3 && key[0] == 'F' && allDigits(std::string_view(key).substr(1))) {
        const int n = std::stoi(key.substr(1));
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
    }
    if (key.size() == 7 && key.starts_with("NUMPAD") && allDigits(std::string_view(key).substr(6))) {
        return VK_NUMPAD0 + (key[6] - '0');
    }
    for (const auto& [n, vk] : kNamedKeys) {
        if (n == key) {
            return vk;
        }
    }
    return 0;
}

void readKey(const nlohmann::json& hotkeys, const char* field, int& target)
{
    if (!hotkeys.contains(field)) {
        return;
    }
    const auto name = hotkeys[field].get<std::string>();
    if (const int vk = keyFromName(name)) {
        target = vk;
    } else {
        std::cerr << "[config] unknown key \"" << name << "\" for hotkeys." << field << ", keeping " << keyName(target) << '\n';
    }
}

NormRect readRect(const nlohmann::json& j, const NormRect& fallback)
{
    return {
        j.value("x", fallback.x),
        j.value("y", fallback.y),
        j.value("w", fallback.w),
        j.value("h", fallback.h),
    };
}

// A first box plus a step generates evenly spaced boxes, one per scoreboard row.
std::vector<NormRect> readRowSet(const nlohmann::json& rows, const NormRect& fallback)
{
    const NormRect first = readRect(rows.value("first", nlohmann::json::object()), fallback);
    const int count = std::clamp(rows.value("count", 6), 1, 12);
    const double stepX = rows.value("step_x", 0.0);
    const double stepY = rows.value("step_y", first.h);
    std::vector<NormRect> out;
    for (int i = 0; i < count; ++i) {
        out.push_back({first.x + i * stepX, first.y + i * stepY, first.w, first.h});
    }
    return out;
}

// Keeps a value inside [lo, hi], warning when the file asked for something unusable.
template <typename T>
void clampSetting(T& value, T lo, T hi, const char* name)
{
    if (value < lo || value > hi || value != value) {  // value != value: NaN
        const T fixed = value != value ? lo : std::clamp(value, lo, hi);
        std::cerr << "[config] " << name << " = " << value << " is out of range [" << lo << ", " << hi
                  << "], using " << fixed << '\n';
        value = fixed;
    }
}

void validate(AppConfig& cfg)
{
    clampSetting(cfg.monitorIndex, 0, 16, "monitor_index");
    clampSetting(cfg.captureDelayMs, 0, 2000, "capture_delay_ms");
    clampSetting(cfg.calibrationScanMs, 100, 5000, "calibration_scan_ms");
    clampSetting(cfg.referenceHeight, 240, 4320, "reference_height");
    clampSetting(cfg.matchThreshold, 0.3, 0.999, "match_threshold");
    clampSetting(cfg.heroThreshold, 0.3, 0.999, "hero_threshold");
    clampSetting(cfg.heroMargin, 0.0, 1.0, "hero_margin");
    clampSetting(cfg.downscale, 0.2, 1.0, "downscale");
    clampSetting(cfg.iconScale, 0.5, 2.0, "icon_scale");
    clampSetting(cfg.itemIconSize, 0, 200, "item_icon_size");
    clampSetting(cfg.itemCornerMask, 0.0, 0.8, "item_corner_mask");
    clampSetting(cfg.archetypeShare, 0.34, 1.0, "archetype_share");
    clampSetting(cfg.archetypeMinItems, 1, 20, "archetype_min_items");
    clampSetting(cfg.maxSuggestions, 1, 20, "max_suggestions");
    clampSetting(cfg.panelX, 0.0, 0.95, "panel.x");
    clampSetting(cfg.panelY, 0.0, 0.95, "panel.y");
}

void parse(const nlohmann::json& j, AppConfig& cfg)
{
    cfg.gameWindowTitle = j.value("game_window_title", cfg.gameWindowTitle);
    cfg.monitorIndex = j.value("monitor_index", cfg.monitorIndex);
    cfg.captureDelayMs = j.value("capture_delay_ms", cfg.captureDelayMs);
    cfg.calibrationScanMs = j.value("calibration_scan_ms", cfg.calibrationScanMs);
    cfg.templateDir = j.value("template_dir", cfg.templateDir.string());
    cfg.countersFile = j.value("counters_file", cfg.countersFile.string());
    cfg.referenceHeight = j.value("reference_height", cfg.referenceHeight);
    cfg.matchThreshold = j.value("match_threshold", cfg.matchThreshold);
    cfg.downscale = j.value("downscale", cfg.downscale);
    cfg.iconScale = j.value("icon_scale", cfg.iconScale);
    cfg.itemIconSize = j.value("item_icon_size", cfg.itemIconSize);
    cfg.itemCornerMask = j.value("item_corner_mask", cfg.itemCornerMask);
    cfg.maxSuggestions = j.value("max_suggestions", cfg.maxSuggestions);

    if (j.contains("hotkeys")) {
        const auto& h = j["hotkeys"];
        readKey(h, "scoreboard", cfg.hotkeys.scoreboard);
        readKey(h, "calibrate", cfg.hotkeys.calibrate);
        readKey(h, "reload", cfg.hotkeys.reload);
        readKey(h, "debug_dump", cfg.hotkeys.debugDump);
        readKey(h, "tune_scale", cfg.hotkeys.tuneScale);
    }
    if (j.contains("enemy_roi")) {
        cfg.enemyRoi = readRect(j["enemy_roi"], cfg.enemyRoi);
    }
    if (j.contains("enemy_rows")) {
        cfg.enemyRows = readRowSet(j["enemy_rows"], cfg.enemyRoi);
    }
    if (j.contains("enemy_portraits")) {
        cfg.enemyPortraits = readRowSet(j["enemy_portraits"], NormRect{0.0, 0.0, 0.05, 0.07});
    }
    cfg.heroTemplateDir = j.value("hero_template_dir", cfg.heroTemplateDir.string());
    cfg.heroThreshold = j.value("hero_threshold", cfg.heroThreshold);
    cfg.heroMargin = j.value("hero_margin", cfg.heroMargin);
    cfg.archetypeShare = j.value("archetype_share", cfg.archetypeShare);
    cfg.archetypeMinItems = j.value("archetype_min_items", cfg.archetypeMinItems);
    if (j.contains("panel")) {
        cfg.panelX = j["panel"].value("x", cfg.panelX);
        cfg.panelY = j["panel"].value("y", cfg.panelY);
    }
}

}  // namespace

ConfigLoad loadConfig(const std::filesystem::path& file)
{
    ConfigLoad out;
    std::ifstream in(file);
    if (!in) {
        out.error = file.string() + " not found";
    } else {
        try {
            parse(nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true), out.config);
        } catch (const nlohmann::json::exception& e) {
            out.config = AppConfig{};  // never hand out a half-applied file
            out.error = std::string("config.json: ") + e.what();
        }
    }
    if (out.config.enemyRows.empty()) {
        out.config.enemyRows = {out.config.enemyRoi};
    }

    const auto calibration = file.parent_path() / "calibration.json";
    if (out.error.empty() && std::filesystem::exists(calibration)) {
        try {
            std::ifstream calibrationIn(calibration);
            out.config.iconScale = nlohmann::json::parse(calibrationIn).value("icon_scale", out.config.iconScale);
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "[config] ignoring broken calibration.json: " << e.what() << '\n';
        }
    }
    validate(out.config);
    return out;
}

bool saveCalibration(const std::filesystem::path& configFile, double iconScale)
{
    std::ofstream out(configFile.parent_path() / "calibration.json", std::ios::trunc);
    out << nlohmann::json{{"_note", "Written by F6 scale tuning; overrides icon_scale in config.json."},
                          {"icon_scale", iconScale}}
               .dump(2)
        << '\n';
    return static_cast<bool>(out);
}

std::string keyName(int vk)
{
    for (const auto& [name, code] : kNamedKeys) {
        if (code == vk) {
            return std::string(name);
        }
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return std::format("F{}", vk - VK_F1 + 1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return std::format("NUMPAD{}", vk - VK_NUMPAD0);
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return std::string(1, static_cast<char>(vk));
    }
    return std::format("0x{:02X}", vk);
}

std::filesystem::path resolvePath(const std::filesystem::path& base, const std::filesystem::path& p)
{
    if (p.is_absolute()) {
        return p;
    }

    const auto besideExecutable = base / p;
    if (std::filesystem::exists(besideExecutable)) {
        return besideExecutable;
    }

    const auto projectRoot = base.parent_path() / p;
    if (std::filesystem::exists(projectRoot)) {
        return projectRoot;
    }

    return besideExecutable;
}
