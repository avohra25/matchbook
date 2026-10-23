#include "matchbook/engine.hpp"

#include "harness.hpp"

using namespace mb;

namespace {
NewOrder limit(OrderId id, Side side, Price px, Qty q, TimeInForce tif = TimeInForce::GTC) {
    return NewOrder{.id = id, .trader = static_cast<TraderId>(id), .side = side, .type = OrderType::Limit,
                    .price = px, .qty = q, .tif = tif};
}
NewOrder market(OrderId id, Side side, Qty q) {
    return NewOrder{.id = id, .trader = static_cast<TraderId>(id), .side = side, .type = OrderType::Market,
                    .qty = q, .tif = TimeInForce::IOC};
}
template <class T>
std::vector<T> only(const std::vector<Event>& events) {
    std::vector<T> out;
    for (const auto& e : events)
        if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
    return out;
}
}  // namespace

TEST(non_crossing_limit_orders_rest) {
    Engine e;
    auto ev1 = e.submit(limit(1, Side::Buy, 100, 10));
    CHECK_EQ(ev1, (std::vector<Event>{ev::Accepted{1}, ev::Rested{1, Side::Buy, 100, 10}}));
    e.submit(limit(2, Side::Sell, 101, 5));
    CHECK_EQ(e.book().best_bid(), std::optional<Price>{100});
    CHECK_EQ(e.book().best_ask(), std::optional<Price>{101});
}

TEST(crossing_order_trades_at_maker_price) {
    Engine e;
    e.submit(limit(1, Side::Sell, 100, 10));
    auto events = e.submit(limit(2, Side::Buy, 105, 4));
    CHECK_EQ(only<ev::Trade>(events), (std::vector<ev::Trade>{{2, 1, 2, 1, Side::Buy, 100, 4}}));
    CHECK_EQ(e.book().find(1)->remaining, 6);
    CHECK(!e.book().contains(2));
    CHECK_EQ(e.last_trade_price(), std::optional<Price>{100});
}

TEST(taker_sweeps_levels_best_price_first_then_fifo) {
    Engine e;
    e.submit(limit(1, Side::Sell, 102, 5));
    e.submit(limit(2, Side::Sell, 101, 3));
    e.submit(limit(3, Side::Sell, 101, 3));
    e.submit(limit(4, Side::Sell, 103, 5));
    auto trades = only<ev::Trade>(e.submit(limit(5, Side::Buy, 102, 9)));
    CHECK_EQ(trades.size(), 3u);
    CHECK_EQ(trades[0].maker, 2u);
    CHECK_EQ(trades[1].maker, 3u);
    CHECK_EQ(trades[2].maker, 1u);
    CHECK_EQ(trades[2].qty, 3);
    CHECK_EQ(e.book().find(1)->remaining, 2);
    CHECK_EQ(e.book().best_ask(), std::optional<Price>{102});
}

TEST(unfilled_remainder_of_gtc_limit_rests) {
    Engine e;
    e.submit(limit(1, Side::Sell, 100, 3));
    auto events = e.submit(limit(2, Side::Buy, 100, 10));
    CHECK_EQ(only<ev::Rested>(events), (std::vector<ev::Rested>{{2, Side::Buy, 100, 7}}));
    CHECK_EQ(e.book().best_bid(), std::optional<Price>{100});
    CHECK(!e.book().best_ask());
}

TEST(market_order_takes_liquidity_and_cancels_remainder) {
    Engine e;
    e.submit(limit(1, Side::Buy, 99, 5));
    e.submit(limit(2, Side::Buy, 98, 5));
    auto events = e.submit(market(3, Side::Sell, 12));
    auto trades = only<ev::Trade>(events);
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(trades[1].price, 98);
    CHECK_EQ(only<ev::Cancelled>(events), (std::vector<ev::Cancelled>{{3, 2, CancelReason::Unfilled}}));
    CHECK(e.book().empty());
}

