#include "PanelView.h"

#include <algorithm>
#include <cfloat>
#include <format>
#include <iterator>
#include <string_view>

namespace {

constexpr ImU32 kBackground = IM_COL32(13, 15, 21, 240);
constexpr ImU32 kBorder = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kRule = IM_COL32(255, 255, 255, 22);
constexpr ImU32 kText = IM_COL32(236, 238, 243, 255);
constexpr ImU32 kMuted = IM_COL32(150, 156, 170, 255);
constexpr ImU32 kAccent = IM_COL32(255, 200, 92, 255);
constexpr ImU32 kTip = IM_COL32(150, 180, 245, 255);
constexpr ImU32 kAvoid = IM_COL32(236, 124, 112, 255);
constexpr ImU32 kPlaceholder = IM_COL32(40, 44, 54, 255);

constexpr ImU32 kWeapon = IM_COL32(240, 158, 66, 255);
constexpr ImU32 kVitality = IM_COL32(112, 196, 92, 255);
constexpr ImU32 kSpirit = IM_COL32(178, 128, 235, 255);

// Pixels at 1080p.
constexpr float kPadding = 12.0f;
constexpr float kItemIcon = 40.0f;
constexpr float kHeroIcon = 38.0f;
constexpr float kOwnedIcon = 20.0f;
constexpr float kGap = 10.0f;
constexpr float kRowGap = 9.0f;
constexpr float kBody = 15.0f;
constexpr float kSmall = 13.0f;

constexpr size_t kMaxTipsPerPlayer = 1;

// How much text the panel shows. Early in a match everything fits; six enemies with full builds
// need the shorter forms to stay on screen.
struct Detail {
    bool wrap;          // long text may take several lines, instead of one cut short
    size_t maxAvoid;    // items listed in each enemy's "avoid" line
    bool howToBuy;      // show what an item builds on
};
constexpr Detail kDetailLevels[] = {
    {true, 3, true},
    {true, 1, true},
    {false, 1, true},
    {false, 1, false},
    {false, 0, false},
};

ImU32 slotColor(std::string_view slot)
{
    if (slot == "weapon") return kWeapon;
    if (slot == "vitality") return kVitality;
    if (slot == "spirit") return kSpirit;
    return kMuted;
}

const char* slotLabel(std::string_view slot)
{
    if (slot == "weapon") return "Weapon";
    if (slot == "vitality") return "Vitality";
    if (slot == "spirit") return "Spirit";
    return "";
}

ImU32 buildColor(std::string_view build)
{
    if (build == "Gun build") return kWeapon;
    if (build == "Tank build") return kVitality;
    if (build == "Spirit build") return kSpirit;
    return kMuted;
}

const char* timingLabel(BuyTiming timing)
{
    switch (timing) {
    case BuyTiming::Now: return "NOW";
    case BuyTiming::Next: return "NEXT";
    case BuyTiming::Later: return "LATER";
    default: return "";
    }
}

ImU32 timingColor(BuyTiming timing)
{
    switch (timing) {
    case BuyTiming::Now: return kVitality;
    case BuyTiming::Next: return kAccent;
    default: return kMuted;
    }
}

ImU32 withAlpha(ImU32 color, int alpha)
{
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

// counters.json starts each reason with "Spirit T4 | "; the panel shows slot and tier on their own.
std::string_view reasonText(std::string_view reason)
{
    const auto bar = reason.find(" | ");
    return bar == std::string_view::npos ? reason : reason.substr(bar + 3);
}

std::string join(const std::vector<std::string>& parts)
{
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) {
            out += ", ";
        }
        out += p;
    }
    return out;
}

// Draws straight onto the window, top to bottom, and tells ImGui afterwards how tall it all was.
class Canvas {
public:
    // Without `drawing` it only measures: the layout code runs the same, and used() tells the height.
    Canvas(ImVec2 origin, float width, const PanelStyle& style, bool drawing)
        : list_(ImGui::GetWindowDrawList())
        , origin_(origin)
        , width_(width)
        , style_(style)
        , drawing_(drawing)
    {
    }

    bool drawing() const { return drawing_; }

    float px(float value) const { return value * style_.scale; }
    float y() const { return origin_.y + used_; }
    float left() const { return origin_.x; }
    float width() const { return width_; }
    void advance(float height) { used_ += height; }
    float used() const { return used_; }
    ImDrawList* list() const { return list_; }

