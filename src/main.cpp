#include "AnalysisWorker.h"
#include "Config.h"
#include "PlayerAnalysis.h"
#include "Overlay.h"
#include "ScreenCapture.h"
#include "SettingsMenu.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

constexpr int kReminderSeconds = 8;

std::string aboutYou(const AnalysisResult& r)
{
    if (!r.nicknameSet || r.version == 0) {
        return {};
    }
    if (r.mySlot < 0) {
        return "You: not found on the scoreboard";
    }
    return std::format("You: {}, {} items owned ({} souls)", r.myHero.empty() ? "hero not recognised" : r.myHero, r.myItems,
                       r.mySouls);
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

    KeyEdge scoreboardKey, debugKey, reloadKey, calibrateKey, tuneKey, pinKey, menuKey;
    bool pinned = false;

    SettingsMenu menu;
    bool menuOpen = false;
    bool reopenMenuValues = false;   // the settings were reset: show the new values once they arrive
    bool nicknameSet = false;
    std::optional<int> pickedSlot;   // chosen in the menu since the last scan
    std::array<bool, 256> keyWasDown{};
    Clock::time_point menuOpenedAt{};
    ImTextureID previewTexture{};
    std::uint64_t previewVersion = 0;
    const auto closeMenu = [&] {
        overlay.setInteractive(false);
        menuOpen = false;
    };

    int scansThisPress = 0;
    bool calibrating = false;
    Clock::time_point panelHeldUntil{};
    std::vector<EnemyPlayer> playersAtLastPress;  // to tell what enemies bought since then
    std::vector<std::string> purchases;
    Clock::time_point reminderDue = Clock::time_point::max();
    Clock::time_point reminderUntil{};
    Clock::time_point remindersStop{};
    std::optional<Suggestion> reminder;
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
            overlay.setIconFolders(resolvePath(root, cfg.templateDir), resolvePath(root, cfg.heroTemplateDir));
            nicknameSet = snapshot->nicknameSet;
            if (std::exchange(reopenMenuValues, false)) {
                menu.open(cfg);
            }
            notice = std::move(snapshot->summary);
            noticeUntil = now + std::chrono::seconds(4);
            std::cout << "[config] " << notice << '\n';
        }

        const bool gameFocused = !menuOpen && isGameForeground(gameTitle);
        const bool scoreboardDown = gameFocused && keyDown(cfg.hotkeys.scoreboard);

        // Two looks per scoreboard press: a quick one, so a short tap is enough, and a surer one if the
        // key is still down, since the first can catch the scoreboard fading in.
        if (scoreboardKey.pressed(scoreboardDown)) {
            scoreboardDownSince = now;
            scansThisPress = 0;
            worker.requestPrepare();
            playersAtLastPress = shown.players;
            purchases.clear();
            reminder.reset();
        }
        const auto heldFor = now - scoreboardDownSince;
        if (scoreboardDown && ((scansThisPress == 0 && heldFor >= milliseconds(cfg.captureDelayMs)) ||
                               (scansThisPress == 1 && heldFor >= milliseconds(cfg.captureDelayMs + cfg.rescanDelayMs)))) {
            worker.requestScan(false);
            ++scansThisPress;
            lastScan = now;
        }
        if (scoreboardDown) {
            panelHeldUntil = now + milliseconds(cfg.panelHoldMs);
        }
        // While calibrating, keep rescanning so edits to the rows/threshold show up immediately.
        if (calibrating && scoreboardDown && scansThisPress >= 2 && !worker.busy() &&
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

        if (pinKey.pressed(gameFocused && keyDown(cfg.hotkeys.pinPanel))) {
            pinned = !pinned;
        }

        // Edge of the key itself: while a new hotkey is being chosen it must not count as "pressed" later.
        if (menuKey.pressed(keyDown(cfg.hotkeys.menu))) {
            if (gameFocused) {
                menu.open(cfg);
                overlay.setInteractive(true);
                menuOpen = true;
                menuOpenedAt = now;
                previewVersion = 0;
            } else if (menuOpen && !menu.waitingForKey()) {
                closeMenu();
            }
        }
        if (menuOpen) {
            for (int vk = 1; vk < static_cast<int>(keyWasDown.size()) - 1; ++vk) {
                const bool down = keyDown(vk);
                if (down && !keyWasDown[static_cast<size_t>(vk)] && menu.waitingForKey() && menu.keyPressed(vk).apply) {
                    worker.requestSettings(menu.settings());
                }
                keyWasDown[static_cast<size_t>(vk)] = down;
            }
            // Never stay in the way: an interactive window covers the whole monitor.
            if (now - menuOpenedAt > std::chrono::seconds(1) && !overlay.hasFocus()) {
                std::cout << "[menu] closed: it does not have the focus\n";
                closeMenu();
            }
        }

        if (auto fresh = worker.takeIfNewer(shown.version)) {
            // A look that saw nothing at all (the scoreboard was already gone) changes nothing.
            if (fresh->detections.empty() && !shown.detections.empty() && fresh->error.empty()) {
                shown.version = fresh->version;
            } else {
                shown = std::move(*fresh);
                purchases = newPurchases(playersAtLastPress, shown.players);
            }
            pickedSlot.reset();

            // The next reminder counts from now, and they stop once scans have stopped for a while:
            // the match is probably over.
            if (cfg.reminderMinutes > 0) {
                reminderDue = now + std::chrono::minutes(cfg.reminderMinutes);
                remindersStop = now + std::chrono::minutes(5 * cfg.reminderMinutes);
            }
        }

        if (gameFocused && now - lastTopmost > std::chrono::seconds(2)) {
            overlay.keepOnTop();
            lastTopmost = now;
        }

        if (cfg.reminderMinutes > 0 && now >= reminderDue) {
            reminderDue = now + std::chrono::minutes(cfg.reminderMinutes);
            const auto next = std::ranges::find_if(shown.suggestions, [](const Suggestion& s) {
                return s.timing == BuyTiming::Now;
            });
            if (gameFocused && now < remindersStop && next != shown.suggestions.end()) {
                reminder = *next;
                reminderUntil = now + std::chrono::seconds(kReminderSeconds);
            }
        }
        if (reminder && now >= reminderUntil) {
            reminder.reset();
        }

        OverlayModel model;
        // The last result stays up for a while after TAB is released, or for good when pinned, for
        // instance over the shop.
        model.visible = scoreboardDown || (gameFocused && shown.version > 0 && (pinned || now < panelHeldUntil));
        if (gameFocused && !model.visible) {
            model.reminder = reminder;
        }
        model.calibrating = calibrating && gameFocused;
        if (model.visible || model.calibrating) {
            model.panel.status = now < noticeUntil ? notice : statusLine(shown, worker.busy());
            model.panel.you = aboutYou(shown);
            model.panel.newPurchases = purchases;
            model.panel.pinned = pinned;
            model.panel.perPlayer = cfg.enemyRows.size() > 1;
            model.panel.enemyItems = shown.enemyItems;
            model.panel.players = shown.players;
            model.panel.suggestions = shown.suggestions;
            model.revision = std::format("{} {} {} {} {}", shown.version, configVersion, model.calibrating, pinned,
                                         model.panel.status);
        } else if (model.reminder) {
            model.revision = "reminder " + model.reminder->item;
            model.panelX = static_cast<float>(cfg.panelX);
            model.panelY = static_cast<float>(cfg.panelY);
        }
        if (model.calibrating) {
            model.rows = cfg.enemyRows;
            model.portraits = cfg.enemyPortraits;
            model.detections = shown.detections;
            model.banner = banner;
        }

        MenuActions menuActions;
        if (menuOpen) {
            if (previewVersion != shown.version) {
                previewVersion = shown.version;
                previewTexture = overlay.setPreview(shown.preview);
            }
            MenuContext context;
            context.preview = previewTexture;
            context.previewSize = shown.preview.size();
            context.frameSize = shown.frameSize;
            context.nicknameSet = nicknameSet;
            context.justPicked = pickedSlot.has_value();
            context.mySlot = pickedSlot.value_or(shown.mySlot);
            context.myHero = shown.myHero;
            context.notice = now < noticeUntil ? notice : std::string{};
            model.menu = [&menu, &menuActions, context] { menuActions = menu.draw(context); };
        }
        overlay.render(model);

        if (menuActions.apply) {
            worker.requestSettings(menu.settings());
        }
        if (menuActions.reset) {
            worker.requestResetSettings();
            reopenMenuValues = true;
        }
        if (menuActions.pickMe) {
            worker.requestPickMe(*menuActions.pickMe);
            pickedSlot = *menuActions.pickMe;
        }
        if (menuActions.close) {
            closeMenu();
        }
    }
    return 0;
}