TEST(market_order_into_empty_book_is_cancelled) {
    Engine e;
    auto events = e.submit(market(1, Side::Buy, 5));
    CHECK_EQ(events, (std::vector<Event>{ev::Accepted{1}, ev::Cancelled{1, 5, CancelReason::Unfilled}}));
}

TEST(ioc_never_rests) {
    Engine e;
    e.submit(limit(1, Side::Sell, 100, 2));
    auto events = e.submit(limit(2, Side::Buy, 100, 5, TimeInForce::IOC));
    CHECK_EQ(only<ev::Trade>(events).size(), 1u);
    CHECK_EQ(only<ev::Cancelled>(events), (std::vector<ev::Cancelled>{{2, 3, CancelReason::Unfilled}}));
    CHECK(e.book().empty());
}

TEST(fok_fills_completely_or_not_at_all) {
    Engine e;
    e.submit(limit(1, Side::Sell, 100, 4));
    e.submit(limit(2, Side::Sell, 101, 4));
    e.submit(limit(3, Side::Sell, 102, 4));

    auto miss = e.submit(limit(4, Side::Buy, 101, 9, TimeInForce::FOK));
    CHECK(only<ev::Trade>(miss).empty());
    CHECK_EQ(only<ev::Cancelled>(miss), (std::vector<ev::Cancelled>{{4, 9, CancelReason::Unfilled}}));
    CHECK_EQ(e.book().order_count(), 3u);

    auto hit = e.submit(limit(5, Side::Buy, 102, 9, TimeInForce::FOK));
    CHECK_EQ(only<ev::Trade>(hit).size(), 3u);
    CHECK(only<ev::Cancelled>(hit).empty());
    CHECK_EQ(e.book().find(3)->remaining, 3);
}

TEST(post_only_rejects_when_it_would_take) {
    Engine e;
    e.submit(limit(1, Side::Sell, 100, 4));
    NewOrder po = limit(2, Side::Buy, 100, 1);
    po.post_only = true;
    CHECK_EQ(e.submit(po), (std::vector<Event>{ev::Rejected{2, RejectReason::PostOnlyWouldCross}}));
    po.price = 99;
    CHECK_EQ(only<ev::Rested>(e.submit(po)).size(), 1u);
}

TEST(validation_rejects_bad_orders) {
    Engine e;
    e.submit(limit(1, Side::Buy, 100, 1));
    CHECK_EQ(e.submit(limit(1, Side::Buy, 100, 1)), (std::vector<Event>{ev::Rejected{1, RejectReason::DuplicateId}}));
    CHECK_EQ(e.submit(limit(2, Side::Buy, 100, 0)), (std::vector<Event>{ev::Rejected{2, RejectReason::InvalidQuantity}}));
    CHECK_EQ(e.submit(limit(3, Side::Buy, 0, 5)), (std::vector<Event>{ev::Rejected{3, RejectReason::InvalidPrice}}));
    NewOrder bad = market(4, Side::Buy, 1);
    bad.post_only = true;
    CHECK_EQ(e.submit(bad), (std::vector<Event>{ev::Rejected{4, RejectReason::InvalidCombination}}));
}

TEST(cancel_removes_resting_orders_and_rejects_unknown_ids) {
    Engine e;
    e.submit(limit(1, Side::Buy, 100, 7));
    CHECK_EQ(e.cancel(1), (std::vector<Event>{ev::Cancelled{1, 7, CancelReason::Requested}}));
    CHECK(e.book().empty());
    CHECK_EQ(e.cancel(1), (std::vector<Event>{ev::Rejected{1, RejectReason::UnknownOrder}}));
}

TEST(filled_ids_can_be_reused) {
    Engine e;
    e.submit(limit(1, Side::Sell, 100, 1));
    e.submit(limit(2, Side::Buy, 100, 1));
    auto events = e.submit(limit(1, Side::Buy, 90, 1));
    CHECK_EQ(only<ev::Rested>(events).size(), 1u);
}
