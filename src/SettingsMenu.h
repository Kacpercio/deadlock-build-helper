#pragma once

#include "Config.h"

#include <imgui.h>

#include <opencv2/core.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// What the menu shows about the current state.
struct MenuContext {
    ImTextureID preview = ImTextureID{};  // the scoreboard on the last scan
    cv::Size previewSize;
    cv::Size frameSize;                   // the frame the preview was cut from
    bool nicknameSet = false;
    int mySlot = -1;                      // your column on the last scan
    std::string myHero;
    const std::vector<std::pair<std::string, std::string>>* itemNames = nullptr;  // (display name, id)
    bool justPicked = false;              // a nickname was picked after the last scan
    std::string howFound;                 // what the HUD and the nickname each said on the last scan
    std::string notice;                   // what the last change did
};

// What the user just did. Everything happens outside the menu.
struct MenuActions {
    bool close = false;
    bool apply = false;          // settings() changed for good: save and use them
    bool reset = false;          // back to config.json
    bool saveScan = false;       // save the last scan's frame
    std::optional<int> pickMe;   // this column of your team shows your nickname; -1 forgets it
    std::optional<std::pair<std::string, cv::Rect>> learnIcon;  // this part of the frame looks like this item id
};

// The in-game settings window. It edits its own copy of the settings and shows changes right away;
// sliders count as applied once they are released. Only needs Dear ImGui.
class SettingsMenu {
public:
    void open(const AppConfig& current);
    const AppConfig& settings() const { return edited_; }

    bool waitingForKey() const { return waitingForKey_ != nullptr; }
    // The key pressed while waitingForKey(): assigns it. Escape cancels; other unnamed keys are ignored.
    MenuActions keyPressed(int vk);

    MenuActions draw(const MenuContext& context);

private:
    void drawScoreboard(const MenuContext& context, MenuActions& actions);
    void drawIconTeacher(const MenuContext& context, MenuActions& actions);
    void drawPositions(MenuActions& actions);
    void drawRecognition(MenuActions& actions);
    void drawHotkeys();
    void keyRow(const char* label, int& key);

    AppConfig edited_;
    int* waitingForKey_ = nullptr;

    // The icon being taught.
    bool teachEnemySide_ = true;
    bool iconPicked_ = false;
    cv::Point iconCentre_;           // frame pixels
    char iconFilter_[64] = "";
};

// Colours for the menu's widgets. Call once, after creating the ImGui context.
void applyMenuStyle();