    float fontSize(float size) const { return px(size); }
    ImVec2 measure(bool bold, float size, std::string_view text, float wrap = 0.0f) const
    {
        ImFont* font = fontOf(bold);
        return font->CalcTextSizeA(px(size), FLT_MAX, wrap, text.data(), text.data() + text.size());
    }

    // Returns the height used.
    float text(ImVec2 at, bool bold, float size, ImU32 color, std::string_view s, float wrap = 0.0f) const
    {
        if (drawing_) {
            list_->AddText(fontOf(bold), px(size), at, color, s.data(), s.data() + s.size(), wrap);
        }
        return measure(bold, size, s, wrap).y;
    }

    // Several lines if the detail level allows it, otherwise one line, cut short.
    float longText(ImVec2 at, ImU32 color, std::string_view s, float width, const Detail& detail) const
    {
        if (detail.wrap) {
            return text(at, false, kSmall, color, s, width);
        }
        std::string line(s);
        while (!line.empty() && measure(false, kSmall, line + "...").x > width) {
            line.pop_back();
        }
        return text(at, false, kSmall, color, line.size() == s.size() ? line : line + "...");
    }

    void picture(ImVec2 at, float size, ImTextureID texture, ImU32 frame, std::string_view fallback) const
    {
        if (!drawing_) {
            return;
        }
        const ImVec2 end(at.x + size, at.y + size);
        const float round = size * 0.14f;
        list_->AddRectFilled(at, end, kPlaceholder, round);
        if (texture != ImTextureID{}) {
            list_->AddImageRounded(texture, at, end, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
        } else if (!fallback.empty()) {
            const std::string_view letter = fallback.substr(0, 1);
            const ImVec2 box = measure(true, kBody, letter);
            text(ImVec2(at.x + (size - box.x) / 2.0f, at.y + (size - box.y) / 2.0f), true, kBody, kMuted, letter);
        }
        list_->AddRect(at, end, frame, round, 0, std::max(1.0f, px(1.5f)));
    }

    void heading(std::string_view title)
    {
        const float h = text(ImVec2(left(), y()), true, kSmall, kAccent, title);
        const float x = left() + measure(true, kSmall, title).x + px(8.0f);
        const float mid = y() + h * 0.5f;
        if (drawing_) {
            list_->AddLine(ImVec2(x, mid), ImVec2(left() + width_, mid), kRule, 1.0f);
        }
        advance(h + px(7.0f));
    }

private:
    ImFont* fontOf(bool bold) const
    {
        ImFont* font = bold ? style_.bold : style_.regular;
        return font ? font : ImGui::GetFont();
    }

    ImDrawList* list_;
    ImVec2 origin_;
    float width_;
    PanelStyle style_;
    bool drawing_;
    float used_ = 0.0f;
};

void drawSuggestion(Canvas& c, const Suggestion& s, const PanelIcons& icons, const Detail& detail)
{
    const float top = c.y();
    const float iconSize = c.px(kItemIcon);
    const ImU32 color = slotColor(s.slot);
    c.picture(ImVec2(c.left(), top), iconSize, s.itemId.empty() || !c.drawing() ? ImTextureID{} : icons.item(s.itemId),
              color, s.item);

    const float x = c.left() + iconSize + c.px(kGap);
    const float wrap = c.left() + c.width() - x;
    float line = top - c.px(1.0f);
    const float nameHeight = c.text(ImVec2(x, line), true, kBody, kText, s.item);

    // When to buy it, at the far end of the name's line.
    if (const std::string_view timing = timingLabel(s.timing); !timing.empty() && c.drawing()) {
        const ImVec2 size = c.measure(true, kSmall, timing);
        const float pad = c.px(5.0f);
        const ImVec2 at(c.left() + c.width() - size.x - pad, line + (nameHeight - size.y) * 0.5f);
        c.list()->AddRectFilled(ImVec2(at.x - pad, at.y), ImVec2(at.x + size.x + pad, at.y + size.y),
                                withAlpha(timingColor(s.timing), 50), size.y * 0.5f);
        c.text(at, true, kSmall, timingColor(s.timing), timing);
    }
    line += nameHeight;

    // "Spirit T4   6400", the shop tab first: that is where to look.
    float metaX = x;
    const std::string shop = s.tier > 0 ? std::format("{} T{}", slotLabel(s.slot), s.tier) : slotLabel(s.slot);
    float metaHeight = c.text(ImVec2(metaX, line), true, kSmall, color, shop);
    metaX += c.measure(true, kSmall, shop).x + c.px(8.0f);
    if (s.cost > 0) {
        const std::string cost = std::format("{} souls", s.cost);
        c.text(ImVec2(metaX, line), false, kSmall, kMuted, cost);
        metaX += c.measure(false, kSmall, cost).x + c.px(8.0f);
    }
    if (s.buyMinute > 0) {
        const std::string minute = std::format("usually min {}", s.buyMinute);
        const ImVec2 size = c.measure(false, kSmall, minute);
        const float right = c.left() + c.width();
        if (right - size.x > metaX) {
            c.text(ImVec2(right - size.x, line), false, kSmall, kMuted, minute);
        }
    }
    line += metaHeight;
    if (detail.howToBuy && !s.upgrades.empty()) {
        line += c.longText(ImVec2(x, line), kVitality, "Upgrades your " + s.upgrades, wrap, detail);
    } else if (detail.howToBuy && !s.buildFrom.empty()) {
        line += c.longText(ImVec2(x, line), kTip, "Cheaper after " + join(s.buildFrom), wrap, detail);
    }
    if (const std::string_view reason = reasonText(s.reason); !reason.empty()) {
        line += c.longText(ImVec2(x, line), kMuted, reason, wrap, detail);
    }
    c.advance(std::max(line - top, iconSize) + c.px(kRowGap));
}

void drawPlayer(Canvas& c, const EnemyPlayer& p, const PanelIcons& icons, const Detail& detail)
{
    const float top = c.y();
    const float iconSize = c.px(kHeroIcon);
    const ImU32 color = p.build.empty() ? kMuted : buildColor(p.build);
    c.picture(ImVec2(c.left(), top), iconSize, p.heroId.empty() || !c.drawing() ? ImTextureID{} : icons.hero(p.heroId),
              color, "?");

    const float x = c.left() + iconSize + c.px(kGap);
    const float wrap = c.left() + c.width() - x;
    const std::string name = p.hero.empty() ? std::format("Enemy #{}", p.slot + 1) : p.hero;
    float line = top - c.px(1.0f);
    const float nameHeight = c.text(ImVec2(x, line), true, kBody, p.hero.empty() ? kMuted : kText, name);

    // Build pill, on the same line as the name.
    if (!p.build.empty() && c.drawing()) {
        const ImVec2 size = c.measure(true, kSmall, p.build);
        const ImVec2 at(x + c.measure(true, kBody, name).x + c.px(10.0f), line + (nameHeight - size.y) * 0.5f);
        const float pad = c.px(5.0f);
        c.list()->AddRectFilled(ImVec2(at.x - pad, at.y), ImVec2(at.x + size.x + pad, at.y + size.y), withAlpha(color, 50),
                                size.y * 0.5f);
        c.text(at, true, kSmall, color, p.build);
    }
    line += nameHeight + c.px(1.0f);

    if (p.itemIds.empty()) {
        line += c.text(ImVec2(x, line), false, kSmall, kMuted, "no items yet");
    } else {
        const float owned = c.px(kOwnedIcon);
        const float step = owned + c.px(2.0f);
        float ownedX = x;
        for (const auto& id : p.itemIds) {
            if (ownedX + owned > c.left() + c.width()) {
                ownedX = x;
                line += step;
            }
            c.picture(ImVec2(ownedX, line), owned, c.drawing() ? icons.item(id) : ImTextureID{}, kRule, {});
            ownedX += step;
        }
        line += step;
    }

    for (size_t t = 0; t < p.tips.size() && t < kMaxTipsPerPlayer; ++t) {
        line += c.longText(ImVec2(x, line), kTip, p.tips[t], wrap, detail);
    }
    if (!p.avoid.empty() && detail.maxAvoid > 0) {
        const std::vector<std::string> shown(p.avoid.begin(),
                                             p.avoid.begin() + static_cast<std::ptrdiff_t>(std::min(detail.maxAvoid, p.avoid.size())));
        line += c.longText(ImVec2(x, line), kAvoid, "Avoid: " + join(shown), wrap, detail);
    }
    c.advance(std::max(line - top, iconSize) + c.px(kRowGap));
}

void layout(Canvas& c, const PanelModel& model, const PanelIcons& icons, const Detail& detail)
{
    const float inner = c.width();

    // Title line, status under it.
    const float titleHeight = c.text(ImVec2(c.left(), c.y()), true, kBody, kText, "BUILD HELPER");
    if (model.pinned) {
        const std::string_view pinned = "PINNED";
        const float x = c.left() + inner - c.measure(true, kSmall, pinned).x;
        c.text(ImVec2(x, c.y() + c.px(1.0f)), true, kSmall, kAccent, pinned);
    }
    c.advance(titleHeight);
    c.advance(c.text(ImVec2(c.left(), c.y()), false, kSmall, kMuted, model.status, inner) + c.px(2.0f));
    if (!model.you.empty()) {
        c.advance(c.text(ImVec2(c.left(), c.y()), false, kSmall, kTip, model.you, inner));
    }
    c.advance(c.px(8.0f));

    if (model.me) {
        c.heading("YOU");
        drawPlayer(c, *model.me, icons, detail);
        for (const auto& line : model.myWeakItems) {
            c.advance(c.longText(ImVec2(c.left(), c.y()), kAvoid, line, inner, detail));
        }
        c.advance(c.px(6.0f));
    }

    if (!model.newPurchases.empty()) {
        c.heading("NEW SINCE LAST LOOK");
        for (const auto& line : model.newPurchases) {
            c.advance(c.longText(ImVec2(c.left(), c.y()), kAvoid, line, inner, detail));
        }
        c.advance(c.px(8.0f));
    }

    c.heading("BUY ORDER");
    if (model.suggestions.empty()) {
        c.advance(c.text(ImVec2(c.left(), c.y()), false, kSmall, kMuted, "Nothing to counter yet.") + c.px(kRowGap));
    }
    for (const auto& s : model.suggestions) {
        drawSuggestion(c, s, icons, detail);
    }

    c.advance(c.px(3.0f));
    if (model.perPlayer) {
        c.heading("ENEMIES");
        if (model.players.empty()) {
            c.advance(c.text(ImVec2(c.left(), c.y()), false, kSmall, kMuted, "None detected. Hold TAB in a match."));
        }
        for (const auto& p : model.players) {
            drawPlayer(c, p, icons, detail);
        }
    } else {
        c.heading("ENEMY ITEMS");
        if (model.enemyItems.empty()) {
            c.advance(c.text(ImVec2(c.left(), c.y()), false, kSmall, kMuted, "None detected."));
        }
        for (const auto& item : model.enemyItems) {
            const std::string label =
                item.count > 1 ? std::format("{}  x{}", item.displayName, item.count) : item.displayName;
            c.advance(c.text(ImVec2(c.left(), c.y()), false, kBody, kText, label) + c.px(2.0f));
        }
    }
}

}  // namespace

