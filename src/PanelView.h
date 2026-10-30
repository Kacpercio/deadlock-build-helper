#pragma once

#include "Types.h"

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// What the panel shows. It only needs Dear ImGui, so it can be rendered without Windows or D3D.
struct PanelModel {
    std::string status;
    std::string you;         // about you, empty when no nickname has been picked
    std::optional<EnemyPlayer> me;           // your hero and items, once found
    std::vector<std::string> myWeakItems;    // items you own that do badly against these enemies
    std::vector<std::string> newPurchases;  // what enemies bought since your last look
    bool pinned = false;     // stays on screen after TAB is released
    bool perPlayer = false;  // more than one enemy column configured: list items per enemy
    std::vector<EnemyItem> enemyItems;
    std::vector<EnemyPlayer> players;
    std::vector<Suggestion> suggestions;
};

struct PanelStyle {
    ImFont* regular = nullptr;  // nullptr = the current font
    ImFont* bold = nullptr;
    float scale = 1.0f;         // UI size relative to a 1080p screen
};

// id -> texture of that item or hero; 0 when there is no picture.
struct PanelIcons {
    std::function<ImTextureID(const std::string&)> item;
    std::function<ImTextureID(const std::string&)> hero;
};

void drawPanel(const PanelModel& model, ImVec2 pos, float width, float maxHeight, const PanelStyle& style,
               const PanelIcons& icons);

// A small note instead of the panel, such as a reminder.
void drawReminder(const Suggestion& next, ImVec2 pos, float width, const PanelStyle& style, const PanelIcons& icons);
