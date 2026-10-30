#include "AnalysisWorker.h"
#include "Config.h"
#include "Overlay.h"
#include "ScreenCapture.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

// Polls the key state; installs no hook and injects nothing.
bool keyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Feed it the key state every frame; reports the frame on which the key went down.
class KeyEdge {
public:
    bool pressed(bool down)
    {
        const bool edge = down && !wasDown_;
        wasDown_ = down;
        return edge;
    }

private:
    bool wasDown_ = false;
};

fs::path executableDir()
{
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

// Built with DBH_HIDE_CONSOLE there is nowhere for std::cout/std::cerr to go, so send them to a file.
void redirectLogsIfNoConsole(const fs::path& root)
{
    if (GetConsoleWindow()) {
        return;
    }
    const fs::path log = root / "build_helper.log";
    std::ofstream(log, std::ios::trunc).close();
    // Both streams append: each FILE keeps its own write position, so "w" + "a" would let
    // stdout overwrite what stderr wrote.
    FILE* file = nullptr;
    if (_wfreopen_s(&file, log.c_str(), L"a", stdout) == 0) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        _wfreopen_s(&file, log.c_str(), L"a", stderr);
    }
}

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

// Compares the foreground window's title only - no handle to the game process is ever opened.
bool isGameForeground(const std::wstring& title)
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    wchar_t buffer[256];
    const int length = GetWindowTextW(foreground, buffer, static_cast<int>(std::size(buffer)));
    return std::wstring_view(buffer, static_cast<size_t>(length)) == title;
}

std::string statusLine(const AnalysisResult& r, bool busy)
{
    if (busy) {
        return "scanning...";
    }
    if (!r.error.empty()) {
        return r.error;
    }
    if (r.version == 0) {
        return "waiting for scoreboard";
    }
    const auto heroes = std::ranges::count_if(r.detections, &Detection::isHero);
    return std::format("{} items, {} heroes | {:.0f} ms{}{}", r.detections.size() - static_cast<size_t>(heroes), heroes,
                       r.elapsedMs, r.warning.empty() ? "" : " | ", r.warning);
}

std::string calibrationBanner(const Hotkeys& keys)
{
    return std::format("CALIBRATION   hold {}: live scan   {}+{}: auto icon size   {}: reload config   "
                       "{}: save debug images   {}: exit",
                       keyName(keys.scoreboard), keyName(keys.scoreboard), keyName(keys.tuneScale), keyName(keys.reload),
                       keyName(keys.debugDump), keyName(keys.calibrate));
}

}  // namespace

int main()
{
    // Before any window or DXGI call: keeps captured pixels and overlay pixels 1:1 on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const fs::path root = executableDir();
    redirectLogsIfNoConsole(root);

    ConfigLoad loaded = loadConfig(root / "assets" / "config.json");
    if (!loaded.error.empty()) {
        std::cerr << "[config] " << loaded.error << " - using defaults\n";
    }
    AppConfig cfg = std::move(loaded.config);

    ScreenCapture capture(cfg.monitorIndex);
    if (!capture.ok()) {
        std::cerr << "Screen capture unavailable. Check monitor_index in assets/config.json.\n";
        return 1;
    }

    Overlay overlay(capture.desktopRect());
    if (!overlay.init()) {
        std::cerr << "Overlay window / D3D11 initialization failed.\n";
        return 1;
    }

    const RECT monitor = capture.desktopRect();

    // Declared after capture so it is destroyed (and joined) before it. From here on only the
    // worker thread touches `capture`.
    AnalysisWorker worker(capture, cfg, root);

    std::cout << std::format("Deadlock Build Helper - monitor {}x{}, Ctrl+End quits\n",
                             monitor.right - monitor.left, monitor.bottom - monitor.top);

    std::uint64_t configVersion = 0;
    std::wstring gameTitle;
    std::string banner;
    std::string notice;
    Clock::time_point noticeUntil{};

    KeyEdge scoreboardKey, debugKey, reloadKey, calibrateKey, tuneKey;
    bool scannedThisPress = false;
    bool calibrating = false;
    Clock::time_point scoreboardDownSince{};
    Clock::time_point lastScan{};
    Clock::time_point lastTopmost = Clock::now();
    AnalysisResult shown;

    while (overlay.pumpMessages()) {
        if (keyDown(VK_CONTROL) && keyDown(VK_END)) {
            break;
        }
        const auto now = Clock::now();

        // The worker publishes the config it runs with: at startup and after every reload.
        if (auto snapshot = worker.takeConfigIfNewer(configVersion)) {
            configVersion = snapshot->version;
            cfg = std::move(snapshot->config);
            gameTitle = widen(cfg.gameWindowTitle);
            banner = calibrationBanner(cfg.hotkeys);
            notice = std::move(snapshot->summary);
            noticeUntil = now + std::chrono::seconds(4);
            std::cout << "[config] " << notice << '\n';
        }

        const bool gameFocused = isGameForeground(gameTitle);
        const bool scoreboardDown = gameFocused && keyDown(cfg.hotkeys.scoreboard);

        // One scan per scoreboard press, once it has had time to fade in.
        if (scoreboardKey.pressed(scoreboardDown)) {
            scoreboardDownSince = now;
            scannedThisPress = false;
        }
        if (scoreboardDown && !scannedThisPress && now - scoreboardDownSince >= milliseconds(cfg.captureDelayMs)) {
            worker.requestScan(false);
            scannedThisPress = true;
            lastScan = now;
        }
        // While calibrating, keep rescanning so edits to the rows/threshold show up immediately.
        if (calibrating && scoreboardDown && scannedThisPress && !worker.busy() &&
            now - lastScan >= milliseconds(cfg.calibrationScanMs)) {
            worker.requestScan(false);
            lastScan = now;
        }

        if (debugKey.pressed(gameFocused && keyDown(cfg.hotkeys.debugDump))) {
            worker.requestScan(true);
        }
        if (reloadKey.pressed(gameFocused && keyDown(cfg.hotkeys.reload))) {
            worker.requestReload();
        }
        if (tuneKey.pressed(gameFocused && scoreboardDown && keyDown(cfg.hotkeys.tuneScale))) {
            worker.requestScaleTune();
        }
        if (calibrateKey.pressed(gameFocused && keyDown(cfg.hotkeys.calibrate))) {
            calibrating = !calibrating;
            std::cout << "[calibration] " << (calibrating ? "on" : "off") << '\n';
        }

        if (auto fresh = worker.takeIfNewer(shown.version)) {
            shown = std::move(*fresh);
        }

        if (gameFocused && now - lastTopmost > std::chrono::seconds(2)) {
            overlay.keepOnTop();
            lastTopmost = now;
        }

        OverlayModel model;
        model.visible = scoreboardDown;
        model.calibrating = calibrating && gameFocused;
        if (model.visible || model.calibrating) {
            model.status = now < noticeUntil ? notice : statusLine(shown, worker.busy());
            model.perPlayer = cfg.enemyRows.size() > 1;
            model.enemyItems = shown.enemyItems;
            model.players = shown.players;
            model.suggestions = shown.suggestions;
            model.panelX = static_cast<float>(cfg.panelX);
            model.panelY = static_cast<float>(cfg.panelY);
        }
        if (model.calibrating) {
            model.rows = cfg.enemyRows;
            model.portraits = cfg.enemyPortraits;
            model.detections = shown.detections;
            model.banner = banner;
        }
        overlay.render(model);
    }
    return 0;
}
