#pragma once

#include "matchbook/types.hpp"

#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mb {

/// An order that is resting in the book, waiting to be matched.
struct Resting {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty remaining = 0;
    std::uint64_t seq = 0;  ///< arrival sequence, used for time priority
    /// Iceberg orders only: quantity held in reserve behind the visible
    /// `remaining` slice, and the slice size used to replenish it.
    Qty hidden = 0;
    Qty display = 0;

    bool operator==(const Resting&) const = default;
};

/// All orders at one price, oldest first (FIFO / time priority).
struct Level {
    Qty total = 0;
    std::list<Resting> orders;
};

/// Aggregated view of one price level, as published in market data. Only
/// visible quantity is counted; iceberg reserves stay hidden.
struct LevelView {
    Price price = 0;
    Qty qty = 0;
    std::size_t orders = 0;

    bool operator==(const LevelView&) const = default;
};

std::ostream& operator<<(std::ostream& os, const LevelView& v);

/// One entry of the incremental (level 2) market-data feed: the new state of
/// a price level. `qty == 0` means the level is gone.
struct LevelUpdate {
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    std::size_t orders = 0;

    [[nodiscard]] bool deleted() const { return qty == 0; }
    bool operator==(const LevelUpdate&) const = default;
};

std::string to_string(const LevelUpdate& u);
std::ostream& operator<<(std::ostream& os, const LevelUpdate& u);

/// Price-time priority limit order book.
///
/// Each side is an ordered map from price to a FIFO queue of orders; an
/// id -> iterator index makes cancel and in-place reduce O(1) after the map
/// lookup. The book is a passive data structure: matching rules live in
/// Engine, so the book can be tested (and reused) on its own.
class Book {
public:
    /// Adds an order at the back of its price level. Throws
    /// std::invalid_argument on a duplicate id or non-positive quantity.
    void insert(const Resting& order);

    /// Removes an order. Returns false if the id is not resting.
    bool erase(OrderId id);

    /// Changes an order's remaining quantity without losing queue position.
    /// Throws std::invalid_argument for unknown ids or quantity <= 0.
    void set_remaining(OrderId id, Qty remaining);

    /// Changes an order's hidden reserve without touching its queue position.
    /// Throws std::invalid_argument for unknown ids or quantity < 0.
    void set_hidden(OrderId id, Qty hidden);

    /// Gives an order a new visible slice and hidden reserve and moves it to
    /// the back of its price level with a new sequence number (iceberg
    /// replenishment: each new slice queues like a fresh order).
    void requeue(OrderId id, Qty visible, Qty hidden, std::uint64_t seq);

    [[nodiscard]] const Resting* find(OrderId id) const;
    [[nodiscard]] bool contains(OrderId id) const { return index_.contains(id); }

    [[nodiscard]] std::optional<Price> best(Side side) const;
    [[nodiscard]] std::optional<Price> best_bid() const { return best(Side::Buy); }
    [[nodiscard]] std::optional<Price> best_ask() const { return best(Side::Sell); }

    /// Oldest order at the best price on `side`, or nullptr when that side is empty.
    [[nodiscard]] const Resting* front(Side side) const;

    /// Visible quantity at a price (iceberg reserves are not included).
    [[nodiscard]] Qty volume_at(Side side, Price price) const;
    /// Up to `max_levels` levels from the best price outward.
    [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t max_levels) const;

    [[nodiscard]] std::size_t order_count() const { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const { return levels(side).size(); }
    [[nodiscard]] bool empty() const { return index_.empty(); }

    /// Starts (or stops) recording which price levels change. While on, every
    /// insert, erase and quantity change remembers the level's state before
    /// its first change, so take_level_updates() can report net differences.
    void track_levels(bool on);
    [[nodiscard]] bool tracking_levels() const { return track_; }

    /// Returns the levels whose visible quantity or order count changed since
    /// the last call, in the order they were first touched, and resets the
    /// record. Levels that changed and then changed back are left out, so an
    /// iceberg replenishing in place or a cancel/re-add of the same size
    /// produces no update.
    std::vector<LevelUpdate> take_level_updates();

    /// Visits resting orders on `side` in matching priority (best price first,
    /// then oldest first). `fn` returns false to stop early.
    template <class Fn>
    void walk(Side side, Fn&& fn) const {
        auto visit = [&](auto first, auto last) {
            for (; first != last; ++first)
                for (const Resting& r : first->second.orders)
                    if (!fn(r)) return;
        };
        if (side == Side::Buy) visit(bids_.rbegin(), bids_.rend());
        else visit(asks_.begin(), asks_.end());
    }

private:
    using Levels = std::map<Price, Level>;
    struct Locator {
        Side side;
        Price price;
        std::list<Resting>::iterator it;
    };

    void touch(Side side, Price price);
    [[nodiscard]] LevelView view(Side side, Price price) const;

    Levels& levels(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const Levels& levels(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    Levels bids_;  // best bid = highest key
    Levels asks_;  // best ask = lowest key
    std::unordered_map<OrderId, Locator> index_;

    bool track_ = false;
    std::map<std::pair<Side, Price>, LevelView> before_;  // state at first touch
    std::vector<std::pair<Side, Price>> touched_;         // first-touch order
};

}  // namespace mb