void drawReminder(const Suggestion& next, ImVec2 pos, float width, const PanelStyle& style, const PanelIcons& icons)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * style.scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadding * style.scale, kPadding * style.scale));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##reminder", nullptr, flags)) {
        const float inner = width - 2.0f * kPadding * style.scale;
        Canvas c(ImGui::GetCursorScreenPos(), inner, style, true);
        c.heading("TIME TO BUY");
        drawSuggestion(c, next, icons, kDetailLevels[0]);
        c.advance(c.text(ImVec2(c.left(), c.y()), false, kSmall, kMuted, "Hold TAB to look at the enemies again."));
        ImGui::Dummy(ImVec2(inner, c.used()));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void drawPanel(const PanelModel& model, ImVec2 pos, float width, float maxHeight, const PanelStyle& style,
               const PanelIcons& icons)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, maxHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * style.scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadding * style.scale, kPadding * style.scale));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##build_helper", nullptr, flags)) {
        const float inner = width - 2.0f * kPadding * style.scale;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // The most detail that still fits on screen.
        const float room = maxHeight - 2.0f * kPadding * style.scale;
        Detail detail = kDetailLevels[std::size(kDetailLevels) - 1];
        for (const Detail& candidate : kDetailLevels) {
            Canvas measuring(origin, inner, style, false);
            layout(measuring, model, icons, candidate);
            if (measuring.used() <= room) {
                detail = candidate;
                break;
            }
        }

        Canvas c(origin, inner, style, true);
        layout(c, model, icons, detail);
        ImGui::Dummy(ImVec2(inner, c.used()));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
