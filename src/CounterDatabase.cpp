#include "CounterDatabase.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace {

// Shop prices per tier (the same for all items of a tier). Tier 5 items are not bought with souls.
int costOfTier(int tier)
{
    switch (tier) {
    case 1: return 800;
    case 2: return 1600;
    case 3: return 3200;
    case 4: return 6400;
    default: return 0;
    }
}

// What you can reasonably save up for, by what you have spent so far: tier 1-2 in the lane, tier 3
// once you own a few items, tier 4 late. The next tier up is "next", anything above "later".
BuyTiming timingOf(int tier, int spentSouls)
{
    if (spentSouls < 0 || tier <= 0) {
        return BuyTiming::Unknown;
    }
    const int nowUpToTier = spentSouls < 3000 ? 2 : spentSouls < 15000 ? 3 : 4;
    return tier <= nowUpToTier ? BuyTiming::Now : tier == nowUpToTier + 1 ? BuyTiming::Next : BuyTiming::Later;
}

template <typename Table>
Table parseSection(const nlohmann::json& root, const char* section)
{
    Table table;
    if (!root.contains(section)) {
        return table;
    }
    for (const auto& [id, node] : root[section].items()) {
        typename Table::mapped_type entry;
        entry.display = node.value("display", id);
        entry.weight = node.value("weight", 1);
        entry.slot = node.value("slot", std::string{});
        entry.tier = node.value("tier", 0);
        entry.buyMinute = node.value("buy_minute", 0);
        entry.components = node.value("components", std::vector<std::string>{});
        entry.tip = node.value("tip", std::string{});
        entry.avoid = node.value("avoid", std::vector<std::string>{});
        for (const auto& c : node.value("counters", nlohmann::json::array())) {
            entry.counters.push_back({c.at("item").get<std::string>(), c.value("reason", std::string{}),
                                      c.value("priority", 1)});
        }
        table.emplace(id, std::move(entry));
    }
    return table;
}

}  // namespace

bool CounterDatabase::load(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in) {
        std::cerr << "[counters] cannot open " << file.string() << '\n';
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
        if (!j.contains("items")) {
            std::cerr << "[counters] " << file.string() << " has no \"items\" section\n";
            return false;
        }
        Table items = parseSection<Table>(j, "items");
        Table heroes = parseSection<Table>(j, "heroes");
        Table archetypes = parseSection<Table>(j, "archetypes");

        items_ = std::move(items);
        heroes_ = std::move(heroes);
        archetypes_ = std::move(archetypes);
        itemIdByName_.clear();
        for (const auto& [id, entry] : items_) {
            itemIdByName_[entry.display] = id;
        }
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[counters] parse error in " << file.string() << ": " << e.what() << '\n';
        return false;
    }
}

std::string CounterDatabase::nameIn(const Table& table, const std::string& id)
{
    const auto it = table.find(id);
    return it != table.end() ? it->second.display : id;
}

std::string CounterDatabase::displayName(const std::string& itemId) const
{
    return nameIn(items_, itemId);
}

std::set<std::string> CounterDatabase::heroIds() const
{
    std::set<std::string> ids;
    for (const auto& [id, entry] : heroes_) {
        ids.insert(id);
    }
    return ids;
}

std::string CounterDatabase::heroName(const std::string& heroId) const
{
    return nameIn(heroes_, heroId);
}

std::string CounterDatabase::archetypeName(const std::string& slot) const
{
    return nameIn(archetypes_, slot);
}

std::string CounterDatabase::slotOf(const std::string& itemId) const
{
    const auto it = items_.find(itemId);
    return it != items_.end() ? it->second.slot : std::string{};
}

int CounterDatabase::costOf(const std::string& itemId) const
{
    const auto it = items_.find(itemId);
    return it != items_.end() ? costOfTier(it->second.tier) : 0;
}

std::string CounterDatabase::tipFor(const std::string& itemId) const
{
    const auto it = items_.find(itemId);
    return it != items_.end() ? it->second.tip : std::string{};
}

std::vector<std::string> CounterDatabase::avoidAgainst(const std::string& heroId) const
{
    const auto it = heroes_.find(heroId);
    return it != heroes_.end() ? it->second.avoid : std::vector<std::string>{};
}

std::vector<Suggestion> CounterDatabase::suggest(const SuggestInput& input, size_t maxResults) const
{
    std::map<std::string, Suggestion> byItem;
    std::map<std::string, int> strongest;  // largest single contribution per suggestion so far

    const auto accumulate = [&](const Table& table, const std::map<std::string, int>& seen) {
        for (const auto& [id, count] : seen) {
            const auto it = table.find(id);
            if (it == table.end()) {
                continue;
            }
            const Entry& entry = it->second;
            for (const auto& counter : entry.counters) {
                Suggestion& s = byItem[counter.item];
                const int points = count * entry.weight * counter.priority;
                // Show the reason of whatever contributes most to this suggestion.
                if (int& best = strongest[counter.item]; points > best) {
                    best = points;
                    s.reason = counter.reason;
                }
                s.item = counter.item;
                s.score += points;
                s.answers.push_back(entry.display);
            }
        }
    };
    accumulate(items_, input.items);
    accumulate(heroes_, input.heroes);
    accumulate(archetypes_, input.archetypes);

    std::vector<Suggestion> out;
    out.reserve(byItem.size());
    for (auto& [_, s] : byItem) {
        if (const auto id = itemIdByName_.find(s.item); id != itemIdByName_.end()) {
            if (input.owned.contains(id->second)) {
                continue;
            }
            const Entry& item = items_.at(id->second);
            s.itemId = id->second;
            s.slot = item.slot;
            s.tier = item.tier;
            s.cost = costOfTier(item.tier);
            s.timing = timingOf(item.tier, input.spentSouls);
            s.buyMinute = item.buyMinute;
            for (const auto& component : item.components) {
                if (input.owned.contains(component)) {
                    s.upgrades = displayName(component);
                    s.cost = std::max(0, s.cost - costOf(component));
                } else {
                    s.buildFrom.push_back(displayName(component));
                }
            }
        }
        out.push_back(std::move(s));
    }
    std::ranges::stable_sort(out, std::greater{}, &Suggestion::score);
    if (out.size() > maxResults) {
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(maxResults), out.end());
    }
    std::ranges::stable_sort(out, {}, &Suggestion::timing);
    return out;
}
