#pragma once

#include "matchbook/book.hpp"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mb {

enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

/// What happens when an incoming order would trade against a resting order
/// from the same trader.
enum class StpMode : std::uint8_t {
    None,         ///< allow self-trades
    CancelTaker,  ///< cancel the rest of the incoming order
    CancelMaker,  ///< cancel the resting order and keep matching
    CancelBoth,   ///< cancel both
};

struct NewOrder {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;  ///< limit price in ticks; ignored for market orders
    Qty qty = 0;
    TimeInForce tif = TimeInForce::GTC;
    bool post_only = false;
    /// When set, the order waits off-book until the last trade price reaches
    /// it (buy: last >= stop, sell: last <= stop), then enters as a market or
    /// limit order depending on `type`.
    std::optional<Price> stop;
};

enum class RejectReason : std::uint8_t {
    DuplicateId,
    InvalidQuantity,
    InvalidPrice,
    InvalidCombination,
    PostOnlyWouldCross,
    UnknownOrder,
};

enum class CancelReason : std::uint8_t {
    Requested,  ///< explicit cancel
    Unfilled,   ///< IOC/FOK/market remainder that could not trade
    SelfTrade,  ///< removed by self-trade prevention
};

namespace ev {
struct Accepted {
    OrderId id;
    bool operator==(const Accepted&) const = default;
};
struct Rejected {
    OrderId id;
    RejectReason reason;
    bool operator==(const Rejected&) const = default;
};
struct Trade {
    OrderId taker;
    OrderId maker;
    TraderId taker_trader;
    TraderId maker_trader;
    Side taker_side;
    Price price;  ///< always the maker's (resting) price
    Qty qty;
    bool operator==(const Trade&) const = default;
};
struct Rested {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    bool operator==(const Rested&) const = default;
};
struct Cancelled {
    OrderId id;
    Qty remaining;
    CancelReason reason;
    bool operator==(const Cancelled&) const = default;
};
struct Modified {
    OrderId id;
    Price price;
    Qty qty;
    bool kept_priority;
    bool operator==(const Modified&) const = default;
};
struct StopParked {
    OrderId id;
    Price stop;
    bool operator==(const StopParked&) const = default;
};
struct StopTriggered {
    OrderId id;
    Price last;
    bool operator==(const StopTriggered&) const = default;
};
}  // namespace ev

using Event = std::variant<ev::Accepted, ev::Rejected, ev::Trade, ev::Rested, ev::Cancelled,
                           ev::Modified, ev::StopParked, ev::StopTriggered>;

std::string to_string(RejectReason r);
std::string to_string(CancelReason r);
std::string to_string(const Event& e);
std::ostream& operator<<(std::ostream& os, const Event& e);

/// Single-instrument matching engine with price-time priority.
///
/// Every call returns the events it produced, in order. The engine is
/// deterministic: the same command sequence always yields the same events,
/// which is what makes replay and differential testing possible.
class Engine {
public:
    explicit Engine(StpMode stp = StpMode::None) : stp_(stp) {}

    std::vector<Event> submit(const NewOrder& order);
    std::vector<Event> cancel(OrderId id);
    /// Cancel/replace a resting order. Reducing quantity at the same price keeps
    /// queue priority; any price change or size increase re-queues the order
    /// (and may trade immediately if the new price crosses).
    std::vector<Event> modify(OrderId id, Price new_price, Qty new_qty);

    [[nodiscard]] const Book& book() const { return book_; }
    [[nodiscard]] std::optional<Price> last_trade_price() const { return last_; }
    [[nodiscard]] std::size_t pending_stops() const { return stop_index_.size(); }
    [[nodiscard]] StpMode stp_mode() const { return stp_; }

private:
    using StopKey = std::pair<Price, std::uint64_t>;  // (stop price, sequence)
    struct StopLoc {
        Side side;
        StopKey key;
    };

    [[nodiscard]] bool live(OrderId id) const { return book_.contains(id) || stop_index_.contains(id); }
    [[nodiscard]] bool would_cross(Side side, Price limit) const;
    [[nodiscard]] bool fok_fillable(const NewOrder& o) const;
    void execute(const NewOrder& o, std::uint64_t seq);
    void trigger_stops();

    Book book_;
    StpMode stp_;
    std::uint64_t seq_ = 0;
    std::optional<Price> last_;
    std::map<StopKey, NewOrder> buy_stops_;   // lowest stop triggers first
    std::map<StopKey, NewOrder> sell_stops_;  // highest stop triggers first
    std::unordered_map<OrderId, StopLoc> stop_index_;
    std::vector<Event> out_;
};

}  // namespace mb
