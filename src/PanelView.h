#pragma once

#include "Types.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

// What the panel shows. It only needs Dear ImGui, so it can be rendered without Windows or D3D.
struct PanelModel {
    std::string status;
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
