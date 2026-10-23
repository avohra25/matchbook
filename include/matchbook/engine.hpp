#pragma once

#include "matchbook/book.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mb {

enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

struct NewOrder {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;  ///< limit price in ticks; ignored for market orders
    Qty qty = 0;
    TimeInForce tif = TimeInForce::GTC;
    bool post_only = false;
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
}  // namespace ev

using Event = std::variant<ev::Accepted, ev::Rejected, ev::Trade, ev::Rested, ev::Cancelled>;

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
    std::vector<Event> submit(const NewOrder& order);
    std::vector<Event> cancel(OrderId id);

    [[nodiscard]] const Book& book() const { return book_; }
    [[nodiscard]] std::optional<Price> last_trade_price() const { return last_; }

private:
    [[nodiscard]] bool live(OrderId id) const { return book_.contains(id); }
    [[nodiscard]] bool would_cross(Side side, Price limit) const;
    [[nodiscard]] bool fok_fillable(const NewOrder& o) const;
    void execute(const NewOrder& o, std::uint64_t seq);

    Book book_;
    std::uint64_t seq_ = 0;
    std::optional<Price> last_;
    std::vector<Event> out_;
};

}  // namespace mb
