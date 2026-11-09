#include "matchbook/book.hpp"

#include <stdexcept>
#include <string>

namespace mb {

std::ostream& operator<<(std::ostream& os, const LevelView& v) {
    return os << v.qty << "@" << v.price << " (" << v.orders << ")";
}

std::string to_string(const LevelUpdate& u) {
    std::string s = std::string("L2 ") + (u.side == Side::Buy ? "bid" : "ask") + " px=" + std::to_string(u.price);
    if (u.deleted()) return s + " deleted";
    return s + " qty=" + std::to_string(u.qty) + " orders=" + std::to_string(u.orders);
}

std::ostream& operator<<(std::ostream& os, const LevelUpdate& u) { return os << to_string(u); }

void Book::touch(Side side, Price price) {
    if (!track_) return;
    if (before_.try_emplace({side, price}, view(side, price)).second) touched_.emplace_back(side, price);
}

LevelView Book::view(Side side, Price price) const {
    const Levels& lv = levels(side);
    auto it = lv.find(price);
    if (it == lv.end()) return {price, 0, 0};
    return {price, it->second.total, it->second.orders.size()};
}

void Book::track_levels(bool on) {
    track_ = on;
    before_.clear();
    touched_.clear();
}

std::vector<LevelUpdate> Book::take_level_updates() {
    std::vector<LevelUpdate> out;
    for (const auto& [side, price] : touched_) {
        const LevelView now = view(side, price);
        if (now != before_.at({side, price})) out.push_back({side, price, now.qty, now.orders});
    }
    before_.clear();
    touched_.clear();
    return out;
}

void Book::insert(const Resting& order) {
    if (order.remaining <= 0)
        throw std::invalid_argument("resting quantity must be positive");
    if (index_.contains(order.id))
        throw std::invalid_argument("duplicate order id " + std::to_string(order.id));
    touch(order.side, order.price);
    Level& level = levels(order.side)[order.price];
    level.orders.push_back(order);
    level.total += order.remaining;
    index_.emplace(order.id, Locator{order.side, order.price, std::prev(level.orders.end())});
}

bool Book::erase(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Locator loc = found->second;
    touch(loc.side, loc.price);
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
    touch(loc.side, loc.price);
    Level& level = levels(loc.side).at(loc.price);
    level.total += remaining - loc.it->remaining;
    loc.it->remaining = remaining;
}

void Book::set_hidden(OrderId id, Qty hidden) {
    if (hidden < 0) throw std::invalid_argument("hidden quantity must not be negative");
    auto found = index_.find(id);
    if (found == index_.end())
        throw std::invalid_argument("unknown order id " + std::to_string(id));
    found->second.it->hidden = hidden;
}

void Book::requeue(OrderId id, Qty visible, Qty hidden, std::uint64_t seq) {
    const Resting* current = find(id);
    if (current == nullptr) throw std::invalid_argument("unknown order id " + std::to_string(id));
    if (visible <= 0) throw std::invalid_argument("resting quantity must be positive");
    if (hidden < 0) throw std::invalid_argument("hidden quantity must not be negative");
    Resting moved = *current;
    moved.remaining = visible;
    moved.hidden = hidden;
    moved.seq = seq;
    erase(id);
    insert(moved);
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
