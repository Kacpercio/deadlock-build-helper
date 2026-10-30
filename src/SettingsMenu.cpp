#include "SettingsMenu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace {

constexpr ImU32 kItemBox = IM_COL32(255, 215, 0, 255);
constexpr ImU32 kPortraitBox = IM_COL32(0, 220, 255, 200);
constexpr ImU32 kNameBox = IM_COL32(120, 230, 140, 255);
constexpr ImU32 kNameBoxHover = IM_COL32(120, 230, 140, 90);
constexpr ImU32 kYouBox = IM_COL32(255, 110, 255, 255);
constexpr ImU32 kYouFill = IM_COL32(255, 110, 255, 70);

constexpr int kReferenceHeight = 1080;  // sizes in this menu are pixels on a 1080p screen
constexpr int kVK_ESCAPE = 0x1B;
constexpr float kItemIconPixels = 27.0f;  // an item icon on a 1080p scoreboard

void heading(const char* title)
{
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

// A value in 1080p pixels, with buttons for moving it by one.
bool pixelSlider(const char* label, const char* id, double& fraction, int lowest, int highest)
{
    int px = static_cast<int>(std::lround(fraction * kReferenceHeight));
    bool done = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::SliderInt("##value", &px, lowest, highest, "%d px")) {
        fraction = static_cast<double>(px) / kReferenceHeight;
    }
    done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
        fraction = static_cast<double>(std::max(lowest, px - 1)) / kReferenceHeight;
        done = true;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
        fraction = static_cast<double>(std::min(highest, px + 1)) / kReferenceHeight;
        done = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return done;
}

bool sliderDouble(const char* label, double& value, double lowest, double highest, const char* format)
{
    float v = static_cast<float>(value);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::SliderFloat(label, &v, static_cast<float>(lowest), static_cast<float>(highest), format)) {
        value = v;
    }
    return ImGui::IsItemDeactivatedAfterEdit();
}

bool sliderInt(const char* label, int& value, int lowest, int highest)
{
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::SliderInt(label, &value, lowest, highest);
    return ImGui::IsItemDeactivatedAfterEdit();
}

}  // namespace

void applyMenuStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    const ImVec4 accent(1.00f, 0.78f, 0.36f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.98f);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.10f, 0.13f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.19f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.20f, 0.25f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.23f, 0.29f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.17f, 0.18f, 0.23f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.26f, 0.32f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.31f, 0.38f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(accent.x, accent.y, accent.z, 0.45f);  // the value is written across it
    colors[ImGuiCol_SliderGrabActive] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.62f, 0.67f, 1.0f);
}

void SettingsMenu::open(const AppConfig& current)
{
    edited_ = current;
    waitingForKey_ = nullptr;
}

MenuActions SettingsMenu::keyPressed(int vk)
{
    MenuActions actions;
    if (!waitingForKey_) {
        return actions;
    }
    if (vk == kVK_ESCAPE) {
        waitingForKey_ = nullptr;
    } else if (isNamedKey(vk)) {
        *waitingForKey_ = vk;
        waitingForKey_ = nullptr;
        actions.apply = true;
    }
    return actions;
}

MenuActions SettingsMenu::draw(const MenuContext& context)
{
    MenuActions actions;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size(viewport->Size.x * 0.62f, viewport->Size.y * 0.88f);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings;
    bool stayOpen = true;
    if (ImGui::Begin("Deadlock Build Helper - settings", &stayOpen, flags)) {
        if (!context.notice.empty()) {
            ImGui::TextDisabled("%s", context.notice.c_str());
        }
        drawScoreboard(context, actions);
        drawIconTeacher(context, actions);
        drawPositions(actions);
        drawRecognition(actions);
        drawHotkeys();

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close")) {
            actions.close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) {
            actions.reset = true;
        }
    }
    ImGui::End();

    if (!stayOpen || (!waitingForKey_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        actions.close = true;
    }
    return actions;
}

