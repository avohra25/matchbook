#include "matchbook/book.hpp"

#include <stdexcept>
#include <string>

namespace mb {

std::ostream& operator<<(std::ostream& os, const LevelView& v) {
    return os << v.qty << "@" << v.price << " (" << v.orders << ")";
}

void Book::insert(const Resting& order) {
    if (order.remaining <= 0)
        throw std::invalid_argument("resting quantity must be positive");
    if (index_.contains(order.id))
        throw std::invalid_argument("duplicate order id " + std::to_string(order.id));
    Level& level = levels(order.side)[order.price];
    level.orders.push_back(order);
    level.total += order.remaining;
    index_.emplace(order.id, Locator{order.side, order.price, std::prev(level.orders.end())});
}

bool Book::erase(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Locator loc = found->second;
    Levels& side = levels(loc.side);
    auto level = side.find(loc.price);
    level->second.total -= loc.it->remaining;
    level->second.orders.erase(loc.it);
    if (level->second.orders.empty()) side.erase(level);
    index_.erase(found);
    return true;
}

void Book::set_remaining(OrderId id, Qty remaining) {
    if (remaining <= 0) throw std::invalid_argument("remaining must be positive");
    auto found = index_.find(id);
    if (found == index_.end())
        throw std::invalid_argument("unknown order id " + std::to_string(id));
    const Locator& loc = found->second;
    Level& level = levels(loc.side).at(loc.price);
    level.total += remaining - loc.it->remaining;
    loc.it->remaining = remaining;
}

const Resting* Book::find(OrderId id) const {
    auto found = index_.find(id);
    return found == index_.end() ? nullptr : &*found->second.it;
}

std::optional<Price> Book::best(Side side) const {
    const Levels& lv = levels(side);
    if (lv.empty()) return std::nullopt;
    return side == Side::Buy ? lv.rbegin()->first : lv.begin()->first;
}

const Resting* Book::front(Side side) const {
    const Levels& lv = levels(side);
    if (lv.empty()) return nullptr;
    const Level& level = side == Side::Buy ? lv.rbegin()->second : lv.begin()->second;
    return &level.orders.front();
}

Qty Book::volume_at(Side side, Price price) const {
    const Levels& lv = levels(side);
    auto it = lv.find(price);
    return it == lv.end() ? 0 : it->second.total;
}

std::vector<LevelView> Book::depth(Side side, std::size_t max_levels) const {
    std::vector<LevelView> out;
    auto collect = [&](auto first, auto last) {
        for (; first != last && out.size() < max_levels; ++first)
            out.push_back({first->first, first->second.total, first->second.orders.size()});
    };
    const Levels& lv = levels(side);
    if (side == Side::Buy) collect(lv.rbegin(), lv.rend());
    else collect(lv.begin(), lv.end());
    return out;
}

}  // namespace mb
