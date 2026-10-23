#include "matchbook/engine.hpp"

#include "harness.hpp"

using namespace mb;

namespace {
NewOrder limit(OrderId id, TraderId trader, Side side, Price px, Qty q) {
    return NewOrder{.id = id, .trader = trader, .side = side, .type = OrderType::Limit, .price = px, .qty = q};
}
NewOrder stop_market(OrderId id, Side side, Price stop, Qty q) {
    return NewOrder{.id = id, .trader = 900, .side = side, .type = OrderType::Market, .qty = q,
                    .tif = TimeInForce::IOC, .stop = stop};
}
template <class T>
std::vector<T> only(const std::vector<Event>& events) {
    std::vector<T> out;
    for (const auto& e : events)
        if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
    return out;
}
}  // namespace

TEST(stop_parks_until_last_trade_reaches_it) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 5));
    e.submit(limit(2, 1, Side::Sell, 105, 5));
    auto parked = e.submit(stop_market(3, Side::Buy, 100, 2));
    CHECK_EQ(parked, (std::vector<Event>{ev::Accepted{3}, ev::StopParked{3, 100}}));
    CHECK_EQ(e.pending_stops(), 1u);

    auto events = e.submit(limit(4, 2, Side::Buy, 100, 1));  // trades at 100 -> stop fires
    CHECK_EQ(only<ev::StopTriggered>(events), (std::vector<ev::StopTriggered>{{3, 100}}));
    auto trades = only<ev::Trade>(events);
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(trades[1].taker, 3u);
    CHECK_EQ(trades[1].qty, 2);
    CHECK_EQ(e.pending_stops(), 0u);
}

TEST(stop_already_through_triggers_on_submit) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 50, 5));
    e.submit(limit(2, 2, Side::Sell, 50, 1));  // last = 50
    auto events = e.submit(stop_market(3, Side::Sell, 60, 2));
    CHECK_EQ(only<ev::StopTriggered>(events), (std::vector<ev::StopTriggered>{{3, 50}}));
    CHECK_EQ(only<ev::Trade>(events).size(), 1u);
}

TEST(sell_stops_cascade) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 100, 1));
    e.submit(limit(2, 1, Side::Buy, 95, 1));
    e.submit(limit(3, 1, Side::Buy, 90, 1));
    e.submit(stop_market(10, Side::Sell, 100, 1));  // fires at 100, hits 95
    e.submit(stop_market(11, Side::Sell, 95, 1));   // then fires at 95, hits 90
    auto events = e.submit(limit(4, 2, Side::Sell, 100, 1));
    std::vector<OrderId> fired;
    for (auto& t : only<ev::StopTriggered>(events)) fired.push_back(t.id);
    CHECK_EQ(fired, (std::vector<OrderId>{10, 11}));
    CHECK_EQ(e.last_trade_price(), std::optional<Price>{90});
    CHECK(e.book().empty());
}

TEST(stop_limit_rests_when_it_cannot_fill) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 1));
    e.submit(limit(2, 1, Side::Sell, 110, 5));
    NewOrder sl{.id = 3, .trader = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 105, .qty = 2, .stop = 100};
    e.submit(sl);
    auto events = e.submit(limit(4, 4, Side::Buy, 100, 1));
    CHECK_EQ(only<ev::Rested>(events), (std::vector<ev::Rested>{{3, Side::Buy, 105, 2}}));
}

TEST(parked_stops_can_be_cancelled) {
    Engine e;
    e.submit(stop_market(1, Side::Buy, 100, 4));
    CHECK_EQ(e.cancel(1), (std::vector<Event>{ev::Cancelled{1, 4, CancelReason::Requested}}));
    CHECK_EQ(e.pending_stops(), 0u);
    CHECK_EQ(e.submit(stop_market(1, Side::Buy, 100, 4)).size(), 2u);  // id is free again
}

TEST(stp_cancel_taker_stops_matching_at_own_order) {
    Engine e(StpMode::CancelTaker);
    e.submit(limit(1, 2, Side::Sell, 100, 3));
    e.submit(limit(2, 7, Side::Sell, 100, 3));
    e.submit(limit(3, 2, Side::Sell, 100, 3));
    auto events = e.submit(limit(4, 7, Side::Buy, 100, 9));
    CHECK_EQ(only<ev::Trade>(events).size(), 1u);
    CHECK_EQ(only<ev::Cancelled>(events), (std::vector<ev::Cancelled>{{4, 6, CancelReason::SelfTrade}}));
    CHECK(e.book().contains(2));
}