void SettingsMenu::drawScoreboard(const MenuContext& context, MenuActions& actions)
{
    heading("You");
    if (!context.preview || context.frameSize.height == 0) {
        ImGui::TextWrapped("Nothing to show yet: hold TAB once in a match, then open this menu again. "
                           "The last scan appears here.");
        return;
    }
    if (context.justPicked) {
        ImGui::TextWrapped(context.nicknameSet ? "Saved. It is used from the next scan on." : "Nickname forgotten.");
    } else if (!context.nicknameSet) {
        ImGui::TextWrapped("Click your nickname on your team's side (the green boxes). You only do this once.");
    } else if (context.mySlot >= 0) {
        const std::string hero = context.myHero.empty() ? "hero not recognised" : context.myHero;
        ImGui::Text("Found you on the last scan: %s. Click another box if that is wrong.", hero.c_str());
    } else {
        ImGui::TextWrapped("Nickname saved, but it was not on the last scan. Hold TAB in a match to check it.");
    }
    if (context.nicknameSet) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Forget")) {
            actions.pickMe = -1;
        }
    }

    // The scoreboard, with the boxes the helper reads on top of it.
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = width * static_cast<float>(context.previewSize.height) / static_cast<float>(context.previewSize.width);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(context.preview, ImVec2(width, height));

    ImDrawList* list = ImGui::GetWindowDrawList();
    const float sx = width;
    const float sy = height * static_cast<float>(context.frameSize.height) / static_cast<float>(context.previewSize.height);
    const auto corners = [&](const NormRect& r) {
        return std::pair(ImVec2(origin.x + static_cast<float>(r.x) * sx, origin.y + static_cast<float>(r.y) * sy),
                         ImVec2(origin.x + static_cast<float>(r.x + r.w) * sx, origin.y + static_cast<float>(r.y + r.h) * sy));
    };
    for (const auto& r : edited_.enemyRows) {
        const auto [a, b] = corners(r);
        list->AddRect(a, b, kItemBox);
    }
    for (const auto& r : edited_.enemyPortraits) {
        const auto [a, b] = corners(r);
        list->AddRect(a, b, kPortraitBox);
    }

    const ImVec2 afterImage = ImGui::GetCursorScreenPos();
    for (size_t i = 0; i < edited_.allyNames.size(); ++i) {
        const auto [a, b] = corners(edited_.allyNames[i]);
        const bool isYou = static_cast<int>(i) == context.mySlot;
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##name", ImVec2(b.x - a.x, b.y - a.y))) {
            actions.pickMe = static_cast<int>(i);
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered || isYou) {
            list->AddRectFilled(a, b, isYou ? kYouFill : kNameBoxHover);
        }
        list->AddRect(a, b, isYou ? kYouBox : kNameBox);
    }
    ImGui::SetCursorScreenPos(afterImage);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void SettingsMenu::drawIconTeacher(const MenuContext& context, MenuActions& actions)
{
    heading("Teach an icon");
    ImGui::TextWrapped("Active items look different on the scoreboard, so they have to be taught once. "
                       "Click an icon that has no name in the panel, then pick its name.");
    if (!context.preview || edited_.enemyRows.empty() || !context.itemNames) {
        return;
    }
    if (ImGui::RadioButton("Enemy side", teachEnemySide_)) {
        teachEnemySide_ = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Your side", !teachEnemySide_)) {
        teachEnemySide_ = false;
    }

    // All the item strips of one side, enlarged.
    const NormRect& first = edited_.enemyRows.front();
    const NormRect& last = edited_.enemyRows.back();
    NormRect strip{first.x, first.y, last.x + last.w - first.x, first.h};
    if (!teachEnemySide_) {
        strip.x = 1.0 - strip.x - strip.w;
    }
    const float fw = static_cast<float>(context.frameSize.width);
    const float fh = static_cast<float>(context.frameSize.height);
    const float width = ImGui::GetContentRegionAvail().x;
    const float zoom = width / (static_cast<float>(strip.w) * fw);
    const ImVec2 size(width, static_cast<float>(strip.h) * fh * zoom);
    const ImVec2 uv0(static_cast<float>(strip.x), static_cast<float>(strip.y) * fh / static_cast<float>(context.previewSize.height));
    const ImVec2 uv1(static_cast<float>(strip.x + strip.w),
                     static_cast<float>(strip.y + strip.h) * fh / static_cast<float>(context.previewSize.height));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(context.preview, size, uv0, uv1);

    const float iconPixels = kItemIconPixels * fh / kReferenceHeight;
    if (ImGui::IsItemClicked()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        iconCentre_ = cv::Point(static_cast<int>(static_cast<float>(strip.x) * fw + (mouse.x - origin.x) / zoom),
                                static_cast<int>(static_cast<float>(strip.y) * fh + (mouse.y - origin.y) / zoom));
        iconPicked_ = true;
        iconFilter_[0] = '\0';
        ImGui::OpenPopup("##pick_item");
    }
    if (!iconPicked_) {
        return;
    }

    // Where the picture will be cut, and what it will be called.
    const ImVec2 at(origin.x + (static_cast<float>(iconCentre_.x) - static_cast<float>(strip.x) * fw - iconPixels * 0.5f) * zoom,
                    origin.y + (static_cast<float>(iconCentre_.y) - static_cast<float>(strip.y) * fh - iconPixels * 0.5f) * zoom);
    ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + iconPixels * zoom, at.y + iconPixels * zoom), kYouBox, 0.0f, 0, 2.0f);

    if (ImGui::BeginPopup("##pick_item")) {
        ImGui::TextUnformatted("Which item is in the pink box? Type to filter.");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##filter", iconFilter_, sizeof(iconFilter_));
        std::string filter(iconFilter_);
        std::ranges::transform(filter, filter.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (ImGui::BeginChild("##names", ImVec2(ImGui::GetFontSize() * 20.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f),
                              ImGuiChildFlags_Borders)) {
            for (const auto& [name, id] : *context.itemNames) {
                std::string lower = name;
                std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (!filter.empty() && lower.find(filter) == std::string::npos) {
                    continue;
                }
                if (ImGui::Selectable(name.c_str())) {
                    const int s = static_cast<int>(std::lround(iconPixels));
                    actions.learnIcon = std::pair(id, cv::Rect(iconCentre_.x - s / 2, iconCentre_.y - s / 2, s, s));
                    iconPicked_ = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
}

void SettingsMenu::drawPositions(MenuActions& actions)
{
    heading("Positions");
    ImGui::TextDisabled("Yellow boxes above: where each enemy's items are read.");

    if (!edited_.enemyRows.empty()) {
        // Moving the top keeps the bottom where it is, so no row of icons falls out.
        double top = edited_.enemyRows.front().y;
        const double bottom = top + edited_.enemyRows.front().h;
        if (pixelSlider("Item area: top edge", "itemTop", top, 200, 450)) {
            actions.apply = true;
        }
        double end = bottom;
        if (pixelSlider("Item area: bottom edge", "itemBottom", end, 451, 800)) {
            actions.apply = true;
        }
        for (auto& r : edited_.enemyRows) {
            r.y = top;
            r.h = end - top;
        }
    }

    float panelX = static_cast<float>(edited_.panelX);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::SliderFloat("Panel: left edge", &panelX, 0.0f, 0.83f, "%.3f")) {
        edited_.panelX = panelX;
    }
    actions.apply = ImGui::IsItemDeactivatedAfterEdit() || actions.apply;
    float panelY = static_cast<float>(edited_.panelY);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::SliderFloat("Panel: top edge", &panelY, 0.0f, 0.60f, "%.3f")) {
        edited_.panelY = panelY;
    }
    actions.apply = ImGui::IsItemDeactivatedAfterEdit() || actions.apply;
}

void SettingsMenu::drawRecognition(MenuActions& actions)
{
    heading("Recognition");
    actions.apply = sliderDouble("Item match threshold", edited_.matchThreshold, 0.70, 0.99, "%.2f") || actions.apply;
    actions.apply = sliderDouble("Hero match threshold", edited_.heroThreshold, 0.60, 0.99, "%.2f") || actions.apply;
    actions.apply = sliderDouble("Hero lead over the next-best", edited_.heroMargin, 0.05, 0.60, "%.2f") || actions.apply;
    actions.apply = sliderInt("Scan threads (more = quicker scan, less for the game)", edited_.scanThreads, 1, 8) || actions.apply;
    actions.apply = sliderInt("Wait after pressing TAB (ms)", edited_.captureDelayMs, 0, 1000) || actions.apply;
    actions.apply = sliderInt("Keep the panel after TAB (ms)", edited_.panelHoldMs, 0, 20000) || actions.apply;
    actions.apply = sliderInt("Buying reminder every (minutes, 0 = off)", edited_.reminderMinutes, 0, 15) || actions.apply;
    actions.apply = sliderInt("Suggestions shown", edited_.maxSuggestions, 3, 10) || actions.apply;
}

void SettingsMenu::keyRow(const char* label, int& key)
{
    ImGui::PushID(label);
    const bool waiting = waitingForKey_ == &key;
    const std::string text = waiting ? std::string("Press a key...") : keyName(key);
    if (ImGui::Button(text.c_str(), ImVec2(130.0f * ImGui::GetFontSize() / 15.0f, 0.0f))) {
        waitingForKey_ = &key;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
}

void SettingsMenu::drawHotkeys()
{
    heading("Hotkeys");
    ImGui::TextDisabled("Click a key, then press the new one. Escape cancels.");
    keyRow("Read the scoreboard", edited_.hotkeys.scoreboard);
    keyRow("Pin the panel", edited_.hotkeys.pinPanel);
    keyRow("This menu", edited_.hotkeys.menu);
    keyRow("Calibration mode", edited_.hotkeys.calibrate);
    keyRow("Reload files", edited_.hotkeys.reload);
    keyRow("Save debug images", edited_.hotkeys.debugDump);
    keyRow("Auto-tune icon size (with TAB)", edited_.hotkeys.tuneScale);
}