TEST(stp_cancel_maker_removes_own_orders_and_continues) {
    Engine e(StpMode::CancelMaker);
    e.submit(limit(1, 7, Side::Sell, 100, 3));
    e.submit(limit(2, 2, Side::Sell, 101, 3));
    auto events = e.submit(limit(3, 7, Side::Buy, 101, 3));
    CHECK_EQ(only<ev::Cancelled>(events), (std::vector<ev::Cancelled>{{1, 3, CancelReason::SelfTrade}}));
    CHECK_EQ(only<ev::Trade>(events), (std::vector<ev::Trade>{{3, 2, 7, 2, Side::Buy, 101, 3}}));
}

TEST(stp_cancel_both) {
    Engine e(StpMode::CancelBoth);
    e.submit(limit(1, 7, Side::Sell, 100, 3));
    auto events = e.submit(limit(2, 7, Side::Buy, 100, 5));
    CHECK_EQ(only<ev::Cancelled>(events),
             (std::vector<ev::Cancelled>{{1, 3, CancelReason::SelfTrade}, {2, 5, CancelReason::SelfTrade}}));
    CHECK(e.book().empty());
}

TEST(fok_respects_self_trade_prevention) {
    Engine e(StpMode::CancelTaker);
    e.submit(limit(1, 7, Side::Sell, 100, 5));
    e.submit(limit(2, 2, Side::Sell, 101, 5));
    NewOrder fok = limit(3, 7, Side::Buy, 101, 5);
    fok.tif = TimeInForce::FOK;
    auto events = e.submit(fok);
    CHECK(only<ev::Trade>(events).empty());
    CHECK_EQ(e.book().order_count(), 2u);
}

TEST(modify_down_keeps_priority) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 100, 10));
    e.submit(limit(2, 2, Side::Buy, 100, 10));
    CHECK_EQ(e.modify(1, 100, 4), (std::vector<Event>{ev::Modified{1, 100, 4, true}}));
    auto trades = only<ev::Trade>(e.submit(limit(3, 3, Side::Sell, 100, 4)));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].maker, 1u);
}

TEST(modify_up_or_reprice_loses_priority) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 100, 10));
    e.submit(limit(2, 2, Side::Buy, 100, 10));
    CHECK_EQ(e.modify(1, 100, 11), (std::vector<Event>{ev::Modified{1, 100, 11, false}, ev::Rested{1, Side::Buy, 100, 11}}));
    auto trades = only<ev::Trade>(e.submit(limit(3, 3, Side::Sell, 100, 1)));
    CHECK_EQ(trades[0].maker, 2u);
}

TEST(modify_into_cross_trades_immediately) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 105, 3));
    e.submit(limit(2, 2, Side::Buy, 100, 5));
    auto events = e.modify(2, 105, 5);
    CHECK_EQ(only<ev::Trade>(events), (std::vector<ev::Trade>{{2, 1, 2, 1, Side::Buy, 105, 3}}));
    CHECK_EQ(e.book().find(2)->remaining, 2);
}

TEST(modify_rejects_unknown_and_invalid) {
    Engine e;
    CHECK_EQ(e.modify(9, 100, 1), (std::vector<Event>{ev::Rejected{9, RejectReason::UnknownOrder}}));
    e.submit(limit(1, 1, Side::Buy, 100, 1));
    CHECK_EQ(e.modify(1, 100, 0), (std::vector<Event>{ev::Rejected{1, RejectReason::InvalidQuantity}}));
    CHECK_EQ(e.modify(1, -3, 1), (std::vector<Event>{ev::Rejected{1, RejectReason::InvalidPrice}}));
    CHECK_EQ(e.book().find(1)->remaining, 1);
}

TEST(event_strings_are_stable) {
    CHECK_EQ(to_string(Event{ev::Trade{2, 1, 20, 10, Side::Buy, 100, 4}}),
             std::string("TRADE taker=2 maker=1 side=buy px=100 qty=4"));
    CHECK_EQ(to_string(Event{ev::Cancelled{3, 2, CancelReason::Unfilled}}),
             std::string("CANCELLED id=3 remaining=2 reason=unfilled"));
}
